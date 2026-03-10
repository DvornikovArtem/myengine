// TransformComponent.h

#pragma once

#include <myengine/ecs/Component.h>
#include <myengine/ecs/components/Vector3.h>

namespace myengine::ecs::components
{
    struct TransformComponent : Component
    {
        Vec3 position{};
        Vec3 rotationDeg{};
        Vec3 scale{1.0f, 1.0f, 1.0f};
    };
}