#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        constexpr char kAddPopup[] = "##AddPopup";
        constexpr char kReloadPopup[] = "##ReloadPopup";

        // A status bar text segment: icon (optional), dim caption, value; measured and drawn right to left
        struct StatusSegment
        {
            const char* icon = nullptr;
            ImU32 iconColor = style::kTextDim;
            std::string caption; // dim
            std::string value;   // normal text
            ImU32 valueColor = style::kText;
            bool dot = false;
            ImU32 dotColor = 0;
        };

        float SegmentWidth(const StatusSegment& segment)
        {
            float width = 0.0f;
            if (segment.dot)
            {
                width += 14.0f;
            }
            if (segment.icon != nullptr)
            {
                width += 20.0f;
            }
            if (!segment.caption.empty())
            {
                width += ImGui::CalcTextSize(segment.caption.c_str()).x + (segment.value.empty() ? 0.0f : 6.0f);
            }
            if (!segment.value.empty())
            {
                width += ImGui::CalcTextSize(segment.value.c_str()).x;
            }
            return width;
        }

        void DrawSegment(ImDrawList* drawList, const StatusSegment& segment, float x, const float centerY)
        {
            const float textY = std::floor(centerY - FontRoleSize(FontRole::Secondary) * 0.5f - 0.5f);
            if (segment.dot)
            {
                drawList->AddCircleFilled(ImVec2(x + 4.0f, centerY), 4.0f, segment.dotColor);
                x += 14.0f;
            }
            if (segment.icon != nullptr)
            {
                DrawIcon(drawList, IconSize::Row14, segment.icon, ImVec2(x + 7.0f, centerY), segment.iconColor);
                x += 20.0f;
            }
            if (!segment.caption.empty())
            {
                drawList->AddText(ImVec2(x, textY), style::kTextDim, segment.caption.c_str());
                x += ImGui::CalcTextSize(segment.caption.c_str()).x + (segment.value.empty() ? 0.0f : 6.0f);
            }
            if (!segment.value.empty())
            {
                drawList->AddText(ImVec2(x, textY), segment.valueColor, segment.value.c_str());
            }
        }

        // Status bar shortcut: icon + label, the whole thing is the button
        bool ShortcutButton(const char* id, const char* icon, const char* label)
        {
            const float textWidth = ImGui::CalcTextSize(label).x;
            const ImVec2 size(10.0f + 14.0f + 6.0f + textWidth + 10.0f, style::kStatusBarHeight - 6.0f);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const bool pressed = ImGui::InvisibleButton(id, size);
            const bool hovered = ImGui::IsItemHovered();
            auto* drawList = ImGui::GetWindowDrawList();
            if (hovered)
            {
                drawList->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), style::kControl, 3.0f);
            }
            const float centerY = min.y + size.y * 0.5f;
            DrawIcon(drawList, IconSize::Row14, icon, ImVec2(min.x + 10.0f + 7.0f, centerY), hovered ? style::kText : style::kTextDim);
            drawList->AddText(ImVec2(min.x + 10.0f + 14.0f + 6.0f, std::floor(centerY - FontRoleSize(FontRole::Secondary) * 0.5f - 0.5f)),
                              hovered ? style::kTextStrong : style::kText, label);
            return pressed;
        }
    }

    void SceneEditor::BuildMainToolbar(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        const bool isEditMode = editorState.mode == editor::RuntimeMode::Edit;

        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(style::kTitle));
        if (ImGui::BeginViewportSideBar("##MainToolbar", ImGui::GetMainViewport(), ImGuiDir_Up, style::kToolbarHeight, flags))
        {
            auto* drawList = ImGui::GetWindowDrawList();

            // Save: a warning dot in the corner while the map has unsaved changes
            const ImVec2 savePosition = ImGui::GetCursorScreenPos();
            if (IconButton("##save", ICON_SAVE, "Save Map", false, isEditMode && services_.saveScene != nullptr, 0,
                           style::kToolButton, "Ctrl+S", IconSize::Toolbar18))
            {
                if (services_.saveScene())
                {
                    editorState.sceneDirty = false;
                }
            }
            if (editorState.sceneDirty)
            {
                drawList->AddCircleFilled(ImVec2(savePosition.x + 25.0f, savePosition.y + 8.0f), 3.0f, style::kWarning);
            }
            ImGui::SameLine();
            ToolbarSeparator();
            ImGui::SameLine();

            // + Add
            const ImVec2 addPosition = ImGui::GetCursorScreenPos();
            if (SplitButton("##add", ICON_SQUARE_PLUS, "Add", isEditMode, 0, true, "Add") != SplitButtonPart::None)
            {
                ImGui::OpenPopup(kAddPopup);
            }
            ImGui::SetNextWindowPos(ImVec2(addPosition.x, addPosition.y + style::kToolButton + 4.0f));
            if (BeginMenuPopup(kAddPopup))
            {
                if (MenuItemIcon(ICON_CIRCLE_DASHED, "Empty Entity", nullptr, false, isEditMode, false, 300.0f))
                {
                    CreateEmptyEntity(windowContext.windowId);
                }
                ImGui::Separator();
                PushFontRole(FontRole::Tiny);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
                ImGui::TextUnformatted("Meshes and prefabs: drag from the Content Browser");
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                ImGui::PopStyleColor();
                PopFontRole();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ToolbarSeparator();
            ImGui::SameLine();

            // Play / Stop
            if (isEditMode)
            {
                if (IconButton("##play", ICON_PLAY, "Play", false, true, style::kPlay, style::kToolButton, nullptr, IconSize::Toolbar18))
                {
                    if (services_.captureSceneSnapshot)
                    {
                        editorState.playModeSnapshot = services_.captureSceneSnapshot();
                        editorState.mode = editor::RuntimeMode::Play;
                        physicsState.physicsPaused = false;
                    }
                }
            }
            else
            {
                // Playing: the button stays visible as a dimmed green
                const ImVec2 position = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(style::kToolButton, style::kToolButton));
                DrawIcon(drawList, IconSize::Toolbar18, ICON_PLAY,
                         ImVec2(position.x + style::kToolButton * 0.5f, position.y + style::kToolButton * 0.5f), style::kPlayDisabled);
                Tooltip("Play");
            }
            ImGui::SameLine();
            const bool stopPressed = IconButton("##stop", ICON_SQUARE, "Stop", false, !isEditMode, style::kStop, style::kToolButton,
                                                nullptr, IconSize::Toolbar18);
            if (stopPressed)
            {
                StopPlayMode();
            }
            ImGui::SameLine();
            ToolbarSeparator();
            ImGui::SameLine();

            // Reload Scripts: the main part reloads, the chevron opens the options
            const ImVec2 reloadPosition = ImGui::GetCursorScreenPos();
            const SplitButtonPart reload = SplitButton("##reload", ICON_REFRESH_CW, "Reload Scripts", true, 0, false,
                                                       "Reload Scripts", "F5");
            if (reload == SplitButtonPart::Main && services_.reloadScripts)
            {
                services_.reloadScripts();
            }
            else if (reload == SplitButtonPart::Chevron)
            {
                ImGui::OpenPopup(kReloadPopup);
            }
            ImGui::SetNextWindowPos(ImVec2(reloadPosition.x, reloadPosition.y + style::kToolButton + 4.0f));
            if (BeginMenuPopup(kReloadPopup))
            {
                if (MenuItemIcon(ICON_REFRESH_CW, "Reload All Scripts", "F5", false, true, false, 330.0f) && services_.reloadScripts)
                {
                    services_.reloadScripts();
                }
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
                Checkbox("Keep script state on reload", &editorState.scriptReloadKeepsState);
                PushFontRole(FontRole::Tiny);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 36.0f);
                ImGui::TextUnformatted("On: running objects keep their state.");
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 36.0f);
                ImGui::TextUnformatted("Off: they start over with OnStart.");
                ImGui::PopStyleColor();
                PopFontRole();
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                ImGui::Separator();
                PushFontRole(FontRole::Tiny);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
                ImGui::TextUnformatted("Saved files reload automatically.");
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                ImGui::PopStyleColor();
                PopFontRole();
                ImGui::EndPopup();
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

        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 3.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(style::kTitle));
        if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, style::kStatusBarHeight, flags))
        {
            PushFontRole(FontRole::Secondary);
            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 windowPosition = ImGui::GetWindowPos();
            const float windowWidth = ImGui::GetWindowWidth();
            drawList->AddLine(windowPosition, ImVec2(windowPosition.x + windowWidth, windowPosition.y), style::kInput, 1.0f);

            // Left: shortcuts that bring a panel to the front
            if (ShortcutButton("##sbContent", ICON_FOLDER, "Content Browser"))
            {
                editorState.showAssetBrowser = true;
                ImGui::SetWindowFocus(kAssetBrowserWindowName);
            }
            ImGui::SameLine(0.0f, 4.0f);
            if (ShortcutButton("##sbLog", ICON_SCROLL_TEXT, "Output Log"))
            {
                editorState.showScriptConsole = true;
                ImGui::SetWindowFocus(kScriptConsoleWindowName);
            }

            // Right: mode, FPS, entities, project / map, save state (drawn right to left)
            const auto& project = core::ServiceLocator::GetProjectContext();
            const std::string mapName = std::filesystem::u8path(editorState.mapPath).stem().u8string();
            char fps[32];
            std::snprintf(fps, sizeof(fps), "%.0f", windowState.timings.averageFps);

            std::vector<StatusSegment> segments;
            {
                StatusSegment mode;
                mode.dot = true;
                mode.dotColor = isEditMode ? style::kEditIndicator : style::kPlay;
                mode.value = isEditMode ? "Edit" : "Playing";
                mode.valueColor = style::kText;
                segments.push_back(mode);
            }
            {
                StatusSegment item;
                item.caption = "FPS";
                item.value = fps;
                segments.push_back(item);
            }
            {
                StatusSegment item;
                item.caption = "Entities";
                item.value = std::to_string(windowState.renderStats.totalEntities);
                segments.push_back(item);
            }
            if (project.IsInitialized() || !mapName.empty())
            {
                StatusSegment item;
                item.icon = ICON_MAP;
                item.value = (project.Name().empty() ? std::string("-") : project.Name()) + " / " + (mapName.empty() ? std::string("(no map)") : mapName);
                segments.push_back(item);
            }
            {
                StatusSegment item;
                item.icon = ICON_SAVE;
                item.iconColor = editorState.sceneDirty ? style::kWarning : style::kTextDim;
                item.value = editorState.sceneDirty ? "Unsaved changes" : "All saved";
                item.valueColor = editorState.sceneDirty ? style::kWarning : style::kTextDim;
                segments.push_back(item);
            }

            const float gap = 16.0f;
            float total = 0.0f;
            for (const StatusSegment& segment : segments)
            {
                total += SegmentWidth(segment) + gap;
            }
            total -= gap;
            float x = windowPosition.x + windowWidth - 12.0f - total;
            const float centerY = windowPosition.y + style::kStatusBarHeight * 0.5f;
            for (const StatusSegment& segment : segments)
            {
                DrawSegment(drawList, segment, x, centerY);
                x += SegmentWidth(segment) + gap;
            }
            PopFontRole();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
}
