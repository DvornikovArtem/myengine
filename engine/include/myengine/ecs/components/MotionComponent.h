// MotionComponent.h

#pragma once

#include <myengine/ecs/Component.h>
#include <myengine/ecs/components/Vector3.h>

namespace myengine::ecs::components
{
    struct MotionComponent : Component
    {
        Vec3 linearVelocity{};
        Vec3 angularVelocityDeg{};
    };
}