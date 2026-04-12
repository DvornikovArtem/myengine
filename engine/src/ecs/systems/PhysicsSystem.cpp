#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/ColliderComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/systems/PhysicsSystem.h>
#include <myengine/physics/PhysicsEvents.h>
#include <myengine/spatial/UniformGrid3D.h>

namespace myengine::ecs::systems
{
    namespace
    {
        using components::Abs;
        using components::Clamp;
        using components::ColliderComponent;
        using components::ColliderType;
        using components::Dot;
        using components::HadamardMul;
        using components::Length;
        using components::LengthSquared;
        using components::Max;
        using components::Normalize;
        using components::RigidbodyComponent;
        using components::TransformComponent;
        using components::Vec3;

        struct WorldAabb
        {
            Vec3 min{};
            Vec3 max{};
        };

        struct WorldSphere
        {
            Vec3 center{};
            float radius = 0.5f;
        };

        struct ContactManifold
        {
            bool hasCollision = false;
            Vec3 normal{};
            Vec3 point{};
            float penetration = 0.0f;
        };

        struct CollisionBody
        {
            EntityId entity = kInvalidEntity;
            TransformComponent* transform = nullptr;
            RigidbodyComponent* rigidbody = nullptr;
            ColliderComponent* collider = nullptr;
            WorldAabb aabb{};
            WorldSphere sphere{};
            float inverseMass = 0.0f;
        };

        constexpr float kCollisionEpsilon = 1e-5f;
        constexpr float kPositionCorrectionPercent = 0.8f;
        constexpr float kPositionCorrectionSlop = 0.001f;
        constexpr float kGroundNormalThreshold = 0.75f;
        constexpr float kBounceVelocityThreshold = 1.0f;
        constexpr float kGroundSnapVelocity = 0.08f;
        constexpr float kCollisionEventImpulseThreshold = 0.35f;
        constexpr std::uint32_t kSolverIterations = 4;
        constexpr float kMinimumSupportOverlapRatio = 0.35f;

        std::uint64_t MakePairKey(const EntityId a, const EntityId b)
        {
            const EntityId first = std::min(a, b);
            const EntityId second = std::max(a, b);
            return (static_cast<std::uint64_t>(first) << 32) | static_cast<std::uint64_t>(second);
        }

        float ComputeInverseMass(const RigidbodyComponent* rigidbody)
        {
            if (rigidbody == nullptr || rigidbody->isKinematic || rigidbody->mass <= 0.0f)
            {
                return 0.0f;
            }

            return 1.0f / rigidbody->mass;
        }

        Vec3 GetColliderWorldCenter(const TransformComponent& transform, const ColliderComponent& collider)
        {
            return transform.position + HadamardMul(collider.offset, transform.scale);
        }

        Vec3 GetScaledHalfExtents(const TransformComponent& transform, const ColliderComponent& collider)
        {
            const Vec3 safeScale = Max(Abs(transform.scale), Vec3{0.001f, 0.001f, 0.001f});
            return HadamardMul(collider.halfExtents, safeScale);
        }

        float GetScaledRadius(const TransformComponent& transform, const ColliderComponent& collider)
        {
            const Vec3 safeScale = Max(Abs(transform.scale), Vec3{0.001f, 0.001f, 0.001f});
            const float maxScale = std::max(safeScale.x, std::max(safeScale.y, safeScale.z));
            return collider.radius * maxScale;
        }

        WorldAabb BuildAabb(const TransformComponent& transform, const ColliderComponent& collider)
        {
            const Vec3 center = GetColliderWorldCenter(transform, collider);
            const Vec3 extents = GetScaledHalfExtents(transform, collider);
            return {center - extents, center + extents};
        }

        WorldSphere BuildSphere(const TransformComponent& transform, const ColliderComponent& collider)
        {
            return {GetColliderWorldCenter(transform, collider), GetScaledRadius(transform, collider)};
        }

        void RefreshCollisionBodyBounds(CollisionBody& body)
        {
            if (body.transform == nullptr || body.collider == nullptr)
            {
                return;
            }

            body.aabb = BuildAabb(*body.transform, *body.collider);
            body.sphere = BuildSphere(*body.transform, *body.collider);
        }

        Vec3 GetAabbCenter(const WorldAabb& aabb)
        {
            return (aabb.min + aabb.max) * 0.5f;
        }

        ContactManifold IntersectAabbAabb(const WorldAabb& a, const WorldAabb& b)
        {
            const Vec3 centerDelta = GetAabbCenter(b) - GetAabbCenter(a);
            const Vec3 aExtents = (a.max - a.min) * 0.5f;
            const Vec3 bExtents = (b.max - b.min) * 0.5f;
            const Vec3 overlap{
                aExtents.x + bExtents.x - std::abs(centerDelta.x),
                aExtents.y + bExtents.y - std::abs(centerDelta.y),
                aExtents.z + bExtents.z - std::abs(centerDelta.z),
            };

            if (overlap.x <= 0.0f || overlap.y <= 0.0f || overlap.z <= 0.0f)
            {
                return {};
            }

            ContactManifold manifold;
            manifold.hasCollision = true;
            manifold.penetration = overlap.x;
            manifold.normal = {centerDelta.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};

            if (overlap.y < manifold.penetration)
            {
                manifold.penetration = overlap.y;
                manifold.normal = {0.0f, centerDelta.y >= 0.0f ? 1.0f : -1.0f, 0.0f};
            }

            if (overlap.z < manifold.penetration)
            {
                manifold.penetration = overlap.z;
                manifold.normal = {0.0f, 0.0f, centerDelta.z >= 0.0f ? 1.0f : -1.0f};
            }

            const bool verticalContact = std::abs(manifold.normal.y) > 0.5f;
            if (verticalContact)
            {
                const float minSupportOverlapX = std::min(aExtents.x, bExtents.x) * 2.0f * kMinimumSupportOverlapRatio;
                const float minSupportOverlapZ = std::min(aExtents.z, bExtents.z) * 2.0f * kMinimumSupportOverlapRatio;
                const bool hasStableSupport = overlap.x >= minSupportOverlapX && overlap.z >= minSupportOverlapZ;
                if (!hasStableSupport)
                {
                    if (overlap.x < overlap.z)
                    {
                        manifold.penetration = overlap.x;
                        manifold.normal = {centerDelta.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
                    }
                    else
                    {
                        manifold.penetration = overlap.z;
                        manifold.normal = {0.0f, 0.0f, centerDelta.z >= 0.0f ? 1.0f : -1.0f};
                    }
                }
            }

            const Vec3 pointOnA = Clamp(GetAabbCenter(b), a.min, a.max);
            const Vec3 pointOnB = Clamp(pointOnA, b.min, b.max);
            manifold.point = (pointOnA + pointOnB) * 0.5f;
            return manifold;
        }

        ContactManifold IntersectSphereSphere(const WorldSphere& a, const WorldSphere& b)
        {
            const Vec3 delta = b.center - a.center;
            const float distanceSquared = LengthSquared(delta);
            const float radiusSum = a.radius + b.radius;
            if (distanceSquared >= radiusSum * radiusSum)
            {
                return {};
            }

            const float distance = std::sqrt(std::max(distanceSquared, kCollisionEpsilon));

            ContactManifold manifold;
            manifold.hasCollision = true;
            manifold.normal = (distance > kCollisionEpsilon) ? delta / distance : Vec3{0.0f, 1.0f, 0.0f};
            manifold.penetration = radiusSum - distance;
            manifold.point = a.center + manifold.normal * (a.radius - manifold.penetration * 0.5f);
            return manifold;
        }

        ContactManifold IntersectSphereAabb(const WorldSphere& sphere, const WorldAabb& aabb)
        {
            const Vec3 closestPoint = Clamp(sphere.center, aabb.min, aabb.max);
            const Vec3 delta = closestPoint - sphere.center;
            const float distanceSquared = LengthSquared(delta);
            if (distanceSquared > sphere.radius * sphere.radius)
            {
                return {};
            }

            ContactManifold manifold;
            manifold.hasCollision = true;

            const float distance = std::sqrt(std::max(distanceSquared, kCollisionEpsilon));
            if (distance > kCollisionEpsilon)
            {
                manifold.normal = delta / distance;
                manifold.penetration = sphere.radius - distance;
                const Vec3 pointOnSphere = sphere.center + manifold.normal * sphere.radius;
                manifold.point = (pointOnSphere + closestPoint) * 0.5f;
                return manifold;
            }

            const Vec3 aabbCenter = GetAabbCenter(aabb);
            const Vec3 extents = (aabb.max - aabb.min) * 0.5f;
            const Vec3 local = sphere.center - aabbCenter;
            const Vec3 distances{
                extents.x - std::abs(local.x),
                extents.y - std::abs(local.y),
                extents.z - std::abs(local.z),
            };

            manifold.penetration = distances.x + sphere.radius;
            manifold.normal = {local.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
            float distanceToFace = distances.x;

            if (distances.y < distances.x)
            {
                manifold.penetration = distances.y + sphere.radius;
                manifold.normal = {0.0f, local.y >= 0.0f ? 1.0f : -1.0f, 0.0f};
                distanceToFace = distances.y;
            }

            if (distances.z < std::min(distances.x, distances.y))
            {
                manifold.penetration = distances.z + sphere.radius;
                manifold.normal = {0.0f, 0.0f, local.z >= 0.0f ? 1.0f : -1.0f};
                distanceToFace = distances.z;
            }

            const Vec3 pointOnSphere = sphere.center + manifold.normal * sphere.radius;
            const Vec3 pointOnBox = sphere.center + manifold.normal * distanceToFace;
            manifold.point = (pointOnSphere + pointOnBox) * 0.5f;
            return manifold;
        }

        ContactManifold Intersect(const CollisionBody& a, const CollisionBody& b)
        {
            if (a.collider == nullptr || b.collider == nullptr)
            {
                return {};
            }

            if (a.collider->type == ColliderType::Box && b.collider->type == ColliderType::Box)
            {
                return IntersectAabbAabb(a.aabb, b.aabb);
            }

            if (a.collider->type == ColliderType::Sphere && b.collider->type == ColliderType::Sphere)
            {
                return IntersectSphereSphere(a.sphere, b.sphere);
            }

            if (a.collider->type == ColliderType::Sphere && b.collider->type == ColliderType::Box)
            {
                return IntersectSphereAabb(a.sphere, b.aabb);
            }

            if (a.collider->type == ColliderType::Box && b.collider->type == ColliderType::Sphere)
            {
                ContactManifold manifold = IntersectSphereAabb(b.sphere, a.aabb);
                if (manifold.hasCollision)
                {
                    manifold.normal = -manifold.normal;
                }
                return manifold;
            }

            return {};
        }

        void StabilizeGroundedVelocity(RigidbodyComponent& rigidbody)
        {
            if (std::abs(rigidbody.velocity.y) < kGroundSnapVelocity)
            {
                rigidbody.velocity.y = 0.0f;
            }

            const float horizontalSpeedSq =
                rigidbody.velocity.x * rigidbody.velocity.x +
                rigidbody.velocity.z * rigidbody.velocity.z;
            const float sleepSpeedSq = rigidbody.sleepLinearSpeed * rigidbody.sleepLinearSpeed;
            if (horizontalSpeedSq <= sleepSpeedSq)
            {
                rigidbody.velocity.x = 0.0f;
                rigidbody.velocity.z = 0.0f;
            }
        }

        float ResolveCollision(CollisionBody& a, CollisionBody& b, const ContactManifold& manifold)
        {
            if (a.transform == nullptr || b.transform == nullptr)
            {
                return 0.0f;
            }

            const float inverseMassSum = a.inverseMass + b.inverseMass;
            if (inverseMassSum <= kCollisionEpsilon)
            {
                return 0.0f;
            }

            const float correctionMagnitude =
                std::max(manifold.penetration - kPositionCorrectionSlop, 0.0f) * kPositionCorrectionPercent / inverseMassSum;
            const Vec3 correction = manifold.normal * correctionMagnitude;

            if (a.inverseMass > 0.0f)
            {
                a.transform->position -= correction * a.inverseMass;
                RefreshCollisionBodyBounds(a);
            }

            if (b.inverseMass > 0.0f)
            {
                b.transform->position += correction * b.inverseMass;
                RefreshCollisionBodyBounds(b);
            }

            if (a.rigidbody == nullptr && b.rigidbody == nullptr)
            {
                return 0.0f;
            }

            const Vec3 velocityA = a.rigidbody != nullptr ? a.rigidbody->velocity : Vec3{};
            const Vec3 velocityB = b.rigidbody != nullptr ? b.rigidbody->velocity : Vec3{};
            const Vec3 relativeVelocity = velocityB - velocityA;
            const float velocityAlongNormal = Dot(relativeVelocity, manifold.normal);
            if (velocityAlongNormal > 0.0f)
            {
                return 0.0f;
            }

            const float restitutionA = a.collider != nullptr ? a.collider->bounciness : 0.0f;
            const float restitutionB = b.collider != nullptr ? b.collider->bounciness : 0.0f;
            float restitution = std::max(restitutionA, restitutionB);
            if (-velocityAlongNormal < kBounceVelocityThreshold)
            {
                restitution = 0.0f;
            }

            const float impulseScalar = -(1.0f + restitution) * velocityAlongNormal / inverseMassSum;
            const Vec3 impulse = manifold.normal * impulseScalar;

            if (a.rigidbody != nullptr && a.inverseMass > 0.0f)
            {
                a.rigidbody->velocity -= impulse * a.inverseMass;
            }

            if (b.rigidbody != nullptr && b.inverseMass > 0.0f)
            {
                b.rigidbody->velocity += impulse * b.inverseMass;
            }

            Vec3 postVelocityA = a.rigidbody != nullptr ? a.rigidbody->velocity : Vec3{};
            Vec3 postVelocityB = b.rigidbody != nullptr ? b.rigidbody->velocity : Vec3{};
            Vec3 tangent = postVelocityB - postVelocityA - manifold.normal * Dot(postVelocityB - postVelocityA, manifold.normal);
            const float tangentLengthSq = LengthSquared(tangent);
            if (tangentLengthSq <= kCollisionEpsilon)
            {
                if (a.rigidbody != nullptr && -manifold.normal.y > kGroundNormalThreshold)
                {
                    a.rigidbody->isGrounded = true;
                    StabilizeGroundedVelocity(*a.rigidbody);
                }

                if (b.rigidbody != nullptr && manifold.normal.y > kGroundNormalThreshold)
                {
                    b.rigidbody->isGrounded = true;
                    StabilizeGroundedVelocity(*b.rigidbody);
                }

                return impulseScalar;
            }

            tangent = tangent / std::sqrt(tangentLengthSq);
            const float frictionA = a.collider != nullptr ? a.collider->friction : 0.0f;
            const float frictionB = b.collider != nullptr ? b.collider->friction : 0.0f;
            const float friction = std::sqrt(std::max(frictionA, 0.0f) * std::max(frictionB, 0.0f));
            const float tangentImpulseScalar = -Dot(postVelocityB - postVelocityA, tangent) / inverseMassSum;
            const float frictionLimit = impulseScalar * friction;
            const float clampedFrictionImpulse = std::clamp(tangentImpulseScalar, -frictionLimit, frictionLimit);
            const Vec3 frictionImpulse = tangent * clampedFrictionImpulse;

            if (a.rigidbody != nullptr && a.inverseMass > 0.0f)
            {
                a.rigidbody->velocity -= frictionImpulse * a.inverseMass;
            }

            if (b.rigidbody != nullptr && b.inverseMass > 0.0f)
            {
                b.rigidbody->velocity += frictionImpulse * b.inverseMass;
            }

            if (a.rigidbody != nullptr && -manifold.normal.y > kGroundNormalThreshold)
            {
                a.rigidbody->isGrounded = true;
                StabilizeGroundedVelocity(*a.rigidbody);
            }

            if (b.rigidbody != nullptr && manifold.normal.y > kGroundNormalThreshold)
            {
                b.rigidbody->isGrounded = true;
                StabilizeGroundedVelocity(*b.rigidbody);
            }

            return impulseScalar;
        }

        void RebuildDebugGeometry(World& world, physics::PhysicsWorldState& state)
        {
            state.debugAabbs.clear();
            state.debugSpheres.clear();

            world.ForEach<TransformComponent, ColliderComponent>(
                [&](const EntityId, TransformComponent& transform, ColliderComponent& collider)
                {
                    const core::Color triggerColor{0.96f, 0.42f, 0.88f, 1.0f};
                    const core::Color solidColor{0.17f, 0.80f, 0.95f, 1.0f};

                    if (collider.type == ColliderType::Box)
                    {
                        const WorldAabb aabb = BuildAabb(transform, collider);
                        state.debugAabbs.push_back({aabb.min, aabb.max, collider.isTrigger ? triggerColor : solidColor});
                    }
                    else
                    {
                        const WorldSphere sphere = BuildSphere(transform, collider);
                        state.debugSpheres.push_back({sphere.center, sphere.radius, collider.isTrigger ? triggerColor : solidColor});
                    }
                });
        }
    }

    void PhysicsSystem::Update(World& world, const float deltaTime)
    {
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        physicsState.stats = {};
        physicsState.debugVectors.clear();

        if (physicsState.physicsPaused)
        {
            RebuildDebugGeometry(world, physicsState);
            return;
        }

        const float fixedTimeStep = std::max(physicsState.fixedTimeStep, 1.0f / 240.0f);
        accumulator_ = std::min(accumulator_ + deltaTime, fixedTimeStep * 8.0f);

        while (accumulator_ >= fixedTimeStep)
        {
            accumulator_ -= fixedTimeStep;
            physicsState.stats.fixedStepCount += 1;
            physicsState.stats.rigidbodyCount = 0;
            physicsState.stats.broadPhasePairs = 0;
            physicsState.stats.collisionPairs = 0;
            physicsState.stats.triggerPairs = 0;

            world.ForEach<TransformComponent, RigidbodyComponent>(
                [&](const EntityId, TransformComponent& transform, RigidbodyComponent& rigidbody)
                {
                    rigidbody.isGrounded = false;
                    if (rigidbody.isKinematic || rigidbody.mass <= 0.0f)
                    {
                        return;
                    }

                    Vec3 acceleration = rigidbody.acceleration;
                    if (rigidbody.useGravity)
                    {
                        acceleration.y -= physicsState.gravityStrength * rigidbody.gravityScale;
                    }

                    rigidbody.velocity += acceleration * fixedTimeStep;
                    const float dampingFactor = std::clamp(1.0f - rigidbody.linearDamping * fixedTimeStep, 0.0f, 1.0f);
                    rigidbody.velocity *= dampingFactor;

                    transform.position += rigidbody.velocity * fixedTimeStep;
                    physicsState.stats.rigidbodyCount += 1;
                });

            std::vector<CollisionBody> bodies;
            bodies.reserve(world.GetEntities().size());
            std::unordered_map<EntityId, std::size_t> bodyIndexByEntity;
            bodyIndexByEntity.reserve(world.GetEntities().size());
            spatial::UniformGrid3D broadPhaseGrid(1.4f);

            world.ForEach<TransformComponent, ColliderComponent>(
                [&](const EntityId entity, TransformComponent& transform, ColliderComponent& collider)
                {
                    CollisionBody body;
                    body.entity = entity;
                    body.transform = &transform;
                    body.collider = &collider;
                    body.rigidbody = world.TryGet<RigidbodyComponent>(entity);
                    body.inverseMass = ComputeInverseMass(body.rigidbody);
                    body.aabb = BuildAabb(transform, collider);
                    body.sphere = BuildSphere(transform, collider);
                    broadPhaseGrid.Insert(entity, body.aabb.min.x, body.aabb.min.y, body.aabb.min.z, body.aabb.max.x, body.aabb.max.y, body.aabb.max.z);
                    bodyIndexByEntity[entity] = bodies.size();
                    bodies.push_back(body);
                });

            std::unordered_set<std::uint64_t> currentCollisionPairs;
            std::unordered_set<std::uint64_t> currentTriggerPairs;
            const auto candidatePairs = broadPhaseGrid.BuildCandidatePairs();
            physicsState.stats.broadPhasePairs = static_cast<std::uint32_t>(candidatePairs.size());

            for (std::uint32_t solverIteration = 0; solverIteration < kSolverIterations; ++solverIteration)
            {
                const bool collectContactState = (solverIteration == 0);

                for (const auto& [entityA, entityB] : candidatePairs)
                {
                    const auto bodyAIndexIt = bodyIndexByEntity.find(entityA);
                    const auto bodyBIndexIt = bodyIndexByEntity.find(entityB);
                    if (bodyAIndexIt == bodyIndexByEntity.end() || bodyBIndexIt == bodyIndexByEntity.end())
                    {
                        continue;
                    }

                    CollisionBody& bodyA = bodies[bodyAIndexIt->second];
                    CollisionBody& bodyB = bodies[bodyBIndexIt->second];
                    const ContactManifold manifold = Intersect(bodyA, bodyB);
                    if (!manifold.hasCollision)
                    {
                        continue;
                    }

                    const std::uint64_t pairKey = MakePairKey(bodyA.entity, bodyB.entity);

                    if ((bodyA.collider != nullptr && bodyA.collider->isTrigger) || (bodyB.collider != nullptr && bodyB.collider->isTrigger))
                    {
                        if (!collectContactState)
                        {
                            continue;
                        }

                        currentTriggerPairs.insert(pairKey);
                        physicsState.stats.triggerPairs += 1;

                        if (activeTriggerPairs_.find(pairKey) == activeTriggerPairs_.end())
                        {
                            if (bodyA.collider != nullptr && bodyA.collider->isTrigger)
                            {
                                core::ServiceLocator::GetEventBus().Publish(physics::TriggerEvent{bodyA.entity, bodyB.entity, manifold.point});
                            }
                            if (bodyB.collider != nullptr && bodyB.collider->isTrigger)
                            {
                                core::ServiceLocator::GetEventBus().Publish(physics::TriggerEvent{bodyB.entity, bodyA.entity, manifold.point});
                            }
                        }

                        continue;
                    }

                    const float normalImpulse = ResolveCollision(bodyA, bodyB, manifold);
                    if (!collectContactState)
                    {
                        continue;
                    }

                    currentCollisionPairs.insert(pairKey);
                    physicsState.stats.collisionPairs += 1;
                    physicsState.debugVectors.push_back({manifold.point, manifold.point + manifold.normal * 0.45f, core::Color{1.0f, 0.35f, 0.25f, 1.0f}});

                    if (activeCollisionPairs_.find(pairKey) == activeCollisionPairs_.end() &&
                        normalImpulse >= kCollisionEventImpulseThreshold)
                    {
                        core::ServiceLocator::GetEventBus().Publish(physics::CollisionEvent{
                            bodyA.entity,
                            bodyB.entity,
                            manifold.point,
                            manifold.normal,
                            normalImpulse,
                        });
                    }
                }
            }

            activeCollisionPairs_ = std::move(currentCollisionPairs);
            activeTriggerPairs_ = std::move(currentTriggerPairs);
        }

        RebuildDebugGeometry(world, physicsState);
    }
}