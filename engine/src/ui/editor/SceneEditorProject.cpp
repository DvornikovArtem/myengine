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

    void SceneEditor::DrawProjectMenuItems(const bool editMode)
    {
        if (projectUi_ == nullptr)
        {
            return;
        }

        ImGui::Separator();
        if (ImGui::MenuItem("New Map...", nullptr, false, editMode))
        {
            projectUi_->mapDialog = ProjectUiState::MapDialog::NewMap;
            projectUi_->openMapDialogRequested = true;
            projectUi_->mapName = "NewMap";
            projectUi_->message.clear();
        }

        if (ImGui::MenuItem("Open Map...", nullptr, false, editMode))
        {
            projectUi_->mapDialog = ProjectUiState::MapDialog::OpenMap;
            projectUi_->openMapDialogRequested = true;
            projectUi_->message.clear();
            CollectMaps(core::ServiceLocator::GetProjectContext(), projectUi_->mapChoices);
        }

        if (ImGui::MenuItem("Save Map As...", nullptr, false, editMode))
        {
            projectUi_->mapDialog = ProjectUiState::MapDialog::SaveMapAs;
            projectUi_->openMapDialogRequested = true;
            projectUi_->mapName = fs::u8path(core::ServiceLocator::GetEditorRuntimeState().mapPath).stem().u8string();
            projectUi_->message.clear();
            projectUi_->confirmOverwrite = false;
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Open Project..."))
        {
            core::ServiceLocator::GetEditorRuntimeState().showProjectBrowser = true;
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
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();

        // ---- Map dialogs
        const char* mapPopup = nullptr;
        switch (state.mapDialog)
        {
            case ProjectUiState::MapDialog::NewMap: mapPopup = kNewMapPopup; break;
            case ProjectUiState::MapDialog::OpenMap: mapPopup = kOpenMapPopup; break;
            case ProjectUiState::MapDialog::SaveMapAs: mapPopup = kSaveMapAsPopup; break;
            case ProjectUiState::MapDialog::None: break;
        }

        if (mapPopup != nullptr)
        {
            if (state.openMapDialogRequested)
            {
                ImGui::OpenPopup(mapPopup);
                state.openMapDialogRequested = false;
            }

            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(720.0f, 520.0f));
            bool open = true;
            if (ImGui::BeginPopupModal(mapPopup, &open, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextDisabled("Maps folder: %s", project.ToProjectRelative(project.MapsDir()).c_str());
                ImGui::Spacing();

                if (state.mapDialog == ProjectUiState::MapDialog::OpenMap)
                {
                    if (state.mapChoices.empty())
                    {
                        ImGui::TextDisabled("There are no maps in this project yet.");
                    }

                    ImGui::BeginChild("##map_list", ImVec2(0.0f, 240.0f), ImGuiChildFlags_Borders);
                    for (const auto& map : state.mapChoices)
                    {
                        const bool current = map == editorState.mapPath;
                        if (ImGui::Selectable(map.c_str(), current, ImGuiSelectableFlags_AllowDoubleClick) &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            RequestOpenScene(map);
                            state.mapDialog = ProjectUiState::MapDialog::None;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::EndChild();
                    ImGui::TextDisabled("Double click a map to open it.");
                }
                else
                {
                    ImGui::SetNextItemWidth(320.0f);
                    if (ImGui::InputText("Name", &state.mapName))
                    {
                        state.message.clear();
                        state.confirmOverwrite = false;
                    }

                    const std::string key = IsValidMapName(state.mapName) ? MapKeyFor(project, state.mapName) : std::string();
                    if (!key.empty())
                    {
                        ImGui::TextDisabled("%s", key.c_str());
                    }
                    if (!state.message.empty())
                    {
                        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", state.message.c_str());
                    }

                    const bool isNew = state.mapDialog == ProjectUiState::MapDialog::NewMap;
                    ImGui::BeginDisabled(key.empty());
                    if (ImGui::Button(isNew ? "Create" : (state.confirmOverwrite ? "Overwrite" : "Save")))
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
                    ImGui::EndDisabled();
                }

                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    state.mapDialog = ProjectUiState::MapDialog::None;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
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

            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 0.0f), ImVec2(860.0f, 640.0f));
            bool open = true;
            if (ImGui::BeginPopupModal(kProjectBrowserPopup, &open, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Current project: %s", project.Name().c_str());
                ImGui::TextDisabled("%s", project.Root().u8string().c_str());
                ImGui::Separator();

                ImGui::TextUnformatted("Recent projects");
                ImGui::BeginChild("##recent_projects", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Borders);
                if (state.recentProjects.empty())
                {
                    ImGui::TextDisabled("Projects that you open appear here.");
                }
                for (std::size_t index = 0; index < state.recentProjects.size(); ++index)
                {
                    const fs::path file = fs::u8path(state.recentProjects[index]);
                    ImGui::PushID(static_cast<int>(index));
                    const std::string label = file.stem().u8string() + "##recent";
                    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        RequestOpenProject(state.recentProjects[index]);
                        editorState.showProjectBrowser = false;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", file.parent_path().u8string().c_str());
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::TextDisabled("Double click a project to open it.");

                if (ImGui::Button("Open Project..."))
                {
                    const fs::path picked = core::project::PickProjectFile();
                    if (!picked.empty())
                    {
                        RequestOpenProject(picked.u8string());
                        editorState.showProjectBrowser = false;
                        ImGui::CloseCurrentPopup();
                    }
                }

                ImGui::Separator();
                ImGui::TextUnformatted("New project");
                ImGui::SetNextItemWidth(300.0f);
                ImGui::InputText("Name##new_project", &state.newProjectName);
                ImGui::SetNextItemWidth(300.0f);
                ImGui::InputText("Folder##new_project", &state.newProjectFolder);
                ImGui::SameLine();
                if (ImGui::Button("Browse..."))
                {
                    const fs::path picked = core::project::PickFolder();
                    if (!picked.empty())
                    {
                        state.newProjectFolder = picked.u8string();
                    }
                }

                if (ImGui::Button("Create Project"))
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
                if (!state.browserMessage.empty())
                {
                    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", state.browserMessage.c_str());
                }
                ImGui::TextDisabled("Creates Content/{Scripts,Prefabs,Materials,Models,Textures}, Maps/ and an empty map.");

                ImGui::Separator();
                if (ImGui::Button("Close"))
                {
                    editorState.showProjectBrowser = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
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
