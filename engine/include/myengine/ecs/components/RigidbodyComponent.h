#pragma once

#include <myengine/ecs/Component.h>
#include <myengine/ecs/components/Vector3.h>

namespace myengine::ecs::components
{
    struct RigidbodyComponent : Component
    {
        Vec3 velocity{};
        Vec3 acceleration{};
        float mass = 1.0f;
        float gravityScale = 1.0f;
        float linearDamping = 0.14f;
        float sleepLinearSpeed = 0.08f;
        bool useGravity = true;
        bool isKinematic = false;
        bool isGrounded = false;
    };
}