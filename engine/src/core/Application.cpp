// Application.cpp

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>

#include <myengine/core/Application.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/editor/EditorState.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/CameraControllerComponent.h>
#include <myengine/ecs/components/ColliderComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/MotionComponent.h>
#include <myengine/ecs/components/PlayerControllerComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/ecs/systems/CameraControlSystem.h>
#include <myengine/ecs/systems/DebugRenderSystem.h>
#include <myengine/ecs/systems/MotionSystem.h>
#include <myengine/ecs/systems/PhysicsSystem.h>
#include <myengine/ecs/systems/PlayerControlSystem.h>
#include <myengine/ecs/systems/RenderSystem.h>
#include <myengine/physics/PhysicsEvents.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>
#include <myengine/scene/SceneSerializer.h>

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

        bool TryBuildEditorRenderRegion(
            const Window& window,
            render::IntRect& outRegion,
            std::uint32_t& outWidth,
            std::uint32_t& outHeight)
        {
            const auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
            if (!editorState.showViewport)
            {
                return false;
            }

            const auto* windowState = editorState.FindWindowState(window.Id());
            if (windowState == nullptr || !windowState->viewport.IsValid())
            {
                return false;
            }

            const int windowWidth = static_cast<int>(window.Width());
            const int windowHeight = static_cast<int>(window.Height());
            const int left = std::clamp(static_cast<int>(std::floor(windowState->viewport.x)), 0, windowWidth);
            const int top = std::clamp(static_cast<int>(std::floor(windowState->viewport.y)), 0, windowHeight);
            const int right = std::clamp(static_cast<int>(std::ceil(windowState->viewport.x + windowState->viewport.width)), 0, windowWidth);
            const int bottom = std::clamp(static_cast<int>(std::ceil(windowState->viewport.y + windowState->viewport.height)), 0, windowHeight);

            if (right <= left || bottom <= top)
            {
                return false;
            }

            outRegion = {left, top, right, bottom};
            outWidth = static_cast<std::uint32_t>(right - left);
            outHeight = static_cast<std::uint32_t>(bottom - top);
            return true;
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

        resourceManager_ = std::make_unique<resource::ResourceManager>(*renderAdapter_, logger_);

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

            runtime.window->Show();
            windows_.push_back(std::move(runtime));
        }

        ConfigureInputBindings();

        ui::SceneEditorServices sceneEditorServices;
        sceneEditorServices.world = &world_;
        sceneEditorServices.resourceManager = resourceManager_.get();
        sceneEditorServices.logger = &logger_;
        sceneEditorServices.requestQuit = [this]() { RequestQuit(); };
        sceneEditorServices.saveScene = [this]() { return SaveSceneToDisk(); };
        sceneEditorServices.loadScene = [this]() { return LoadSceneFromDisk(); };
        sceneEditorServices.captureSceneSnapshot = [this]() { return CaptureSceneSnapshot(); };
        sceneEditorServices.restoreSceneSnapshot = [this](std::string_view snapshot) { return RestoreSceneSnapshot(snapshot); };
        if (!uiManager_.Initialize(*renderAdapter_, logger_, std::move(sceneEditorServices)))
        {
            logger_.Error("UI manager initialization failed");
            return false;
        }

        for (const auto& runtime : windows_)
        {
            if (runtime.window != nullptr && runtime.surface.IsValid())
            {
                uiManager_.RegisterWindow(
                    runtime.window->Id(),
                    runtime.window->Handle(),
                    runtime.surface,
                    runtime.window->Width(),
                    runtime.window->Height());
            }
        }

        world_.AddUpdateSystem(std::make_unique<ecs::systems::CameraControlSystem>(input_, inputOwnerWindowId_));
        world_.AddUpdateSystem(std::make_unique<ecs::systems::PlayerControlSystem>(input_));
        world_.AddUpdateSystem(std::make_unique<ecs::systems::MotionSystem>());
        world_.AddUpdateSystem(std::make_unique<ecs::systems::PhysicsSystem>());
        world_.AddRenderSystem(std::make_unique<ecs::systems::RenderSystem>());
        world_.AddRenderSystem(std::make_unique<ecs::systems::DebugRenderSystem>());
        sceneSavePath_ = std::filesystem::path(MYENGINE_SOURCE_DIR) / "assets/scenes/scene.json";

        BindRuntimeEventListeners();

        if (resourceManager_ != nullptr)
        {
            resourceManager_->LoadManifest("assets/manifests/demo_assets.json");
        }

        if (!scene::LoadWorldFromJson(world_, sceneSavePath_, &logger_))
        {
            BuildDemoScene();
            scene::SaveWorldToJson(world_, sceneSavePath_, &logger_);
        }
        else
        {
            RebindWindowControlledEntities();
        }
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        editorState.mode = editor::RuntimeMode::Edit;
        editorState.selectedEntity = ecs::kInvalidEntity;
        editorState.playModeSnapshot.clear();
        core::ServiceLocator::GetPhysicsWorldState().physicsPaused = true;

        timer_.Reset();
        logger_.Info("Application initialization finished");
        return true;
    }

    int Application::Run()
    {
        MSG msg{};
        struct FrameTimingStats
        {
            double worldUpdateMs = 0.0;
            double stateUpdateMs = 0.0;
            double hotReloadMs = 0.0;
            double uiUpdateMs = 0.0;
            double renderMs = 0.0;
            double totalMs = 0.0;
            std::uint32_t frames = 0;
        } frameTimingStats;

        logger_.Info("Main loop started");

        while (!quitRequested_)
        {
            input_.BeginFrame();

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
            const auto frameStartTime = std::chrono::steady_clock::now();

            const auto worldUpdateStartTime = std::chrono::steady_clock::now();
            world_.UpdateSystems(deltaTime);
            const auto worldUpdateEndTime = std::chrono::steady_clock::now();

            const auto stateUpdateStartTime = std::chrono::steady_clock::now();
            stateMachine_.Update(*this, deltaTime);
            const auto stateUpdateEndTime = std::chrono::steady_clock::now();

            const auto hotReloadStartTime = std::chrono::steady_clock::now();
            if (resourceManager_ != nullptr)
            {
                resourceManager_->UpdateHotReload();
            }
            const auto hotReloadEndTime = std::chrono::steady_clock::now();

            const auto uiUpdateStartTime = std::chrono::steady_clock::now();
            uiManager_.Update(deltaTime);
            const auto uiUpdateEndTime = std::chrono::steady_clock::now();

            const auto renderStartTime = std::chrono::steady_clock::now();
            RenderFrame();
            const auto renderEndTime = std::chrono::steady_clock::now();

            const auto millisecondsBetween =
                [](const std::chrono::steady_clock::time_point start, const std::chrono::steady_clock::time_point end)
                {
                    return std::chrono::duration<double, std::milli>(end - start).count();
                };

            frameTimingStats.worldUpdateMs += millisecondsBetween(worldUpdateStartTime, worldUpdateEndTime);
            frameTimingStats.stateUpdateMs += millisecondsBetween(stateUpdateStartTime, stateUpdateEndTime);
            frameTimingStats.hotReloadMs += millisecondsBetween(hotReloadStartTime, hotReloadEndTime);
            frameTimingStats.uiUpdateMs += millisecondsBetween(uiUpdateStartTime, uiUpdateEndTime);
            frameTimingStats.renderMs += millisecondsBetween(renderStartTime, renderEndTime);
            frameTimingStats.totalMs += millisecondsBetween(frameStartTime, renderEndTime);
            frameTimingStats.frames += 1;

            PublishFrameStatistics(
                deltaTime,
                millisecondsBetween(worldUpdateStartTime, worldUpdateEndTime),
                millisecondsBetween(stateUpdateStartTime, stateUpdateEndTime),
                millisecondsBetween(hotReloadStartTime, hotReloadEndTime),
                millisecondsBetween(uiUpdateStartTime, uiUpdateEndTime),
                millisecondsBetween(renderStartTime, renderEndTime),
                millisecondsBetween(frameStartTime, renderEndTime));

            deltaLogAccumulator_ += deltaTime;
            if (deltaLogAccumulator_ >= 1.0f && frameTimingStats.frames > 0)
            {
                const double inverseFrameCount = 1.0 / static_cast<double>(frameTimingStats.frames);
                std::ostringstream timingMessage;
                timingMessage.setf(std::ios::fixed);
                timingMessage.precision(2);
                timingMessage
                    << "FrameTimes avg_ms total=" << frameTimingStats.totalMs * inverseFrameCount
                    << " world=" << frameTimingStats.worldUpdateMs * inverseFrameCount
                    << " state=" << frameTimingStats.stateUpdateMs * inverseFrameCount
                    << " hot_reload=" << frameTimingStats.hotReloadMs * inverseFrameCount
                    << " ui=" << frameTimingStats.uiUpdateMs * inverseFrameCount
                    << " render=" << frameTimingStats.renderMs * inverseFrameCount
                    << " | deltaTime=" << deltaTime;
                logger_.Debug(timingMessage.str());

                deltaLogAccumulator_ = 0.0f;
                frameTimingStats = {};
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
        cameraControlActive_ = false;
        SetCursorVisible(true);

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.mode == editor::RuntimeMode::Play && !editorState.playModeSnapshot.empty())
        {
            RestoreSceneSnapshot(editorState.playModeSnapshot);
            editorState.mode = editor::RuntimeMode::Edit;
            editorState.playModeSnapshot.clear();
            core::ServiceLocator::GetPhysicsWorldState().physicsPaused = true;
        }

        if (!sceneSavePath_.empty() && !world_.GetEntities().empty())
        {
            scene::SaveWorldToJson(world_, sceneSavePath_, &logger_);
        }

        uiManager_.Shutdown();
        resourceManager_.reset();

        if (renderAdapter_)
        {
            logger_.Info("Shutting down render adapter");
            renderAdapter_->Shutdown();
            renderAdapter_.reset();
        }

        windows_.clear();
        world_ = ecs::World{};
    }

    void Application::RequestQuit()
    {
        quitRequested_ = true;
    }

    LRESULT Application::HandleWindowMessage(Window& window, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        InputEvent event;
        event.windowId = window.Id();

        const bool hasMouseClientPosition =
            msg == WM_MOUSEMOVE ||
            msg == WM_LBUTTONDOWN ||
            msg == WM_LBUTTONUP ||
            msg == WM_LBUTTONDBLCLK ||
            msg == WM_RBUTTONDOWN ||
            msg == WM_RBUTTONUP ||
            msg == WM_RBUTTONDBLCLK ||
            msg == WM_MBUTTONDOWN ||
            msg == WM_MBUTTONUP ||
            msg == WM_MBUTTONDBLCLK;
        const float mouseClientX = hasMouseClientPosition
            ? static_cast<float>(static_cast<short>(LOWORD(lparam)))
            : 0.0f;
        const float mouseClientY = hasMouseClientPosition
            ? static_cast<float>(static_cast<short>(HIWORD(lparam)))
            : 0.0f;
        const bool startSceneNavigationNow =
            (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) &&
            uiManager_.CanStartSceneNavigation(window.Id(), mouseClientX, mouseClientY);
        const bool bypassUiForCamera =
            startSceneNavigationNow ||
            cameraControlActive_ &&
            (msg == WM_RBUTTONDOWN ||
                msg == WM_RBUTTONUP ||
                msg == WM_RBUTTONDBLCLK ||
                msg == WM_MOUSEMOVE ||
                msg == WM_MOUSEWHEEL);

        if (!bypassUiForCamera)
        {
            uiManager_.HandleWindowMessage(window.Id(), window.Handle(), msg, wparam, lparam);
        }

        const bool uiWantsMouseCapture = !bypassUiForCamera && uiManager_.WantsMouseCapture(window.Id());
        const bool uiWantsKeyboardCapture = uiManager_.WantsKeyboardCapture(window.Id());
        const bool cameraConsumesKeyboard = cameraControlActive_ && inputOwnerWindowId_ == window.Id();
        const bool uiAllowsSceneNavigation =
            startSceneNavigationNow ||
            uiManager_.CanStartSceneNavigation(window.Id(), mouseClientX, mouseClientY);

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
                    runtime->controlledEntity = ecs::kInvalidEntity;
                }
                uiManager_.UnregisterWindow(window.Id());
                if (inputOwnerWindowId_ == window.Id())
                {
                    cameraControlActive_ = false;
                    SetCursorVisible(true);
                    SetInputOwnerWindow(0);
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

            case WM_KILLFOCUS:
            {
                if (inputOwnerWindowId_ == window.Id())
                {
                    cameraControlActive_ = false;
                    input_.OnMouseUp(MouseButton::Right);
                    SetCursorVisible(true);
                    ReleaseCapture();
                    SetInputOwnerWindow(0);
                }
                return 0;
            }

            case WM_KEYDOWN:
            {
                const auto key = static_cast<std::uint32_t>(wparam);

                if (uiWantsKeyboardCapture && !cameraConsumesKeyboard && key != VK_F3)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                input_.OnKeyDown(key);
                event.type = InputEventType::KeyDown;
                event.key = key;

                logger_.Debug("KeyDown window=" + std::to_string(event.windowId) + " key=" + std::to_string(event.key));

                const bool firstPress = (lparam & (1 << 30)) == 0;
                if (firstPress)
                {
                    if (key == VK_F3)
                    {
                        TogglePhysicsDebugDraw();
                    }
                }

                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_KEYUP:
            {
                if (uiWantsKeyboardCapture && !cameraConsumesKeyboard)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                input_.OnKeyUp(static_cast<std::uint32_t>(wparam));
                event.type = InputEventType::KeyUp;
                event.key = static_cast<std::uint32_t>(wparam);
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            {
                if (uiWantsMouseCapture)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                input_.OnMouseDown(MouseButton::Left);
                event.type = InputEventType::MouseDown;
                event.mouseButton = MouseButton::Left;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_LBUTTONUP:
            {
                if (uiWantsMouseCapture)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                input_.OnMouseUp(MouseButton::Left);
                event.type = InputEventType::MouseUp;
                event.mouseButton = MouseButton::Left;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_RBUTTONDOWN:
            {
                if (!uiAllowsSceneNavigation)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                SetInputOwnerWindow(window.Id());
                cameraControlActive_ = true;
                SetCapture(window.Handle());
                input_.OnMouseDown(MouseButton::Right);
                SetCursorVisible(false);
                WarpCursorToWindowCenter(window);
                event.type = InputEventType::MouseDown;
                event.mouseButton = MouseButton::Right;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_RBUTTONUP:
            {
                if (!cameraControlActive_)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                cameraControlActive_ = false;
                ReleaseCapture();
                input_.OnMouseUp(MouseButton::Right);
                SetCursorVisible(true);
                SetInputOwnerWindow(0);
                event.type = InputEventType::MouseUp;
                event.mouseButton = MouseButton::Right;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_MOUSEWHEEL:
            {
                if (uiWantsMouseCapture)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                if (cameraControlActive_ && inputOwnerWindowId_ == window.Id())
                {
                    const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wparam);
                    input_.OnMouseWheel(wheelDelta);
                }
                return 0;
            }

            case WM_MOUSEMOVE:
            {
                if (uiWantsMouseCapture)
                {
                    return 0;
                }

                if (cameraControlActive_ && inputOwnerWindowId_ == window.Id() && input_.IsMouseDown(MouseButton::Right))
                {
                    const int mouseX = static_cast<int>(static_cast<short>(LOWORD(lparam)));
                    const int mouseY = static_cast<int>(static_cast<short>(HIWORD(lparam)));
                    const int centerX = static_cast<int>(window.Width() / 2u);
                    const int centerY = static_cast<int>(window.Height() / 2u);
                    const int deltaX = mouseX - centerX;
                    const int deltaY = mouseY - centerY;

                    if (deltaX != 0 || deltaY != 0)
                    {
                        input_.AddMouseDelta(deltaX, deltaY);
                        WarpCursorToWindowCenter(window);
                    }
                }
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

    ecs::World& Application::GetWorld()
    {
        return world_;
    }

    input::InputManager& Application::GetInputManager()
    {
        return input_;
    }

    Logger& Application::GetLogger()
    {
        return logger_;
    }

    resource::ResourceManager& Application::GetResourceManager()
    {
        return *resourceManager_;
    }

    const std::filesystem::path& Application::GetSceneSavePath() const
    {
        return sceneSavePath_;
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

        uiManager_.SetStateLabel(label);
    }

    bool Application::SaveSceneToDisk()
    {
        if (sceneSavePath_.empty())
        {
            return false;
        }

        return scene::SaveWorldToJson(world_, sceneSavePath_, &logger_);
    }

    bool Application::LoadSceneFromDisk()
    {
        if (sceneSavePath_.empty())
        {
            return false;
        }

        if (!scene::LoadWorldFromJson(world_, sceneSavePath_, &logger_))
        {
            return false;
        }

        RebindWindowControlledEntities();
        return true;
    }

    std::string Application::CaptureSceneSnapshot() const
    {
        return scene::SerializeWorldToString(world_);
    }

    bool Application::RestoreSceneSnapshot(const std::string_view snapshotJson)
    {
        if (!scene::LoadWorldFromString(world_, snapshotJson, &logger_))
        {
            return false;
        }

        RebindWindowControlledEntities();
        return true;
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

    void Application::SetInputOwnerWindow(const WindowId id)
    {
        if (inputOwnerWindowId_ != id)
        {
            inputOwnerWindowId_ = id;
            input_.ResetMouseTracking();
        }
    }

    void Application::SetCursorVisible(const bool visible)
    {
        if (visible)
        {
            if (!cursorHidden_)
            {
                return;
            }

            while (ShowCursor(TRUE) < 0)
            {
            }
            cursorHidden_ = false;
            return;
        }

        if (cursorHidden_)
        {
            return;
        }

        while (ShowCursor(FALSE) >= 0)
        {
        }
        cursorHidden_ = true;
    }

    void Application::WarpCursorToWindowCenter(const Window& window)
    {
        const int centerX = static_cast<int>(window.Width() / 2u);
        const int centerY = static_cast<int>(window.Height() / 2u);

        POINT center{
            static_cast<LONG>(centerX),
            static_cast<LONG>(centerY)};
        ClientToScreen(window.Handle(), &center);
        SetCursorPos(center.x, center.y);
        input_.SetMousePositionReference(centerX, centerY);
    }

    void Application::ConfigureInputBindings()
    {
        input_.BindAction("camera_forward", 'W');
        input_.BindAction("camera_forward", VK_UP);
        input_.BindAction("camera_backward", 'S');
        input_.BindAction("camera_backward", VK_DOWN);
        input_.BindAction("camera_left", 'A');
        input_.BindAction("camera_left", VK_LEFT);
        input_.BindAction("camera_right", 'D');
        input_.BindAction("camera_right", VK_RIGHT);
        input_.BindAction("camera_up", VK_SPACE);
        input_.BindAction("camera_down", VK_SHIFT);
        input_.BindAction("camera_down", VK_LSHIFT);

        input_.BindAction("player_forward", 'W');
        input_.BindAction("player_forward", VK_UP);
        input_.BindAction("player_backward", 'S');
        input_.BindAction("player_backward", VK_DOWN);
        input_.BindAction("player_left", 'A');
        input_.BindAction("player_left", VK_LEFT);
        input_.BindAction("player_right", 'D');
        input_.BindAction("player_right", VK_RIGHT);
        input_.BindAction("player_jump", VK_SPACE);
    }

    void Application::BindRuntimeEventListeners()
    {
        if (runtimeEventsBound_)
        {
            return;
        }

        runtimeEventsBound_ = true;

        core::ServiceLocator::GetEventBus().Subscribe<physics::CollisionEvent>(
            [this](const physics::CollisionEvent& event)
            {
                std::ostringstream message;
                message.setf(std::ios::fixed);
                message.precision(2);
                message << "Collision " << event.entityA << " vs " << event.entityB
                        << " | impulse=" << event.impulse
                        << " | point=(" << event.point.x << ", " << event.point.y << ", " << event.point.z << ")";

                core::ServiceLocator::GetPhysicsWorldState().PushRecentEvent(message.str());
                logger_.Info(message.str());
            });

        core::ServiceLocator::GetEventBus().Subscribe<physics::TriggerEvent>(
            [this](const physics::TriggerEvent& event)
            {
                std::ostringstream message;
                message.setf(std::ios::fixed);
                message.precision(2);
                message << "Trigger " << event.triggerEntity << " -> " << event.otherEntity
                        << " | point=(" << event.point.x << ", " << event.point.y << ", " << event.point.z << ")";

                core::ServiceLocator::GetPhysicsWorldState().PushRecentEvent(message.str());
                logger_.Info(message.str());
            });
    }

    void Application::ResetDemoScene()
    {
        world_.ClearEntities();
        core::ServiceLocator::GetPhysicsWorldState().recentEvents.clear();
        BuildDemoScene();
    }

    void Application::SpawnDemoBox(const WindowId windowId)
    {
        const auto* runtime = FindWindow(windowId);
        if (runtime == nullptr)
        {
            return;
        }

        static constexpr char kCubeMesh[] = "assets/models/crate.obj";
        static constexpr char kWarmMaterial[] = "assets/materials/warm.material.json";

        const std::size_t bodyIndex = world_.GetEntities().size() + 1u;
        const float laneOffset = static_cast<float>(bodyIndex % 5u) * 0.7f - 1.4f;

        const ecs::EntityId entity = world_.CreateEntity();
        world_.Emplace<ecs::components::TagComponent>(entity).name = "SpawnedBox_" + std::to_string(entity);
        auto& transform = world_.Emplace<ecs::components::TransformComponent>(entity);
        transform.position = {laneOffset, 4.8f + static_cast<float>(bodyIndex % 3u) * 0.55f, 3.8f};
        transform.scale = {0.85f, 0.85f, 0.85f};
        auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(entity);
        renderer.meshPath = kCubeMesh;
        renderer.materialPath = kWarmMaterial;
        renderer.visible = true;
        world_.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = runtime->window->Id();

        auto& rigidbody = world_.Emplace<ecs::components::RigidbodyComponent>(entity);
        rigidbody.mass = 1.0f;
        rigidbody.useGravity = true;

        auto& collider = world_.Emplace<ecs::components::ColliderComponent>(entity);
        collider.type = ecs::components::ColliderType::Box;
        collider.halfExtents = {0.5f, 0.5f, 0.5f};
        collider.friction = 0.55f;
        collider.bounciness = 0.12f;
    }

    void Application::SpawnDemoSphere(const WindowId windowId)
    {
        const auto* runtime = FindWindow(windowId);
        if (runtime == nullptr)
        {
            return;
        }

        static constexpr char kSphereMesh[] = "assets/models/sphere.obj";
        static constexpr char kCoolMaterial[] = "assets/materials/cool.material.json";

        const std::size_t bodyIndex = world_.GetEntities().size() + 1u;
        const float laneOffset = static_cast<float>(bodyIndex % 5u) * 0.65f - 1.3f;

        const ecs::EntityId entity = world_.CreateEntity();
        world_.Emplace<ecs::components::TagComponent>(entity).name = "SpawnedSphere_" + std::to_string(entity);
        auto& transform = world_.Emplace<ecs::components::TransformComponent>(entity);
        transform.position = {laneOffset, 5.2f + static_cast<float>(bodyIndex % 2u) * 0.6f, 2.6f};
        transform.scale = {0.78f, 0.78f, 0.78f};
        auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(entity);
        renderer.meshPath = kSphereMesh;
        renderer.materialPath = kCoolMaterial;
        renderer.visible = true;
        world_.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = runtime->window->Id();

        auto& rigidbody = world_.Emplace<ecs::components::RigidbodyComponent>(entity);
        rigidbody.mass = 0.65f;
        rigidbody.useGravity = true;

        auto& collider = world_.Emplace<ecs::components::ColliderComponent>(entity);
        collider.type = ecs::components::ColliderType::Sphere;
        collider.radius = 0.5f;
        collider.friction = 0.28f;
        collider.bounciness = 0.18f;
    }

    void Application::SpawnDemoBurst(const WindowId windowId)
    {
        for (int index = 0; index < 3; ++index)
        {
            SpawnDemoBox(windowId);
            SpawnDemoSphere(windowId);
        }
    }

    void Application::TogglePhysicsPause()
    {
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        physicsState.physicsPaused = !physicsState.physicsPaused;
    }

    void Application::TogglePhysicsDebugDraw()
    {
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        physicsState.debugDrawEnabled = !physicsState.debugDrawEnabled;
    }

    void Application::AdjustGravity(const float delta)
    {
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        physicsState.gravityStrength = std::clamp(physicsState.gravityStrength + delta, 0.0f, 30.0f);
    }

    void Application::PublishFrameStatistics(
        const float deltaTime,
        const double worldUpdateMs,
        const double stateUpdateMs,
        const double hotReloadMs,
        const double uiUpdateMs,
        const double renderMs,
        const double frameMs)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        fpsAccumulatorTime_ += deltaTime;
        fpsAccumulatorFrames_ += 1;
        if (fpsAccumulatorTime_ >= 1.0f)
        {
            averagedFps_ = fpsAccumulatorFrames_ / std::max(fpsAccumulatorTime_, 0.0001f);
            fpsAccumulatorTime_ = 0.0f;
            fpsAccumulatorFrames_ = 0;
        }

        const float averageFps = averagedFps_ > 0.0f
            ? averagedFps_
            : (frameMs > 0.0001 ? static_cast<float>(1000.0 / frameMs) : 0.0f);

        for (auto& runtime : windows_)
        {
            if (runtime.window == nullptr)
            {
                continue;
            }

            auto& windowState = editorState.GetOrCreateWindowState(runtime.window->Id());
            windowState.timings.deltaTime = deltaTime;
            windowState.timings.averageFps = averageFps;
            windowState.timings.frameMs = frameMs;
            windowState.timings.worldUpdateMs = worldUpdateMs;
            windowState.timings.stateUpdateMs = stateUpdateMs;
            windowState.timings.hotReloadMs = hotReloadMs;
            windowState.timings.uiUpdateMs = uiUpdateMs;
            windowState.timings.renderMs = renderMs;
        }
    }

    void Application::BuildDemoScene()
    {
        static constexpr char kCubeMesh[] = "assets/models/crate.obj";
        static constexpr char kSphereMesh[] = "assets/models/sphere.obj";
        static constexpr char kDefaultMaterial[] = "assets/materials/default.material.json";
        static constexpr char kWarmMaterial[] = "assets/materials/warm.material.json";
        static constexpr char kCoolMaterial[] = "assets/materials/cool.material.json";

        if (resourceManager_ != nullptr)
        {
            resourceManager_->Load<resource::MeshAsset>(kCubeMesh);
            resourceManager_->Load<resource::MeshAsset>(kSphereMesh);
            resourceManager_->Load<resource::MaterialAsset>(kDefaultMaterial);
            resourceManager_->Load<resource::MaterialAsset>(kWarmMaterial);
            resourceManager_->Load<resource::MaterialAsset>(kCoolMaterial);
        }

        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        physicsState.physicsPaused = false;
        physicsState.debugDrawEnabled = false;
        physicsState.gravityStrength = 9.81f;
        physicsState.recentEvents.clear();

        for (std::size_t i = 0; i < windows_.size(); ++i)
        {
            auto& runtime = windows_[i];
            if (runtime.closed || runtime.window == nullptr)
            {
                continue;
            }

            const WindowId windowId = runtime.window->Id();

            const ecs::EntityId cameraEntity = world_.CreateEntity();
            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(cameraEntity);
                tag.name = "Camera_" + std::to_string(windowId);

                auto& camera = world_.Emplace<ecs::components::CameraComponent>(cameraEntity);
                camera.position = {0.0f, 3.0f, -10.5f};
                camera.rotationDeg = {8.0f, 0.0f, 0.0f};
                camera.fovYDeg = 60.0f;
                camera.orthographicHalfHeight = 1.0f;
                camera.nearPlane = 0.01f;
                camera.farPlane = 200.0f;
                camera.isPrimary = true;

                auto& controller = world_.Emplace<ecs::components::CameraControllerComponent>(cameraEntity);
                controller.moveSpeed = std::max(0.1f, config_.input.moveSpeed);
                controller.rotateSpeedDeg = std::max(10.0f, config_.input.rotateSpeedDeg * 0.7f);
                controller.zoomSpeed = std::max(0.1f, config_.input.scaleSpeed);
                controller.mouseSensitivityDeg = std::clamp(config_.input.rotateSpeedDeg * 0.0015f, 0.03f, 0.7f);

                world_.Emplace<ecs::components::WindowBindingComponent>(cameraEntity).windowId = windowId;
            }

            const ecs::EntityId controlledEntity = world_.CreateEntity();
            runtime.controlledEntity = controlledEntity;

            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(controlledEntity);
                tag.name = "Controlled_" + std::to_string(windowId);

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(controlledEntity);
                transform.position = {-2.2f, 0.2f, 2.0f};
                transform.scale = {0.95f, 0.95f, 0.95f};

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(controlledEntity);
                renderer.meshPath = kCubeMesh;
                renderer.materialPath = kDefaultMaterial;

                auto& rigidbody = world_.Emplace<ecs::components::RigidbodyComponent>(controlledEntity);
                rigidbody.mass = 1.0f;
                rigidbody.useGravity = true;

                auto& collider = world_.Emplace<ecs::components::ColliderComponent>(controlledEntity);
                collider.type = ecs::components::ColliderType::Box;
                collider.halfExtents = {0.5f, 0.5f, 0.5f};
                collider.friction = 0.75f;
                collider.bounciness = 0.08f;

                auto& controller = world_.Emplace<ecs::components::PlayerControllerComponent>(controlledEntity);
                controller.windowId = windowId;
                controller.moveSpeed = 5.3f;
                controller.jumpSpeed = 6.7f;
                controller.airControl = 0.42f;

                world_.Emplace<ecs::components::WindowBindingComponent>(controlledEntity).windowId = windowId;
            }

            const auto createBoxBody =
                [&](const std::string& tagName,
                    const ecs::components::Vec3& position,
                    const ecs::components::Vec3& scale,
                    const char* materialPath,
                    const float mass,
                    const bool isTrigger = false)
            {
                const ecs::EntityId entity = world_.CreateEntity();
                world_.Emplace<ecs::components::TagComponent>(entity).name = tagName;

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(entity);
                transform.position = position;
                transform.scale = scale;

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(entity);
                renderer.meshPath = kCubeMesh;
                renderer.materialPath = materialPath;

                auto& collider = world_.Emplace<ecs::components::ColliderComponent>(entity);
                collider.type = ecs::components::ColliderType::Box;
                collider.halfExtents = {0.5f, 0.5f, 0.5f};
                collider.isTrigger = isTrigger;
                collider.friction = mass > 0.0f ? 0.58f : 0.85f;
                collider.bounciness = mass > 0.0f ? 0.12f : 0.02f;

                if (mass > 0.0f)
                {
                    auto& rigidbody = world_.Emplace<ecs::components::RigidbodyComponent>(entity);
                    rigidbody.mass = mass;
                    rigidbody.useGravity = true;
                }

                world_.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = windowId;
                return entity;
            };

            const auto createSphereBody =
                [&](const std::string& tagName,
                    const ecs::components::Vec3& position,
                    const ecs::components::Vec3& scale,
                    const char* materialPath,
                    const float mass)
            {
                const ecs::EntityId entity = world_.CreateEntity();
                world_.Emplace<ecs::components::TagComponent>(entity).name = tagName;

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(entity);
                transform.position = position;
                transform.scale = scale;

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(entity);
                renderer.meshPath = kSphereMesh;
                renderer.materialPath = materialPath;

                //auto& motion = world_.Emplace<ecs::components::MotionComponent>(africanHeadEntity);
                //motion.angularVelocityDeg.y = 10.0f;
                //motion.angularVelocityDeg.z = 10.0f;

                auto& rigidbody = world_.Emplace<ecs::components::RigidbodyComponent>(entity);
                rigidbody.mass = mass;
                rigidbody.useGravity = true;

                auto& collider = world_.Emplace<ecs::components::ColliderComponent>(entity);
                collider.type = ecs::components::ColliderType::Sphere;
                collider.radius = 0.5f;
                collider.friction = 0.22f;
                collider.bounciness = 0.18f;

                world_.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = windowId;
                return entity;
            };

            createBoxBody("Floor_" + std::to_string(windowId), {0.0f, -1.55f, 3.0f}, {8.5f, 1.0f, 8.5f}, kCoolMaterial, 0.0f);
            createBoxBody("Platform_" + std::to_string(windowId), {2.6f, 0.0f, 3.8f}, {2.6f, 0.55f, 2.0f}, kCoolMaterial, 0.0f);
            createBoxBody("WallLeft_" + std::to_string(windowId), {-5.0f, 0.1f, 3.0f}, {0.5f, 2.2f, 6.6f}, kCoolMaterial, 0.0f);
            createBoxBody("WallRight_" + std::to_string(windowId), {5.0f, 0.1f, 3.0f}, {0.5f, 2.2f, 6.6f}, kCoolMaterial, 0.0f);
            createBoxBody("TriggerGate_" + std::to_string(windowId), {0.0f, 0.15f, 5.8f}, {2.2f, 1.6f, 0.55f}, kWarmMaterial, 0.0f, true);

            createBoxBody("BoxStackA_" + std::to_string(windowId), {-0.2f, -0.45f, 3.4f}, {0.9f, 0.9f, 0.9f}, kWarmMaterial, 1.0f);
            createBoxBody("BoxStackB_" + std::to_string(windowId), {-0.2f, 0.55f, 3.4f}, {0.9f, 0.9f, 0.9f}, kWarmMaterial, 1.0f);
            createBoxBody("BoxStackC_" + std::to_string(windowId), {-0.2f, 1.55f, 3.4f}, {0.9f, 0.9f, 0.9f}, kWarmMaterial, 1.0f);

            createSphereBody("SphereA_" + std::to_string(windowId), {1.8f, 2.6f, 2.6f}, {0.85f, 0.85f, 0.85f}, kDefaultMaterial, 0.7f);
            createSphereBody("SphereB_" + std::to_string(windowId), {2.6f, 4.1f, 2.8f}, {0.72f, 0.72f, 0.72f}, kDefaultMaterial, 0.55f);

            logger_.Info("Demo scene created for window id=" + std::to_string(windowId));
        }
    }

    void Application::RebindWindowControlledEntities()
    {
        for (auto& runtime : windows_)
        {
            runtime.controlledEntity = ecs::kInvalidEntity;
        }

        for (auto& runtime : windows_)
        {
            if (runtime.closed || runtime.window == nullptr)
            {
                continue;
            }

            const WindowId windowId = runtime.window->Id();
            const std::string expectedTag = "Controlled_" + std::to_string(windowId);

            world_.ForEach<ecs::components::TagComponent, ecs::components::TransformComponent, ecs::components::WindowBindingComponent>(
                [&](const ecs::EntityId entity,
                    ecs::components::TagComponent& tag,
                    ecs::components::TransformComponent&,
                    ecs::components::WindowBindingComponent& binding)
                {
                    if (runtime.controlledEntity != ecs::kInvalidEntity || binding.windowId != windowId)
                    {
                        return;
                    }

                    if (tag.name == expectedTag)
                    {
                        runtime.controlledEntity = entity;
                    }
                });
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

            render::IntRect renderRegion{};
            std::uint32_t renderWidth = runtime.window->Width();
            std::uint32_t renderHeight = runtime.window->Height();
            const bool hasEditorRenderRegion =
                runtime.window != nullptr &&
                TryBuildEditorRenderRegion(*runtime.window, renderRegion, renderWidth, renderHeight);
            renderAdapter_->SetRenderRegion(runtime.surface, hasEditorRenderRegion ? &renderRegion : nullptr);

            ecs::RenderFrameContext context{
                *renderAdapter_,
                runtime.surface,
                runtime.clearColor,
                runtime.window->Id(),
                renderWidth,
                renderHeight,
                &logger_,
                resourceManager_.get(),
            };

            world_.RenderSystems(context);
            renderAdapter_->SetRenderRegion(runtime.surface, nullptr);
            uiManager_.RenderWindow(runtime.window->Id());
            renderAdapter_->EndFrame(runtime.surface);
        }
    }
}