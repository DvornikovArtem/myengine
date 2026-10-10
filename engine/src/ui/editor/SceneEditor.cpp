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
        // The order of the Begin calls is the tab order of a freshly docked group
        BuildHierarchyPanel(windowContext);
        BuildViewportPanel(windowContext);
        DrawSceneVisibilityMask(windowState.viewport);
        BuildInspectorPanel(windowContext);
        BuildPrefabsPanel(windowContext);
        BuildMaterialEditorPanel(windowContext);
        BuildAssetBrowserPanel(windowContext);
        BuildScriptConsolePanel(windowContext);
        BuildAssistantPanel(windowContext);
        BuildStatisticsPanel(windowContext);
        BuildProjectDialogs();

        // The viewport shows no tab strip while it is alone in its node (spec 3.1)
        if (ImGuiWindow* viewportWindow = ImGui::FindWindowByName(kViewportWindowName);
            viewportWindow != nullptr && viewportWindow->DockNode != nullptr)
        {
            // NoTabBar (not AutoHideTabBar) so that no tab-strip toggle triangle shows in the corner; a node with
            // several windows gets its strip back, otherwise the extra tabs could not be switched
            ImGuiDockNode* node = viewportWindow->DockNode;
            if (node->Windows.Size <= 1)
            {
                node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
            }
            else
            {
                node->LocalFlags &= ~ImGuiDockNodeFlags_NoTabBar;
            }
        }

        // After a fresh layout the windows exist from the second frame on: bring Content Browser and Details to
        // the front of their groups (one window per frame, a second focus in the same frame wins over the first)
        if (layoutFocusFrames_ > 0)
        {
            --layoutFocusFrames_;
            if (layoutFocusFrames_ == 2)
            {
                ImGui::SetWindowFocus(kAssetBrowserWindowName);
            }
            else if (layoutFocusFrames_ == 1)
            {
                ImGui::SetWindowFocus(kInspectorWindowName);
            }
        }

        if (editorState.showImGuiDemo)
        {
            ImGui::ShowDemoWindow(&editorState.showImGuiDemo);
        }
        DrawWidgetsGallery(&showWidgetsGallery_);
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
        const bool isEditMode = editorState.mode == editor::RuntimeMode::Edit;

        // 28 px bar: the body font + 2 x padding
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, (style::kMenuBarHeight - FontRoleSize(FontRole::Body)) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        if (ImGui::BeginMainMenuBar())
        {
            // Logo
            {
                const ImVec2 position = ImGui::GetCursorScreenPos();
                DrawIcon(ImGui::GetWindowDrawList(), IconSize::Button16, ICON_CUBOID,
                         ImVec2(position.x + 16.0f, position.y + style::kMenuBarHeight * 0.5f), style::kText);
                ImGui::Dummy(ImVec2(30.0f, 0.0f));
                ImGui::SameLine(0.0f, 0.0f);
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
            if (ImGui::BeginMenu("File"))
            {
                DrawFileMenu(isEditMode);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                const bool canUndo = history_ != nullptr && history_->CanUndo() && isEditMode;
                const bool canRedo = history_ != nullptr && history_->CanRedo() && isEditMode;
                if (MenuItemIcon(ICON_UNDO_2, "Undo", "Ctrl+Z", false, canUndo, false, 200.0f))
                {
                    history_->Undo();
                }
                if (MenuItemIcon(ICON_REDO_2, "Redo", "Ctrl+Y", false, canRedo, false, 200.0f))
                {
                    history_->Redo();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Window"))
            {
                const float width = 230.0f;
                MenuSection("LEVEL EDITOR");
                if (MenuItemIcon(ICON_MONITOR, "Viewport", nullptr, editorState.showViewport, true, true, width)) { editorState.showViewport = !editorState.showViewport; }
                if (MenuItemIcon(ICON_LIST_TREE, "Outliner", nullptr, editorState.showHierarchy, true, true, width)) { editorState.showHierarchy = !editorState.showHierarchy; }
                if (MenuItemIcon(ICON_SLIDERS_HORIZONTAL, "Details", nullptr, editorState.showInspector, true, true, width)) { editorState.showInspector = !editorState.showInspector; }
                MenuSection("CONTENT");
                if (MenuItemIcon(ICON_FOLDER, "Content Browser", nullptr, editorState.showAssetBrowser, true, true, width)) { editorState.showAssetBrowser = !editorState.showAssetBrowser; }
                if (MenuItemIcon(ICON_PACKAGE, "Prefabs", nullptr, editorState.showPrefabs, true, true, width)) { editorState.showPrefabs = !editorState.showPrefabs; }
                if (MenuItemIcon(ICON_PALETTE, "Material Editor", nullptr, editorState.showMaterialEditor, true, true, width)) { editorState.showMaterialEditor = !editorState.showMaterialEditor; }
                MenuSection("TOOLS");
                if (MenuItemIcon(ICON_SCROLL_TEXT, "Output Log", nullptr, editorState.showScriptConsole, true, true, width)) { editorState.showScriptConsole = !editorState.showScriptConsole; }
                if (MenuItemIcon(ICON_ACTIVITY, "Statistics", nullptr, editorState.showStatistics, true, true, width)) { editorState.showStatistics = !editorState.showStatistics; }
                if (MenuItemIcon(ICON_SPARKLES, "Assistant", nullptr, editorState.showAssistant, true, true, width)) { editorState.showAssistant = !editorState.showAssistant; }
                ImGui::Separator();
                if (MenuItemIcon(ICON_LAYOUT_GRID, "Reset Layout", nullptr, false, true, false, width))
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
                if (ImGui::BeginMenu("Developer"))
                {
                    ImGui::MenuItem("ImGui Demo", nullptr, &editorState.showImGuiDemo);
                    ImGui::MenuItem("Widgets Gallery", nullptr, &showWidgetsGallery_);
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            ImGui::PopStyleVar();

            // Right: map, unsaved dot, project, Playing chip
            {
                const auto& project = core::ServiceLocator::GetProjectContext();
                std::string mapName = std::filesystem::u8path(editorState.mapPath).stem().u8string();
                if (mapName.empty())
                {
                    mapName = "Untitled";
                }
                const std::string projectName = project.Name();

                PushFontRole(FontRole::Secondary);
                const float mapWidth = ImGui::CalcTextSize(mapName.c_str()).x;
                const float projectWidth = projectName.empty() ? 0.0f : ImGui::CalcTextSize(projectName.c_str()).x + 22.0f;
                const float dotWidth = editorState.sceneDirty ? 14.0f : 0.0f;
                PopFontRole();
                const float chipWidth = isEditMode ? 0.0f : 64.0f;
                const float total = mapWidth + dotWidth + projectWidth + chipWidth + (chipWidth > 0.0f ? 8.0f : 0.0f);

                const ImVec2 windowPosition = ImGui::GetWindowPos();
                const float windowWidth = ImGui::GetWindowWidth();
                float x = windowPosition.x + windowWidth - 14.0f - total;
                const float centerY = windowPosition.y + style::kMenuBarHeight * 0.5f;
                auto* drawList = ImGui::GetWindowDrawList();
                PushFontRole(FontRole::Secondary);
                const float textY = std::floor(centerY - FontRoleSize(FontRole::Secondary) * 0.5f - 0.5f);
                drawList->AddText(ImVec2(x, textY), style::kText, mapName.c_str());
                x += mapWidth;
                if (editorState.sceneDirty)
                {
                    drawList->AddCircleFilled(ImVec2(x + 8.0f, centerY), 3.0f, style::kWarning);
                    x += 14.0f;
                }
                if (!projectName.empty())
                {
                    drawList->AddText(ImVec2(x + 6.0f, textY), style::kTextDim, "-");
                    drawList->AddText(ImVec2(x + 22.0f, textY), style::kTextDim, projectName.c_str());
                    x += projectWidth;
                }
                PopFontRole();
                if (!isEditMode)
                {
                    ImGui::SetCursorScreenPos(ImVec2(x + 8.0f, centerY - 9.0f));
                    Chip("Playing", ChipKind::Green);
                }
            }

            ImGui::EndMainMenuBar();
        }
        ImGui::PopStyleVar(2);
    }

    void SceneEditor::CreateDefaultDockLayout(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;

        ImGuiID dockspaceId = ImGui::GetID("MyEngineEditorDockSpace");
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

        // Unreal-style default (spec 3.1): Viewport in the middle without a tab strip, Outliner over the
        // Details | Prefabs | Material Editor group on the right, Content Browser | Output Log | Assistant |
        // Statistics along the bottom
        ImGuiID dockRight = 0;
        ImGuiID dockRightTop = 0;
        ImGuiID dockRightBottom = 0;
        ImGuiID dockBottom = 0;
        ImGuiID dockCenter = dockspaceId;

        // Split ratios are relative to the size the layout is built at (the window may be small and get maximised
        // later), so the target sizes are absolute: right column 400..480 px, bottom area 270..340 px
        const ImVec2 workSize = ImGui::GetMainViewport()->WorkSize;
        const float rightWidth = std::clamp(workSize.x * 0.22f, 400.0f, 480.0f);
        const float bottomHeight = std::clamp(workSize.y * 0.30f, 270.0f, 340.0f);
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, std::min(rightWidth / std::max(workSize.x, 1.0f), 0.5f), &dockRight, &dockCenter);
        ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Up, 0.38f, &dockRightTop, &dockRightBottom);
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, std::min(bottomHeight / std::max(workSize.y, 1.0f), 0.5f), &dockBottom, &dockCenter);

        ImGui::DockBuilderDockWindow(kHierarchyWindowName, dockRightTop);
        ImGui::DockBuilderDockWindow(kInspectorWindowName, dockRightBottom);
        ImGui::DockBuilderDockWindow(kPrefabsWindowName, dockRightBottom);
        ImGui::DockBuilderDockWindow(kMaterialEditorWindowName, dockRightBottom);
        ImGui::DockBuilderDockWindow(kAssetBrowserWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kScriptConsoleWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kAssistantWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kStatisticsWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kViewportWindowName, dockCenter);
        ImGui::DockBuilderFinish(dockspaceId);
        layoutFocusFrames_ = 4; // once the windows exist, BuildWindowUi brings Details and Content Browser to the front
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

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false) && editorState.mode == editor::RuntimeMode::Edit)
        {
            OpenMapDialog(); // same path as File > Open Map...
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
