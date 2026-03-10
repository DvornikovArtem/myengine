// MotionSystem.cpp

#include <algorithm>

#include <myengine/ecs/World.h>
#include <myengine/ecs/components/MotionComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/systems/MotionSystem.h>

namespace myengine::ecs::systems
{
    void MotionSystem::Update(World& world, const float deltaTime)
    {
        constexpr float kMinPosition = -0.9f;
        constexpr float kMaxPosition = 0.9f;

        world.ForEach<components::TransformComponent, components::MotionComponent>(
            [deltaTime](const EntityId, components::TransformComponent& transform, components::MotionComponent& motion)
            {
                transform.position.x += motion.linearVelocity.x * deltaTime;
                transform.position.y += motion.linearVelocity.y * deltaTime;
                transform.position.z += motion.linearVelocity.z * deltaTime;

                transform.rotationDeg.x += motion.angularVelocityDeg.x * deltaTime;
                transform.rotationDeg.y += motion.angularVelocityDeg.y * deltaTime;
                transform.rotationDeg.z += motion.angularVelocityDeg.z * deltaTime;

                if (transform.position.x < kMinPosition || transform.position.x > kMaxPosition)
                {
                    transform.position.x = std::clamp(transform.position.x, kMinPosition, kMaxPosition);
                    motion.linearVelocity.x *= -1.0f;
                }

                if (transform.position.y < kMinPosition || transform.position.y > kMaxPosition)
                {
                    transform.position.y = std::clamp(transform.position.y, kMinPosition, kMaxPosition);
                    motion.linearVelocity.y *= -1.0f;
                }
            });
    }
}

