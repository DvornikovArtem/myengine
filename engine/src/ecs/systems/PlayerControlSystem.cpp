#include <algorithm>
#include <cmath>

#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/PlayerControllerComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/ecs/systems/PlayerControlSystem.h>
#include <myengine/input/InputManager.h>

namespace myengine::ecs::systems
{
    namespace
    {
        constexpr float kDegToRad = 0.0174532925f;
    }

    PlayerControlSystem::PlayerControlSystem(input::InputManager& input)
        : input_(input)
    {
    }

    void PlayerControlSystem::Update(World& world, const float deltaTime)
    {
        if (core::ServiceLocator::GetEditorRuntimeState().mode != editor::RuntimeMode::Play)
        {
            return;
        }

        const core::WindowId activeWindowId = input_.GetActiveWindowId();
        if (activeWindowId == 0 || input_.IsMouseDown(core::MouseButton::Right))
        {
            return;
        }

        float cameraYawDeg = 0.0f;
        world.ForEach<components::CameraComponent, components::WindowBindingComponent>(
            [&](const EntityId, components::CameraComponent& camera, components::WindowBindingComponent& binding)
            {
                if (binding.windowId == activeWindowId && camera.isPrimary)
                {
                    cameraYawDeg = camera.rotationDeg.y;
                }
            });

        const float yawRad = cameraYawDeg * kDegToRad;
        const components::Vec3 forward{
            std::sin(yawRad),
            0.0f,
            std::cos(yawRad),
        };
        const components::Vec3 right{forward.z, 0.0f, -forward.x};

        world.ForEach<components::PlayerControllerComponent, components::RigidbodyComponent>(
            [&](const EntityId entity, components::PlayerControllerComponent& controller, components::RigidbodyComponent& rigidbody)
            {
                const auto* binding = world.TryGet<components::WindowBindingComponent>(entity);
                const core::WindowId entityWindowId =
                    binding != nullptr && binding->windowId != 0 ? binding->windowId : controller.windowId;

                if (entityWindowId != activeWindowId)
                {
                    return;
                }

                components::Vec3 moveDirection{};
                if (input_.IsActionDown("player_forward"))
                {
                    moveDirection += forward;
                }
                if (input_.IsActionDown("player_backward"))
                {
                    moveDirection -= forward;
                }
                if (input_.IsActionDown("player_left"))
                {
                    moveDirection -= right;
                }
                if (input_.IsActionDown("player_right"))
                {
                    moveDirection += right;
                }

                if (components::LengthSquared(moveDirection) > 0.0f)
                {
                    moveDirection = components::Normalize(moveDirection);
                }

                const float controlBlend = std::clamp((rigidbody.isGrounded ? 1.0f : controller.airControl) * deltaTime * 10.0f, 0.0f, 1.0f);
                const components::Vec3 desiredVelocity = moveDirection * controller.moveSpeed;

                rigidbody.velocity.x += (desiredVelocity.x - rigidbody.velocity.x) * controlBlend;
                rigidbody.velocity.z += (desiredVelocity.z - rigidbody.velocity.z) * controlBlend;

                if (components::LengthSquared(moveDirection) <= 0.0f && rigidbody.isGrounded)
                {
                    const float damping = std::clamp(1.0f - deltaTime * 8.0f, 0.0f, 1.0f);
                    rigidbody.velocity.x *= damping;
                    rigidbody.velocity.z *= damping;
                }

                if (input_.WasActionPressed("player_jump") && rigidbody.isGrounded)
                {
                    rigidbody.velocity.y = controller.jumpSpeed;
                    rigidbody.isGrounded = false;
                }
            });
    }
}