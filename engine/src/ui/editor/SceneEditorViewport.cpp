#include "SceneEditorInternal.h"

#include "EditorWidgets.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        // ---- geometry (spec 5.1) ----
        constexpr float kGroupPadding = 2.0f;
        constexpr float kGroupGap = 4.0f;
        constexpr float kGroupHeight = style::kViewportButton + kGroupPadding * 2.0f;
        constexpr float kTextPaddingX = 8.0f;
        constexpr float kIconTextGap = 6.0f;
        constexpr float kSeparatorWidth = 9.0f;
        constexpr float kPlayFrame = 2.0f;
        constexpr float kHudTop = style::kViewportInset + style::kViewportButton + 14.0f;
        constexpr float kStatsTop = style::kViewportInset + style::kViewportButton + 12.0f;

        // Camera speed levels 1..8 inside the range the camera system clamps to ([0.2, 30], geometric scale)
        constexpr float kSpeedLevels[8] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 30.0f};
        constexpr float kMinFov = 15.0f;
        constexpr float kMaxFov = 160.0f;

        // Transform group while the game runs: dimmed selected button (spec 5.1 item 5)
        constexpr ImU32 kDisabledSelectedFill = IM_COL32(0x23, 0x3E, 0x5C, 255);
        constexpr ImU32 kDisabledSelectedIcon = IM_COL32(0x8A, 0xA0, 0xB8, 255);

        float TextWidth(const FontRole role, const char* text)
        {
            PushFontRole(role);
            const float width = ImGui::CalcTextSize(text).x;
            PopFontRole();
            return width;
        }

        int NearestSpeedLevel(const float moveSpeed)
        {
            // Distance on the geometric scale: 1.4 is closer to 1 than to 2
            int best = 0;
            float bestDistance = 1e9f;
            for (int index = 0; index < 8; ++index)
            {
                const float distance = std::fabs(std::log(std::max(moveSpeed, 0.01f) / kSpeedLevels[index]));
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = index;
                }
            }
            return best;
        }

        struct ToolbarLayout
        {
            ImVec2 menuMin;
            float menuWidth = 0.0f;
            ImVec2 perspectiveMin;
            float perspectiveWidth = 0.0f;
            ImVec2 litMin;
            float litWidth = 0.0f;
            ImVec2 showMin;
            float showWidth = 0.0f;
            ImVec2 opsMin;
            float opsWidth = 0.0f;
            ImVec2 cameraMin;
            float cameraWidth = 0.0f;
            ImVec2 graphicsMin;
            float graphicsWidth = 0.0f;
            editor::ViewportRect left;
            editor::ViewportRect right;
        };

        float TextButtonWidth(const float textWidth, const bool chevron)
        {
            return kTextPaddingX + 16.0f + kIconTextGap + textWidth + (chevron ? 4.0f + 12.0f : 0.0f) + kTextPaddingX;
        }

        ToolbarLayout ComputeLayout(const editor::ViewportRect& viewport, const bool wireframe)
        {
            ToolbarLayout layout;
            const float y = viewport.y + style::kViewportInset;

            float x = viewport.x + style::kViewportInset;
            layout.menuMin = ImVec2(x, y);
            layout.menuWidth = style::kViewportButton + kGroupPadding * 2.0f;
            x += layout.menuWidth + kGroupGap;
            layout.perspectiveMin = ImVec2(x, y);
            layout.perspectiveWidth = TextButtonWidth(TextWidth(FontRole::Secondary, "Perspective"), false) + kGroupPadding * 2.0f;
            x += layout.perspectiveWidth + kGroupGap;
            layout.litMin = ImVec2(x, y);
            layout.litWidth = TextButtonWidth(TextWidth(FontRole::Secondary, wireframe ? "Wireframe" : "Lit"), true) + kGroupPadding * 2.0f;
            x += layout.litWidth + kGroupGap;
            layout.showMin = ImVec2(x, y);
            layout.showWidth = TextButtonWidth(TextWidth(FontRole::Secondary, "Show"), true) + kGroupPadding * 2.0f;
            x += layout.showWidth;
            layout.left = {layout.menuMin.x, y, x - layout.menuMin.x, kGroupHeight};

            // Right side is laid out from the right edge: graphics, camera speed, transform tools
            float right = viewport.x + viewport.width - style::kViewportInset;
            layout.graphicsWidth = kTextPaddingX + 16.0f + 4.0f + 12.0f + kTextPaddingX + kGroupPadding * 2.0f;
            right -= layout.graphicsWidth;
            layout.graphicsMin = ImVec2(right, y);
            right -= kGroupGap;
            layout.cameraWidth = kTextPaddingX + 16.0f + 5.0f + TextWidth(FontRole::Secondary, "8") + kTextPaddingX + kGroupPadding * 2.0f;
            right -= layout.cameraWidth;
            layout.cameraMin = ImVec2(right, y);
            right -= kGroupGap;
            layout.opsWidth = style::kViewportButton * 4.0f + kSeparatorWidth + kGroupPadding * 2.0f;
            right -= layout.opsWidth;
            layout.opsMin = ImVec2(right, y);
            const float rightEdge = viewport.x + viewport.width - style::kViewportInset;
            layout.right = {layout.opsMin.x, y, rightEdge - layout.opsMin.x, kGroupHeight};
            return layout;
        }

        bool Contains(const editor::ViewportRect& rect, const ImVec2& point)
        {
            return rect.IsValid() && point.x >= rect.x && point.x <= rect.x + rect.width && point.y >= rect.y && point.y <= rect.y + rect.height;
        }

        void DrawGroupBackground(ImDrawList* drawList, const ImVec2 min, const float width)
        {
            drawList->AddRectFilled(min, ImVec2(min.x + width, min.y + kGroupHeight), style::WithAlpha(style::kTitle, 0.86f), style::kRounding);
        }

        struct ButtonState
        {
            bool pressed = false;
            bool hovered = false;
            ImVec2 min;
            ImVec2 max;
        };

        ButtonState SubmitButton(const char* id, const ImVec2 min, const ImVec2 size, const bool enabled)
        {
            ButtonState state;
            state.min = min;
            state.max = ImVec2(min.x + size.x, min.y + size.y);
            ImGui::SetCursorScreenPos(min);
            ImGui::BeginDisabled(!enabled);
            state.pressed = ImGui::InvisibleButton(id, size) && enabled;
            state.hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && enabled;
            ImGui::EndDisabled();
            return state;
        }

        void DrawButtonBackground(ImDrawList* drawList, const ButtonState& state, const bool selected, const bool popupOpen, const bool enabled)
        {
            if (selected)
            {
                drawList->AddRectFilled(state.min, state.max, enabled ? (state.hovered ? style::kPrimaryHover : style::kPrimary) : kDisabledSelectedFill, style::kRoundingSmall);
            }
            else if (state.hovered || popupOpen)
            {
                drawList->AddRectFilled(state.min, state.max, ImGui::IsItemActive() ? style::kControlActive : style::kControl, style::kRoundingSmall);
            }
        }

        // Square icon button of the toolbar; returns true when clicked
        bool ToolbarIconButton(
            ImDrawList* drawList,
            const char* id,
            const char* icon,
            const ImVec2 min,
            const bool selected,
            const bool enabled,
            const char* tooltip,
            const char* detail = nullptr)
        {
            const auto state = SubmitButton(id, min, ImVec2(style::kViewportButton, style::kViewportButton), enabled);
            DrawButtonBackground(drawList, state, selected, false, enabled);
            ImU32 color = style::kText;
            if (!enabled)
            {
                color = selected ? kDisabledSelectedIcon : style::kTextDisabled;
            }
            else if (selected || state.hovered)
            {
                color = style::kTextStrong;
            }
            DrawIcon(drawList, IconSize::Button16, icon, ImVec2((state.min.x + state.max.x) * 0.5f, (state.min.y + state.max.y) * 0.5f), color);
            if (tooltip != nullptr)
            {
                Tooltip(tooltip, nullptr, detail);
            }
            return state.pressed;
        }

        // Icon + label (+ chevron) button; `interactive` false draws a plain caption (Perspective)
        ButtonState ToolbarTextButton(
            ImDrawList* drawList,
            const char* id,
            const char* icon,
            const char* label,
            const ImVec2 groupMin,
            const float groupWidth,
            const bool chevron,
            const bool interactive,
            const bool popupOpen,
            const char* tooltip = nullptr)
        {
            const ImVec2 min(groupMin.x + kGroupPadding, groupMin.y + kGroupPadding);
            const ImVec2 size(groupWidth - kGroupPadding * 2.0f, style::kViewportButton);
            ButtonState state;
            state.min = min;
            state.max = ImVec2(min.x + size.x, min.y + size.y);
            if (interactive)
            {
                state = SubmitButton(id, min, size, true);
                DrawButtonBackground(drawList, state, false, popupOpen, true);
            }
            const bool bright = interactive && (state.hovered || popupOpen);
            const ImU32 color = bright ? style::kTextStrong : style::kText;
            const float centerY = (min.y + state.max.y) * 0.5f;
            DrawIcon(drawList, IconSize::Button16, icon, ImVec2(min.x + kTextPaddingX + 8.0f, centerY), color);
            PushFontRole(FontRole::Secondary);
            const ImVec2 textSize = ImGui::CalcTextSize(label);
            drawList->AddText(ImVec2(min.x + kTextPaddingX + 16.0f + kIconTextGap, centerY - textSize.y * 0.5f), color, label);
            PopFontRole();
            if (chevron)
            {
                DrawIcon(drawList, IconSize::Chevron12, ICON_CHEVRON_DOWN, ImVec2(state.max.x - kTextPaddingX - 6.0f, centerY), style::kTextDim);
            }
            if (interactive && tooltip != nullptr && !popupOpen)
            {
                Tooltip(tooltip);
            }
            return state;
        }

        // ---- popups of the toolbar ----
        bool BeginToolbarPopup(const char* id, const ImVec2 anchorMin, const ImVec2 anchorMax, const float width)
        {
            ImGui::SetNextWindowPos(ImVec2(anchorMin.x, anchorMax.y + 4.0f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, style::kRounding);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PopupBg, style::ToVec4(style::kRecessed));
            ImGui::PushStyleColor(ImGuiCol_Border, style::ToVec4(style::kBorderLight));
            const bool open = ImGui::BeginPopup(id);
            if (!open)
            {
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(3);
            }
            return open;
        }

        void EndToolbarPopup()
        {
            ImGui::EndPopup();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
        }

        void PopupSection(const char* text)
        {
            PushFontRole(FontRole::Tiny);
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            PopFontRole();
        }

        enum class RowKind
        {
            Plain,
            Checkbox, // 16 px box, blue when on
            Radio,    // a check mark in the left gutter on the chosen row
        };

        // One full-width row of a toolbar popup; returns true when clicked. The popup stays open.
        bool PopupRow(const char* id, const char* label, const char* shortcut, const RowKind kind, const bool checked, const bool enabled = true)
        {
            const float width = ImGui::GetContentRegionAvail().x;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float height = 26.0f;
            ImGui::BeginDisabled(!enabled);
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height)) && enabled;
            const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && enabled;
            ImGui::EndDisabled();

            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 max(min.x + width, min.y + height);
            if (hovered)
            {
                drawList->AddRectFilled(min, max, style::kControl, style::kRoundingSmall);
            }
            const float centerY = (min.y + max.y) * 0.5f;
            float x = min.x + 8.0f;
            if (kind == RowKind::Checkbox)
            {
                const ImVec2 boxMin(x, centerY - 8.0f);
                const ImVec2 boxMax(x + 16.0f, centerY + 8.0f);
                if (checked)
                {
                    drawList->AddRectFilled(boxMin, boxMax, style::kPrimary, style::kRoundingSmall);
                    DrawIcon(drawList, IconSize::Row14, ICON_CHECK, ImVec2(x + 8.0f, centerY), style::kTextStrong);
                }
                else
                {
                    drawList->AddRectFilled(boxMin, boxMax, style::kInput, style::kRoundingSmall);
                    drawList->AddRect(boxMin, boxMax, style::kCheckBorder, style::kRoundingSmall);
                }
                x += 16.0f + 8.0f;
            }
            else if (kind == RowKind::Radio)
            {
                if (checked)
                {
                    DrawIcon(drawList, IconSize::Row14, ICON_CHECK, ImVec2(x + 7.0f, centerY), style::kTextStrong);
                }
                x += 14.0f + 8.0f;
            }
            const ImU32 textColor = !enabled ? style::kTextDisabled : (hovered ? style::kTextStrong : style::kText);
            PushFontRole(FontRole::Body);
            const float textHeight = ImGui::CalcTextSize(label).y;
            drawList->AddText(ImVec2(x, centerY - textHeight * 0.5f), textColor, label);
            PopFontRole();
            if (shortcut != nullptr && shortcut[0] != '\0')
            {
                PushFontRole(FontRole::Secondary);
                const ImVec2 shortcutSize = ImGui::CalcTextSize(shortcut);
                drawList->AddText(ImVec2(max.x - 8.0f - shortcutSize.x, centerY - shortcutSize.y * 0.5f), style::kTextDim, shortcut);
                PopFontRole();
            }
            return pressed;
        }

        // The camera the viewport of this window shows: the same one the render system takes (primary, bound to the window)
        ecs::EntityId FindViewportCamera(ecs::World& world, const core::WindowId windowId)
        {
            ecs::EntityId found = ecs::kInvalidEntity;
            world.ForEach<ecs::components::CameraComponent, ecs::components::WindowBindingComponent>(
                [&](const ecs::EntityId entity, ecs::components::CameraComponent& camera, const ecs::components::WindowBindingComponent& binding)
                {
                    if (found == ecs::kInvalidEntity && binding.windowId == windowId && camera.isPrimary)
                    {
                        found = entity;
                    }
                });
            return found;
        }
    }

    void SceneEditor::BuildViewportPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showViewport)
        {
            return;
        }

        auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        windowState.viewportToolbarRight = {};
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
            // The toolbar is two floating groups; the strip between them belongs to the scene (picking, navigation)
            const auto layout = ComputeLayout(windowState.viewport, editorState.viewportWireframe);
            windowState.viewportToolbar = layout.left;
            windowState.viewportToolbarRight = layout.right;
            const ImVec2 mousePosition = ImGui::GetIO().MousePos;
            const bool mouseInToolbarRegion = Contains(layout.left, mousePosition) || Contains(layout.right, mousePosition);

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

            {
                auto* drawList = ImGui::GetWindowDrawList();
                const auto& viewport = windowState.viewport;
                drawList->PushClipRect(ImVec2(viewport.x, viewport.y), ImVec2(viewport.x + viewport.width, viewport.y + viewport.height), true);

                // FPS / ms in the top right corner, under the toolbar (viewport toolbar > Show > Stats)
                if (editorState.showStatsOverlay)
                {
                    char fps[32];
                    char ms[32];
                    std::snprintf(fps, sizeof(fps), "%.0f FPS", windowState.timings.averageFps);
                    std::snprintf(ms, sizeof(ms), "%.2f ms", windowState.timings.frameMs);
                    PushFontRole(FontRole::Mono);
                    const float lineHeight = ImGui::GetFontSize() + 2.0f;
                    float y = viewport.y + kStatsTop;
                    for (const char* line : {fps, ms})
                    {
                        const ImVec2 size = ImGui::CalcTextSize(line);
                        const float x = viewport.x + viewport.width - style::kViewportInset - size.x;
                        drawList->AddText(ImVec2(x + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, 255), line);
                        drawList->AddText(ImVec2(x, y), style::kStatsText, line);
                        y += lineHeight;
                    }
                    PopFontRole();
                }

                // Script HUD (Play), below the toolbar
                if (editorState.mode == editor::RuntimeMode::Play && services_.scriptHudLines)
                {
                    PushFontRole(FontRole::Secondary);
                    ImVec2 position(viewport.x + style::kViewportInset + 6.0f, viewport.y + kHudTop + 4.0f);
                    for (const auto& text : services_.scriptHudLines())
                    {
                        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
                        drawList->AddRectFilled(
                            ImVec2(position.x - 6.0f, position.y - 4.0f),
                            ImVec2(position.x + size.x + 6.0f, position.y + size.y + 4.0f),
                            IM_COL32(0, 0, 0, 153),
                            style::kRounding);
                        drawList->AddText(position, style::kTextStrong, text.c_str());
                        position.y += size.y + 6.0f + 8.0f;
                    }
                    PopFontRole();
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
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        auto* drawList = ImGui::GetWindowDrawList();
        const bool isPlay = editorState.mode == editor::RuntimeMode::Play;

        // Play frame (same colour as the mode indicators)
        if (isPlay)
        {
            const float half = kPlayFrame * 0.5f;
            drawList->AddRect(
                ImVec2(viewportRect.x + half, viewportRect.y + half),
                ImVec2(viewportRect.x + viewportRect.width - half, viewportRect.y + viewportRect.height - half),
                style::kPlay,
                0.0f,
                0,
                kPlayFrame);
        }

        const auto layout = ComputeLayout(viewportRect, editorState.viewportWireframe);
        const ecs::EntityId cameraEntity = FindViewportCamera(*services_.world, windowContext.windowId);
        auto* camera = cameraEntity != ecs::kInvalidEntity ? services_.world->TryGet<ecs::components::CameraComponent>(cameraEntity) : nullptr;
        auto* controller = cameraEntity != ecs::kInvalidEntity ? services_.world->TryGet<ecs::components::CameraControllerComponent>(cameraEntity) : nullptr;

        for (const auto& group : {std::make_pair(layout.menuMin, layout.menuWidth), std::make_pair(layout.perspectiveMin, layout.perspectiveWidth),
                 std::make_pair(layout.litMin, layout.litWidth), std::make_pair(layout.showMin, layout.showWidth),
                 std::make_pair(layout.opsMin, layout.opsWidth), std::make_pair(layout.cameraMin, layout.cameraWidth),
                 std::make_pair(layout.graphicsMin, layout.graphicsWidth)})
        {
            DrawGroupBackground(drawList, group.first, group.second);
        }

        // ---- left ----
        // Viewport options: FOV of the camera that renders this viewport
        {
            const ImVec2 min(layout.menuMin.x + kGroupPadding, layout.menuMin.y + kGroupPadding);
            const bool open = ImGui::IsPopupOpen("##viewport_options");
            if (ToolbarIconButton(drawList, "##viewport_options_button", ICON_MENU, min, open, true, "Viewport Options"))
            {
                ImGui::OpenPopup("##viewport_options");
            }
            if (BeginToolbarPopup("##viewport_options", layout.menuMin, ImVec2(layout.menuMin.x, layout.menuMin.y + kGroupHeight), 260.0f))
            {
                PopupSection("VIEWPORT OPTIONS");
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
                ImGui::BeginDisabled(camera == nullptr);
                ImGui::TextUnformatted("Field of View");
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
                float fov = camera != nullptr ? camera->fovYDeg : 60.0f;
                if (ImGui::SliderFloat("##fov", &fov, kMinFov, kMaxFov, "%.0f\xC2\xB0") && camera != nullptr)
                {
                    camera->fovYDeg = std::clamp(fov, kMinFov, kMaxFov);
                    editorState.sceneDirty = true;
                }
                if (camera != nullptr && !isPlay)
                {
                    RecordSceneMutationFromItem("Edit Camera Field of View");
                }
                ImGui::EndDisabled();
                if (camera == nullptr)
                {
                    PushFontRole(FontRole::Tiny);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
                    ImGui::TextUnformatted("No camera in this viewport");
                    ImGui::PopStyleColor();
                    PopFontRole();
                }
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                EndToolbarPopup();
            }
        }

        // Perspective: a caption, there is no orthographic camera
        ToolbarTextButton(drawList, "##perspective", ICON_CUBOID, "Perspective", layout.perspectiveMin, layout.perspectiveWidth, false, false, false);

        // View mode: Lit / Wireframe
        {
            const bool open = ImGui::IsPopupOpen("##viewport_view_mode");
            const auto state = ToolbarTextButton(
                drawList, "##view_mode", editorState.viewportWireframe ? ICON_BOX : ICON_SUN, editorState.viewportWireframe ? "Wireframe" : "Lit",
                layout.litMin, layout.litWidth, true, true, open, "View mode");
            if (state.pressed)
            {
                ImGui::OpenPopup("##viewport_view_mode");
            }
            if (BeginToolbarPopup("##viewport_view_mode", state.min, state.max, 180.0f))
            {
                PopupSection("VIEW MODE");
                if (PopupRow("##mode_lit", "Lit", nullptr, RowKind::Radio, !editorState.viewportWireframe) && editorState.viewportWireframe)
                {
                    editorState.viewportWireframe = false;
                    if (services_.setWireframe)
                    {
                        services_.setWireframe(false);
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (PopupRow("##mode_wireframe", "Wireframe", nullptr, RowKind::Radio, editorState.viewportWireframe) && !editorState.viewportWireframe)
                {
                    editorState.viewportWireframe = true;
                    if (services_.setWireframe)
                    {
                        services_.setWireframe(true);
                    }
                    ImGui::CloseCurrentPopup();
                }
                EndToolbarPopup();
            }
        }

        // Show: overlays
        {
            const bool open = ImGui::IsPopupOpen("##viewport_show");
            const auto state = ToolbarTextButton(drawList, "##show", ICON_EYE, "Show", layout.showMin, layout.showWidth, true, true, open, "Show");
            if (state.pressed)
            {
                ImGui::OpenPopup("##viewport_show");
            }
            if (BeginToolbarPopup("##viewport_show", state.min, state.max, 210.0f))
            {
                PopupSection("OVERLAYS");
                if (PopupRow("##show_stats", "Stats (FPS, ms)", nullptr, RowKind::Checkbox, editorState.showStatsOverlay))
                {
                    editorState.showStatsOverlay = !editorState.showStatsOverlay;
                }
                if (PopupRow("##show_colliders", "Colliders", "F3", RowKind::Checkbox, physicsState.debugDrawEnabled))
                {
                    physicsState.debugDrawEnabled = !physicsState.debugDrawEnabled; // the same flag as the F3 key
                }
                EndToolbarPopup();
            }
        }

        // ---- right ----
        // Transform tools and the space; the whole group is dimmed in Play
        {
            const bool enabled = !isPlay;
            ImVec2 position(layout.opsMin.x + kGroupPadding, layout.opsMin.y + kGroupPadding);
            if (ToolbarIconButton(drawList, "##tool_translate", ICON_MOVE, position, editorState.gizmoOperation == editor::GizmoOperation::Translate, enabled, "Translate"))
            {
                editorState.gizmoOperation = editor::GizmoOperation::Translate;
            }
            position.x += style::kViewportButton;
            if (ToolbarIconButton(drawList, "##tool_rotate", ICON_ROTATE_CW, position, editorState.gizmoOperation == editor::GizmoOperation::Rotate, enabled, "Rotate"))
            {
                editorState.gizmoOperation = editor::GizmoOperation::Rotate;
            }
            position.x += style::kViewportButton;
            if (ToolbarIconButton(drawList, "##tool_scale", ICON_SCALING, position, editorState.gizmoOperation == editor::GizmoOperation::Scale, enabled, "Scale"))
            {
                editorState.gizmoOperation = editor::GizmoOperation::Scale;
            }
            position.x += style::kViewportButton;
            const float separatorX = position.x + kSeparatorWidth * 0.5f;
            drawList->AddLine(ImVec2(separatorX, position.y + 5.0f), ImVec2(separatorX, position.y + style::kViewportButton - 5.0f), style::kControl);
            position.x += kSeparatorWidth;
            const bool local = editorState.gizmoSpace == editor::GizmoSpace::Local;
            if (ToolbarIconButton(
                    drawList, "##tool_space", local ? ICON_BOX : ICON_GLOBE, position, false, enabled,
                    local ? "Local space - click for World" : "World space - click for Local"))
            {
                editorState.gizmoSpace = local ? editor::GizmoSpace::World : editor::GizmoSpace::Local;
            }
        }

        // Camera speed: 1..8, the number is the level nearest to the current speed
        {
            const bool open = ImGui::IsPopupOpen("##viewport_camera_speed");
            const int level = controller != nullptr ? NearestSpeedLevel(controller->moveSpeed) + 1 : 0;
            char number[8];
            std::snprintf(number, sizeof(number), "%d", level);
            const ImVec2 min(layout.cameraMin.x + kGroupPadding, layout.cameraMin.y + kGroupPadding);
            const ImVec2 size(layout.cameraWidth - kGroupPadding * 2.0f, style::kViewportButton);
            const auto state = SubmitButton("##camera_speed", min, size, controller != nullptr);
            DrawButtonBackground(drawList, state, false, open, controller != nullptr);
            const ImU32 color = controller == nullptr ? style::kTextDisabled : ((state.hovered || open) ? style::kTextStrong : style::kText);
            const float centerY = (min.y + state.max.y) * 0.5f;
            DrawIcon(drawList, IconSize::Button16, ICON_CAMERA, ImVec2(min.x + kTextPaddingX + 8.0f, centerY), color);
            PushFontRole(FontRole::Secondary);
            const ImVec2 numberSize = ImGui::CalcTextSize(controller != nullptr ? number : "-");
            drawList->AddText(ImVec2(min.x + kTextPaddingX + 16.0f + 5.0f, centerY - numberSize.y * 0.5f), color, controller != nullptr ? number : "-");
            PopFontRole();
            Tooltip("Camera speed", nullptr, "Mouse wheel while the right button is held changes it too");
            if (state.pressed)
            {
                ImGui::OpenPopup("##viewport_camera_speed");
            }
            if (BeginToolbarPopup("##viewport_camera_speed", ImVec2(state.max.x - 240.0f, state.min.y), state.max, 240.0f))
            {
                PopupSection("CAMERA SPEED");
                if (controller != nullptr)
                {
                    int sliderLevel = level;
                    char value[16];
                    std::snprintf(value, sizeof(value), "%.2f", controller->moveSpeed);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
                    ImGui::TextUnformatted("Camera Speed");
                    PushFontRole(FontRole::Secondary);
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(value).x - 8.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
                    ImGui::TextUnformatted(value);
                    ImGui::PopStyleColor();
                    PopFontRole();
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
                    if (ImGui::SliderInt("##speed_level", &sliderLevel, 1, 8, "%d"))
                    {
                        controller->moveSpeed = kSpeedLevels[std::clamp(sliderLevel, 1, 8) - 1];
                    }
                    ImGui::Dummy(ImVec2(0.0f, 4.0f));
                }
                EndToolbarPopup();
            }
        }

        // Graphics: VSync
        {
            const bool open = ImGui::IsPopupOpen("##viewport_graphics");
            const ImVec2 min(layout.graphicsMin.x + kGroupPadding, layout.graphicsMin.y + kGroupPadding);
            const ImVec2 size(layout.graphicsWidth - kGroupPadding * 2.0f, style::kViewportButton);
            const auto state = SubmitButton("##graphics", min, size, true);
            DrawButtonBackground(drawList, state, false, open, true);
            const ImU32 color = (state.hovered || open) ? style::kTextStrong : style::kText;
            const float centerY = (min.y + state.max.y) * 0.5f;
            DrawIcon(drawList, IconSize::Button16, ICON_MONITOR_COG, ImVec2(min.x + kTextPaddingX + 8.0f, centerY), color);
            DrawIcon(drawList, IconSize::Chevron12, ICON_CHEVRON_DOWN, ImVec2(state.max.x - kTextPaddingX - 6.0f, centerY), style::kTextDim);
            Tooltip("Graphics");
            if (state.pressed)
            {
                ImGui::OpenPopup("##viewport_graphics");
            }
            if (BeginToolbarPopup("##viewport_graphics", ImVec2(state.max.x - 200.0f, state.min.y), state.max, 200.0f))
            {
                PopupSection("GRAPHICS");
                const bool available = services_.isVSyncEnabled && services_.setVSync;
                const bool vsync = available && services_.isVSyncEnabled();
                if (PopupRow("##vsync", "VSync", nullptr, RowKind::Checkbox, vsync, available) && available)
                {
                    services_.setVSync(!vsync);
                }
                EndToolbarPopup();
            }
        }

        // Anything open or under the mouse belongs to the toolbar, not to the scene
        const auto mouse = ImGui::GetIO().MousePos;
        return Contains(layout.left, mouse) || Contains(layout.right, mouse) || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
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

        const ImVec2 mouse(mouseX, mouseY);
        const bool insideToolbar = Contains(windowState->viewportToolbar, mouse) || Contains(windowState->viewportToolbarRight, mouse);

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
