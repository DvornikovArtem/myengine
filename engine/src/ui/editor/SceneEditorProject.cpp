#include <fstream>
#include <system_error>

#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        namespace fs = std::filesystem;

        constexpr char kProjectBrowserPopup[] = "Project Browser";
        constexpr char kNewMapPopup[] = "New Map";
        constexpr char kOpenMapPopup[] = "Open Map";
        constexpr char kSaveMapAsPopup[] = "Save Map As";

        bool IsValidMapName(const std::string& name)
        {
            return core::project::IsValidProjectName(name);
        }

        // Where a project is created when the user does not pick a folder
        std::string DefaultProjectsFolder()
        {
            wchar_t buffer[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer, static_cast<DWORD>(std::size(buffer)));
            const fs::path home = length > 0 && length < std::size(buffer) ? fs::path(buffer) : fs::current_path();
            return (home / "Documents" / "myengine projects").u8string();
        }

        std::string MapKeyFor(const core::ProjectContext& project, const std::string& name)
        {
            return project.ToProjectRelative(project.MapsDir() / fs::u8path(name + ".json"));
        }

        void CollectMaps(const core::ProjectContext& project, std::vector<std::string>& maps)
        {
            maps.clear();
            std::error_code error;
            const fs::path mapsDir = project.MapsDir();
            if (!fs::is_directory(mapsDir, error))
            {
                return;
            }

            for (fs::recursive_directory_iterator it(mapsDir, fs::directory_options::skip_permission_denied, error), end;
                 !error && it != end;
                 it.increment(error))
            {
                std::error_code statusError;
                if (!it->is_regular_file(statusError) || it->path().extension() != ".json")
                {
                    continue;
                }

                maps.push_back(project.ToProjectRelative(it->path()));
            }
            std::sort(maps.begin(), maps.end());
        }
    }

    void SceneEditor::OpenMapDialog()
    {
        if (projectUi_ == nullptr)
        {
            return;
        }

        projectUi_->mapDialog = ProjectUiState::MapDialog::OpenMap;
        projectUi_->openMapDialogRequested = true;
        projectUi_->message.clear();
        CollectMaps(core::ServiceLocator::GetProjectContext(), projectUi_->mapChoices);
    }

    void SceneEditor::DrawFileMenu(const bool editMode)
    {
        const float width = 290.0f;
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();

        if (MenuItemIcon(ICON_MAP, "New Map...", nullptr, false, editMode && projectUi_ != nullptr, false, width))
        {
            projectUi_->mapDialog = ProjectUiState::MapDialog::NewMap;
            projectUi_->openMapDialogRequested = true;
            projectUi_->mapName = "NewMap";
            projectUi_->message.clear();
        }

        if (MenuItemIcon(ICON_FOLDER_OPEN, "Open Map...", "Ctrl+O", false, editMode && projectUi_ != nullptr, false, width))
        {
            OpenMapDialog();
        }

        if (MenuItemIcon(ICON_SAVE, "Save Map", "Ctrl+S", false, editMode && services_.saveScene != nullptr, false, width))
        {
            if (services_.saveScene())
            {
                editorState.sceneDirty = false;
            }
        }

        if (MenuItemIcon(ICON_SAVE, "Save Map As...", nullptr, false, editMode && projectUi_ != nullptr, false, width))
        {
            projectUi_->mapDialog = ProjectUiState::MapDialog::SaveMapAs;
            projectUi_->openMapDialogRequested = true;
            projectUi_->mapName = fs::u8path(editorState.mapPath).stem().u8string();
            projectUi_->message.clear();
            projectUi_->confirmOverwrite = false;
        }

        // Re-reads the open map from disk (what "Load Scene" did before)
        if (MenuItemIcon(ICON_ROTATE_CCW, "Revert Map", nullptr, false, editMode && services_.loadScene != nullptr, false, width))
        {
            if (services_.loadScene())
            {
                editorState.selectedEntity = ecs::kInvalidEntity;
                editorState.sceneDirty = false;
                if (history_ != nullptr)
                {
                    history_->Clear();
                }
            }
        }

        ImGui::Separator();
        if (MenuItemIcon(ICON_FOLDER, "Open Project...", nullptr, false, true, false, width))
        {
            editorState.showProjectBrowser = true;
        }

        ImGui::Separator();
        if (MenuItemIcon(nullptr, "Quit", nullptr, false, services_.requestQuit != nullptr, false, width))
        {
            services_.requestQuit();
        }
    }

    void SceneEditor::RequestOpenProject(const std::string& projectFile)
    {
        if (projectUi_ == nullptr || !services_.restartWithProject || projectFile.empty())
        {
            return;
        }

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.sceneDirty && editorState.mode == editor::RuntimeMode::Edit)
        {
            projectUi_->pendingOpenProject = projectFile; // DrawOpenScenePrompt asks what to do with the changes
            return;
        }

        services_.restartWithProject(projectFile, true);
    }

    void SceneEditor::BuildProjectDialogs()
    {
        if (projectUi_ == nullptr)
        {
            return;
        }

        auto& state = *projectUi_;
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        const auto& project = core::ServiceLocator::GetProjectContext();

        // ---- Map dialogs
        const char* mapPopup = nullptr;
        const char* mapTitle = "";
        switch (state.mapDialog)
        {
            case ProjectUiState::MapDialog::NewMap: mapPopup = kNewMapPopup; mapTitle = "New Map"; break;
            case ProjectUiState::MapDialog::OpenMap: mapPopup = kOpenMapPopup; mapTitle = "Open Map"; break;
            case ProjectUiState::MapDialog::SaveMapAs: mapPopup = kSaveMapAsPopup; mapTitle = "Save Map As"; break;
            case ProjectUiState::MapDialog::None: break;
        }

        if (mapPopup != nullptr)
        {
            if (state.openMapDialogRequested)
            {
                ImGui::OpenPopup(mapPopup);
                state.openMapDialogRequested = false;
            }

            bool open = true;
            const float dialogWidth = state.mapDialog == ProjectUiState::MapDialog::OpenMap ? 460.0f : 480.0f;
            if (BeginDialog(mapPopup, mapTitle, ICON_MAP, dialogWidth, &open))
            {
                if (state.mapDialog == ProjectUiState::MapDialog::OpenMap)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                    PushFontRole(FontRole::Secondary);
                    ImGui::Text("Maps folder: %s", project.ToProjectRelative(project.MapsDir()).c_str());
                    PopFontRole();
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0.0f, 4.0f));

                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kRecessed));
                    ImGui::BeginChild("##map_list", ImVec2(0.0f, 220.0f), ImGuiChildFlags_None);
                    if (state.mapChoices.empty())
                    {
                        EmptyState(ICON_MAP, nullptr, "There are no maps in this project yet.");
                    }

                    PushSelectionColors(true);
                    for (const auto& map : state.mapChoices)
                    {
                        const bool current = map == editorState.mapPath;
                        ImGui::PushID(map.c_str());
                        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                        const bool clicked = ImGui::Selectable("##map", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, 26.0f));
                        const bool opened = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                        const ImVec2 rowMax = ImGui::GetItemRectMax();
                        auto* drawList = ImGui::GetWindowDrawList();
                        const float centerY = (rowMin.y + rowMax.y) * 0.5f;
                        DrawIcon(drawList, IconSize::Row14, ICON_MAP, ImVec2(rowMin.x + 18.0f, centerY), style::kTypeScene);
                        const std::string label = fs::u8path(map).stem().u8string();
                        drawList->AddText(ImVec2(rowMin.x + 34.0f, std::floor(centerY - style::kFontBody * 0.5f - 0.5f)), style::kTextStrong, label.c_str());
                        if (current)
                        {
                            const ImVec2 after = ImGui::GetCursorScreenPos();
                            PushFontRole(FontRole::Tiny);
                            const float chipWidth = ImGui::CalcTextSize("current").x + 16.0f;
                            PopFontRole();
                            ImGui::SetCursorScreenPos(ImVec2(rowMax.x - chipWidth - 8.0f, rowMin.y + 4.0f));
                            Chip("current", ChipKind::Blue);
                            ImGui::SetCursorScreenPos(after);
                        }
                        ImGui::PopID();

                        if (opened)
                        {
                            RequestOpenScene(map);
                            state.mapDialog = ProjectUiState::MapDialog::None;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    PopSelectionColors();
                    ImGui::EndChild();
                    ImGui::PopStyleColor();

                    ImGui::Dummy(ImVec2(0.0f, 2.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                    PushFontRole(FontRole::Tiny);
                    ImGui::TextUnformatted("Double-click a map to open it.");
                    PopFontRole();
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0.0f, 10.0f));

                    DialogFooter();
                    DialogAlignRight(80.0f);
                    if (Button("Cancel", nullptr, true, 80.0f))
                    {
                        state.mapDialog = ProjectUiState::MapDialog::None;
                        ImGui::CloseCurrentPopup();
                    }
                }
                else
                {
                    const bool isNew = state.mapDialog == ProjectUiState::MapDialog::NewMap;
                    const std::string key = IsValidMapName(state.mapName) ? MapKeyFor(project, state.mapName) : std::string();

                    // Form row: label | field (the error colours its outline)
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Name");
                    ImGui::SameLine(90.0f);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 16.0f);
                    if (ImGui::InputText("##map_name", &state.mapName))
                    {
                        state.message.clear();
                        state.confirmOverwrite = false;
                    }
                    if (!state.message.empty())
                    {
                        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), style::kError, style::kRounding, 0, 1.0f);
                    }
                    else
                    {
                        FocusOutline();
                    }

                    PushFontRole(FontRole::Tiny);
                    if (!key.empty())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 74.0f);
                        ImGui::TextUnformatted(key.c_str());
                        ImGui::PopStyleColor();
                    }
                    if (!state.message.empty())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kDestructiveText));
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 74.0f);
                        ImGui::TextUnformatted(state.message.c_str());
                        ImGui::PopStyleColor();
                    }
                    PopFontRole();
                    ImGui::Dummy(ImVec2(0.0f, 12.0f));

                    DialogFooter();
                    DialogAlignRight(110.0f + 8.0f + 80.0f);
                    const char* actionLabel = isNew ? "Create" : (state.confirmOverwrite ? "Overwrite" : "Save");
                    bool action = false;
                    if (!isNew && state.confirmOverwrite)
                    {
                        action = DestructiveButton(actionLabel, nullptr, !key.empty());
                    }
                    else
                    {
                        action = PrimaryButton(actionLabel, nullptr, !key.empty(), 110.0f);
                    }

                    if (action && !key.empty())
                    {
                        std::error_code error;
                        const fs::path target = project.ResolveContentPath(fs::u8path(key));
                        const bool exists = fs::exists(target, error);
                        if (isNew)
                        {
                            if (exists)
                            {
                                state.message = "A map with this name already exists.";
                            }
                            else
                            {
                                fs::create_directories(target.parent_path(), error);
                                std::ofstream file(target, std::ios::binary | std::ios::trunc);
                                file << core::project::EmptyMapText();
                                file.close();
                                if (!file)
                                {
                                    state.message = "The map could not be written.";
                                }
                                else
                                {
                                    RequestOpenScene(key);
                                    state.mapDialog = ProjectUiState::MapDialog::None;
                                    ImGui::CloseCurrentPopup();
                                }
                            }
                        }
                        else if (exists && !state.confirmOverwrite && key != editorState.mapPath)
                        {
                            state.message = "This map already exists. Press Overwrite to replace it.";
                            state.confirmOverwrite = true;
                        }
                        else if (!services_.saveSceneAs || !services_.saveSceneAs(key))
                        {
                            state.message = "The map could not be saved.";
                        }
                        else
                        {
                            editorState.sceneDirty = false;
                            state.mapDialog = ProjectUiState::MapDialog::None;
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    ImGui::SameLine();
                    if (Button("Cancel", nullptr, true, 80.0f))
                    {
                        state.mapDialog = ProjectUiState::MapDialog::None;
                        ImGui::CloseCurrentPopup();
                    }
                }
                EndDialog();
            }

            if (!open)
            {
                state.mapDialog = ProjectUiState::MapDialog::None;
            }
        }

        // ---- Project Browser
        if (editorState.showProjectBrowser)
        {
            if (!state.browserWasOpen)
            {
                state.browserWasOpen = true;
                state.browserMessage.clear();
                if (state.newProjectFolder.empty())
                {
                    state.newProjectFolder = DefaultProjectsFolder();
                }

                state.recentProjects.clear();
                std::error_code error;
                for (const auto& file : core::project::LoadRecentProjects(core::project::DefaultRecentProjectsFile()))
                {
                    if (fs::is_regular_file(file, error))
                    {
                        state.recentProjects.push_back(file.u8string());
                    }
                }
                ImGui::OpenPopup(kProjectBrowserPopup);
            }

            bool open = true;
            if (BeginDialog(kProjectBrowserPopup, "Project Browser", ICON_FOLDER_OPEN, 760.0f, &open))
            {
                const float bodyHeight = 250.0f;
                if (ImGui::BeginTable("##projectColumns", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings))
                {
                    ImGui::TableSetupColumn("recent", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn("new", ImGuiTableColumnFlags_WidthFixed, 316.0f);
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, bodyHeight);

                    // Left: recent projects, the current one first with a chip
                    ImGui::TableSetColumnIndex(0);
                    PushFontRole(FontRole::Tiny);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                    ImGui::TextUnformatted("RECENT PROJECTS");
                    ImGui::PopStyleColor();
                    PopFontRole();
                    ImGui::Dummy(ImVec2(0.0f, 2.0f));

                    struct ProjectRow
                    {
                        std::string name;
                        std::string folder;
                        std::string file; // empty: the current project, nothing to open
                        bool current = false;
                    };
                    std::vector<ProjectRow> rows;
                    std::string currentFile;
                    if (project.IsInitialized())
                    {
                        ProjectRow row;
                        row.name = project.Name();
                        row.folder = project.Root().u8string();
                        row.current = true;
                        currentFile = project.ProjectFile().u8string();
                        rows.push_back(row);
                    }
                    for (const std::string& recent : state.recentProjects)
                    {
                        if (!currentFile.empty() && recent == currentFile)
                        {
                            continue;
                        }
                        ProjectRow row;
                        const fs::path file = fs::u8path(recent);
                        row.name = file.stem().u8string();
                        row.folder = file.parent_path().u8string();
                        row.file = recent;
                        rows.push_back(row);
                    }

                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertU32ToFloat4(style::kHeader));
                    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(style::kHeader));
                    if (rows.empty())
                    {
                        PushFontRole(FontRole::Secondary);
                        ImGui::TextDisabled("Projects that you open appear here.");
                        PopFontRole();
                    }
                    for (std::size_t index = 0; index < rows.size(); ++index)
                    {
                        const ProjectRow& row = rows[index];
                        ImGui::PushID(static_cast<int>(index));
                        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                        const bool clicked = ImGui::Selectable("##recent", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, 44.0f));
                        const bool opened = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !row.file.empty();
                        const ImVec2 rowMax = ImGui::GetItemRectMax();
                        auto* drawList = ImGui::GetWindowDrawList();
                        DrawIcon(drawList, IconSize::Empty30, ICON_FOLDER, ImVec2(rowMin.x + 22.0f, rowMin.y + 22.0f), style::kTypeFolder);
                        PushFontRole(FontRole::Body);
                        drawList->AddText(ImVec2(rowMin.x + 46.0f, rowMin.y + 6.0f), style::kTextStrong, row.name.c_str());
                        const float nameWidth = ImGui::CalcTextSize(row.name.c_str()).x;
                        PopFontRole();
                        if (row.current)
                        {
                            const ImVec2 after = ImGui::GetCursorScreenPos();
                            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + 46.0f + nameWidth + 8.0f, rowMin.y + 6.0f));
                            Chip("current", ChipKind::Blue);
                            ImGui::SetCursorScreenPos(after);
                        }
                        PushFontRole(FontRole::Tiny);
                        const float pathLeft = rowMin.x + 46.0f;
                        const float pathRight = rowMax.x - 8.0f;
                        const ImVec2 pathSize = ImGui::CalcTextSize(row.folder.c_str());
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                        ImGui::RenderTextEllipsis(drawList, ImVec2(pathLeft, rowMin.y + 25.0f), ImVec2(pathRight, rowMin.y + 25.0f + pathSize.y),
                                                  pathRight, row.folder.c_str(), nullptr, &pathSize);
                        ImGui::PopStyleColor();
                        PopFontRole();
                        ImGui::PopID();

                        if (opened)
                        {
                            RequestOpenProject(row.file);
                            editorState.showProjectBrowser = false;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::PopStyleColor(2);

                    ImGui::Dummy(ImVec2(0.0f, 4.0f));
                    PushFontRole(FontRole::Tiny);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                    ImGui::TextUnformatted("Double-click a project to open it. The editor restarts.");
                    ImGui::PopStyleColor();
                    PopFontRole();

                    // Right: new project form
                    ImGui::TableSetColumnIndex(1);
                    PushFontRole(FontRole::Tiny);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                    ImGui::TextUnformatted("NEW PROJECT");
                    ImGui::PopStyleColor();
                    PopFontRole();
                    ImGui::Dummy(ImVec2(0.0f, 2.0f));

                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Name");
                    ImGui::SameLine(82.0f);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 16.0f);
                    ImGui::InputText("##new_project_name", &state.newProjectName);
                    FocusOutline();

                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Location");
                    ImGui::SameLine(82.0f);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 44.0f);
                    ImGui::InputText("##new_project_folder", &state.newProjectFolder);
                    FocusOutline();
                    ImGui::SameLine();
                    if (IconButton("##browse", ICON_FOLDER_OPEN, "Browse...", false, true, 0, style::kFrameHeight, nullptr, IconSize::Row14))
                    {
                        const fs::path picked = core::project::PickFolder();
                        if (!picked.empty())
                        {
                            state.newProjectFolder = picked.u8string();
                        }
                    }

                    ImGui::Dummy(ImVec2(0.0f, 6.0f));
                    PushFontRole(FontRole::Tiny);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted("Creates Content/ (Scripts, Prefabs, Materials, Models, Textures), Maps/ and an empty map.");
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                    PopFontRole();

                    if (!state.browserMessage.empty())
                    {
                        ImGui::Dummy(ImVec2(0.0f, 6.0f));
                        Banner(BannerKind::Error, state.browserMessage.c_str());
                    }
                    ImGui::EndTable();
                }

                ImGui::Dummy(ImVec2(0.0f, 8.0f));
                DialogFooter();
                if (Button("Open Other Project...", ICON_FOLDER_OPEN))
                {
                    const fs::path picked = core::project::PickProjectFile();
                    if (!picked.empty())
                    {
                        RequestOpenProject(picked.u8string());
                        editorState.showProjectBrowser = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                DialogAlignRight(150.0f + 8.0f + 80.0f);
                if (PrimaryButton("Create Project", ICON_FOLDER_PLUS, true, 150.0f))
                {
                    std::error_code error;
                    const fs::path folder = fs::u8path(state.newProjectFolder);
                    fs::create_directories(folder, error);

                    fs::path projectFile;
                    std::string createError;
                    if (core::project::CreateProject(folder, state.newProjectName, projectFile, &createError))
                    {
                        RequestOpenProject(projectFile.u8string());
                        editorState.showProjectBrowser = false;
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        state.browserMessage = createError;
                    }
                }
                ImGui::SameLine();
                if (Button("Cancel", nullptr, true, 80.0f))
                {
                    editorState.showProjectBrowser = false;
                    ImGui::CloseCurrentPopup();
                }
                EndDialog();
            }

            if (!open)
            {
                editorState.showProjectBrowser = false;
            }
        }
        else
        {
            state.browserWasOpen = false;
        }
    }
}
