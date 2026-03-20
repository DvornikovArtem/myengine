// System.h

#pragma once

#include <myengine/core/Types.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/render/RenderTypes.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::resource
{
    class ResourceManager;
}

namespace myengine::ecs
{
    class World;

    struct RenderFrameContext
    {
        render::IRenderAdapter& renderAdapter;
        render::RenderSurfaceHandle surface;
        core::Color clearColor;
        core::WindowId windowId = 0;
        std::uint32_t windowWidth = 1;
        std::uint32_t windowHeight = 1;
        core::Logger* logger = nullptr;
        resource::ResourceManager* resourceManager = nullptr;
    };

    class IUpdateSystem
    {
    public:
        virtual ~IUpdateSystem() = default;
        virtual void Update(World& world, float deltaTime) = 0;
    };

    class IRenderSystem
    {
    public:
        virtual ~IRenderSystem() = default;
        virtual void Render(World& world, const RenderFrameContext& context) = 0;
    };
}