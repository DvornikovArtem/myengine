// MotionSystem.h

#pragma once

#include <myengine/ecs/System.h>

namespace myengine::ecs::systems
{
    class MotionSystem final : public IUpdateSystem
    {
    public:
        void Update(World& world, float deltaTime) override;
    };
}