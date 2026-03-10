// CameraControllerComponent.h

#pragma once

#include <myengine/ecs/Component.h>

namespace myengine::ecs::components
{
    struct CameraControllerComponent : Component
    {
        float moveSpeed = 1.4f;
        float zoomSpeed = 1.0f;
        float rotateSpeedDeg = 80.0f;
        float mouseSensitivityDeg = 0.12f;
    };
}