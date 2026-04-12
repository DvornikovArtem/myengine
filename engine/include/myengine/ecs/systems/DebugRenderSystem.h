#pragma once

#include <myengine/ecs/System.h>

namespace myengine::ecs::systems
{
    class DebugRenderSystem final : public IRenderSystem
    {
    public:
        void Render(World& world, const RenderFrameContext& context) override;
    };
}