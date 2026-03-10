// TagComponent.h

#pragma once

#include <string>

#include <myengine/ecs/Component.h>

namespace myengine::ecs::components
{
    struct TagComponent : Component
    {
        std::string name;
    };
}