#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        enum class ToolIcon
        {
            Save,
            Undo,
            Redo,
            Translate,
            Rotate,
            Scale,
            Play,
            Stop,
        };

        constexpr float kToolButtonSize = 28.0f;
        constexpr float kToolbarVerticalPadding = 5.0f;
        constexpr float kToolbarGroupGap = 10.0f;
        constexpr ImU32 kStopIconColor = IM_COL32(224, 82, 74, 255);

        void FillArrowHead(ImDrawList* drawList, const ImVec2 tip, const ImVec2 direction, const float length, const float halfWidth, const ImU32 color)
        {
            const ImVec2 side{-direction.y, direction.x};
            const ImVec2 baseCenter{tip.x - direction.x * length, tip.y - direction.y * length};
            drawList->AddTriangleFilled(
                tip,
                ImVec2(baseCenter.x + side.x * halfWidth, baseCenter.y + side.y * halfWidth),
                ImVec2(baseCenter.x - side.x * halfWidth, baseCenter.y - side.y * halfWidth),
                color);
        }

        // Arc with an arrow head at its end; angles in radians, clockwise on screen
        void DrawArcArrow(ImDrawList* drawList, const ImVec2 center, const float radius, const float from, const float to, const float thickness, const ImU32 color)
        {
            drawList->PathClear();
            const int steps = 18;
            for (int step = 0; step <= steps; ++step)
            {
                const float angle = from + (to - from) * static_cast<float>(step) / static_cast<float>(steps);
                drawList->PathLineTo(ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius));
            }
            drawList->PathStroke(color, ImDrawFlags_None, thickness);

            const float sign = to >= from ? 1.0f : -1.0f;
            const ImVec2 tipPoint{center.x + std::cos(to) * radius, center.y + std::sin(to) * radius};
            const ImVec2 tangent{-std::sin(to) * sign, std::cos(to) * sign};
            FillArrowHead(drawList, ImVec2(tipPoint.x + tangent.x * radius * 0.45f, tipPoint.y + tangent.y * radius * 0.45f), tangent, radius * 0.75f, radius * 0.42f, color);
        }

        void DrawToolIcon(ImDrawList* drawList, const ToolIcon icon, const ImVec2 center, const float r, const ImU32 color)
        {
            constexpr float kPi = 3.14159265f;
            const float thickness = 1.7f;
            switch (icon)
            {
                case ToolIcon::Play:
                    drawList->AddTriangleFilled(
                        ImVec2(center.x - r * 0.55f, center.y - r * 0.95f),
                        ImVec2(center.x - r * 0.55f, center.y + r * 0.95f),
                        ImVec2(center.x + r * 0.95f, center.y),
                        color);
                    break;

                case ToolIcon::Stop:
                    drawList->AddRectFilled(
                        ImVec2(center.x - r * 0.75f, center.y - r * 0.75f),
                        ImVec2(center.x + r * 0.75f, center.y + r * 0.75f),
                        color,
                        1.5f);
                    break;

                case ToolIcon::Save:
                {
                    const ImVec2 min{center.x - r * 0.9f, center.y - r * 0.9f};
                    const ImVec2 max{center.x + r * 0.9f, center.y + r * 0.9f};
                    drawList->AddRect(min, max, color, 2.0f, 0, thickness);
                    drawList->AddRectFilled(
                        ImVec2(center.x - r * 0.45f, min.y),
                        ImVec2(center.x + r * 0.45f, center.y - r * 0.15f),
                        color);
                    drawList->AddRect(
                        ImVec2(center.x - r * 0.5f, center.y + r * 0.2f),
                        ImVec2(center.x + r * 0.5f, max.y),
                        color,
                        0.0f,
                        0,
                        thickness);
                    break;
                }

                case ToolIcon::Undo:
                case ToolIcon::Redo:
                {
                    // Open arc over the top that ends in an arrow head pointing down
                    const float side = icon == ToolIcon::Undo ? 1.0f : -1.0f;
                    const float radius = r * 0.72f;
                    drawList->PathClear();
                    const int steps = 14;
                    for (int step = 0; step <= steps; ++step)
                    {
                        const float angle = kPi * 0.3f - (kPi * 1.3f) * static_cast<float>(step) / static_cast<float>(steps);
                        drawList->PathLineTo(ImVec2(center.x + side * std::cos(angle) * radius, center.y + r * 0.1f + std::sin(angle) * radius));
                    }
                    drawList->PathStroke(color, ImDrawFlags_None, thickness);
                    const ImVec2 tip{center.x - side * radius, center.y + r * 0.1f + r * 0.62f};
                    FillArrowHead(drawList, tip, ImVec2(0.0f, 1.0f), r * 0.75f, r * 0.45f, color);
                    break;
                }

                case ToolIcon::Translate:
                {
                    const float length = r * 0.95f;
                    const float head = r * 0.5f;
                    drawList->AddLine(ImVec2(center.x - length, center.y), ImVec2(center.x + length, center.y), color, thickness);
                    drawList->AddLine(ImVec2(center.x, center.y - length), ImVec2(center.x, center.y + length), color, thickness);
                    FillArrowHead(drawList, ImVec2(center.x + length + 1.0f, center.y), ImVec2(1.0f, 0.0f), head, head * 0.7f, color);
                    FillArrowHead(drawList, ImVec2(center.x - length - 1.0f, center.y), ImVec2(-1.0f, 0.0f), head, head * 0.7f, color);
                    FillArrowHead(drawList, ImVec2(center.x, center.y - length - 1.0f), ImVec2(0.0f, -1.0f), head, head * 0.7f, color);
                    FillArrowHead(drawList, ImVec2(center.x, center.y + length + 1.0f), ImVec2(0.0f, 1.0f), head, head * 0.7f, color);
                    break;
                }

                case ToolIcon::Rotate:
                    DrawArcArrow(drawList, center, r * 0.78f, -kPi * 0.75f, kPi * 0.78f, thickness, color);
                    break;

                case ToolIcon::Scale:
                {
                    drawList->AddRect(
                        ImVec2(center.x - r * 0.9f, center.y - r * 0.1f),
                        ImVec2(center.x + r * 0.1f, center.y + r * 0.9f),
                        color,
                        1.0f,
                        0,
                        thickness);
                    drawList->AddLine(ImVec2(center.x - r * 0.15f, center.y + r * 0.15f), ImVec2(center.x + r * 0.7f, center.y - r * 0.7f), color, thickness);
                    FillArrowHead(drawList, ImVec2(center.x + r * 0.95f, center.y - r * 0.95f), ImVec2(0.7071f, -0.7071f), r * 0.75f, r * 0.42f, color);
                    break;
                }
            }
        }

        ImU32 WithAlpha(const ImU32 color, const float alpha)
        {
            ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
            value.w *= alpha;
            return ImGui::ColorConvertFloat4ToU32(value);
        }

        // Square icon button. `selected` paints the accent background (active tool); returns true on click.
        bool ToolButton(const char* id, const ToolIcon icon, const bool selected, const bool enabled, const char* tooltip, const ImU32 enabledColor = 0)
        {
            const ImVec2 size{kToolButtonSize, kToolButtonSize};
            const ImVec2 min = ImGui::GetCursorScreenPos();

            ImGui::BeginDisabled(!enabled);
            const bool pressed = ImGui::InvisibleButton(id, size);
            const bool hovered = ImGui::IsItemHovered();
            const bool held = ImGui::IsItemActive();
            ImGui::EndDisabled();

            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 max{min.x + size.x, min.y + size.y};
            if (selected)
            {
                drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_Header), 4.0f);
            }
            else if (enabled && (held || hovered))
            {
                drawList->AddRectFilled(min, max, ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), 4.0f);
            }

            ImU32 iconColor = enabledColor != 0 ? enabledColor : ImGui::GetColorU32(ImGuiCol_Text);
            if (!enabled)
            {
                iconColor = WithAlpha(ImGui::GetColorU32(ImGuiCol_Text), 0.30f);
            }
            DrawToolIcon(drawList, icon, ImVec2(min.x + size.x * 0.5f, min.y + size.y * 0.5f), 7.5f, iconColor);

            if (hovered && tooltip != nullptr)
            {
                ImGui::SetTooltip("%s", tooltip);
            }
            return pressed && enabled;
        }

        // Text toggle with the same look as ToolButton
        bool ToolTextToggle(const char* label, const bool selected, const bool enabled, const char* tooltip)
        {
            const ImVec2 textSize = ImGui::CalcTextSize(label);
            const ImVec2 size{textSize.x + 16.0f, kToolButtonSize};
            const ImVec2 min = ImGui::GetCursorScreenPos();

            ImGui::BeginDisabled(!enabled);
            const bool pressed = ImGui::InvisibleButton(label, size);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::EndDisabled();

            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 max{min.x + size.x, min.y + size.y};
            if (selected)
            {
                drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_Header), 4.0f);
            }
            else if (enabled && hovered)
            {
                drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 4.0f);
            }
            const ImU32 textColor = enabled ? ImGui::GetColorU32(ImGuiCol_Text) : WithAlpha(ImGui::GetColorU32(ImGuiCol_Text), 0.30f);
            drawList->AddText(ImVec2(min.x + 8.0f, min.y + (size.y - textSize.y) * 0.5f), textColor, label);

            if (hovered && tooltip != nullptr)
            {
                ImGui::SetTooltip("%s", tooltip);
            }
            return pressed && enabled;
        }

        void ToolSeparator()
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(min.x + kToolbarGroupGap * 0.5f, min.y + 4.0f),
                ImVec2(min.x + kToolbarGroupGap * 0.5f, min.y + kToolButtonSize - 4.0f),
                ImGui::GetColorU32(ImGuiCol_Border),
                1.0f);
            ImGui::Dummy(ImVec2(kToolbarGroupGap, kToolButtonSize));
        }
    }

    void SceneEditor::BuildMainToolbar(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        const bool isEditMode = editorState.mode == editor::RuntimeMode::Edit;
        const ImGuiStyle& style = ImGui::GetStyle();

        const float toolbarHeight = kToolButtonSize + kToolbarVerticalPadding * 2.0f;
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, kToolbarVerticalPadding));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
        if (ImGui::BeginViewportSideBar("##MainToolbar", ImGui::GetMainViewport(), ImGuiDir_Up, toolbarHeight, flags))
        {
            // Left: scene and gizmo
            if (ToolButton("##save", ToolIcon::Save, false, isEditMode && services_.saveScene != nullptr, "Save scene (Ctrl+S)"))
            {
                if (services_.saveScene())
                {
                    editorState.sceneDirty = false;
                }
            }
            ImGui::SameLine();
            if (ToolButton("##undo", ToolIcon::Undo, false, isEditMode && history_ != nullptr && history_->CanUndo(), "Undo (Ctrl+Z)"))
            {
                history_->Undo();
            }
            ImGui::SameLine();
            if (ToolButton("##redo", ToolIcon::Redo, false, isEditMode && history_ != nullptr && history_->CanRedo(), "Redo (Ctrl+Y)"))
            {
                history_->Redo();
            }
            ImGui::SameLine();
            ToolSeparator();
            ImGui::SameLine();
            if (ToolButton("##translate", ToolIcon::Translate, editorState.gizmoOperation == editor::GizmoOperation::Translate, isEditMode, "Translate"))
            {
                editorState.gizmoOperation = editor::GizmoOperation::Translate;
            }
            ImGui::SameLine();
            if (ToolButton("##rotate", ToolIcon::Rotate, editorState.gizmoOperation == editor::GizmoOperation::Rotate, isEditMode, "Rotate"))
            {
                editorState.gizmoOperation = editor::GizmoOperation::Rotate;
            }
            ImGui::SameLine();
            if (ToolButton("##scale", ToolIcon::Scale, editorState.gizmoOperation == editor::GizmoOperation::Scale, isEditMode, "Scale"))
            {
                editorState.gizmoOperation = editor::GizmoOperation::Scale;
            }
            ImGui::SameLine();
            ToolSeparator();
            ImGui::SameLine();
            if (ToolTextToggle("Local", editorState.gizmoSpace == editor::GizmoSpace::Local, isEditMode, "Local space"))
            {
                editorState.gizmoSpace = editor::GizmoSpace::Local;
            }
            ImGui::SameLine();
            if (ToolTextToggle("World", editorState.gizmoSpace == editor::GizmoSpace::World, isEditMode, "World space"))
            {
                editorState.gizmoSpace = editor::GizmoSpace::World;
            }

            // Centre: mode badge, Play / Stop, script reload. Widths are measured so the group is centred
            // and does not jump when the badge text changes between EDIT and PLAYING.
            const char* reloadLabel = "Reload scripts";
            const char* keepLabel = "Keep script state";
            const ImVec2 badgeTextSize = ImGui::CalcTextSize("PLAYING");
            const float badgeWidth = badgeTextSize.x + 20.0f;
            const float reloadWidth = ImGui::CalcTextSize(reloadLabel).x + style.FramePadding.x * 2.0f;
            const float keepWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(keepLabel).x;
            const float gap = 8.0f;
            const float groupWidth = badgeWidth + gap + kToolButtonSize * 2.0f + gap + reloadWidth + gap + keepWidth;
            const ImVec2 windowSize = ImGui::GetWindowSize();
            float groupX = (windowSize.x - groupWidth) * 0.5f;
            groupX = std::max(groupX, ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f);

            ImGui::SameLine();
            ImGui::SetCursorPosX(groupX);

            // Mode badge: bright in Play, calm in Edit
            {
                const ImU32 badgeColor = isEditMode ? kEditBadgeColor : kPlayBadgeColor;
                const char* badgeText = isEditMode ? "EDIT" : "PLAYING";
                const ImVec2 textSize = ImGui::CalcTextSize(badgeText);
                const ImVec2 badgeMin = ImGui::GetCursorScreenPos();
                const ImVec2 badgeSize{badgeWidth, kToolButtonSize};
                auto* drawList = ImGui::GetWindowDrawList();
                drawList->AddRectFilled(badgeMin, ImVec2(badgeMin.x + badgeSize.x, badgeMin.y + badgeSize.y), badgeColor, 4.0f);
                drawList->AddText(
                    ImVec2(badgeMin.x + (badgeSize.x - textSize.x) * 0.5f, badgeMin.y + (badgeSize.y - textSize.y) * 0.5f),
                    IM_COL32(255, 255, 255, 255),
                    badgeText);
                ImGui::Dummy(badgeSize);
            }
            ImGui::SameLine(0.0f, gap);

            if (ToolButton("##play", ToolIcon::Play, !isEditMode, isEditMode, "Play", kPlayBadgeColor))
            {
                if (isEditMode && services_.captureSceneSnapshot)
                {
                    editorState.playModeSnapshot = services_.captureSceneSnapshot();
                    editorState.mode = editor::RuntimeMode::Play;
                    physicsState.physicsPaused = false;
                }
            }
            ImGui::SameLine();
            const bool stopPressed = ToolButton("##stop", ToolIcon::Stop, false, !isEditMode, "Stop", kStopIconColor);
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
            ImGui::SameLine(0.0f, gap);

            // Vertical centring of the stock widgets (they are shorter than the 28 px buttons)
            const float widgetOffsetY = (kToolButtonSize - ImGui::GetFrameHeight()) * 0.5f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + widgetOffsetY);
            if (ImGui::Button(reloadLabel) && services_.reloadScripts)
            {
                services_.reloadScripts();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Hot reload of every script (F5). Saved files are reloaded automatically");
            }
            ImGui::SameLine(0.0f, gap);
            ImGui::Checkbox(keepLabel, &editorState.scriptReloadKeepsState);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Hot reload in Play.\n"
                    "On: running objects keep their state, fields with a changed default take the new value (L2).\n"
                    "Off: running objects start over with OnStart (L1)");
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    void SceneEditor::BuildStatusBar(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        const auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        const bool isEditMode = editorState.mode == editor::RuntimeMode::Edit;

        const float height = ImGui::GetFrameHeight() + 4.0f;
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 2.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
        if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags))
        {
            ImGui::AlignTextToFramePadding();

            // Mode: coloured dot + text
            const ImVec2 dotCenter{ImGui::GetCursorScreenPos().x + 5.0f, ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight() * 0.5f};
            ImGui::GetWindowDrawList()->AddCircleFilled(dotCenter, 4.0f, isEditMode ? kEditBadgeColor : kPlayBadgeColor);
            ImGui::Dummy(ImVec2(10.0f, 0.0f));
            ImGui::SameLine();
            ImGui::TextUnformatted(isEditMode ? "Edit" : "Play");

            ImGui::SameLine(0.0f, 18.0f);
            ImGui::TextDisabled("FPS");
            ImGui::SameLine();
            ImGui::Text("%.0f", windowState.timings.averageFps);

            ImGui::SameLine(0.0f, 18.0f);
            ImGui::TextDisabled("Entities");
            ImGui::SameLine();
            ImGui::Text("%u", windowState.renderStats.totalEntities);

            // "Project - Map" instead of a full path
            const auto& project = core::ServiceLocator::GetProjectContext();
            const std::string mapName = std::filesystem::u8path(editorState.mapPath).stem().u8string();
            if (project.IsInitialized() || !mapName.empty())
            {
                ImGui::SameLine(0.0f, 18.0f);
                ImGui::TextDisabled("Project");
                ImGui::SameLine();
                ImGui::TextUnformatted(project.Name().empty() ? "-" : project.Name().c_str());
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::TextDisabled("-");
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::TextUnformatted(mapName.empty() ? "(no map)" : mapName.c_str());
                if (editorState.sceneDirty)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(modified)");
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
}
