#pragma once

#include <cstdint>

#include <myengine/ecs/Component.h>
#include <myengine/ecs/components/Vector3.h>

namespace myengine::ecs::components
{
    enum class ColliderType : std::uint8_t
    {
        Box = 0,
        Sphere = 1,
    };

    struct ColliderComponent : Component
    {
        ColliderType type = ColliderType::Box;
        Vec3 halfExtents{0.5f, 0.5f, 0.5f};
        float radius = 0.5f;
        Vec3 offset{};
        float friction = 0.65f;
        float bounciness = 0.12f;
        bool isTrigger = false;
    };
}