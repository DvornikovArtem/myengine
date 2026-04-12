#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <myengine/core/Types.h>
#include <myengine/ecs/components/Vector3.h>

namespace myengine::physics
{
    struct DebugAabb
    {
        ecs::components::Vec3 min{};
        ecs::components::Vec3 max{};
        core::Color color{0.17f, 0.80f, 0.95f, 1.0f};
    };

    struct DebugSphere
    {
        ecs::components::Vec3 center{};
        float radius = 0.5f;
        core::Color color{0.96f, 0.80f, 0.24f, 1.0f};
    };

    struct DebugVector
    {
        ecs::components::Vec3 start{};
        ecs::components::Vec3 end{};
        core::Color color{1.0f, 0.35f, 0.25f, 1.0f};
    };

    struct PhysicsStats
    {
        std::uint32_t fixedStepCount = 0;
        std::uint32_t rigidbodyCount = 0;
        std::uint32_t broadPhasePairs = 0;
        std::uint32_t collisionPairs = 0;
        std::uint32_t triggerPairs = 0;
    };

    struct PhysicsWorldState
    {
        bool physicsPaused = false;
        bool debugDrawEnabled = true;
        float gravityStrength = 9.81f;
        float fixedTimeStep = 1.0f / 60.0f;
        PhysicsStats stats{};
        std::vector<DebugAabb> debugAabbs;
        std::vector<DebugSphere> debugSpheres;
        std::vector<DebugVector> debugVectors;
        std::vector<std::string> recentEvents;

        void ClearDebugGeometry()
        {
            debugAabbs.clear();
            debugSpheres.clear();
            debugVectors.clear();
        }

        void PushRecentEvent(std::string message)
        {
            recentEvents.push_back(std::move(message));
            constexpr std::size_t kMaxRecentEvents = 12;
            if (recentEvents.size() > kMaxRecentEvents)
            {
                recentEvents.erase(recentEvents.begin(), recentEvents.begin() + static_cast<std::ptrdiff_t>(recentEvents.size() - kMaxRecentEvents));
            }
        }
    };
}