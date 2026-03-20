// RenderSystem.h

#pragma once

#include <unordered_set>

#include <myengine/ecs/Entity.h>
#include <myengine/ecs/System.h>

namespace myengine::ecs
{
    class World;
}

namespace myengine::ecs::systems
{
    class RenderSystem final : public IRenderSystem
    {
    public:
        void Render(World& world, const RenderFrameContext& context) override;

    private:
        std::unordered_set<EntityId> hierarchyCycleWarningLogged_;
    };
}