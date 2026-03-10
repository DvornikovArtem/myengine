// CameraComponent.h

#pragma once

#include <myengine/ecs/Component.h>
#include <myengine/ecs/components/Vector3.h>

namespace myengine::ecs::components
{
    struct CameraComponent : Component
    {
        Vec3 position{};
        Vec3 rotationDeg{};
        float fovYDeg = 60.0f;
        float orthographicHalfHeight = 1.0f;
        float nearPlane = 0.01f;
        float farPlane = 200.0f;
        bool isPrimary = true;
    };
}