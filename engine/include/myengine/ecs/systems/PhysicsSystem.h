#pragma once

#include <cstdint>
#include <unordered_set>

#include <myengine/ecs/System.h>

namespace myengine::ecs::systems
{
    class PhysicsSystem final : public IUpdateSystem
    {
    public:
        void Update(World& world, float deltaTime) override;

    private:
        float accumulator_ = 0.0f;
        std::unordered_set<std::uint64_t> activeCollisionPairs_;
        std::unordered_set<std::uint64_t> activeTriggerPairs_;
    };
}