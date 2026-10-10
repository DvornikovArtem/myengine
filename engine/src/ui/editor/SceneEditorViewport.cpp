#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    void SceneEditor::BuildViewportPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showViewport)
        {
            return;
        }

        auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin(kViewportWindowName, nullptr, flags))
        {
            const ImVec2 contentMin = ImGui::GetCursorScreenPos();
            const ImVec2 contentSize = ImGui::GetContentRegionAvail();
            windowState.viewport = {
                contentMin.x,
                contentMin.y,
                std::max(contentSize.x, 1.0f),
                std::max(contentSize.y, 1.0f),
            };
            const float toolbarWidth =
                std::min(std::max(windowState.viewport.width - kViewportToolbarPadding * 2.0f, 320.0f), 860.0f);
            const ImVec2 toolbarMin{
                windowState.viewport.x + kViewportToolbarPadding,
                windowState.viewport.y + kViewportToolbarPadding,
            };
            const ImVec2 toolbarMax{
                toolbarMin.x + toolbarWidth,
                toolbarMin.y + kViewportToolbarHeight,
            };
            windowState.viewportToolbar = {
                toolbarMin.x,
                toolbarMin.y,
                toolbarWidth,
                kViewportToolbarHeight,
            };
            const ImVec2 mousePosition = ImGui::GetIO().MousePos;
            const bool mouseInToolbarRegion =
                mousePosition.x >= toolbarMin.x &&
                mousePosition.x <= toolbarMax.x &&
                mousePosition.y >= toolbarMin.y &&
                mousePosition.y <= toolbarMax.y;

            ImGui::ItemSize(ImVec2(windowState.viewport.width, windowState.viewport.height), 0.0f);
            const ImRect canvasRect(
                ImVec2(windowState.viewport.x, windowState.viewport.y),
                ImVec2(windowState.viewport.x + windowState.viewport.width, windowState.viewport.y + windowState.viewport.height));
            const bool canvasHovered =
                ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                canvasRect.Contains(mousePosition);
            const bool canvasLeftClicked =
                canvasHovered &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            windowState.viewportHovered = canvasHovered;
            windowState.viewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

            if (ImGui::BeginDragDropTargetCustom(canvasRect, ImGui::GetID("##viewport_canvas_target")))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMeshPayloadType))
                {
                    const std::string beforeSnapshot = services_.captureSceneSnapshot();
                    const std::string meshPath(static_cast<const char*>(payload->Data), payload->DataSize - 1u);
                    const std::string materialPath = ResolveSuggestedMaterialForMesh(meshPath);
                    SpawnRenderableEntity(windowContext.windowId, meshPath, materialPath);
                    RecordSceneMutationImmediate("Spawn Mesh From Asset Browser", beforeSnapshot);
                }

                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMaterialPayloadType))
                {
                    const std::string materialPath(static_cast<const char*>(payload->Data), payload->DataSize - 1u);
                    ecs::EntityId targetEntity = PickEntityAtScreenPosition(
                        windowContext,
                        windowState,
                        mousePosition);
                    if (targetEntity == ecs::kInvalidEntity)
                    {
                        targetEntity = editorState.selectedEntity;
                    }

                    if (targetEntity != ecs::kInvalidEntity)
                    {
                        if (auto* renderer = services_.world->TryGet<ecs::components::MeshRendererComponent>(targetEntity);
                            renderer != nullptr && renderer->materialPath != materialPath)
                        {
                            const std::string beforeSnapshot = services_.captureSceneSnapshot();
                            renderer->materialPath = materialPath;
                            editorState.selectedEntity = targetEntity;
                            RecordSceneMutationImmediate("Assign Material From Asset Browser", beforeSnapshot);
                        }
                    }
                }

                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTexturePayloadType))
                {
                    const std::string beforeSnapshot = services_.captureSceneSnapshot();
                    const std::string texturePath(static_cast<const char*>(payload->Data), payload->DataSize - 1u);
                    const std::string materialPath = EnsureTexturePreviewMaterial(texturePath);
                    if (!materialPath.empty())
                    {
                        SpawnRenderableEntity(windowContext.windowId, kCubeMeshPath, materialPath);
                        RecordSceneMutationImmediate("Spawn Textured Cube From Asset Browser", beforeSnapshot);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            bool toolbarHovered = false;

            if (editorState.mode == editor::RuntimeMode::Edit &&
                editorState.selectedEntity != ecs::kInvalidEntity &&
                windowState.renderStats.camera.available)
            {
                const bool wasUsing = gizmoWasUsing_;
                const bool gizmoChanged = gizmo_->DrawAndHandle(editor::TransformGizmo::Context{
                    *services_.world,
                    editorState.selectedEntity,
                    windowContext.windowId,
                    windowState.viewport,
                    scene::ToDirectXMatrix(windowState.renderStats.camera.view),
                    scene::ToDirectXMatrix(windowState.renderStats.camera.projection),
                    editorState.gizmoOperation,
                    editorState.gizmoSpace,
                    !mouseInToolbarRegion,
                    services_.logger,
                });

                windowState.gizmoHovered = gizmo_->IsHovered();
                windowState.gizmoActive = gizmo_->IsUsing();
                if (gizmoChanged)
                {
                    editorState.sceneDirty = true;
                }

                if (!wasUsing && gizmo_->IsUsing() && pendingGizmoMutationSnapshot_.empty() && services_.captureSceneSnapshot != nullptr)
                {
                    pendingGizmoMutationSnapshot_ = services_.captureSceneSnapshot();
                }

                if (wasUsing && !gizmo_->IsUsing())
                {
                    CommitPendingGizmoMutation();
                }
                gizmoWasUsing_ = gizmo_->IsUsing();
            }

            toolbarHovered = BuildToolbar(windowContext, windowState.viewport);

            if (editorState.mode == editor::RuntimeMode::Play && services_.scriptHudLines)
            {
                auto* drawList = ImGui::GetWindowDrawList();
                const auto& viewport = windowState.viewport;
                drawList->PushClipRect(ImVec2(viewport.x, viewport.y), ImVec2(viewport.x + viewport.width, viewport.y + viewport.height), true);
                ImVec2 position(viewport.x + 12.0f, viewport.y + kViewportToolbarHeight + 12.0f);
                for (const auto& text : services_.scriptHudLines())
                {
                    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
                    drawList->AddRectFilled(ImVec2(position.x - 6.0f, position.y - 4.0f),
                        ImVec2(position.x + size.x + 6.0f, position.y + size.y + 4.0f), IM_COL32(0, 0, 0, 160), 4.0f);
                    drawList->AddText(position, IM_COL32(255, 255, 255, 255), text.c_str());
                    position.y += size.y + 12.0f;
                }
                drawList->PopClipRect();
            }

            if (canvasLeftClicked &&
                editorState.mode == editor::RuntimeMode::Edit &&
                !mouseInToolbarRegion &&
                !toolbarHovered &&
                !windowState.gizmoHovered &&
                !windowState.gizmoActive)
            {
                editorState.selectedEntity = PickEntityAtScreenPosition(
                    windowContext,
                    windowState,
                    mousePosition);
            }

            windowState.viewportAcceptsCameraNavigation =
                canvasHovered &&
                !mouseInToolbarRegion &&
                !toolbarHovered &&
                !windowState.gizmoHovered &&
                !windowState.gizmoActive &&
                !ImGui::IsAnyItemActive();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    bool SceneEditor::BuildToolbar(const SceneEditorWindowContext& windowContext, const editor::ViewportRect& viewportRect)
    {
        (void)windowContext;

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        const float overlayWidth = std::min(std::max(viewportRect.width - kViewportToolbarPadding * 2.0f, 320.0f), 860.0f);
        const ImVec2 overlayPosition{viewportRect.x + kViewportToolbarPadding, viewportRect.y + kViewportToolbarPadding};

        ImGui::SetCursorScreenPos(overlayPosition);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.17f, 0.20f, 0.24f, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.24f, 0.29f, 0.34f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.28f, 0.34f, 0.40f, 0.98f));

        ImGui::BeginGroup();

        const bool isEditMode = editorState.mode == editor::RuntimeMode::Edit;

        // Mode badge: bright in Play, calm in Edit; the viewport gets a frame of the same colour in Play
        {
            const ImU32 badgeColor = isEditMode ? kEditBadgeColor : kPlayBadgeColor;
            const char* badgeText = isEditMode ? "EDIT" : "PLAYING";
            const ImVec2 textSize = ImGui::CalcTextSize(badgeText);
            const ImVec2 badgePadding{10.0f, 3.0f};
            const ImVec2 badgeMin = ImGui::GetCursorScreenPos();
            const ImVec2 badgeSize{textSize.x + badgePadding.x * 2.0f, ImGui::GetFrameHeight()};
            auto* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(badgeMin, ImVec2(badgeMin.x + badgeSize.x, badgeMin.y + badgeSize.y), badgeColor, 6.0f);
            drawList->AddText(
                ImVec2(badgeMin.x + badgePadding.x, badgeMin.y + (badgeSize.y - textSize.y) * 0.5f),
                IM_COL32(255, 255, 255, 255),
                badgeText);
            ImGui::Dummy(badgeSize);

            if (!isEditMode)
            {
                const float half = kPlayFrameThickness * 0.5f;
                drawList->AddRect(
                    ImVec2(viewportRect.x + half, viewportRect.y + half),
                    ImVec2(viewportRect.x + viewportRect.width - half, viewportRect.y + viewportRect.height - half),
                    badgeColor,
                    0.0f,
                    0,
                    kPlayFrameThickness);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        ImGui::BeginDisabled(!isEditMode);
        if (ImGui::Button("Play"))
        {
            if (isEditMode && services_.captureSceneSnapshot)
            {
                editorState.playModeSnapshot = services_.captureSceneSnapshot();
                editorState.mode = editor::RuntimeMode::Play;
                physicsState.physicsPaused = false;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(isEditMode);
        const bool stopPressed = ImGui::Button("Stop");
        ImGui::EndDisabled();
        if (stopPressed && editorState.mode == editor::RuntimeMode::Play)
        {
            const bool restoredScene =
                editorState.playModeSnapshot.empty() ||
                (services_.restoreSceneSnapshot != nullptr &&
                    services_.restoreSceneSnapshot(editorState.playModeSnapshot));

            if (restoredScene)
            {
                editorState.mode = editor::RuntimeMode::Edit;
                physicsState.physicsPaused = true;
                editorState.playModeSnapshot.clear();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload scripts") && services_.reloadScripts)
        {
            services_.reloadScripts();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Hot reload of every script (F5). Saved files are reloaded automatically");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Keep script state", &editorState.scriptReloadKeepsState);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Hot reload in Play.\n"
                "On: running objects keep their state, fields with a changed default take the new value (L2).\n"
                "Off: running objects start over with OnStart (L1)");
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        const bool editEnabled = isEditMode;
        if (!editEnabled)
        {
            ImGui::BeginDisabled();
        }

        if (ImGui::RadioButton("Translate", editorState.gizmoOperation == editor::GizmoOperation::Translate))
        {
            editorState.gizmoOperation = editor::GizmoOperation::Translate;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate", editorState.gizmoOperation == editor::GizmoOperation::Rotate))
        {
            editorState.gizmoOperation = editor::GizmoOperation::Rotate;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale", editorState.gizmoOperation == editor::GizmoOperation::Scale))
        {
            editorState.gizmoOperation = editor::GizmoOperation::Scale;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::RadioButton("Local", editorState.gizmoSpace == editor::GizmoSpace::Local))
        {
            editorState.gizmoSpace = editor::GizmoSpace::Local;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("World", editorState.gizmoSpace == editor::GizmoSpace::World))
        {
            editorState.gizmoSpace = editor::GizmoSpace::World;
        }

        if (!editEnabled)
        {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", editorState.sceneDirty ? "modified" : "saved");

        ImGui::EndGroup();

        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        (void)overlayWidth;

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
        return hovered;
    }

    bool SceneEditor::CanStartSceneNavigation(const core::WindowId windowId, const float mouseX, const float mouseY) const
    {
        const auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        const auto* windowState = editorState.FindWindowState(windowId);
        if (windowState == nullptr || !windowState->viewport.IsValid())
        {
            return false;
        }

        const bool insideViewport =
            mouseX >= windowState->viewport.x &&
            mouseX <= windowState->viewport.x + windowState->viewport.width &&
            mouseY >= windowState->viewport.y &&
            mouseY <= windowState->viewport.y + windowState->viewport.height;
        if (!insideViewport)
        {
            return false;
        }

        const bool insideToolbar =
            windowState->viewportToolbar.IsValid() &&
            mouseX >= windowState->viewportToolbar.x &&
            mouseX <= windowState->viewportToolbar.x + windowState->viewportToolbar.width &&
            mouseY >= windowState->viewportToolbar.y &&
            mouseY <= windowState->viewportToolbar.y + windowState->viewportToolbar.height;

        return !insideToolbar && !windowState->gizmoHovered && !windowState->gizmoActive;
    }

    ecs::EntityId SceneEditor::PickEntityAtScreenPosition(
        const SceneEditorWindowContext& windowContext,
        const editor::WindowEditorState& windowState,
        const ImVec2& mousePosition) const
    {
        if (!windowState.renderStats.camera.available)
        {
            return ecs::kInvalidEntity;
        }

        DirectX::XMVECTOR rayOrigin = DirectX::XMVectorZero();
        DirectX::XMVECTOR rayDirection = DirectX::XMVectorZero();
        if (!BuildScreenRay(
                mousePosition,
                windowState.viewport,
                scene::ToDirectXMatrix(windowState.renderStats.camera.view),
                scene::ToDirectXMatrix(windowState.renderStats.camera.projection),
                rayOrigin,
                rayDirection))
        {
            return ecs::kInvalidEntity;
        }

        ecs::World& world = *services_.world;
        std::unordered_map<ecs::EntityId, DirectX::XMFLOAT4X4> worldMatrixCache;
        std::unordered_set<ecs::EntityId> visiting;

        ecs::EntityId closestEntity = ecs::kInvalidEntity;
        float closestDistance = std::numeric_limits<float>::max();

        world.ForEach<ecs::components::TransformComponent, ecs::components::MeshRendererComponent>(
            [&](const ecs::EntityId entity, ecs::components::TransformComponent&, ecs::components::MeshRendererComponent& renderer)
            {
                if (!renderer.visible || !MatchesWindowBinding(world, entity, windowContext.windowId))
                {
                    return;
                }

                const DirectX::XMMATRIX worldMatrix = scene::ResolveWorldMatrix(
                    world,
                    entity,
                    worldMatrixCache,
                    visiting,
                    nullptr,
                    services_.logger);

                float hitDistance = 0.0f;
                if (IntersectRayWithEntity(world, entity, rayOrigin, rayDirection, worldMatrix, hitDistance) &&
                    hitDistance < closestDistance)
                {
                    closestDistance = hitDistance;
                    closestEntity = entity;
                }
            });

        return closestEntity;
    }
}
