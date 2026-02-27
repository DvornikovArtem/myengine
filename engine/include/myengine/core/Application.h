// Application.h

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <myengine/config/AppConfig.h>
#include <myengine/core/Logger.h>
#include <myengine/core/Timer.h>
#include <myengine/core/Types.h>
#include <myengine/core/Window.h>
#include <myengine/input/InputManager.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/scene/TestPrimitive.h>
#include <myengine/state/StateMachine.h>

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
        input::InputManager& GetInputManager();
        Logger& GetLogger();
        void SetStateLabel(const std::string& label);

    private:
        struct WindowRuntime
        {
            std::unique_ptr<Window> window;
            std::wstring baseTitle;
            render::RenderSurfaceHandle surface;
            Color clearColor;
            scene::TestPrimitive primitive;
            bool continuousZoomOut = false;
            bool closed = false;
        };

        // Search for a WindowRuntime (window) by ID
        WindowRuntime* FindWindow(WindowId id);
        const WindowRuntime* FindWindow(WindowId id) const;

        bool AllWindowsClosed() const;
        void ApplyInput(float deltaTime);
        void RenderFrame();

        HINSTANCE instance_ = nullptr;
        bool quitRequested_ = false;

        config::AppConfig config_;

        Logger logger_;
        Timer timer_;
        input::InputManager input_;
        state::StateMachine stateMachine_;

        std::unique_ptr<render::IRenderAdapter> renderAdapter_;
        std::vector<WindowRuntime> windows_;
        WindowId inputOwnerWindowId_ = 0;

        float deltaLogAccumulator_ = 0.0f;
    };
}