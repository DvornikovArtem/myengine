// Application.cpp

#include <algorithm>
#include <filesystem>
#include <string>

#include <myengine/core/Application.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>

namespace myengine::core
{
    namespace
    {
        std::wstring Utf8ToWide(const std::string& text)
        {
            if (text.empty())
            {
                return std::wstring();
            }

            const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            if (sizeNeeded <= 0)
            {
                return std::wstring();
            }

            std::wstring result(sizeNeeded - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), sizeNeeded);
            return result;
        }

        std::filesystem::path GetExecutableDirectory()
        {
            wchar_t modulePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            return std::filesystem::path(modulePath).parent_path();
        }
    }

    Application::Application(HINSTANCE instance) : instance_(instance) {}

    Application::~Application()
    {
        Shutdown();
    }

    bool Application::Initialize(const config::AppConfig& config)
    {
        config_ = config;

        const auto logPath = GetExecutableDirectory() / "logs/myengine.log";
        if (!logger_.Initialize(logPath))
        {
            return false;
        }

        logger_.Info("Application initialization started");

        if (config_.windows.empty())
        {
            config_ = config::AppConfig::Default();
        }

        logger_.Info("Configured windows count: " + std::to_string(config_.windows.size()));

        renderAdapter_ = std::make_unique<render::dx12::Dx12RenderAdapter>(logger_);
        if (!renderAdapter_->Initialize())
        {
            logger_.Error("Render adapter initialization failed");
            return false;
        }

        WindowId nextWindowId = 1;
        windows_.reserve(config_.windows.size());

        for (const auto& windowConfig : config_.windows)
        {
            WindowDesc desc;
            desc.id = nextWindowId++;
            desc.title = Utf8ToWide(windowConfig.title);
            desc.width = windowConfig.width;
            desc.height = windowConfig.height;

            auto window = std::make_unique<Window>(desc);
            if (!window->Create(this))
            {
                logger_.Error("Failed to create a window");
                return false;
            }

            auto surface = renderAdapter_->CreateSurface(window->Handle(), windowConfig.width, windowConfig.height);
            if (!surface.IsValid())
            {
                logger_.Error("Failed to create DX12 surface for a window");
                return false;
            }

            WindowRuntime runtime;
            runtime.window = std::move(window);
            runtime.baseTitle = desc.title;
            runtime.surface = surface;
            runtime.clearColor = windowConfig.clearColor;
            runtime.primitive = scene::TestPrimitive{};

            runtime.window->Show();
            windows_.push_back(std::move(runtime));
        }

        timer_.Reset();
        logger_.Info("Application initialization finished");
        return true;
    }

    int Application::Run()
    {
        MSG msg{};

        logger_.Info("Main loop started");

        while (!quitRequested_)
        {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);

                if (msg.message == WM_QUIT)
                {
                    quitRequested_ = true;
                }
            }

            if (quitRequested_)
            {
                break;
            }

            timer_.Tick();
            const float deltaTime = timer_.DeltaTime();

            ApplyInput(deltaTime);
            stateMachine_.Update(*this, deltaTime);
            RenderFrame();

            deltaLogAccumulator_ += deltaTime;
            if (deltaLogAccumulator_ >= 1.0f)
            {
                logger_.Debug("deltaTime=" + std::to_string(deltaTime));
                deltaLogAccumulator_ = 0.0f;
            }

            if (AllWindowsClosed())
            {
                RequestQuit();
                PostQuitMessage(0);
            }
        }

        logger_.Info("Main loop finished");
        return static_cast<int>(msg.wParam);
    }

    void Application::Shutdown()
    {
        if (renderAdapter_)
        {
            logger_.Info("Shutting down render adapter");
            renderAdapter_->Shutdown();
            renderAdapter_.reset();
        }

        windows_.clear();
    }

    void Application::RequestQuit()
    {
        quitRequested_ = true;
    }

    LRESULT Application::HandleWindowMessage(Window& window, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        InputEvent event;
        event.windowId = window.Id();

        switch (msg)
        {
            case WM_CLOSE:
            {
                DestroyWindow(window.Handle());
                return 0;
            }

            case WM_DESTROY:
            {
                if (auto* runtime = FindWindow(window.Id()); runtime != nullptr)
                {
                    runtime->closed = true;
                }
                if (inputOwnerWindowId_ == window.Id())
                {
                    inputOwnerWindowId_ = 0;
                }

                event.type = InputEventType::WindowClose;
                stateMachine_.HandleEvent(*this, event);

                if (AllWindowsClosed())
                {
                    RequestQuit();
                    PostQuitMessage(0);
                }

                return 0;
            }

            case WM_SIZE:
            {
                const auto width = static_cast<std::uint32_t>(LOWORD(lparam));
                const auto height = static_cast<std::uint32_t>(HIWORD(lparam));

                window.SetClientSize(width, height);

                if (width > 0 && height > 0)
                {
                    if (auto* runtime = FindWindow(window.Id()); runtime != nullptr && runtime->surface.IsValid() && renderAdapter_)
                    {
                        renderAdapter_->ResizeSurface(runtime->surface, width, height);
                    }
                }

                event.type = InputEventType::WindowResize;
                event.width = width;
                event.height = height;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_KEYDOWN:
            {
                inputOwnerWindowId_ = window.Id();
                input_.OnKeyDown(static_cast<std::uint32_t>(wparam));
                event.type = InputEventType::KeyDown;
                event.key = static_cast<std::uint32_t>(wparam);
                logger_.Debug("KeyDown window=" + std::to_string(event.windowId) + " key=" + std::to_string(event.key));
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_KEYUP:
            {
                inputOwnerWindowId_ = window.Id();
                input_.OnKeyUp(static_cast<std::uint32_t>(wparam));
                event.type = InputEventType::KeyUp;
                event.key = static_cast<std::uint32_t>(wparam);
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_LBUTTONDOWN:
            {
                inputOwnerWindowId_ = window.Id();
                SetCapture(window.Handle());
                input_.OnMouseDown(MouseButton::Left);
                if (auto* runtime = FindWindow(window.Id()); runtime != nullptr)
                {
                    runtime->continuousZoomOut = false;
                }
                event.type = InputEventType::MouseDown;
                event.mouseButton = MouseButton::Left;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_LBUTTONDBLCLK:
            {
                inputOwnerWindowId_ = window.Id();
                if (auto* runtime = FindWindow(window.Id()); runtime != nullptr)
                {
                    runtime->continuousZoomOut = true;
                }
                SetCapture(window.Handle());
                input_.OnMouseDown(MouseButton::Left);
                logger_.Info("Double click LMB on window=" + std::to_string(window.Id()) + " -> hold to zoom out");
                return 0;
            }

            case WM_LBUTTONUP:
            {
                inputOwnerWindowId_ = window.Id();
                ReleaseCapture();
                input_.OnMouseUp(MouseButton::Left);
                if (auto* runtime = FindWindow(window.Id()); runtime != nullptr)
                {
                    runtime->continuousZoomOut = false;
                }
                event.type = InputEventType::MouseUp;
                event.mouseButton = MouseButton::Left;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_RBUTTONDOWN:
            {
                inputOwnerWindowId_ = window.Id();
                SetCapture(window.Handle());
                input_.OnMouseDown(MouseButton::Right);
                event.type = InputEventType::MouseDown;
                event.mouseButton = MouseButton::Right;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_RBUTTONUP:
            {
                inputOwnerWindowId_ = window.Id();
                ReleaseCapture();
                input_.OnMouseUp(MouseButton::Right);
                event.type = InputEventType::MouseUp;
                event.mouseButton = MouseButton::Right;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            default:
            {
                return DefWindowProcW(window.Handle(), msg, wparam, lparam);
            }
        }
    }

    state::StateMachine& Application::GetStateMachine()
    {
        return stateMachine_;
    }

    input::InputManager& Application::GetInputManager()
    {
        return input_;
    }

    Logger& Application::GetLogger()
    {
        return logger_;
    }

    void Application::SetStateLabel(const std::string& label)
    {
        const std::wstring suffix = L" [" + Utf8ToWide(label) + L"]";
        for (auto& runtime : windows_)
        {
            if (!runtime.closed && runtime.window)
            {
                runtime.window->SetTitle(runtime.baseTitle + suffix);
            }
        }
    }

    Application::WindowRuntime* Application::FindWindow(const WindowId id)
    {
        for (auto& runtime : windows_)
        {
            if (runtime.window->Id() == id)
            {
                return &runtime;
            }
        }
        return nullptr;
    }

    const Application::WindowRuntime* Application::FindWindow(const WindowId id) const
    {
        for (const auto& runtime : windows_)
        {
            if (runtime.window->Id() == id)
            {
                return &runtime;
            }
        }
        return nullptr;
    }

    bool Application::AllWindowsClosed() const
    {
        if (windows_.empty())
        {
            return true;
        }

        for (const auto& runtime : windows_)
        {
            if (!runtime.closed)
            {
                return false;
            }
        }
        return true;
    }

    void Application::ApplyInput(const float deltaTime)
    {
        for (auto& runtime : windows_)
        {
            if (runtime.closed)
            {
                continue;
            }

            if (runtime.window->Id() != inputOwnerWindowId_)
            {
                continue;
            }

            auto& primitive = runtime.primitive;

            if (input_.IsKeyDown(VK_LEFT))
            {
                primitive.positionX -= config_.input.moveSpeed * deltaTime;
            }
            if (input_.IsKeyDown(VK_RIGHT))
            {
                primitive.positionX += config_.input.moveSpeed * deltaTime;
            }
            if (input_.IsKeyDown(VK_UP))
            {
                primitive.positionY += config_.input.moveSpeed * deltaTime;
            }
            if (input_.IsKeyDown(VK_DOWN))
            {
                primitive.positionY -= config_.input.moveSpeed * deltaTime;
            }

            if (input_.IsMouseDown(MouseButton::Left))
            {
                if (runtime.continuousZoomOut)
                {
                    primitive.scale -= config_.input.scaleSpeed * deltaTime;
                    if (primitive.scale <= 1.0f)
                    {
                        primitive.scale = 1.0f;
                    }
                }
                else
                {
                    primitive.scale += config_.input.scaleSpeed * deltaTime;
                }
            }
            if (input_.IsMouseDown(MouseButton::Right))
            {
                primitive.rotationDeg += config_.input.rotateSpeedDeg * deltaTime;
            }

            primitive.scale = std::clamp(primitive.scale, 0.2f, 3.0f);
            primitive.positionX = std::clamp(primitive.positionX, -0.9f, 0.9f);
            primitive.positionY = std::clamp(primitive.positionY, -0.9f, 0.9f);
        }
    }

    void Application::RenderFrame()
    {
        stateMachine_.Render(*this);

        for (auto& runtime : windows_)
        {
            if (runtime.closed || !runtime.surface.IsValid())
            {
                continue;
            }

            if (!renderAdapter_->BeginFrame(runtime.surface, runtime.clearColor))
            {
                continue;
            }

            render::Transform2D transform;
            transform.positionX = runtime.primitive.positionX;
            transform.positionY = runtime.primitive.positionY;
            transform.scale = runtime.primitive.scale;
            transform.rotationDeg = runtime.primitive.rotationDeg;

            renderAdapter_->DrawPrimitive(runtime.surface, transform);
            renderAdapter_->EndFrame(runtime.surface);
        }
    }
}