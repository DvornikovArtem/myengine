// MeshRendererComponent.h

#pragma once

#include <string>

#include <myengine/ecs/Component.h>
#include <myengine/core/Types.h>
#include <myengine/render/RenderTypes.h>

namespace myengine::ecs::components
{
    struct MeshRendererComponent : Component
    {
        render::PrimitiveType primitive = render::PrimitiveType::Triangle;
        core::Color color{1.0f, 1.0f, 1.0f, 1.0f};
        std::string materialName;
        std::string texturePath;
    };
}