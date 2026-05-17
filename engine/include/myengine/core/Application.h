// Application.h

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <cstdint>
#include <vector>

#include <myengine/config/AppConfig.h>
#include <myengine/core/Logger.h>
#include <myengine/core/Timer.h>
#include <myengine/core/Types.h>
#include <myengine/core/Window.h>
#include <myengine/ecs/World.h>
#include <myengine/input/InputManager.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/resource/ResourceManager.h>
#include <myengine/state/StateMachine.h>
#include <myengine/ui/UiManager.h>

namespace myengine::core
{
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

        bool Initialize(const config::AppConfig& config);
        int Run();
        void Shutdown();

        void RequestQuit();

        LRESULT HandleWindowMessage(Window& window, UINT msg, WPARAM wparam, LPARAM lparam);

        state::StateMachine& GetStateMachine();
        ecs::World& GetWorld();
        input::InputManager& GetInputManager();
        Logger& GetLogger();
        resource::ResourceManager& GetResourceManager();
        const std::filesystem::path& GetSceneSavePath() const;
        void SetStateLabel(const std::string& label);
        bool SaveSceneToDisk();
        bool LoadSceneFromDisk();
        std::string CaptureSceneSnapshot() const;
        bool RestoreSceneSnapshot(std::string_view snapshotJson);

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

        bool AllWindowsClosed() const;
        void SetInputOwnerWindow(WindowId id);
        void SetCursorVisible(bool visible);
        void WarpCursorToWindowCenter(const Window& window);
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

        HINSTANCE instance_ = nullptr;
        bool quitRequested_ = false;

        config::AppConfig config_;

        Logger logger_;
        Timer timer_;
        input::InputManager input_;
        state::StateMachine stateMachine_;
        ecs::World world_;

        std::unique_ptr<render::IRenderAdapter> renderAdapter_;
        std::unique_ptr<resource::ResourceManager> resourceManager_;
        ui::UiManager uiManager_;
        std::vector<WindowRuntime> windows_;
        WindowId inputOwnerWindowId_ = 0;
        std::filesystem::path sceneSavePath_;
        bool cameraControlActive_ = false;
        bool cursorHidden_ = false;
        bool runtimeEventsBound_ = false;

        float deltaLogAccumulator_ = 0.0f;
        float fpsAccumulatorTime_ = 0.0f;
        std::uint32_t fpsAccumulatorFrames_ = 0;
        float averagedFps_ = 0.0f;
    };
}