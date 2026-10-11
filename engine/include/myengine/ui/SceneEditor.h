#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/editor/EditorState.h>
#include <myengine/scripting/ScriptRuntime.h>
#include <myengine/ui/ScriptConsole.h>

namespace myengine::core
{
    class Logger;
}

struct ImFont;
struct ImVec2;

namespace myengine::ecs
{
    class World;
}

namespace myengine::editor
{
    class EditorCommandHistory;
    class ThumbnailService;
    class TransformGizmo;
}

namespace myengine::assistant
{
    class AssistantTools;
}

namespace myengine::resource
{
    class ResourceManager;
    struct MaterialAsset;
}

namespace myengine::scene
{
    class PrefabLibrary;
}

namespace myengine::ui
{
    class ContentBrowser;
    struct ContentEntry;
    struct ContentThumbnail;
    struct ProjectUiState;
    class AssistantPanel;
    class PrefabInspector;
    enum class AssetViewerType : std::uint8_t;
    struct AssetViewerTab;

    struct SceneEditorServices
    {
        ecs::World* world = nullptr;
        resource::ResourceManager* resourceManager = nullptr;
        core::Logger* logger = nullptr;
        scene::PrefabLibrary* prefabLibrary = nullptr;
        editor::ThumbnailService* thumbnails = nullptr; // asset previews (null without a render adapter)
        std::function<void()> requestQuit;
        std::function<bool()> saveScene;
        std::function<bool()> loadScene;
        std::function<bool(const std::string& scenePath)> saveSceneAs; // writes the map and makes it the open one
        std::function<bool(const std::string& projectFile, bool discardScene)> restartWithProject; // new process, then quit
        std::function<bool(const std::string& scenePath)> openScene; // scene file by asset key ("assets/scenes/x.json"); it becomes the one Save writes
        std::function<std::string()> captureSceneSnapshot;
        std::function<bool(std::string_view)> restoreSceneSnapshot;
        std::function<void()> reloadScripts; // hot reload of every script, same as F5
        // Script fields for the inspector (ScriptRuntime::DescribeFields)
        std::function<std::vector<scripting::ScriptFieldInfo>(const std::string& module, const std::string& className)> describeScriptFields;
        // Play only: "Active" / "Starting" / "Faulted" and current field values of a running script object
        std::function<std::string(ecs::EntityId entity, std::size_t scriptIndex)> scriptStatus;
        std::function<nlohmann::json(ecs::EntityId entity, std::size_t scriptIndex)> liveScriptFields;
        std::function<std::vector<std::string>()> scriptHudLines;
        ScriptConsoleServices scriptConsole;
        // Viewport toolbar > Graphics / Lit: renderer switches
        std::function<bool()> isVSyncEnabled;
        std::function<void(bool)> setVSync;
        std::function<void(bool)> setWireframe;
        // A small RGBA picture as an ImGui texture (the assistant's attachment chips): the id to give ImGui::AddImage, 0 on
        // failure; the second function releases it. Set by UiManager.
        std::function<std::uint64_t(const unsigned char* rgba, std::uint32_t width, std::uint32_t height)> createUiTexture;
        std::function<void(std::uint64_t)> destroyUiTexture;
    };

    struct SceneEditorWindowContext
    {
        core::WindowId windowId = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        ImFont* monoFont = nullptr;
        std::string stateLabel;
    };

    class SceneEditor
    {
    public:
        SceneEditor();
        ~SceneEditor();

        void Initialize(SceneEditorServices services);
        void Shutdown();
        void BuildWindowUi(const SceneEditorWindowContext& windowContext);
        bool CanStartSceneNavigation(core::WindowId windowId, float mouseX, float mouseY) const;

    private:
        void BuildDockSpace(const SceneEditorWindowContext& windowContext);
        void BuildMainMenuBar(const SceneEditorWindowContext& windowContext);
        void BuildMainToolbar(const SceneEditorWindowContext& windowContext);
        void BuildStatusBar(const SceneEditorWindowContext& windowContext);
        bool BuildToolbar(const SceneEditorWindowContext& windowContext, const editor::ViewportRect& viewportRect);
        void BuildHierarchyPanel(const SceneEditorWindowContext& windowContext);
        void BuildInspectorPanel(const SceneEditorWindowContext& windowContext);
        void BuildStatisticsPanel(const SceneEditorWindowContext& windowContext);
        void BuildViewportPanel(const SceneEditorWindowContext& windowContext);
        void BuildMaterialEditorPanel(const SceneEditorWindowContext& windowContext);
        void BuildAssetBrowserPanel(const SceneEditorWindowContext& windowContext);
        void DrawFileMenu(bool editMode); // the File menu entries (maps, project, quit)
        void OpenMapDialog();
        void BuildProjectDialogs(); // Project Browser and the map dialogs
        void DrawMaterialPreview(const std::string& materialPath, editor::WindowEditorState& windowState);
        // The Material category (shader, texture, tint): shared by the Material Editor and the Material Viewer
        void DrawMaterialCategory(const std::string& materialPath, bool editEnabled);
        // Asset viewers: tabs next to the Viewport (SceneEditorViewers.cpp)
        void OpenAssetViewer(const std::string& assetPath, AssetViewerType type);
        void BuildAssetViewers(const SceneEditorWindowContext& windowContext);
        void CloseAssetViewers();
        void DrawTextureCanvas(AssetViewerTab& tab, const ImVec2& canvasMin, const ImVec2& canvasSize);
        void DrawTexturePanel(AssetViewerTab& tab);
        void DrawMeshCanvas(AssetViewerTab& tab, const ImVec2& canvasMin, const ImVec2& canvasSize);
        void DrawMeshPanel(AssetViewerTab& tab);
        void DrawMaterialCanvas(AssetViewerTab& tab, const ImVec2& canvasMin, const ImVec2& canvasSize);
        void DrawMaterialPanel(AssetViewerTab& tab);
        void DrawPrefabCanvas(AssetViewerTab& tab, const ImVec2& canvasMin, const ImVec2& canvasSize);
        void DrawPrefabPanel(AssetViewerTab& tab);
        void DrawThumbnailsDebugWindow(); // Help > Developer > Thumbnails (acceptance tool of the preview service)
        void RequestOpenProject(const std::string& projectFile);
        ContentThumbnail MakeContentThumbnail(const ContentEntry& entry, std::uint32_t pixelSize) const;
        void RequestOpenScene(const std::string& scenePath);
        void OpenSceneNow(const std::string& scenePath);
        void DrawOpenScenePrompt();
        void BuildPrefabsPanel(const SceneEditorWindowContext& windowContext);
        void BuildScriptConsolePanel(const SceneEditorWindowContext& windowContext);
        void BuildAssistantPanel(const SceneEditorWindowContext& windowContext);
        void InitializeAssistant();
        void HandleKeyboardShortcuts(const SceneEditorWindowContext& windowContext);
        void ValidateSelection() const;
        void CreateDefaultDockLayout(const SceneEditorWindowContext& windowContext);
        void DrawEntityNode(core::WindowId windowId, ecs::EntityId entity);
        void SpawnRenderableEntity(core::WindowId windowId, std::string meshPath, std::string materialPath);
        bool ApplyMaterialAsset(const std::string& materialPath, const resource::MaterialAsset& asset) const;
        bool PushMaterialAssetCommand(
            const char* label,
            const std::string& materialPath,
            const resource::MaterialAsset& beforeAsset,
            const resource::MaterialAsset& afterAsset);
        // Called right after a material widget: one undo entry per interaction (a drag, a typed value, a pick).
        // `frameStartAsset` is the asset at the start of the frame; it is moved to `current` afterwards.
        void CommitMaterialWidgetEdit(
            const char* label,
            const std::string& materialPath,
            resource::MaterialAsset& frameStartAsset,
            const resource::MaterialAsset& current);
        std::string ResolveSuggestedMaterialForMesh(const std::string& meshPath) const;
        std::string EnsureTexturePreviewMaterial(const std::string& texturePath) const;
        std::string CaptureSceneSnapshot() const;
        ecs::EntityId PickEntityAtScreenPosition(
            const SceneEditorWindowContext& windowContext,
            const editor::WindowEditorState& windowState,
            const ImVec2& mousePosition) const;
        void RecordSceneMutationFromItem(const char* label);
        void RecordSceneMutationImmediate(const char* label, const std::string& beforeSnapshot);
        void CommitPendingGizmoMutation();
        void DeleteSelectedEntity();
        void CreateEmptyEntity(core::WindowId windowId);

        SceneEditorServices services_{};
        std::unique_ptr<editor::EditorCommandHistory> history_;
        std::unique_ptr<editor::TransformGizmo> gizmo_;
        std::unique_ptr<PrefabInspector> prefabInspector_;
        std::unique_ptr<ScriptConsole> scriptConsole_;
        std::unique_ptr<ContentBrowser> contentBrowser_;
        std::unique_ptr<ProjectUiState> projectUi_;
        std::string pendingOpenScenePath_; // waits for the answer of the unsaved changes prompt
        std::string pinnedMaterialPath_; // a material opened from the Content Browser, shown instead of the selected entity's one
        ecs::EntityId pinnedMaterialEntity_ = ecs::kInvalidEntity; // selection when it was pinned; another selection unpins
        std::unique_ptr<assistant::AssistantTools> assistantTools_; // before the panel: the panel's bridge calls into it
        std::unique_ptr<AssistantPanel> assistantPanel_;
        std::string pendingSceneMutationSnapshot_;
        std::string pendingGizmoMutationSnapshot_;
        bool gizmoWasUsing_ = false;
        bool initialized_ = false;
        bool resetLayoutRequested_ = false;
        int layoutFocusFrames_ = 0; // frames until a fresh default layout gets its active tabs
        bool wasPlaying_ = false;   // Edit -> Play edge: the viewport takes the keyboard from any text field
        bool showWidgetsGallery_ = false; // Help > Developer > Widgets Gallery
        bool showThumbnailsDebug_ = false; // Help > Developer > Thumbnails
        float materialPreviewYaw_ = 0.61f;   // orbit camera of the Material Editor preview (radians)
        float materialPreviewPitch_ = 0.44f;
        float materialPreviewDistance_ = 3.0f;
        std::vector<std::unique_ptr<AssetViewerTab>> viewers_; // open Texture / Mesh / Material / Prefab viewer tabs
        std::unique_ptr<resource::MaterialAsset> materialEditBefore_; // the material when the current widget interaction began
        std::vector<std::string> thumbnailDebugAssets_;
        int thumbnailDebugSize_ = 128;
    };
}
