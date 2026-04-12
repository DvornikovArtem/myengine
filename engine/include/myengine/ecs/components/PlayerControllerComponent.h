#pragma once

#include <myengine/core/Types.h>
#include <myengine/ecs/Component.h>

namespace myengine::ecs::components
{
    struct PlayerControllerComponent : Component
    {
        core::WindowId windowId = 0;
        float moveSpeed = 4.5f;
        float jumpSpeed = 6.25f;
        float airControl = 0.45f;
    };
}