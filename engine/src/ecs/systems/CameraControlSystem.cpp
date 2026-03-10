// CameraControlSystem.cpp

#include <algorithm>
#include <cmath>

#include <directxtk/SimpleMath.h>

#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/CameraControllerComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/ecs/systems/CameraControlSystem.h>
#include <myengine/input/InputManager.h>

// If the WIN32_LEAN_AND_MEAN macro is defined before including windows.h, rarely used parts are excluded from the header to speed up compilation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::ecs::systems
{
    namespace
    {
        constexpr float kDegToRad = 0.0174532925f;
    }

    CameraControlSystem::CameraControlSystem(
        input::InputManager& input,
        const core::WindowId& activeWindowId) : input_(input), activeWindowId_(activeWindowId) {}

    void CameraControlSystem::Update(World& world, const float deltaTime)
    {
        if (activeWindowId_ == 0)
        {
            return;
        }

        const auto [mouseDeltaX, mouseDeltaY] = input_.ConsumeMouseDelta();
        const int wheelSteps = input_.ConsumeMouseWheelSteps();

        constexpr float kSpeedMultiplierPerWheelStep = 1.12f;
        constexpr float kMinMoveSpeed = 0.2f;
        constexpr float kMaxMoveSpeed = 30.0f;
        constexpr float kPitchLimitDeg = 89.0f;

        world.ForEach<components::CameraComponent, components::CameraControllerComponent, components::WindowBindingComponent>(
            [&](const EntityId, components::CameraComponent& camera, components::CameraControllerComponent& controller, const components::WindowBindingComponent& binding)
            {
                if (binding.windowId != activeWindowId_)
                {
                    return;
                }

                if (wheelSteps != 0)
                {
                    const float speedMultiplier = std::pow(kSpeedMultiplierPerWheelStep, static_cast<float>(wheelSteps));
                    controller.moveSpeed = std::clamp(controller.moveSpeed * speedMultiplier, kMinMoveSpeed, kMaxMoveSpeed);
                }

                const bool moveLeft = input_.IsKeyDown('A') || input_.IsKeyDown(VK_LEFT);
                const bool moveRight = input_.IsKeyDown('D') || input_.IsKeyDown(VK_RIGHT);
                const bool moveUp = input_.IsKeyDown('W') || input_.IsKeyDown(VK_UP);
                const bool moveDown = input_.IsKeyDown('S') || input_.IsKeyDown(VK_DOWN);
                const bool moveWorldUp = input_.IsKeyDown(VK_SPACE);
                const bool moveWorldDown = input_.IsKeyDown(VK_SHIFT) || input_.IsKeyDown(VK_LSHIFT);

                camera.rotationDeg.y += static_cast<float>(mouseDeltaX) * controller.mouseSensitivityDeg;
                camera.rotationDeg.x += static_cast<float>(mouseDeltaY) * controller.mouseSensitivityDeg;
                camera.rotationDeg.x = std::clamp(camera.rotationDeg.x, -kPitchLimitDeg, kPitchLimitDeg);

                if (camera.rotationDeg.y > 180.0f)
                {
                    camera.rotationDeg.y -= 360.0f;
                }
                else if (camera.rotationDeg.y < -180.0f)
                {
                    camera.rotationDeg.y += 360.0f;
                }

                const DirectX::SimpleMath::Matrix rotation = DirectX::SimpleMath::Matrix::CreateFromYawPitchRoll(
                    camera.rotationDeg.y * kDegToRad,
                    camera.rotationDeg.x * kDegToRad,
                    camera.rotationDeg.z * kDegToRad);

                DirectX::SimpleMath::Vector3 forward = DirectX::SimpleMath::Vector3::TransformNormal(DirectX::SimpleMath::Vector3::Backward, rotation);
                DirectX::SimpleMath::Vector3 right = DirectX::SimpleMath::Vector3::TransformNormal(DirectX::SimpleMath::Vector3::Right, rotation);
                forward.Normalize();
                right.Normalize();

                DirectX::SimpleMath::Vector3 movement = DirectX::SimpleMath::Vector3::Zero;

                if (moveLeft)
                {
                    movement -= right;
                }
                if (moveRight)
                {
                    movement += right;
                }
                if (moveUp)
                {
                    movement += forward;
                }
                if (moveDown)
                {
                    movement -= forward;
                }
                if (moveWorldUp)
                {
                    movement.y += 1.0f;
                }
                if (moveWorldDown)
                {
                    movement.y -= 1.0f;
                }

                if (movement.LengthSquared() > 0.0f)
                {
                    movement.Normalize();

                    camera.position.x += movement.x * controller.moveSpeed * deltaTime;
                    camera.position.y += movement.y * controller.moveSpeed * deltaTime;
                    camera.position.z += movement.z * controller.moveSpeed * deltaTime;
                }
            });
    }
}