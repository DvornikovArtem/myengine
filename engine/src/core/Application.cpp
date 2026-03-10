// Application.cpp

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>

#include <myengine/core/Application.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/CameraControllerComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/MotionComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/ecs/systems/CameraControlSystem.h>
#include <myengine/ecs/systems/MotionSystem.h>
#include <myengine/ecs/systems/RenderSystem.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>
#include <myengine/scene/SceneSerializer.h>

namespace myengine::core
{
    namespace
    {
        constexpr float kControlledTriangleBehindCubeZ = 0.45f;

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

            runtime.window->Show();
            windows_.push_back(std::move(runtime));
        }

        world_.AddUpdateSystem(std::make_unique<ecs::systems::CameraControlSystem>(input_, inputOwnerWindowId_));
        world_.AddUpdateSystem(std::make_unique<ecs::systems::MotionSystem>());
        world_.AddRenderSystem(std::make_unique<ecs::systems::RenderSystem>());
        sceneSavePath_ = std::filesystem::path(MYENGINE_SOURCE_DIR) / "assets/scenes/scene";

        if (!scene::LoadWorldFromJson(world_, sceneSavePath_, &logger_))
        {
            BuildDemoScene();
            scene::SaveWorldToJson(world_, sceneSavePath_, &logger_);
        }
        else
        {
            RebindWindowControlledEntities();
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

            world_.UpdateSystems(deltaTime);
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
        cameraControlActive_ = false;
        SetCursorVisible(true);

        if (!sceneSavePath_.empty() && !world_.GetEntities().empty())
        {
            scene::SaveWorldToJson(world_, sceneSavePath_, &logger_);
        }

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
                input_.OnKeyDown(static_cast<std::uint32_t>(wparam));
                event.type = InputEventType::KeyDown;
                event.key = static_cast<std::uint32_t>(wparam);
                logger_.Debug("KeyDown window=" + std::to_string(event.windowId) + " key=" + std::to_string(event.key));
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_KEYUP:
            {
                input_.OnKeyUp(static_cast<std::uint32_t>(wparam));
                event.type = InputEventType::KeyUp;
                event.key = static_cast<std::uint32_t>(wparam);
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            {
                input_.OnMouseDown(MouseButton::Left);
                event.type = InputEventType::MouseDown;
                event.mouseButton = MouseButton::Left;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_LBUTTONUP:
            {
                input_.OnMouseUp(MouseButton::Left);
                event.type = InputEventType::MouseUp;
                event.mouseButton = MouseButton::Left;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_RBUTTONDOWN:
            {
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
                if (cameraControlActive_ && inputOwnerWindowId_ == window.Id())
                {
                    const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wparam);
                    input_.OnMouseWheel(wheelDelta);
                }
                return 0;
            }

            case WM_MOUSEMOVE:
            {
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

    void Application::BuildDemoScene()
    {
        static constexpr std::array<Color, 5> kPalette =
        {
            Color{0.95f, 0.35f, 0.30f, 1.0f},
            Color{0.22f, 0.75f, 0.42f, 1.0f},
            Color{0.26f, 0.56f, 0.95f, 1.0f},
            Color{0.97f, 0.82f, 0.26f, 1.0f},
            Color{0.72f, 0.38f, 0.92f, 1.0f},
        };

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
                camera.position = {0.0f, 0.0f, -2.0f};
                camera.rotationDeg = {0.0f, 0.0f, 0.0f};
                camera.fovYDeg = 65.0f;
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
                transform.position = {0.0f, 100.0f, kControlledTriangleBehindCubeZ};
                transform.scale = {0.28f, 0.28f, 0.28f};

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(controlledEntity);
                renderer.primitive = render::PrimitiveType::Triangle;
                renderer.color = kPalette[(i + 0) % kPalette.size()];

                world_.Emplace<ecs::components::WindowBindingComponent>(controlledEntity).windowId = windowId;
            }

            const ecs::EntityId parentEntity = world_.CreateEntity();
            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(parentEntity);
                tag.name = "ParentQuad_" + std::to_string(windowId);

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(parentEntity);
                transform.position = {-0.45f, 0.35f, 0.0f};
                transform.scale = {0.32f, 0.32f, 0.32f};

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(parentEntity);
                renderer.primitive = render::PrimitiveType::Quad;
                renderer.color = kPalette[(i + 1) % kPalette.size()];

                auto& motion = world_.Emplace<ecs::components::MotionComponent>(parentEntity);
                motion.angularVelocityDeg.z = 35.0f + 8.0f * static_cast<float>(i);

                world_.Emplace<ecs::components::WindowBindingComponent>(parentEntity).windowId = windowId;
            }

            const ecs::EntityId childEntity = world_.CreateEntity();
            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(childEntity);
                tag.name = "ChildLine_" + std::to_string(windowId);

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(childEntity);
                transform.position = {0.55f, 0.0f, 0.0f};
                transform.scale = {0.55f, 0.55f, 0.55f};

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(childEntity);
                renderer.primitive = render::PrimitiveType::Line;
                renderer.color = kPalette[(i + 2) % kPalette.size()];

                world_.Emplace<ecs::components::WindowBindingComponent>(childEntity).windowId = windowId;
                world_.SetParent(childEntity, parentEntity);
            }

            const ecs::EntityId cubeEntity = world_.CreateEntity();
            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(cubeEntity);
                tag.name = "MovingCube_" + std::to_string(windowId);

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(cubeEntity);
                transform.position = {0.52f, -0.22f, 0.0f};
                transform.scale = {0.23f, 0.23f, 0.23f};

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(cubeEntity);
                renderer.primitive = render::PrimitiveType::Cube;
                renderer.color = kPalette[(i + 3) % kPalette.size()];
                renderer.texturePath = "assets/textures/african_head_diffuse.dds";

                auto& motion = world_.Emplace<ecs::components::MotionComponent>(cubeEntity);
                motion.linearVelocity.x = (i % 2 == 0) ? -0.35f : 0.35f;
                motion.angularVelocityDeg.y = 45.0f;
                motion.angularVelocityDeg.z = 65.0f;

                world_.Emplace<ecs::components::WindowBindingComponent>(cubeEntity).windowId = windowId;
            }

            const ecs::EntityId cubeChildEntity = world_.CreateEntity();
            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(cubeChildEntity);
                tag.name = "CubeChild_" + std::to_string(windowId);

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(cubeChildEntity);
                transform.position = { 0.0f, 0.0f, 0.0f };
                transform.scale = { 0.3f, 5.0f, 0.3f };

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(cubeChildEntity);
                renderer.primitive = render::PrimitiveType::Cube;
                renderer.color = kPalette[(i + 4) % kPalette.size()];
                renderer.texturePath = "assets/textures/african_head_diffuse.dds";

                world_.Emplace<ecs::components::WindowBindingComponent>(cubeChildEntity).windowId = windowId;
                world_.SetParent(cubeChildEntity, cubeEntity);
            }

            const ecs::EntityId cubeChildEntity2 = world_.CreateEntity();
            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(cubeChildEntity2);
                tag.name = "CubeChild_" + std::to_string(windowId);

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(cubeChildEntity2);
                transform.position = { 0.0f, 0.0f, 0.0f };
                transform.scale = { 5.0f, 0.3f, 0.3f };

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(cubeChildEntity2);
                renderer.primitive = render::PrimitiveType::Cube;
                renderer.color = kPalette[(i + 4) % kPalette.size()];
                renderer.texturePath = "assets/textures/african_head_diffuse.dds";

                world_.Emplace<ecs::components::WindowBindingComponent>(cubeChildEntity2).windowId = windowId;
                world_.SetParent(cubeChildEntity2, cubeEntity);
            }

            const ecs::EntityId cubeChildEntity3 = world_.CreateEntity();
            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(cubeChildEntity3);
                tag.name = "CubeChild_" + std::to_string(windowId);

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(cubeChildEntity3);
                transform.position = { 0.0f, 0.0f, 0.0f };
                transform.scale = { 0.3f, 0.3f, 5.0f };

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(cubeChildEntity3);
                renderer.primitive = render::PrimitiveType::Cube;
                renderer.color = kPalette[(i + 4) % kPalette.size()];
                renderer.texturePath = "assets/textures/african_head_diffuse.dds";

                world_.Emplace<ecs::components::WindowBindingComponent>(cubeChildEntity3).windowId = windowId;
                world_.SetParent(cubeChildEntity3, cubeEntity);
            }

            const ecs::EntityId accentEntity = world_.CreateEntity();
            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(accentEntity);
                tag.name = "AccentTriangle_" + std::to_string(windowId);

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(accentEntity);
                transform.position = {-0.1f, -0.58f, 0.0f};
                transform.scale = {0.22f, 0.22f, 0.22f};

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(accentEntity);
                renderer.primitive = render::PrimitiveType::Triangle;
                renderer.color = kPalette[(i + 5) % kPalette.size()];

                world_.Emplace<ecs::components::WindowBindingComponent>(accentEntity).windowId = windowId;
            }

            logger_.Info("ECS demo scene created for window id=" + std::to_string(windowId) + " (entities: 6)");
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

            if (runtime.controlledEntity != ecs::kInvalidEntity)
            {
                auto* transform = world_.TryGet<ecs::components::TransformComponent>(runtime.controlledEntity);
                const auto* renderer = world_.TryGet<ecs::components::MeshRendererComponent>(runtime.controlledEntity);
                if (transform != nullptr &&
                    renderer != nullptr &&
                    renderer->primitive == render::PrimitiveType::Triangle)
                {
                    transform->position.z = kControlledTriangleBehindCubeZ;
                }
                continue;
            }

            world_.ForEach<ecs::components::TransformComponent, ecs::components::WindowBindingComponent>(
                [&](const ecs::EntityId entity,
                    ecs::components::TransformComponent&,
                    ecs::components::WindowBindingComponent& binding)
                {
                    if (runtime.controlledEntity == ecs::kInvalidEntity && binding.windowId == windowId)
                    {
                        runtime.controlledEntity = entity;
                    }
                });

            if (runtime.controlledEntity != ecs::kInvalidEntity)
            {
                auto* transform = world_.TryGet<ecs::components::TransformComponent>(runtime.controlledEntity);
                const auto* renderer = world_.TryGet<ecs::components::MeshRendererComponent>(runtime.controlledEntity);
                if (transform != nullptr &&
                    renderer != nullptr &&
                    renderer->primitive == render::PrimitiveType::Triangle)
                {
                    transform->position.z = kControlledTriangleBehindCubeZ;
                }
            }
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

            ecs::RenderFrameContext context{
                *renderAdapter_,
                runtime.surface,
                runtime.clearColor,
                runtime.window->Id(),
                runtime.window->Width(),
                runtime.window->Height(),
                &logger_,
            };

            world_.RenderSystems(context);
        }
    }
}
