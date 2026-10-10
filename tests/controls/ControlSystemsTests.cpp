#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/CameraControllerComponent.h>
#include <myengine/ecs/components/PlayerControllerComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/ecs/systems/CameraControlSystem.h>
#include <myengine/ecs/systems/PhysicsSystem.h>
#include <myengine/ecs/systems/PlayerControlSystem.h>
#include <myengine/input/InputManager.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/scene/SceneSerializer.h>

namespace
{
    namespace ecs = myengine::ecs;
    namespace components = ecs::components;
    namespace systems = ecs::systems;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void BindPlayerKeys(myengine::input::InputManager& input)
    {
        input.BindAction("player_forward", 'W');
        input.BindAction("player_backward", 'S');
        input.BindAction("player_left", 'A');
        input.BindAction("player_right", 'D');
        input.BindAction("player_jump", VK_SPACE);
        input.SetActiveWindow(1);
    }

    void TestWorldAxisMovementFromRest(const float cameraYawDeg)
    {
        for (const unsigned key : {'W', 'S', 'A', 'D'})
        {
            for (const float deltaTime : {1.0f / 240.0f, 1.0f / 60.0f, 1.0f / 30.0f})
            {
                ecs::World world;
                Check(myengine::scene::LoadWorldFromJson(world,
                    std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "assets/scenes/scripting_demo.json"), "Demo load failed");
                world.Get<components::CameraComponent>(1).rotationDeg.y = cameraYawDeg;
                myengine::core::ServiceLocator::GetPhysicsWorldState() = myengine::physics::PhysicsWorldState{};
                myengine::core::ServiceLocator::GetEditorRuntimeState().mode = myengine::editor::RuntimeMode::Play;

                myengine::input::InputManager input;
                BindPlayerKeys(input);
                systems::PlayerControlSystem player(input);
                systems::PhysicsSystem physics;
                for (unsigned frame = 0; frame < 120; ++frame)
                {
                    player.Update(world, 1.0f / 60.0f);
                    physics.Update(world, 1.0f / 60.0f);
                }

                const auto start = world.Get<components::TransformComponent>(2).position;
                const components::Vec3 forward{0.0f, 0.0f, 1.0f};
                const components::Vec3 right{1.0f, 0.0f, 0.0f};
                const auto side = key == 'W' || key == 'S' ? right : forward;
                const auto direction = key == 'W' || key == 'S' ? forward : right;
                const float directionSign = key == 'W' || key == 'D' ? 1.0f : -1.0f;
                input.OnKeyDown(key);
                float maximumSideDrift = 0.0f;
                const auto frameCount = static_cast<unsigned>(1.0f / deltaTime);
                for (unsigned frame = 0; frame < frameCount; ++frame)
                {
                    player.Update(world, deltaTime);
                    physics.Update(world, deltaTime);
                    const auto displacement = world.Get<components::TransformComponent>(2).position - start;
                    maximumSideDrift = std::max(maximumSideDrift, std::abs(components::Dot(displacement, side)));
                }
                std::cout << "From rest, " << static_cast<char>(key) << ", yaw=" << cameraYawDeg << ", dt=" << deltaTime
                    << ": side drift=" << maximumSideDrift << '\n';
                Check(maximumSideDrift < 0.0001f, "Player movement must stay on the selected world axis regardless of camera yaw");
                const auto displacement = world.Get<components::TransformComponent>(2).position - start;
                Check(components::Dot(displacement, direction) * directionSign > 0.1f, "Player must move in the selected world direction");
            }
        }
    }

    void TestVerticalCameraMovement()
    {
        ecs::World world;
        const auto entity = world.CreateEntity();
        auto& camera = world.Emplace<components::CameraComponent>(entity);
        camera.position = {3.0f, 5.0f, -8.0f};
        camera.rotationDeg = {60.0f, 35.0f, 15.0f};
        world.Emplace<components::CameraControllerComponent>(entity).moveSpeed = 4.0f;
        world.Emplace<components::WindowBindingComponent>(entity).windowId = 1;

        myengine::input::InputManager input;
        input.BindAction("camera_down", 'Q');
        input.BindAction("camera_up", 'E');
        myengine::core::WindowId navigationWindow = 1;
        systems::CameraControlSystem controller(input, navigationWindow);

        input.OnKeyDown('E');
        controller.Update(world, 0.25f);
        Check(camera.position.x == 3.0f && camera.position.y == 6.0f && camera.position.z == -8.0f, "E must move camera along world Y only");
        input.OnKeyUp('E');
        input.OnKeyDown('Q');
        controller.Update(world, 0.25f);
        Check(camera.position.x == 3.0f && camera.position.y == 5.0f && camera.position.z == -8.0f, "Q must move camera along world Y only");
        input.OnKeyDown('E');
        controller.Update(world, 0.25f);
        Check(camera.position.y == 5.0f, "Q and E together must cancel");
        input.OnKeyUp('Q');
        navigationWindow = 0;
        controller.Update(world, 0.25f);
        Check(camera.position.y == 5.0f, "Camera must not move outside navigation mode");
    }

    // me.camera.set marks the controller: in Play the keys must not move a camera that a script drives (D1c / RL1)
    void TestScriptControlledCamera()
    {
        auto& editorState = myengine::core::ServiceLocator::GetEditorRuntimeState();
        const auto previousMode = editorState.mode;

        ecs::World world;
        const auto entity = world.CreateEntity();
        auto& camera = world.Emplace<components::CameraComponent>(entity);
        camera.position = {1.0f, 2.0f, 3.0f};
        auto& controller = world.Emplace<components::CameraControllerComponent>(entity);
        controller.moveSpeed = 4.0f;
        controller.scriptControlled = true;
        world.Emplace<components::WindowBindingComponent>(entity).windowId = 1;

        myengine::input::InputManager input;
        input.BindAction("camera_up", 'E');
        myengine::core::WindowId navigationWindow = 1;
        systems::CameraControlSystem system(input, navigationWindow);

        editorState.mode = myengine::editor::RuntimeMode::Play;
        input.OnKeyDown('E');
        system.Update(world, 0.25f);
        Check(camera.position.y == 2.0f, "A script-controlled camera must ignore the free-fly keys in Play");
        Check(controller.scriptControlled, "The flag must stay while Play runs");

        editorState.mode = myengine::editor::RuntimeMode::Edit;
        system.Update(world, 0.25f);
        Check(!controller.scriptControlled, "The script control must end with Play");
        Check(camera.position.y == 3.0f, "The free-fly camera must work again in Edit");

        editorState.mode = previousMode;
    }

    void TestInputRelease()
    {
        myengine::input::InputManager input;
        BindPlayerKeys(input);
        input.OnKeyDown('W');
        input.OnKeyDown('A');
        input.OnMouseDown(myengine::core::MouseButton::Right);
        input.AddMouseDelta(12, -8);
        input.OnMouseWheel(120);
        input.ReleaseAllInputs();
        Check(!input.IsActionDown("player_forward") && !input.IsActionDown("player_left"), "Focus loss must release held movement keys");
        Check(input.WasKeyReleased('W') && input.WasKeyReleased('A'), "Focus loss must report key release");
        Check(!input.IsMouseDown(myengine::core::MouseButton::Right), "Focus loss must release camera navigation");
        Check(input.GetActiveWindowId() == 0, "Focus loss must clear active input window");
        Check(input.ConsumeMouseDelta() == std::pair<int, int>{0, 0} && input.ConsumeMouseWheelSteps() == 0,
            "Focus loss must clear pending camera input");
        input.OnKeyUp('W');
        input.ReleaseAllInputs();
        Check(!input.IsKeyDown('W'), "Repeated release must be safe");
        input.SetActiveWindow(1);
        input.OnKeyDown('W');
        Check(input.IsActionDown("player_forward"), "Input must work again after focus loss");
    }
}

int main()
{
    myengine::jobs::Initialize(2);
    try
    {
        for (const float cameraYawDeg : {0.0f, 8.52f, 45.0f, -35.25f, 90.0f, 180.0f})
        {
            TestWorldAxisMovementFromRest(cameraYawDeg);
        }
        TestVerticalCameraMovement();
        TestScriptControlledCamera();
        TestInputRelease();
        myengine::jobs::Shutdown();
        std::cout << "OK: player movement from rest, vertical camera navigation and input release\n";
        return 0;
    }
    catch (const std::exception& ex)
    {
        myengine::jobs::Shutdown();
        std::cerr << "FAILED: " << ex.what() << '\n';
        return 1;
    }
}
