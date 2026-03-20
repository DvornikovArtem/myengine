// MeshRendererComponent.h

#pragma once

#include <string>

#include <myengine/ecs/Component.h>

namespace myengine::ecs::components
{
    struct MeshRendererComponent : Component
    {
        std::string meshPath;
        std::string materialPath;
        bool visible = true;
    };
}