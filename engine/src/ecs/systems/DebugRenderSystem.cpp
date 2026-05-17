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

        void AddBox(std::vector<render::DebugLine>& lines, const std::array<components::Vec3, 8>& corners, const core::Color& color)
        {
            AddLine(lines, corners[0], corners[1], color);
            AddLine(lines, corners[1], corners[2], color);
            AddLine(lines, corners[2], corners[3], color);
            AddLine(lines, corners[3], corners[0], color);

            AddLine(lines, corners[4], corners[5], color);
            AddLine(lines, corners[5], corners[6], color);
            AddLine(lines, corners[6], corners[7], color);
            AddLine(lines, corners[7], corners[4], color);

            AddLine(lines, corners[0], corners[4], color);
            AddLine(lines, corners[1], corners[5], color);
            AddLine(lines, corners[2], corners[6], color);
            AddLine(lines, corners[3], corners[7], color);
        }

        void AddOrientedBox(std::vector<render::DebugLine>& lines, const physics::DebugOrientedBox& box)
        {
            AddBox(lines, box.corners, box.color);
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
        lines.reserve(physicsState.debugBoxes.size() * 12 + physicsState.debugSpheres.size() * 54 + physicsState.debugVectors.size());

        for (const auto& box : physicsState.debugBoxes)
        {
            AddOrientedBox(lines, box);
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