#include "SceneEditorInternal.h"

#include <myengine/assistant/AssistantTools.h>

namespace myengine::ui
{
    using namespace detail;

    SceneEditor::SceneEditor() = default;
    SceneEditor::~SceneEditor() = default;

    void SceneEditor::Initialize(SceneEditorServices services)
    {
        services_ = std::move(services);
        history_ = std::make_unique<editor::EditorCommandHistory>();
        gizmo_ = std::make_unique<editor::TransformGizmo>();
        prefabInspector_ = std::make_unique<PrefabInspector>();
        scriptConsole_ = std::make_unique<ScriptConsole>();
        contentBrowser_ = std::make_unique<ContentBrowser>();
        projectUi_ = std::make_unique<ProjectUiState>();
        if (const auto& project = core::ServiceLocator::GetProjectContext(); project.IsInitialized())
        {
            // The content folder of the project; maps get their own root when they are outside of it
            std::error_code relativeError;
            const std::filesystem::path mapsInContent =
                project.MapsDir().lexically_normal().lexically_relative(project.ContentDir().lexically_normal());
            const std::string mapsText = mapsInContent.generic_u8string();
            const bool mapsInsideContent = !mapsInContent.empty() && mapsText != ".." && mapsText.rfind("../", 0) != 0;
            if (mapsInsideContent)
            {
                contentBrowser_->SetRoot(project.ContentDir(), project.ToProjectRelative(project.ContentDir()), {});
            }
            else
            {
                contentBrowser_->SetRoot(
                    project.Root(),
                    "",
                    {project.ToProjectRelative(project.ContentDir()), project.ToProjectRelative(project.MapsDir())});
                contentBrowser_->SetRootLabel(project.Name());
            }
        }
        else if (services_.resourceManager != nullptr)
        {
            contentBrowser_->SetRoot(services_.resourceManager->ResolvePath("assets"));
        }
        InitializeAssistant();
        history_->Clear();
        pendingSceneMutationSnapshot_.clear();
        pendingGizmoMutationSnapshot_.clear();
        gizmoWasUsing_ = false;
        initialized_ = true;
    }

    void SceneEditor::Shutdown()
    {
        if (history_ != nullptr)
        {
            history_->Clear();
        }
        gizmo_.reset();
        prefabInspector_.reset();
        scriptConsole_.reset();
        contentBrowser_.reset();
        projectUi_.reset();
        pendingOpenScenePath_.clear();
        pinnedMaterialPath_.clear();
        assistantPanel_.reset(); // ends a running claude process
        assistantTools_.reset();
        history_.reset();
        pendingSceneMutationSnapshot_.clear();
        pendingGizmoMutationSnapshot_.clear();
        gizmoWasUsing_ = false;
        initialized_ = false;
    }

    void SceneEditor::BuildWindowUi(const SceneEditorWindowContext& windowContext)
    {
        if (!initialized_ || services_.world == nullptr || services_.resourceManager == nullptr)
        {
            return;
        }

        ValidateSelection();
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        windowState.viewportHovered = false;
        windowState.viewportFocused = false;
        windowState.viewportAcceptsCameraNavigation = false;
        windowState.gizmoHovered = false;
        windowState.gizmoActive = false;
        windowState.viewport = {};
        windowState.viewportToolbar = {};
        windowState.materialPreviewEnabled = false;
        windowState.materialPreviewMaterialPath.clear();

        HandleKeyboardShortcuts(windowContext);
        BuildDockSpace(windowContext);
        BuildHierarchyPanel(windowContext);
        BuildInspectorPanel(windowContext);
        BuildStatisticsPanel(windowContext);
        BuildViewportPanel(windowContext);
        DrawSceneVisibilityMask(windowState.viewport);
        BuildMaterialEditorPanel(windowContext);
        BuildPrefabsPanel(windowContext);
        BuildScriptConsolePanel(windowContext);
        BuildAssistantPanel(windowContext);
        // Last, so Content Browser is the visible tab of the bottom group in the default layout
        BuildAssetBrowserPanel(windowContext);
        BuildProjectDialogs();

        if (editorState.showImGuiDemo)
        {
            ImGui::ShowDemoWindow(&editorState.showImGuiDemo);
        }
    }

    void SceneEditor::BuildDockSpace(const SceneEditorWindowContext& windowContext)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        // The menu bar, the main toolbar and the status bar come first: they shrink the work area the dockspace fills
        BuildMainMenuBar(windowContext);
        BuildMainToolbar(windowContext);
        BuildStatusBar(windowContext);

        const ImGuiDockNodeFlags dockSpaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
        const ImGuiID dockspaceId = ImGui::GetID("MyEngineEditorDockSpace");
        // A node already exists when the layout came from the ini file (checked before DockSpaceOverViewport creates one)
        const bool hasSavedLayout = ImGui::DockBuilderGetNode(dockspaceId) != nullptr;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport(), dockSpaceFlags);
        ImGui::PopStyleColor(3);

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        if (!windowState.dockLayoutInitialized)
        {
            if (resetLayoutRequested_ || !hasSavedLayout)
            {
                CreateDefaultDockLayout(windowContext);
            }
            resetLayoutRequested_ = false;
            windowState.dockLayoutInitialized = true;
        }
    }

    void SceneEditor::BuildMainMenuBar(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);

        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, editorState.mode == editor::RuntimeMode::Edit) && services_.saveScene)
                {
                    if (services_.saveScene())
                    {
                        editorState.sceneDirty = false;
                    }
                }

                if (ImGui::MenuItem("Load Scene", "Ctrl+O", false, editorState.mode == editor::RuntimeMode::Edit) && services_.loadScene)
                {
                    if (services_.loadScene())
                    {
                        editorState.selectedEntity = ecs::kInvalidEntity;
                        editorState.sceneDirty = false;
                        history_->Clear();
                    }
                }

                DrawProjectMenuItems(editorState.mode == editor::RuntimeMode::Edit);

                ImGui::Separator();
                if (ImGui::MenuItem("Quit") && services_.requestQuit)
                {
                    services_.requestQuit();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, history_ != nullptr && history_->CanUndo() && editorState.mode == editor::RuntimeMode::Edit))
                {
                    history_->Undo();
                }

                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, history_ != nullptr && history_->CanRedo() && editorState.mode == editor::RuntimeMode::Edit))
                {
                    history_->Redo();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Window"))
            {
                ImGui::MenuItem(kHierarchyWindowName, nullptr, &editorState.showHierarchy);
                ImGui::MenuItem(kInspectorWindowName, nullptr, &editorState.showInspector);
                ImGui::MenuItem(kStatisticsWindowName, nullptr, &editorState.showStatistics);
                ImGui::MenuItem(kViewportWindowName, nullptr, &editorState.showViewport);
                ImGui::MenuItem(kMaterialEditorWindowName, nullptr, &editorState.showMaterialEditor);
                ImGui::MenuItem(kAssetBrowserWindowName, nullptr, &editorState.showAssetBrowser);
                ImGui::MenuItem(kPrefabsWindowName, nullptr, &editorState.showPrefabs);
                ImGui::MenuItem(kScriptConsoleWindowName, nullptr, &editorState.showScriptConsole);
                ImGui::MenuItem(kAssistantWindowName, nullptr, &editorState.showAssistant);
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Layout"))
                {
                    editorState.showHierarchy = true;
                    editorState.showInspector = true;
                    editorState.showStatistics = true;
                    editorState.showViewport = true;
                    editorState.showMaterialEditor = true;
                    editorState.showAssetBrowser = true;
                    editorState.showPrefabs = true;
                    editorState.showScriptConsole = true;
                    editorState.showAssistant = true;
                    resetLayoutRequested_ = true;
                    windowState.dockLayoutInitialized = false; // BuildDockSpace rebuilds the default layout
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help"))
            {
                ImGui::MenuItem("ImGui Demo", nullptr, &editorState.showImGuiDemo);
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }
    }

    void SceneEditor::CreateDefaultDockLayout(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;

        ImGuiID dockspaceId = ImGui::GetID("MyEngineEditorDockSpace");
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

        // Unreal-style default: viewport in the middle, Outliner over Details on the right,
        // Content Browser / Output Log / Statistics / ... as tabs at the bottom
        ImGuiID dockRight = 0;
        ImGuiID dockRightTop = 0;
        ImGuiID dockRightBottom = 0;
        ImGuiID dockBottom = 0;
        ImGuiID dockCenter = dockspaceId;

        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.22f, &dockRight, &dockCenter);
        ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Up, 0.40f, &dockRightTop, &dockRightBottom);
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.28f, &dockBottom, &dockCenter);

        ImGui::DockBuilderDockWindow(kHierarchyWindowName, dockRightTop);
        ImGui::DockBuilderDockWindow(kInspectorWindowName, dockRightBottom);
        ImGui::DockBuilderDockWindow(kScriptConsoleWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kStatisticsWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kPrefabsWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kMaterialEditorWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kAssetBrowserWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kAssistantWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kViewportWindowName, dockCenter);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    void SceneEditor::HandleKeyboardShortcuts(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;

        if (!initialized_)
        {
            return;
        }

        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput)
        {
            return; // Delete / Ctrl+Z belong to the text field, not the selected scene entity
        }
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && services_.saveScene && editorState.mode == editor::RuntimeMode::Edit)
        {
            if (services_.saveScene())
            {
                editorState.sceneDirty = false;
            }
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false) && services_.loadScene && editorState.mode == editor::RuntimeMode::Edit)
        {
            if (services_.loadScene())
            {
                if (history_ != nullptr)
                {
                    history_->Clear();
                }
                editorState.selectedEntity = ecs::kInvalidEntity;
                editorState.sceneDirty = false;
            }
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && history_ != nullptr && history_->CanUndo() && editorState.mode == editor::RuntimeMode::Edit)
        {
            history_->Undo();
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && history_ != nullptr && history_->CanRedo() && editorState.mode == editor::RuntimeMode::Edit)
        {
            history_->Redo();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && editorState.mode == editor::RuntimeMode::Edit)
        {
            DeleteSelectedEntity();
        }
    }

    void SceneEditor::ValidateSelection() const
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.selectedEntity != ecs::kInvalidEntity && !services_.world->IsAlive(editorState.selectedEntity))
        {
            editorState.selectedEntity = ecs::kInvalidEntity;
        }
    }

    std::string SceneEditor::CaptureSceneSnapshot() const
    {
        return services_.captureSceneSnapshot != nullptr ? services_.captureSceneSnapshot() : std::string{};
    }

    void SceneEditor::RecordSceneMutationFromItem(const char* label)
    {
        if (services_.captureSceneSnapshot == nullptr || services_.restoreSceneSnapshot == nullptr)
        {
            pendingSceneMutationSnapshot_.clear();
            return;
        }

        if (ImGui::IsItemActivated() && pendingSceneMutationSnapshot_.empty())
        {
            pendingSceneMutationSnapshot_ = CaptureSceneSnapshot();
        }

        if (!ImGui::IsItemDeactivatedAfterEdit())
        {
            return;
        }

        if (pendingSceneMutationSnapshot_.empty())
        {
            return;
        }

        std::string snapshotBefore = std::move(pendingSceneMutationSnapshot_);
        pendingSceneMutationSnapshot_.clear();

        const std::string snapshotAfter = services_.captureSceneSnapshot();
        if (snapshotBefore == snapshotAfter)
        {
            return;
        }

        const auto restore = services_.restoreSceneSnapshot;
        history_->Push(std::make_unique<editor::LambdaEditorCommand>(
            label,
            [restore, snapshotBefore]() { return restore(snapshotBefore); },
            [restore, snapshotAfter]() { return restore(snapshotAfter); }));
        core::ServiceLocator::GetEditorRuntimeState().sceneDirty = true;
    }

    void SceneEditor::RecordSceneMutationImmediate(const char* label, const std::string& beforeSnapshot)
    {
        if (services_.captureSceneSnapshot == nullptr || services_.restoreSceneSnapshot == nullptr)
        {
            return;
        }

        const std::string afterSnapshot = services_.captureSceneSnapshot();
        if (beforeSnapshot == afterSnapshot)
        {
            return;
        }

        const auto restore = services_.restoreSceneSnapshot;
        history_->Push(std::make_unique<editor::LambdaEditorCommand>(
            label,
            [restore, beforeSnapshot]() { return restore(beforeSnapshot); },
            [restore, afterSnapshot]() { return restore(afterSnapshot); }));
        core::ServiceLocator::GetEditorRuntimeState().sceneDirty = true;
    }

    void SceneEditor::CommitPendingGizmoMutation()
    {
        if (pendingGizmoMutationSnapshot_.empty() || services_.captureSceneSnapshot == nullptr || services_.restoreSceneSnapshot == nullptr)
        {
            pendingGizmoMutationSnapshot_.clear();
            return;
        }

        const std::string afterSnapshot = services_.captureSceneSnapshot();
        const std::string beforeSnapshot = std::exchange(pendingGizmoMutationSnapshot_, {});
        if (beforeSnapshot == afterSnapshot)
        {
            return;
        }

        const auto restore = services_.restoreSceneSnapshot;
        history_->Push(std::make_unique<editor::LambdaEditorCommand>(
            "Manipulate Transform Gizmo",
            [restore, beforeSnapshot]() { return restore(beforeSnapshot); },
            [restore, afterSnapshot]() { return restore(afterSnapshot); }));
        core::ServiceLocator::GetEditorRuntimeState().sceneDirty = true;
    }
}
