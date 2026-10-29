// Application.h

#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <cstdint>
#include <vector>

#include <myengine/ecs/systems/RlGameSystem.h>
#include <myengine/config/AppConfig.h>
#include <myengine/core/Logger.h>
#include <myengine/core/Timer.h>
#include <myengine/core/Types.h>
#include <myengine/core/Window.h>
#include <myengine/ecs/World.h>
#include <myengine/editor/LoadingService.h>
#include <myengine/faceid/FaceIdGate.h>
#include <myengine/input/InputManager.h>
#include <myengine/render/GpuCapture.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/resource/ResourceManager.h>
#include <myengine/scripting/PrefabLibrary.h>
#include <myengine/scripting/ScriptRuntime.h>
#include <myengine/game/DoomController.h>
#include <myengine/state/StateMachine.h>
#include <myengine/ui/UiManager.h>

namespace myengine::rlui
{
    class Host;
}

namespace myengine::game
{
    class DoomGameFlow;
    class RlGameFlow;
    struct RlVideoSettings;
}

namespace myengine::scripting
{
    class ScriptSystem;
}

namespace myengine::ecs::systems
{
    class RlGameSystem;
}

namespace myengine::editor
{
    class ThumbnailService;
}

namespace myengine::core
{
    class UiScript;

    // Command-line diagnostics of the GPU (docs/dev/gpu-debugging.md)
    struct DiagnosticsOptions
    {
        std::uint64_t captureFrame = 0;     // --gpu-capture-frame N: capture the N-th rendered frame with RenderDoc
        bool exitAfterCapture = false;      // --exit-after-capture: quit with code 0 when the capture file is written
        std::uint64_t frameStatsFrame = 0;  // --frame-stats N: write the statistics of the N-th frame and quit
        std::filesystem::path frameStatsOut; // --frame-stats-out <file>: where (default Saved/FrameStats)
        bool renderDoc = false;             // --renderdoc: load RenderDoc at start (F12 and Help > Developer)
        bool pix = false;                   // --pix: load PIX's capturer so that PIX can attach
    };

    class Application
    {
    public:
        /// <summary>
        /// explicit disallows implicit conversions (protects against accidental Application app = something).
        /// The HINSTANCE instance parameter is the WinAPI application handle.
        /// The default value GetModuleHandleW(nullptr) retrieves the handle of the current module (the current executable).
        /// </summary>
        explicit Application(HINSTANCE instance = GetModuleHandleW(nullptr));
        ~Application();

        /// startInPlay: begin in Play (scripts and physics running) instead of Edit.
        bool Initialize(const config::AppConfig& config, const std::filesystem::path& scenePath = {}, bool startInPlay = false);
        /// Before Initialize: the .myproject to open (empty: the project of the engine folder).
        void SetProjectFile(const std::filesystem::path& projectFile);
        /// Before Initialize: open the Project Browser over the editor at start.
        void SetShowProjectBrowser(bool show);
        /// Before Initialize: GPU capture and statistics requested on the command line.
        void SetDiagnosticsOptions(const DiagnosticsOptions& options);
        /// Before Initialize: the role of this window in an online Rocket League match (--rl-host, --rl-join).
        void SetRlOnlineOptions(const ecs::systems::RlOnlineOptions& options);
        /// Before Initialize: the game mode (--game, docs/game/rl-ui.md): the Rocket League game without the editor: menus and HUD of the UI kit, the map
        /// rocket_league.json in Play, the window as the video settings say.
        void SetGameMode(bool enabled);
        bool GameMode() const;
        /// Before Initialize: FlyDoom (docs/game/doom.md): the classic DOOM is the whole window, instead of the Rocket League game or the editor
        /// (--game doom, and FlyDoom.exe). It implies the game mode.
        struct DoomOptions
        {
            std::filesystem::path iwad;             // --iwad: the game data; empty: looked for (assets/doom, Steam, Freedoom)
            std::vector<std::string> args;          // the command line of DOOM itself (--doom-args)
            bool squarePixels = false;              // --doom-square-pixels: no 4:3 stretch of the 320x200 picture
            bool music = true;                      // --doom-no-music
            std::vector<game::DoomMode> modes;      // who can play: the human first, then the controllers of the app (the fly, docs/game/flydoom-fly.md)
            int initialMode = -1;                   // the mode to start with (--fly), -1: a menu asks when there are several
        };
        void SetDoomMode(bool enabled, const DoomOptions& options = {});
        bool DoomMode() const;
        /// The video settings of the game: window mode and size, vertical sync, the limit of frames per second, shadows / MSAA / bloom. A --render-settings
        /// request on the command line wins over the quality of the settings.
        void ApplyVideoSettings(const game::RlVideoSettings& video);
        /// The resolutions the display can be set to (the video settings offer them)
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ListDisplayResolutions() const;
        /// The size of the first window in pixels
        std::pair<std::uint32_t, std::uint32_t> GetWindowSize() const;
        /// The code the process should return when the loop ended on purpose (a capture or statistics request).
        int GetExitCode() const;
        /// A capture of the next frame with RenderDoc (F12, Help > Developer). false: `message` says what to install.
        bool RequestGpuCapture(std::string& message);
        /// Before Initialize: read the start map in the background (the window shows the splash meanwhile) instead of
        /// before Initialize returns. The map is in the world once Run has drawn a few frames.
        void SetDeferredSceneLoad(bool deferred);
        /// Quits after this many frames of the main loop (0: never). For short runs under a sanitizer or a profiler.
        void SetExitAfterFrames(std::uint32_t frames);
        int Run();
        void Shutdown();

        void RequestQuit();

        LRESULT HandleWindowMessage(Window& window, UINT msg, WPARAM wparam, LPARAM lparam);

        state::StateMachine& GetStateMachine();
        ecs::World& GetWorld();
        input::InputManager& GetInputManager();
        Logger& GetLogger();
        resource::ResourceManager& GetResourceManager();
        scripting::ScriptRuntime& GetScriptRuntime();
        const std::filesystem::path& GetSceneSavePath() const;
        void SetStateLabel(const std::string& label);
        bool SaveSceneToDisk();
        bool LoadSceneFromDisk();
        /// Opens a map by its project path ("Maps/Main.json"); it becomes the file that Save writes.
        bool OpenSceneFromDisk(const std::string& scenePath);
        /// Writes the world to a new map file and makes it the current one.
        bool SaveSceneAs(const std::string& scenePath);
        /// Starts a new process for the project and quits this one. discardScene: do not save the map on exit.
        bool RestartWithProject(const std::filesystem::path& projectFile, bool discardScene);
        std::string CaptureSceneSnapshot() const;
        bool RestoreSceneSnapshot(std::string_view snapshotJson);

        /// The start map is in the world (with SetDeferredSceneLoad it takes a few frames after Run starts).
        bool IsSceneLoaded() const;
        /// The long operations that the editor shows (a window for a map, an indicator in the status bar).
        editor::LoadingService& GetLoadingService();
        /// Opens a map without freezing the window: the file is read and built on a worker thread behind a modal
        /// loading window and applied on the main thread when ready. `done(success)` runs there, once. Only one map
        /// loads at a time: a second call fails at once.
        void BeginOpenScene(const std::string& scenePath, std::function<void(bool)> done);
        /// The same for re-reading the current map from disk (Revert Map).
        void BeginRevertScene(std::function<void(bool)> done);

    private:
        struct WindowRuntime
        {
            std::unique_ptr<Window> window;
            std::wstring baseTitle;
            render::RenderSurfaceHandle surface;
            Color clearColor;
            ecs::EntityId controlledEntity = ecs::kInvalidEntity;
            bool closed = false;
        };

        // Search for a WindowRuntime (window) by ID
        WindowRuntime* FindWindow(WindowId id);
        const WindowRuntime* FindWindow(WindowId id) const;

        struct PendingSceneLoad;
        void StartSceneLoad(
            const std::filesystem::path& path,
            bool makeCurrent,
            std::string label,
            std::string subtitle,
            editor::LoadingMode mode,
            std::function<void(bool)> done);
        void PumpPendingSceneLoad();
        void WaitPendingSceneLoad();
        // Polled sources of the background indicators: thumbnails and assets still loading
        void SyncBackgroundOperations();

        bool AllWindowsClosed() const;
        void SetInputOwnerWindow(WindowId id);
        void SetCursorVisible(bool visible);
        void CaptureCursorForGame(bool capture);
        void WarpCursorToWindowCenter(const Window& window);
        // The OS capture is shared by the buttons that are held: it stays until the last one is released
        void AcquireMouseCapture(const Window& window, std::uint32_t button);
        void ReleaseMouseCapture(std::uint32_t button);
        void UpdateWindowTitles();
        void ConfigureInputBindings();
        void BindRuntimeEventListeners();
        void BuildDemoScene();
        void ResetDemoScene();
        void SpawnDemoBox(WindowId windowId);
        void SpawnDemoSphere(WindowId windowId);
        void SpawnDemoBurst(WindowId windowId);
        void PublishFrameStatistics(
            float deltaTime,
            double worldUpdateMs,
            double scriptsMs,
            double stateUpdateMs,
            double hotReloadMs,
            double uiUpdateMs,
            double renderMs,
            double frameMs);
        void TogglePhysicsPause();
        void TogglePhysicsDebugDraw();
        void AdjustGravity(float delta);
        void RebindWindowControlledEntities();
        void RenderFrame();
        void HandleDiagnosticsAfterFrame();
        void WriteFrameStats();

        HINSTANCE instance_ = nullptr;
        bool quitRequested_ = false;

        config::AppConfig config_;

        Logger logger_;
        // Declared before world_: members are destroyed in reverse order, so the world (and ScriptSystem with
        // its Python objects) always goes away before the interpreter
        scripting::ScriptRuntime scriptRuntime_;
        scene::PrefabLibrary prefabLibrary_;
        Timer timer_;
        input::InputManager input_;
        state::StateMachine stateMachine_;
        ecs::World world_;
        scripting::ScriptSystem* scriptSystem_ = nullptr; // owned by world_
        ecs::systems::RlGameSystem* rlGameSystem_ = nullptr; // owned by world_

        std::unique_ptr<render::IRenderAdapter> renderAdapter_;
        std::unique_ptr<resource::ResourceManager> resourceManager_;
        std::unique_ptr<editor::ThumbnailService> thumbnailService_; // after the adapter and the resources it uses
        editor::LoadingService loadingService_;
        std::unique_ptr<PendingSceneLoad> pendingSceneLoad_;
        ui::UiManager uiManager_;
        std::unique_ptr<faceid::FaceIdService> faceIdService_; // Face ID of the editor (docs/editor/faceid.md); null with --no-faceid and in the game mode
        std::unique_ptr<faceid::FaceIdGate> faceIdGate_;
        std::unique_ptr<rlui::Host> uiGallery_; // MYENGINE_UI_GALLERY: the gallery of the UI kit instead of the editor; after uiManager_: goes first
        std::unique_ptr<UiScript> uiScript_; // MYENGINE_UI_SCRIPT: scripted keys, mouse and screenshots for the tests of the game
        std::unique_ptr<game::DoomGameFlow> doomFlow_; // FlyDoom: the game mode with DOOM instead of Rocket League; after uiManager_: goes first
        std::unique_ptr<game::RlGameFlow> gameFlow_; // the game mode: the screens of the game (menus, HUD) instead of the editor; after uiManager_: goes first
        std::vector<WindowRuntime> windows_;
        WindowId inputOwnerWindowId_ = 0;
        std::filesystem::path sceneSavePath_;
        std::filesystem::path projectFile_; // from --project, empty: the project of the engine folder
        bool showProjectBrowser_ = false;
        bool deferredSceneLoad_ = false;
        bool gameMode_ = false;
        bool doomMode_ = false;
        DoomOptions doomOptions_;
        std::string doomTitle_;                     // the title DOOM asked for ("Freedoom: Phase 1")
        int rawMouseX_ = 0;                         // the raw movement of the mouse since DOOM took it (WM_INPUT)
        int rawMouseY_ = 0;
        bool rawMouseRegistered_ = false;
        int fpsLimit_ = 0; // frames per second the main loop is held to, 0: none
        int slowMessageLogs_ = 0; // the slow window messages that were written to the log (a few, to find what holds the loop)
        bool videoApplied_ = false;
        int lastVideoWidth_ = 0;
        int lastVideoHeight_ = 0;
        DiagnosticsOptions diagnostics_;
        ecs::systems::RlOnlineOptions rlOnline_;
        render::GpuCapture gpuCapture_;
        std::uint64_t renderedFrames_ = 0;
        std::uint64_t captureTriggeredAtFrame_ = 0;
        std::uint32_t captureCountAtTrigger_ = 0;
        bool captureTriggered_ = false;
        bool frameStatsDone_ = false;
        int exitCode_ = 0;
        std::uint32_t exitAfterFrames_ = 0;
        std::uint32_t framesRun_ = 0;
        // Another project was asked for: the window stays (with its loading window) until the new process is up
        std::chrono::steady_clock::time_point restartQuitTime_{};
        bool restartPending_ = false;
        bool discardSceneOnExit_ = false; // set when the process restarts for another project without saving
        std::string stateLabel_;
        bool titleShowsPlay_ = false;
        bool sceneLoaded_ = false;
        bool cameraControlActive_ = false;
        std::uint32_t mouseCaptureButtons_ = 0;
        bool cursorHidden_ = false;
        bool runtimeEventsBound_ = false;

        float deltaLogAccumulator_ = 0.0f;
        float fpsAccumulatorTime_ = 0.0f;
        std::uint32_t fpsAccumulatorFrames_ = 0;
        float averagedFps_ = 0.0f;
    };
}
