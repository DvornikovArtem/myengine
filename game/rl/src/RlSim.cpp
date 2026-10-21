// RlSim.cpp

#include <rl/RlSim.h>

#include "RlCarController.h"
#include "RlJolt.h"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace rl
{
    namespace
    {
        using namespace detail;

        constexpr uint64_t kUserDataBall = 100;
        constexpr uint64_t kUserDataArena = 200;
        constexpr uint32_t kSnapshotMagic = 0x31534C52;  // "RLS1"
        constexpr float kParkY = -60.0f;

        static_assert(std::is_trivially_copyable_v<CarRuntime>, "the car runtime is copied as bytes");

        RlVec3 ToRl(JPH::Vec3 v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
        RlQuat ToRl(JPH::Quat q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

        float Interp(const float* xs, const float* ys, int count, float x)
        {
            if (x <= xs[0])
            {
                return ys[0];
            }
            for (int i = 1; i < count; ++i)
            {
                if (x <= xs[i])
                {
                    return ys[i - 1] + (ys[i] - ys[i - 1]) * ((x - xs[i - 1]) / std::max(xs[i] - xs[i - 1], 1e-6f));
                }
            }
            return ys[count - 1];
        }

        // Contact callbacks may come in any order from any thread; the listener only writes them down, Step sorts and applies them.
        class Listener final : public JPH::ContactListener
        {
        public:
            enum Kind : int
            {
                BallCar = 1,
                CarCar = 2
            };

            struct Event
            {
                int kind = 0;
                int a = 0;  // car (the lower index for CarCar)
                int b = 0;  // the other car for CarCar
                JPH::Vec3 point = JPH::Vec3::sZero();
                JPH::Vec3 velocityA = JPH::Vec3::sZero();  // ball velocity for BallCar
                JPH::Vec3 velocityB = JPH::Vec3::sZero();
                JPH::Vec3 positionA = JPH::Vec3::sZero();
                JPH::Vec3 positionB = JPH::Vec3::sZero();
                JPH::Quat carRotation = JPH::Quat::sIdentity();  // of the car in a BallCar event
            };

            void Clear()
            {
                std::lock_guard<std::mutex> lock(mutex_);
                events_.clear();
                arenaTouch_.fill(false);
                arenaNormal_.fill(JPH::Vec3::sZero());
            }

            void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override
            {
                Record(b1, b2, manifold, true);
            }

            void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override
            {
                Record(b1, b2, manifold, false);
            }

            std::vector<Event> TakeEvents(std::array<bool, kMaxCars>& arenaTouch, std::array<JPH::Vec3, kMaxCars>& arenaNormal)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                arenaTouch = arenaTouch_;
                arenaNormal = arenaNormal_;
                std::vector<Event> sorted = events_;
                // insertion sort: the lists are a handful of entries and std::sort differs between standard libraries
                for (size_t i = 1; i < sorted.size(); ++i)
                {
                    Event e = sorted[i];
                    size_t j = i;
                    while (j > 0 && Less(e, sorted[j - 1]))
                    {
                        sorted[j] = sorted[j - 1];
                        --j;
                    }
                    sorted[j] = e;
                }
                return sorted;
            }

        private:
            static bool Less(const Event& l, const Event& r)
            {
                if (l.kind != r.kind)
                {
                    return l.kind < r.kind;
                }
                if (l.a != r.a)
                {
                    return l.a < r.a;
                }
                return l.b < r.b;
            }

            static bool IsCar(const JPH::Body& body) { return body.GetUserData() < static_cast<uint64_t>(kMaxCars); }

            void Record(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& manifold, bool added)
            {
                const uint64_t u1 = b1.GetUserData();
                const uint64_t u2 = b2.GetUserData();
                std::lock_guard<std::mutex> lock(mutex_);
                if ((u1 == kUserDataArena && IsCar(b2)) || (u2 == kUserDataArena && IsCar(b1)))
                {
                    const size_t car = static_cast<size_t>(IsCar(b1) ? u1 : u2);
                    arenaTouch_[car] = true;
                    // the manifold normal runs from body 1 to body 2: from the arena to the car when the arena is body 1
                    arenaNormal_[car] += (u1 == kUserDataArena) ? manifold.mWorldSpaceNormal : -manifold.mWorldSpaceNormal;
                    return;
                }
                if (!added)
                {
                    return;
                }
                Event e;
                e.point = JPH::Vec3(manifold.GetWorldSpaceContactPointOn1(0));
                if ((u1 == kUserDataBall && IsCar(b2)) || (u2 == kUserDataBall && IsCar(b1)))
                {
                    const JPH::Body& ball = u1 == kUserDataBall ? b1 : b2;
                    const JPH::Body& car = u1 == kUserDataBall ? b2 : b1;
                    e.kind = BallCar;
                    e.a = static_cast<int>(car.GetUserData());
                    e.velocityA = ball.GetLinearVelocity();
                    e.velocityB = car.GetLinearVelocity();
                    e.positionA = JPH::Vec3(ball.GetPosition());
                    e.positionB = JPH::Vec3(car.GetPosition());
                    e.carRotation = car.GetRotation();
                    events_.push_back(e);
                }
                else if (IsCar(b1) && IsCar(b2))
                {
                    const JPH::Body& lo = u1 < u2 ? b1 : b2;
                    const JPH::Body& hi = u1 < u2 ? b2 : b1;
                    e.kind = CarCar;
                    e.a = static_cast<int>(lo.GetUserData());
                    e.b = static_cast<int>(hi.GetUserData());
                    e.velocityA = lo.GetLinearVelocity();
                    e.velocityB = hi.GetLinearVelocity();
                    e.positionA = JPH::Vec3(lo.GetPosition());
                    e.positionB = JPH::Vec3(hi.GetPosition());
                    events_.push_back(e);
                }
            }

            std::mutex mutex_;
            std::vector<Event> events_;
            std::array<bool, kMaxCars> arenaTouch_{};
            std::array<JPH::Vec3, kMaxCars> arenaNormal_{};
        };
    }

    struct RlSim::Impl
    {
        // freed last: the Jolt globals must outlive the physics system below
        struct JoltGuard
        {
            JoltGuard() { JoltAcquire(); }
            ~JoltGuard() { JoltRelease(); }
        } guard;

        RlTuning tuning;
        RlArena arena;
        int carCount;

        LayerPairFilter pairFilter;
        BroadPhaseLayerMap broadPhaseMap;
        ObjectVsBroadPhaseFilter objectVsBroadPhase;
        std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
        std::unique_ptr<JPH::JobSystemSingleThreaded> jobSystem;
        JPH::PhysicsSystem physics;
        Listener listener;

        JPH::RefConst<JPH::Shape> arenaShape;
        JPH::BodyID arenaId;
        JPH::BodyID ballId;
        JPH::BodyID carIds[kMaxCars];

        CarRuntime runtimes[kMaxCars];
        CarFrame frames[kMaxCars];
        uint32_t tick = 0;
        int lastToucher = -1;

        RlWorldState state;
        RlWorldState previous;
        RlEvents events;

        Impl(const RlTuning& t, int cars)
            : tuning(t)
            , arena(t.arena)
            , carCount(std::clamp(cars, 0, kMaxCars))
        {
            tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
            jobSystem = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
            physics.Init(32, 0, 1024, 1024, broadPhaseMap, objectVsBroadPhase, pairFilter);
            physics.SetGravity(JPH::Vec3(0.0f, -tuning.gravity, 0.0f));
            physics.SetContactListener(&listener);
            JPH::PhysicsSettings settings = physics.GetPhysicsSettings();
            settings.mAllowSleeping = false;
            settings.mNumVelocitySteps = 10;
            settings.mNumPositionSteps = 2;
            settings.mMinVelocityForRestitution = 0.5f;
            physics.SetPhysicsSettings(settings);

            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            CreateArena(bodies);
            CreateBall(bodies);
            for (int i = 0; i < carCount; ++i)
            {
                CreateCar(bodies, i);
            }
            physics.OptimizeBroadPhase();

            std::memset(runtimes, 0, sizeof(runtimes));
            for (int i = 0; i < carCount; ++i)
            {
                ResetCar(i, 0.0f, 0.0f, 0.0f);
            }
            ResetBall({0.0f, 3.0f, 0.0f}, {}, {});
            FillState();
            previous = state;
        }

        ~Impl()
        {
            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            for (int i = 0; i < carCount; ++i)
            {
                bodies.RemoveBody(carIds[i]);
                bodies.DestroyBody(carIds[i]);
            }
            bodies.RemoveBody(ballId);
            bodies.DestroyBody(ballId);
            bodies.RemoveBody(arenaId);
            bodies.DestroyBody(arenaId);
        }

        void CreateArena(JPH::BodyInterface& bodies)
        {
            JPH::VertexList vertices;
            for (const RlVec3& v : arena.Vertices())
            {
                vertices.push_back(JPH::Float3(v.x, v.y, v.z));
            }
            JPH::IndexedTriangleList triangles;
            const std::vector<uint32_t>& indices = arena.Indices();
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2], 0));
            }
            JPH::MeshShapeSettings meshSettings(vertices, triangles);
            meshSettings.SetEmbedded();
            const JPH::ShapeSettings::ShapeResult result = meshSettings.Create();
            arenaShape = result.Get();

            JPH::BodyCreationSettings settings(arenaShape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::kStatic);
            settings.mFriction = tuning.arena.friction;
            settings.mRestitution = tuning.arena.restitution;
            settings.mUserData = kUserDataArena;
            arenaId = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        }

        void CreateBall(JPH::BodyInterface& bodies)
        {
            const RlBallTuning& b = tuning.ball;
            JPH::BodyCreationSettings settings(new JPH::SphereShape(b.radius), JPH::RVec3(0.0f, 3.0f, 0.0f), JPH::Quat::sIdentity(),
                                               JPH::EMotionType::Dynamic, Layers::kBall);
            settings.mAllowSleeping = false;
            settings.mFriction = b.friction;
            settings.mRestitution = b.restitution;
            settings.mLinearDamping = b.linearDamping;
            settings.mAngularDamping = b.angularDamping;
            settings.mMaxLinearVelocity = b.maxSpeed * 2.0f;
            settings.mMaxAngularVelocity = b.maxAngularSpeed;
            settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
            settings.mEnhancedInternalEdgeRemoval = true;
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = b.mass;
            settings.mUserData = kUserDataBall;
            ballId = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
        }

        void CreateCar(JPH::BodyInterface& bodies, int index)
        {
            const RlCarTuning& c = tuning.car;
            // The origin of the body is the centre of the hitbox; the centre of mass is below and behind it (RL: the hitbox is offset
            // from the origin of the car), with the inertia of the box itself.
            JPH::RefConst<JPH::Shape> box = new JPH::BoxShape(JPH::Vec3(c.halfExtentX, c.halfExtentY, c.halfExtentZ), 0.02f);
            JPH::RefConst<JPH::Shape> shape = new JPH::OffsetCenterOfMassShape(box, JPH::Vec3(0.0f, c.comOffsetY, c.comOffsetZ));
            JPH::BodyCreationSettings settings(shape, JPH::RVec3(0.0f, 1.0f, 0.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::kCar);
            settings.mAllowSleeping = false;
            settings.mFriction = c.friction;
            settings.mRestitution = c.restitution;
            settings.mLinearDamping = 0.0f;
            settings.mAngularDamping = 0.0f;
            settings.mMaxLinearVelocity = c.maxSpeed * 2.0f;
            settings.mMaxAngularVelocity = c.maxAngularSpeed;
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            settings.mMassPropertiesOverride = CarMassProperties(c);
            settings.mUserData = static_cast<uint64_t>(index);
            carIds[index] = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
        }

        static JPH::MassProperties CarMassProperties(const RlCarTuning& c)
        {
            const float w = 2.0f * c.halfExtentX;
            const float h = 2.0f * c.halfExtentY;
            const float l = 2.0f * c.halfExtentZ;
            JPH::MassProperties mp;
            mp.mMass = c.mass;
            mp.mInertia = JPH::Mat44::sScale(JPH::Vec3(c.mass / 12.0f * (h * h + l * l), c.mass / 12.0f * (w * w + l * l), c.mass / 12.0f * (w * w + h * h)) *
                                             c.inertiaMultiplier);
            return mp;
        }

        // Live change of the numbers; the ones built into the bodies stay as they were.
        void SetTuning(const RlTuning& t)
        {
            RlTuning next = t;
            next.arena = tuning.arena;
            next.car.mass = tuning.car.mass;
            next.car.halfExtentX = tuning.car.halfExtentX;
            next.car.halfExtentY = tuning.car.halfExtentY;
            next.car.halfExtentZ = tuning.car.halfExtentZ;
            next.car.comOffsetY = tuning.car.comOffsetY;
            next.car.comOffsetZ = tuning.car.comOffsetZ;
            next.car.inertiaMultiplier = tuning.car.inertiaMultiplier;
            next.ball.radius = tuning.ball.radius;
            next.ball.mass = tuning.ball.mass;
            tuning = next;
            physics.SetGravity(JPH::Vec3(0.0f, -tuning.gravity, 0.0f));
            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            bodies.SetFriction(arenaId, tuning.arena.friction);
            bodies.SetRestitution(arenaId, tuning.arena.restitution);
            bodies.SetFriction(ballId, tuning.ball.friction);
            bodies.SetRestitution(ballId, tuning.ball.restitution);
            for (int i = 0; i < carCount; ++i)
            {
                bodies.SetFriction(carIds[i], tuning.car.friction);
                bodies.SetRestitution(carIds[i], tuning.car.restitution);
            }
        }

        void ResetCar(int index, float x, float z, float yaw)
        {
            CarRuntime& rt = runtimes[index];
            std::memset(&rt, 0, sizeof(rt));
            rt.boost = tuning.car.boostStart;
            rt.jumpTime = 1000.0f;
            rt.gripScale = 1.0f;
            rt.spawnX = x;
            rt.spawnY = tuning.car.spawnHeight;
            rt.spawnZ = z;
            rt.spawnYaw = yaw;
            PlaceCar(index);
        }

        void PlaceCar(int index)
        {
            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            const CarRuntime& rt = runtimes[index];
            bodies.SetObjectLayer(carIds[index], Layers::kCar);
            bodies.SetPositionAndRotation(carIds[index], JPH::RVec3(rt.spawnX, rt.spawnY, rt.spawnZ),
                                          JPH::Quat::sRotation(JPH::Vec3::sAxisY(), rt.spawnYaw), JPH::EActivation::Activate);
            bodies.SetLinearAndAngularVelocity(carIds[index], JPH::Vec3::sZero(), JPH::Vec3::sZero());
        }

        void ResetBall(RlVec3 position, RlVec3 velocity, RlVec3 angularVelocity)
        {
            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            JPH::Vec3 linear(velocity.x, velocity.y, velocity.z);
            if (linear.Length() > tuning.ball.maxSpeed)
            {
                linear = linear.Normalized() * tuning.ball.maxSpeed;
            }
            bodies.SetPositionAndRotation(ballId, JPH::RVec3(position.x, position.y, position.z), JPH::Quat::sIdentity(), JPH::EActivation::Activate);
            bodies.SetLinearAndAngularVelocity(ballId, linear, JPH::Vec3(angularVelocity.x, angularVelocity.y, angularVelocity.z));
            lastToucher = -1;
        }

        void Demolish(int index)
        {
            CarRuntime& rt = runtimes[index];
            rt.demolished = 1;
            rt.respawnTimer = tuning.car.respawnDelay;
            rt.boost = 0.0f;
            rt.dodging = 0;
            physics.GetBodyInterfaceNoLock().SetObjectLayer(carIds[index], Layers::kGhost);
            ++events.demolitions;
        }

        void Park(int index)
        {
            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            bodies.SetPositionAndRotation(carIds[index], JPH::RVec3(-30.0f + 8.0f * static_cast<float>(index), kParkY, 0.0f), JPH::Quat::sIdentity(),
                                          JPH::EActivation::DontActivate);
            bodies.SetLinearAndAngularVelocity(carIds[index], JPH::Vec3::sZero(), JPH::Vec3::sZero());
        }

        void Respawn(int index)
        {
            CarRuntime& rt = runtimes[index];
            rt.demolished = 0;
            rt.respawnTimer = 0.0f;
            rt.boost = tuning.car.boostStart;
            rt.jumpUsed = 0;
            rt.secondUsed = 0;
            rt.airTime = 0.0f;
            rt.jumpTime = 1000.0f;
            PlaceCar(index);
        }

        void Step(const RlCarInput* inputs)
        {
            ZoneScopedN("Rl::Step");
            previous = state;
            events = {};
            const JPH::BodyLockInterfaceNoLock& locks = physics.GetBodyLockInterfaceNoLock();

            {
                ZoneScopedN("Rl::Control");
                const CarContext context{physics, tuning, arenaShape};
                for (int i = 0; i < carCount; ++i)
                {
                    CarRuntime& rt = runtimes[i];
                    frames[i] = CarFrame{};
                    if (rt.demolished != 0)
                    {
                        rt.respawnTimer -= kTickDt;
                        if (rt.respawnTimer <= 0.0f)
                        {
                            Respawn(i);
                        }
                        else
                        {
                            Park(i);
                            continue;
                        }
                    }
                    JPH::Body* body = locks.TryGetBody(carIds[i]);
                    ControlCar(context, *body, inputs[i], rt, frames[i]);
                }
            }

            listener.Clear();
            {
                ZoneScopedN("Rl::Physics");
                physics.Update(kTickDt, 1, tempAllocator.get(), jobSystem.get());
            }

            std::array<bool, kMaxCars> arenaTouch{};
            std::array<JPH::Vec3, kMaxCars> arenaNormal{};
            const std::vector<Listener::Event> contacts = listener.TakeEvents(arenaTouch, arenaNormal);
            for (int i = 0; i < carCount; ++i)
            {
                runtimes[i].bodyContact = arenaTouch[static_cast<size_t>(i)] ? 1 : 0;
                if (runtimes[i].bodyContact != 0)
                {
                    const JPH::Vec3 n = arenaNormal[static_cast<size_t>(i)];
                    const float length = n.Length();
                    const JPH::Vec3 unit = length > 1e-4f ? n / length : JPH::Vec3::sAxisY();
                    runtimes[i].bodyNormalX = unit.GetX();
                    runtimes[i].bodyNormalY = unit.GetY();
                    runtimes[i].bodyNormalZ = unit.GetZ();
                }
            }
            ApplyContacts(contacts);
            LimitBall();
            RecoverEscaped();

            ++tick;
            FillState();
        }

        void ApplyContacts(const std::vector<Listener::Event>& contacts)
        {
            const RlBallTuning& b = tuning.ball;
            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            int lastCar = -1;
            int lastA = -1;
            int lastB = -1;
            for (const Listener::Event& e : contacts)
            {
                if (e.kind == Listener::BallCar)
                {
                    if (e.a == lastCar || runtimes[e.a].demolished != 0)
                    {
                        continue;
                    }
                    lastCar = e.a;
                    CarRuntime& rt = runtimes[e.a];
                    lastToucher = e.a;
                    if (rt.ballHitCooldown > 0)
                    {
                        continue;
                    }
                    rt.ballHitCooldown = b.hitCooldownTicks;
                    // RL pushes the ball along the line from the hit point to its centre, flattened vertically
                    JPH::Vec3 direction = e.positionA - e.point;
                    direction.SetY(direction.GetY() * b.hitNormalYScale);
                    const float length = direction.Length();
                    if (length < 1e-4f)
                    {
                        continue;
                    }
                    direction /= length;
                    float relative = (e.velocityA - e.velocityB).Length();
                    float scale = Interp(b.hitSpeed, b.hitScale, 4, relative);
                    // where on the car the ball was hit and how: the roof pushes less, a hit during a dodge pushes more
                    const JPH::Vec3 local = e.carRotation.Conjugated() * (e.point - e.positionB);
                    if (local.GetY() > 0.12f && local.GetZ() < 0.4f)
                    {
                        scale *= b.roofHitScale;
                    }
                    if (rt.dodging != 0)
                    {
                        scale *= b.dodgeHitScale;
                    }
                    JPH::Vec3 velocity = bodies.GetLinearVelocity(ballId) + direction * (relative * scale);
                    bodies.SetLinearVelocity(ballId, velocity);
                    ++events.ballHits;
                }
                else if (e.kind == Listener::CarCar)
                {
                    if (e.a == lastA && e.b == lastB)
                    {
                        continue;
                    }
                    lastA = e.a;
                    lastB = e.b;
                    const float supersonic = tuning.car.supersonicSpeed;
                    const JPH::Vec3 ab = e.positionB - e.positionA;
                    const bool aHits = e.velocityA.Length() >= supersonic && e.velocityA.Dot(ab) > 0.0f;
                    const bool bHits = e.velocityB.Length() >= supersonic && e.velocityB.Dot(-ab) > 0.0f;
                    if (aHits && runtimes[e.b].demolished == 0)
                    {
                        Demolish(e.b);
                    }
                    if (bHits && runtimes[e.a].demolished == 0)
                    {
                        Demolish(e.a);
                    }
                }
            }
        }

        void LimitBall()
        {
            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            const JPH::Vec3 velocity = bodies.GetLinearVelocity(ballId);
            const float speed = velocity.Length();
            if (speed > tuning.ball.maxSpeed)
            {
                bodies.SetLinearVelocity(ballId, velocity * (tuning.ball.maxSpeed / speed));
            }
        }

        // Nothing should leave the shell; if something does (a tunnelled body) it goes back instead of falling for ever.
        void RecoverEscaped()
        {
            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            const RlVec3 lo = arena.BoundsMin();
            const RlVec3 hi = arena.BoundsMax();
            const auto outside = [&](JPH::Vec3 p) {
                return p.GetX() < lo.x - 1.0f || p.GetX() > hi.x + 1.0f || p.GetY() < lo.y - 1.0f || p.GetY() > hi.y + 1.0f || p.GetZ() < lo.z - 1.0f ||
                       p.GetZ() > hi.z + 1.0f;
            };
            if (outside(JPH::Vec3(bodies.GetPosition(ballId))))
            {
                ResetBall({0.0f, 5.0f, 0.0f}, {}, {});
            }
            for (int i = 0; i < carCount; ++i)
            {
                if (runtimes[i].demolished == 0 && outside(JPH::Vec3(bodies.GetPosition(carIds[i]))))
                {
                    PlaceCar(i);
                }
            }
        }

        void FillState()
        {
            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            state.tick = tick;
            state.carCount = carCount;
            state.lastBallToucher = lastToucher;
            const float supersonic = tuning.car.supersonicSpeed;
            for (int i = 0; i < carCount; ++i)
            {
                RlCarState& s = state.cars[i];
                const CarRuntime& rt = runtimes[i];
                const JPH::Vec3 velocity = bodies.GetLinearVelocity(carIds[i]);
                s.position = ToRl(JPH::Vec3(bodies.GetPosition(carIds[i])));
                s.rotation = ToRl(bodies.GetRotation(carIds[i]));
                s.linearVelocity = ToRl(velocity);
                s.angularVelocity = ToRl(bodies.GetAngularVelocity(carIds[i]));
                s.boost = rt.boost;
                s.wheelContacts = frames[i].wheelContacts;
                s.onGround = frames[i].wheelContacts > 0;
                s.demolished = rt.demolished != 0;
                s.supersonic = !s.demolished && velocity.Length() >= supersonic;
                s.jumped = rt.jumpUsed != 0;
                s.canDodge = frames[i].wheelContacts < 3 && rt.secondUsed == 0 && rt.airTime < tuning.car.dodgeWindow;
                s.dodging = rt.dodging != 0;
                s.respawnTimer = rt.respawnTimer;
                for (int w = 0; w < kWheelCount; ++w)
                {
                    s.wheelCompression[w] = frames[i].wheelCompression[w];
                }
                s.surfaceNormal = ToRl(frames[i].surfaceNormal);
            }
            RlBallState& ball = state.ball;
            ball.position = ToRl(JPH::Vec3(bodies.GetPosition(ballId)));
            ball.rotation = ToRl(bodies.GetRotation(ballId));
            ball.linearVelocity = ToRl(bodies.GetLinearVelocity(ballId));
            ball.angularVelocity = ToRl(bodies.GetAngularVelocity(ballId));
        }

        struct FrameSave
        {
            int32_t wheelContacts;
            float compression[kWheelCount];
            float normal[3];
        };

        struct SnapshotHeader
        {
            uint32_t magic;
            uint32_t tick;
            int32_t carCount;
            int32_t lastToucher;
        };

        void Save(std::vector<uint8_t>& out) const
        {
            FlatRecorder recorder;
            recorder.Bytes().swap(out);
            recorder.Bytes().clear();
            SnapshotHeader header{kSnapshotMagic, tick, carCount, lastToucher};
            recorder.Put(header);
            recorder.WriteBytes(runtimes, sizeof(CarRuntime) * static_cast<size_t>(carCount));
            // what the last tick measured under the wheels: the state shows it and a bot reads it
            for (int i = 0; i < carCount; ++i)
            {
                FrameSave frame{};
                frame.wheelContacts = frames[i].wheelContacts;
                for (int w = 0; w < kWheelCount; ++w)
                {
                    frame.compression[w] = frames[i].wheelCompression[w];
                }
                frame.normal[0] = frames[i].surfaceNormal.GetX();
                frame.normal[1] = frames[i].surfaceNormal.GetY();
                frame.normal[2] = frames[i].surfaceNormal.GetZ();
                recorder.Put(frame);
            }
            physics.SaveState(recorder);
            recorder.Bytes().swap(out);
        }

        bool Restore(const std::vector<uint8_t>& in)
        {
            FlatRecorder recorder;
            recorder.Bytes() = in;
            recorder.BeginRead();
            SnapshotHeader header{};
            recorder.Get(header);
            if (!recorder.ReadOk() || header.magic != kSnapshotMagic || header.carCount != carCount)
            {
                return false;
            }
            recorder.ReadBytes(runtimes, sizeof(CarRuntime) * static_cast<size_t>(carCount));
            FrameSave saved[kMaxCars] = {};
            for (int i = 0; i < carCount; ++i)
            {
                recorder.Get(saved[i]);
            }
            if (!physics.RestoreState(recorder))
            {
                return false;
            }
            tick = header.tick;
            lastToucher = header.lastToucher;
            // the layer of a body is configuration, not simulated state: set it from the restored car state
            JPH::BodyInterface& bodies = physics.GetBodyInterfaceNoLock();
            for (int i = 0; i < carCount; ++i)
            {
                const JPH::ObjectLayer wanted = runtimes[i].demolished != 0 ? Layers::kGhost : Layers::kCar;
                if (bodies.GetObjectLayer(carIds[i]) != wanted)
                {
                    bodies.SetObjectLayer(carIds[i], wanted);
                }
                frames[i] = CarFrame{};
                frames[i].wheelContacts = saved[i].wheelContacts;
                for (int w = 0; w < kWheelCount; ++w)
                {
                    frames[i].wheelCompression[w] = saved[i].compression[w];
                }
                frames[i].surfaceNormal = JPH::Vec3(saved[i].normal[0], saved[i].normal[1], saved[i].normal[2]);
            }
            events = {};
            FillState();
            previous = state;
            return true;
        }
    };

    RlSim::RlSim(const RlTuning& tuning, int carCount)
        : impl_(std::make_unique<Impl>(tuning, carCount))
    {
    }

    RlSim::~RlSim() = default;

    int RlSim::CarCount() const { return impl_->carCount; }
    const RlTuning& RlSim::Tuning() const { return impl_->tuning; }
    void RlSim::SetTuning(const RlTuning& tuning) { impl_->SetTuning(tuning); }
    const RlArena& RlSim::Arena() const { return impl_->arena; }

    void RlSim::ResetCar(int index, float x, float z, float yaw)
    {
        if (index >= 0 && index < impl_->carCount)
        {
            impl_->ResetCar(index, x, z, yaw);
            impl_->FillState();
            impl_->previous = impl_->state;
        }
    }

    void RlSim::SetCarBoost(int index, float boost)
    {
        if (index >= 0 && index < impl_->carCount)
        {
            impl_->runtimes[index].boost = std::clamp(boost, 0.0f, impl_->tuning.car.boostMax);
            impl_->FillState();
        }
    }

    void RlSim::SetRespawnPoint(int index, float x, float z, float yaw)
    {
        if (index >= 0 && index < impl_->carCount)
        {
            CarRuntime& rt = impl_->runtimes[index];
            rt.spawnX = x;
            rt.spawnZ = z;
            rt.spawnYaw = yaw;
        }
    }

    void RlSim::SetCarPose(int index, RlVec3 position, RlQuat rotation, RlVec3 velocity)
    {
        if (index >= 0 && index < impl_->carCount)
        {
            JPH::BodyInterface& bodies = impl_->physics.GetBodyInterfaceNoLock();
            bodies.SetPositionAndRotation(impl_->carIds[index], JPH::RVec3(position.x, position.y, position.z),
                                          JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized(), JPH::EActivation::Activate);
            bodies.SetLinearAndAngularVelocity(impl_->carIds[index], JPH::Vec3(velocity.x, velocity.y, velocity.z), JPH::Vec3::sZero());
            impl_->FillState();
        }
    }

    void RlSim::ResetBall(RlVec3 position, RlVec3 velocity, RlVec3 angularVelocity)
    {
        impl_->ResetBall(position, velocity, angularVelocity);
        impl_->FillState();
        impl_->previous = impl_->state;
    }

    float RlSim::ArenaSegmentFree(RlVec3 from, RlVec3 to) const
    {
        const JPH::Vec3 a(from.x, from.y, from.z);
        const JPH::Vec3 d = JPH::Vec3(to.x, to.y, to.z) - a;
        JPH::RayCastResult hit;
        const JPH::RRayCast ray(a, d);
        const JPH::SpecifiedBroadPhaseLayerFilter broadFilter(BroadPhase::kStatic);
        const JPH::SpecifiedObjectLayerFilter objectFilter(Layers::kStatic);
        if (impl_->physics.GetNarrowPhaseQuery().CastRay(ray, hit, broadFilter, objectFilter))
        {
            return hit.mFraction;
        }
        return 1.0f;
    }

    void RlSim::Step(const RlCarInput* inputs) { impl_->Step(inputs); }
    const RlWorldState& RlSim::State() const { return impl_->state; }
    const RlWorldState& RlSim::PreviousState() const { return impl_->previous; }
    const RlEvents& RlSim::LastEvents() const { return impl_->events; }
    void RlSim::SaveState(std::vector<uint8_t>& out) const { impl_->Save(out); }
    bool RlSim::RestoreState(const std::vector<uint8_t>& in) { return impl_->Restore(in); }

    uint64_t RlSim::HashState(const std::vector<uint8_t>& snapshot)
    {
        uint64_t hash = 1469598103934665603ull;
        for (const uint8_t byte : snapshot)
        {
            hash = (hash ^ byte) * 1099511628211ull;
        }
        return hash;
    }
}
