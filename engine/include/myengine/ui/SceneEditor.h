#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/editor/EditorState.h>
#include <myengine/scripting/ScriptRuntime.h>
#include <myengine/ui/ScriptConsole.h>

namespace rl
{
    struct RlTuning;
}

namespace myengine::core
{
    class Logger;
}

struct ImDrawList;
struct ImFont;
struct ImVec2;

namespace myengine::ecs
{
    class World;
}

namespace myengine::editor
{
    class MaterialEditorModelCache;
    struct MaterialEditorModel;
    struct ShaderBanner;
    class LoadingService;
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

    // The shader programs of the renderer for Help > Developer > Shader Layouts
    struct ShaderStatusView
    {
        struct Program
        {
            std::string name;
            std::uint32_t version = 1;
            std::string lastError;      // the first error of the last reload that failed; empty when the program works
        };
        std::string summary;            // "programs 4 | PSO 7 | root signatures 3 | reloads 12 ok / 2 failed"
        // The same numbers as numbers (the Statistics panel)
        std::uint32_t programCount = 0;
        std::uint32_t pipelineStates = 0;
        std::uint32_t rootSignatures = 0;
        std::uint64_t reloads = 0;
        std::uint64_t reloadFailures = 0;
        bool available = false;         // the renderer told its numbers
        std::vector<Program> programs;
    };

    // A window of another module under Window (the Fly Brain): the menu toggles `open`, `draw` is called every frame inside the ImGui frame of the editor
    // (also when the window is closed, so that the module can stop what it runs). Added with Application::AddEditorToolWindow.
    struct EditorToolWindow
    {
        std::string name;
        bool open = false;
        std::function<void(bool& open, double deltaSeconds)> draw;
    };

    struct SceneEditorServices
    {
        ecs::World* world = nullptr;
        resource::ResourceManager* resourceManager = nullptr;
        core::Logger* logger = nullptr;
        scene::PrefabLibrary* prefabLibrary = nullptr;
        editor::ThumbnailService* thumbnails = nullptr; // asset previews (null without a render adapter)
        editor::LoadingService* loading = nullptr;      // the long operations to show (null: none are shown)
        std::function<void()> requestQuit;
        std::vector<EditorToolWindow>* toolWindows = nullptr; // owned by the Application; entries are only added, never removed
        std::function<bool()> saveScene;
        // Re-read the open map from disk; `done(success)` runs on the main thread when it has been applied
        std::function<void(std::function<void(bool)> done)> loadScene;
        std::function<bool(const std::string& scenePath)> saveSceneAs; // writes the map and makes it the open one
        std::function<bool(const std::string& projectFile, bool discardScene)> restartWithProject; // new process, then quit
        // Scene file by asset key ("assets/scenes/x.json"); it becomes the one Save writes. The map is read in the
        // background behind a loading window; `done(success)` runs on the main thread when it has been applied.
        std::function<void(const std::string& scenePath, std::function<void(bool)> done)> openScene;
        std::function<std::string()> captureSceneSnapshot;
        std::function<bool(std::string_view)> restoreSceneSnapshot;
        std::function<void()> reloadScripts; // hot reload of every script, same as F5
        // GPU frame capture with RenderDoc (F12, Help > Developer). false: `message` says what to install or start.
        std::function<bool(std::string& message)> captureGpuFrame;
        // Script fields for the inspector (ScriptRuntime::DescribeFields)
        std::function<std::vector<scripting::ScriptFieldInfo>(const std::string& module, const std::string& className)> describeScriptFields;
        // Play only: "Active" / "Starting" / "Faulted" and current field values of a running script object
        std::function<std::string(ecs::EntityId entity, std::size_t scriptIndex)> scriptStatus;
        std::function<nlohmann::json(ecs::EntityId entity, std::size_t scriptIndex)> liveScriptFields;
        std::function<std::vector<std::string>()> scriptHudLines;
        // The RL Tuning window (Play, Rocket League game): the numbers of the running game, null when there is none. rlTuningEdited tells
        // the game they changed; Save writes assets/data/rl_tuning.json, Reload reads it again; both return a message for the window.
        std::function<rl::RlTuning*()> rlTuning;
        std::function<void()> rlTuningEdited;
        std::function<std::string()> rlTuningSave;
        std::function<std::string()> rlTuningReload;
        ScriptConsoleServices scriptConsole;
        // Viewport toolbar > Graphics / Lit: renderer switches
        std::function<bool()> isVSyncEnabled;
        std::function<void(bool)> setVSync;
        std::function<void(bool)> setWireframe;
        std::function<ShaderStatusView()> shaderStatus; // the live shader programs: version, last error, GPU object counts
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
        // The form of a material (Material, Textures, Parameters): shared by the Material Editor and the Material Viewer. It is built from
        // the layout of the shader of the material; without a layout (an adapter that cannot reflect, a shader that does not compile)
        // the old look stays: DrawMaterialCategoryPlain (shader, texture, tint).
        void DrawMaterialCategory(const std::string& materialPath, bool editEnabled);
        void DrawMaterialCategoryPlain(const std::string& materialPath, bool editEnabled);
        // SceneEditorMaterialForm.cpp. `model` is null when the shader did not compile (the first time): only the shader can be chosen then.
        void DrawMaterialForm(const std::string& materialPath, const editor::MaterialEditorModel* model, const editor::ShaderBanner& shaderBanner, bool editEnabled);
        // "Could not write the file" with "Try again", while the last write of this material failed
        void DrawMaterialWriteBanner(const std::string& materialPath);
        // A gesture on a material widget that lost its widget (the panel was closed, Play started) ends as if the mouse was released
        void FinishAbandonedMaterialGesture(bool playing);
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
        void DrawShaderLayoutsWindow();   // Help > Developer > Shader Layouts (what the reflection reads from the shaders)
        void DrawToolWindows(core::WindowId windowId);
        void DrawRlTuningWindow();        // Window > RL Tuning: the sliders of the Rocket League numbers (SceneEditorRlTuning.cpp)
        // The loading window of a long operation (SceneEditorLoading.cpp). Also blocks the input while one runs.
        void DrawLoadingUi();
        // The startup splash instead of the editor; true while it is on (nothing else is drawn then)
        bool DrawStartupSplash();
        // The button of the background tasks in the status bar (its right edge at `rightEdge`) and their popup
        void DrawBackgroundTasks(ImDrawList* drawList, float rightEdge, float centerY);
        void ReportLoadError(const std::string& title, const std::string& text);
        void CaptureGpuFrame(); // F12, Help > Developer: a failure is a dialog that says what to install
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
        void StopPlayMode(); // Stop button and Esc: back to the scene snapshot taken at Play, no-op in Edit
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
        std::string loadErrorTitle_; // a map that could not be opened: shown as a dialog when the loading window is gone
        std::string loadErrorText_;
        bool showWidgetsGallery_ = false; // Help > Developer > Widgets Gallery
        bool showThumbnailsDebug_ = false; // Help > Developer > Thumbnails
        bool showShaderLayouts_ = false;   // Help > Developer > Shader Layouts
        bool showRlTuning_ = true;         // Window > RL Tuning (drawn while a Rocket League match runs)
        std::string rlTuningMessage_;      // what the window last did
        std::string shaderLayoutsText_;    // the table of the window; empty: built at the next frame
        int shaderLayoutsFailures_ = 0;    // programs that did not compile
        float materialPreviewYaw_ = 0.61f;   // orbit camera of the Material Editor preview (radians)
        float materialPreviewPitch_ = 0.44f;
        float materialPreviewDistance_ = 3.0f;
        std::vector<std::unique_ptr<AssetViewerTab>> viewers_; // open Texture / Mesh / Material / Prefab viewer tabs
        std::unique_ptr<resource::MaterialAsset> materialEditBefore_; // the material when the current widget interaction began
        std::unordered_map<std::string, std::unique_ptr<editor::MaterialEditorModelCache>> materialModels_; // the rows of the form, per material
        std::string materialEditPath_;  // the material and the name of the command of the gesture in materialEditBefore_
        std::string materialEditLabel_;
        std::string materialParamFilter_; // the filter of the parameters (not kept between runs)
        mutable std::unordered_set<std::string> materialWriteFailures_; // materials whose last write failed: the change is in memory only
        core::WindowId toolWindowOwner_ = 0;   // the editor window that draws the tool windows (the first one that asks), so that they run once per frame
        std::vector<std::string> thumbnailDebugAssets_;
        int thumbnailDebugSize_ = 128;
    };
}
