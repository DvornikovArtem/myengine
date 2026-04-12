#pragma once

#include <myengine/ecs/Entity.h>
#include <myengine/ecs/components/Vector3.h>

namespace myengine::physics
{
    struct CollisionEvent
    {
        ecs::EntityId entityA = ecs::kInvalidEntity;
        ecs::EntityId entityB = ecs::kInvalidEntity;
        ecs::components::Vec3 point{};
        ecs::components::Vec3 normal{};
        float impulse = 0.0f;
    };

    struct TriggerEvent
    {
        ecs::EntityId triggerEntity = ecs::kInvalidEntity;
        ecs::EntityId otherEntity = ecs::kInvalidEntity;
        ecs::components::Vec3 point{};
    };
}