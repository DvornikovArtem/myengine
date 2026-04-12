#include <cmath>
#include <vector>

#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/systems/DebugRenderSystem.h>

namespace myengine::ecs::systems
{
    namespace
    {
        render::Float3 ToFloat3(const components::Vec3& value)
        {
            return {value.x, value.y, value.z};
        }

        void AddLine(std::vector<render::DebugLine>& lines, const components::Vec3& start, const components::Vec3& end, const core::Color& color)
        {
            lines.push_back(render::DebugLine{ToFloat3(start), ToFloat3(end), color});
        }

        void AddAabb(std::vector<render::DebugLine>& lines, const physics::DebugAabb& aabb)
        {
            const components::Vec3 v0{aabb.min.x, aabb.min.y, aabb.min.z};
            const components::Vec3 v1{aabb.max.x, aabb.min.y, aabb.min.z};
            const components::Vec3 v2{aabb.max.x, aabb.max.y, aabb.min.z};
            const components::Vec3 v3{aabb.min.x, aabb.max.y, aabb.min.z};
            const components::Vec3 v4{aabb.min.x, aabb.min.y, aabb.max.z};
            const components::Vec3 v5{aabb.max.x, aabb.min.y, aabb.max.z};
            const components::Vec3 v6{aabb.max.x, aabb.max.y, aabb.max.z};
            const components::Vec3 v7{aabb.min.x, aabb.max.y, aabb.max.z};

            AddLine(lines, v0, v1, aabb.color);
            AddLine(lines, v1, v2, aabb.color);
            AddLine(lines, v2, v3, aabb.color);
            AddLine(lines, v3, v0, aabb.color);

            AddLine(lines, v4, v5, aabb.color);
            AddLine(lines, v5, v6, aabb.color);
            AddLine(lines, v6, v7, aabb.color);
            AddLine(lines, v7, v4, aabb.color);

            AddLine(lines, v0, v4, aabb.color);
            AddLine(lines, v1, v5, aabb.color);
            AddLine(lines, v2, v6, aabb.color);
            AddLine(lines, v3, v7, aabb.color);
        }

        void AddSphere(std::vector<render::DebugLine>& lines, const physics::DebugSphere& sphere)
        {
            constexpr int kSegments = 18;
            constexpr float kTwoPi = 6.2831853071f;

            for (int axis = 0; axis < 3; ++axis)
            {
                for (int segment = 0; segment < kSegments; ++segment)
                {
                    const float angleA = kTwoPi * static_cast<float>(segment) / static_cast<float>(kSegments);
                    const float angleB = kTwoPi * static_cast<float>(segment + 1) / static_cast<float>(kSegments);

                    components::Vec3 pointA = sphere.center;
                    components::Vec3 pointB = sphere.center;

                    if (axis == 0)
                    {
                        pointA.y += std::cos(angleA) * sphere.radius;
                        pointA.z += std::sin(angleA) * sphere.radius;
                        pointB.y += std::cos(angleB) * sphere.radius;
                        pointB.z += std::sin(angleB) * sphere.radius;
                    }
                    else if (axis == 1)
                    {
                        pointA.x += std::cos(angleA) * sphere.radius;
                        pointA.z += std::sin(angleA) * sphere.radius;
                        pointB.x += std::cos(angleB) * sphere.radius;
                        pointB.z += std::sin(angleB) * sphere.radius;
                    }
                    else
                    {
                        pointA.x += std::cos(angleA) * sphere.radius;
                        pointA.y += std::sin(angleA) * sphere.radius;
                        pointB.x += std::cos(angleB) * sphere.radius;
                        pointB.y += std::sin(angleB) * sphere.radius;
                    }

                    AddLine(lines, pointA, pointB, sphere.color);
                }
            }
        }
    }

    void DebugRenderSystem::Render(World& world, const RenderFrameContext& context)
    {
        static_cast<void>(world);

        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        if (!physicsState.debugDrawEnabled)
        {
            return;
        }

        std::vector<render::DebugLine> lines;
        lines.reserve(physicsState.debugAabbs.size() * 12 + physicsState.debugSpheres.size() * 54 + physicsState.debugVectors.size());

        for (const auto& aabb : physicsState.debugAabbs)
        {
            AddAabb(lines, aabb);
        }

        for (const auto& sphere : physicsState.debugSpheres)
        {
            AddSphere(lines, sphere);
        }

        for (const auto& vector : physicsState.debugVectors)
        {
            AddLine(lines, vector.start, vector.end, vector.color);
        }

        if (!lines.empty())
        {
            context.renderAdapter.DrawDebugLines(context.surface, lines);
        }
    }
}