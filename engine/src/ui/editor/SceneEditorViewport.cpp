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

        // Global actions (Play/Stop, gizmo mode, undo...) live in the main toolbar now (BuildMainToolbar).
        // The viewport only marks Play with a frame of the same colour as the mode badge.
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.mode == editor::RuntimeMode::Play)
        {
            const float half = kPlayFrameThickness * 0.5f;
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(viewportRect.x + half, viewportRect.y + half),
                ImVec2(viewportRect.x + viewportRect.width - half, viewportRect.y + viewportRect.height - half),
                kPlayBadgeColor,
                0.0f,
                0,
                kPlayFrameThickness);
        }

        return false;
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
