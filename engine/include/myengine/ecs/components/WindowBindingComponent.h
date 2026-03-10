// WindowBindingComponent.h

#pragma once

#include <myengine/ecs/Component.h>
#include <myengine/core/Types.h>

namespace myengine::ecs::components
{
    struct WindowBindingComponent : Component
    {
        core::WindowId windowId = 0;
    };
}