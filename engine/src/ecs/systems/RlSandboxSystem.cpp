// RlSandboxSystem.cpp

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include <DirectXMath.h>

#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/systems/RlSandboxSystem.h>
#include <myengine/editor/EditorState.h>
#include <myengine/input/InputManager.h>

#include <rl/RlSim.h>

#include <tracy/Tracy.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <Xinput.h>

namespace myengine::ecs::systems
{
    namespace
    {
        namespace dx = DirectX;
        using components::Vec3;

        constexpr float kRadToDeg = 57.29577951f;
        constexpr float kDegToRad = 0.01745329252f;

        dx::XMVECTOR Q(const rl::RlQuat& q) { return dx::XMVectorSet(q.x, q.y, q.z, q.w); }
        dx::XMVECTOR P(const rl::RlVec3& v) { return dx::XMVectorSet(v.x, v.y, v.z, 0.0f); }

        Vec3 ToVec3(dx::FXMVECTOR v)
        {
            return {dx::XMVectorGetX(v), dx::XMVectorGetY(v), dx::XMVectorGetZ(v)};
        }

        // The rotation as the Euler angles (degrees) of the transform, which the renderer turns back with XMMatrixRotationRollPitchYaw.
        Vec3 EulerDegrees(dx::FXMVECTOR rotation)
        {
            dx::XMFLOAT4X4 m;
            dx::XMStoreFloat4x4(&m, dx::XMMatrixRotationQuaternion(rotation));
            float pitch;
            float yaw;
            float roll;
            const float s = std::clamp(-m._32, -1.0f, 1.0f);
            pitch = std::asin(s);
            if (std::fabs(s) > 0.99999f)
            {
                yaw = std::atan2(-m._13, m._11);
                roll = 0.0f;
            }
            else
            {
                yaw = std::atan2(m._31, m._33);
                roll = std::atan2(m._12, m._22);
            }
            return {pitch * kRadToDeg, yaw * kRadToDeg, roll * kRadToDeg};
        }

        std::string Format(const char* format, ...)
        {
            char buffer[160];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        float Deadzone(float value, float zone)
        {
            const float magnitude = std::fabs(value);
            if (magnitude <= zone)
            {
                return 0.0f;
            }
            return std::copysign((magnitude - zone) / (1.0f - zone), value);
        }
    }

    struct RlSandboxSystem::Impl
    {
        input::InputManager& input;

        std::unique_ptr<rl::RlSim> sim;
        bool scanned = false;
        std::string status;

        EntityId arena = kInvalidEntity;
        EntityId ball = kInvalidEntity;
        EntityId camera = kInvalidEntity;
        EntityId cars[rl::kMaxCars] = {};
        EntityId wheels[rl::kMaxCars][rl::kWheelCount] = {};
        int carCount = 0;

        float accumulator = 0.0f;
        bool jumpLatched = false;
        bool ballCam = true;
        bool cameraReady = false;
        Vec3 cameraForward{0.0f, 0.0f, 1.0f};
        Vec3 cameraUp{0.0f, 1.0f, 0.0f};
        float cameraFov = 95.0f;

        // XInput: an unplugged pad is looked for once a second, polling it every frame is slow
        bool padConnected = false;
        float padRetry = 0.0f;
        bool padYPrev = false;
        bool padInUse = false;

        std::vector<std::string> hud;

        explicit Impl(input::InputManager& inputManager)
            : input(inputManager)
        {
        }

        EntityId FindTag(World& world, const std::string& name)
        {
            EntityId found = kInvalidEntity;
            world.ForEach<components::TagComponent>(
                [&](const EntityId entity, const components::TagComponent& tag)
                {
                    if (found == kInvalidEntity && tag.name == name)
                    {
                        found = entity;
                    }
                });
            return found;
        }

        void Stop()
        {
            sim.reset();
            scanned = false;
            status.clear();
            hud.clear();
            accumulator = 0.0f;
            cameraReady = false;
            cameraUp = {0.0f, 1.0f, 0.0f};
            jumpLatched = false;
        }

        bool Start(World& world)
        {
            scanned = true;
            arena = FindTag(world, "RlArena");
            ball = FindTag(world, "RlBall");
            camera = FindTag(world, "RlCamera");
            carCount = 0;
            for (int i = 0; i < rl::kMaxCars; ++i)
            {
                cars[i] = FindTag(world, Format("RlCar%d", i));
                if (cars[i] == kInvalidEntity)
                {
                    break;
                }
                ++carCount;
                for (int w = 0; w < rl::kWheelCount; ++w)
                {
                    wheels[i][w] = FindTag(world, Format("RlCar%dWheel%d", i, w));
                }
            }
            if (arena == kInvalidEntity || ball == kInvalidEntity || carCount == 0)
            {
                return false;  // not an RL sandbox scene
            }

            rl::RlTuning tuning;
            std::string error;
            const auto tuningPath = core::ServiceLocator::GetProjectContext().ResolveContentPath("assets/data/rl_tuning.json");
            if (!tuning.LoadFromFile(tuningPath.generic_u8string(), &error))
            {
                status = "rl_tuning.json: " + error + " (defaults)";
            }
            sim = std::make_unique<rl::RlSim>(tuning, carCount);

            // the cars and the ball start where the scene has them
            for (int i = 0; i < carCount; ++i)
            {
                if (const auto* transform = world.TryGet<components::TransformComponent>(cars[i]))
                {
                    sim->ResetCar(i, transform->position.x, transform->position.z, transform->rotationDeg.y * kDegToRad);
                }
            }
            if (const auto* transform = world.TryGet<components::TransformComponent>(ball))
            {
                sim->ResetBall({transform->position.x, transform->position.y, transform->position.z});
            }
            if (const auto* cam = camera != kInvalidEntity ? world.TryGet<components::CameraComponent>(camera) : nullptr)
            {
                cameraFov = cam->fovYDeg;
            }
            accumulator = 0.0f;
            return true;
        }

        void PollPad(const float deltaTime, rl::RlCarInput& pad, bool& ballCamPressed)
        {
            padInUse = false;
            ballCamPressed = false;
            if (!padConnected)
            {
                padRetry -= deltaTime;
                if (padRetry > 0.0f)
                {
                    return;
                }
                padRetry = 1.0f;
            }
            XINPUT_STATE state{};
            if (XInputGetState(0, &state) != ERROR_SUCCESS)
            {
                padConnected = false;
                return;
            }
            padConnected = true;
            const XINPUT_GAMEPAD& g = state.Gamepad;
            const float lx = Deadzone(static_cast<float>(g.sThumbLX) / 32767.0f, 0.2f);
            const float ly = Deadzone(static_cast<float>(g.sThumbLY) / 32767.0f, 0.2f);
            pad.throttle = static_cast<float>(g.bRightTrigger) / 255.0f - static_cast<float>(g.bLeftTrigger) / 255.0f;
            pad.steer = lx;
            pad.pitch = -ly;  // stick forward pitches the nose down
            pad.jump = (g.wButtons & XINPUT_GAMEPAD_A) != 0;
            pad.boost = (g.wButtons & XINPUT_GAMEPAD_B) != 0;
            pad.handbrake = (g.wButtons & XINPUT_GAMEPAD_X) != 0;
            pad.airRoll = (g.wButtons & XINPUT_GAMEPAD_X) != 0;
            const bool leftBumper = (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            const bool rightBumper = (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
            pad.roll = (rightBumper ? 1.0f : 0.0f) - (leftBumper ? 1.0f : 0.0f);
            const bool y = (g.wButtons & XINPUT_GAMEPAD_Y) != 0;
            ballCamPressed = y && !padYPrev;
            padYPrev = y;
            padInUse = std::fabs(pad.throttle) > 0.05f || std::fabs(pad.steer) > 0.0f || std::fabs(pad.pitch) > 0.0f || pad.jump || pad.boost ||
                       pad.handbrake || leftBumper || rightBumper;
        }

        rl::RlCarInput KeyboardInput()
        {
            rl::RlCarInput in;
            const float forward = (input.IsKeyDown('W') ? 1.0f : 0.0f) - (input.IsKeyDown('S') ? 1.0f : 0.0f);
            const float right = (input.IsKeyDown('D') ? 1.0f : 0.0f) - (input.IsKeyDown('A') ? 1.0f : 0.0f);
            in.throttle = forward;
            in.steer = right;
            in.pitch = -forward;  // W pitches the nose down in the air, like the stick forward
            in.roll = (input.IsKeyDown('E') ? 1.0f : 0.0f) - (input.IsKeyDown('Q') ? 1.0f : 0.0f);
            in.jump = input.IsKeyDown(VK_SPACE);
            in.boost = input.IsKeyDown(VK_SHIFT);
            in.handbrake = input.IsKeyDown(VK_CONTROL);
            in.airRoll = input.IsKeyDown(VK_CONTROL);
            if (input.WasKeyPressed(VK_SPACE))
            {
                jumpLatched = true;
            }
            return in;
        }

        void Update(World& world, const float deltaTime)
        {
            if (!scanned && !Start(world))
            {
                return;
            }
            if (!sim)
            {
                return;
            }

            // ---- input of the player car
            const bool windowActive = input.GetActiveWindowId() != 0;
            rl::RlCarInput keyboard;
            bool ballCamPressed = false;
            if (windowActive)
            {
                keyboard = KeyboardInput();
                if (input.WasKeyPressed('C'))
                {
                    ballCamPressed = true;
                }
                if (input.WasKeyPressed('R'))
                {
                    ResetMatch(world);
                }
            }
            rl::RlCarInput pad;
            bool padBallCam = false;
            PollPad(deltaTime, pad, padBallCam);
            ballCamPressed = ballCamPressed || padBallCam;
            if (ballCamPressed)
            {
                ballCam = !ballCam;
            }
            rl::RlCarInput player = padInUse ? pad : keyboard;
            if (padInUse)
            {
                player.roll = std::clamp(pad.roll + keyboard.roll, -1.0f, 1.0f);
            }

            // ---- fixed ticks
            accumulator += std::min(deltaTime, 0.1f);
            std::vector<rl::RlCarInput> inputs(static_cast<size_t>(sim->CarCount()));
            while (accumulator >= rl::kTickDt)
            {
                accumulator -= rl::kTickDt;
                inputs[0] = player;
                if (jumpLatched)
                {
                    inputs[0].jump = true;
                    jumpLatched = false;
                }
                sim->Step(inputs.data());
            }

            ApplyToScene(world, deltaTime);
            BuildHud();
        }

        void ResetMatch(World& world)
        {
            for (int i = 0; i < carCount; ++i)
            {
                if (const auto* transform = world.TryGet<components::TransformComponent>(cars[i]))
                {
                    sim->ResetCar(i, transform->position.x, transform->position.z, transform->rotationDeg.y * kDegToRad);
                }
            }
            Vec3 ballStart{0.0f, 3.0f, 0.0f};
            if (const auto* transform = world.TryGet<components::TransformComponent>(ball))
            {
                ballStart = transform->position;
            }
            sim->ResetBall({ballStart.x, ballStart.y, ballStart.z});
        }

        void ApplyToScene(World& world, const float deltaTime)
        {
            const float alpha = std::clamp(accumulator / rl::kTickDt, 0.0f, 1.0f);
            const rl::RlWorldState& now = sim->State();
            const rl::RlWorldState& before = sim->PreviousState();
            const rl::RlTuning& tuning = sim->Tuning();

            dx::XMVECTOR carPosition = dx::XMVectorZero();
            dx::XMVECTOR carRotation = dx::XMQuaternionIdentity();
            dx::XMVECTOR ballPosition = dx::XMVectorZero();

            for (int i = 0; i < carCount; ++i)
            {
                const rl::RlCarState& a = before.cars[i];
                const rl::RlCarState& b = now.cars[i];
                const bool jumped = a.demolished != b.demolished;  // do not slide from the park spot to the spawn
                const float t = jumped ? 1.0f : alpha;
                const dx::XMVECTOR position = dx::XMVectorLerp(P(a.position), P(b.position), t);
                const dx::XMVECTOR rotation = dx::XMQuaternionSlerp(Q(a.rotation), Q(b.rotation), t);
                if (i == 0)
                {
                    carPosition = position;
                    carRotation = rotation;
                }

                if (auto* transform = world.TryGet<components::TransformComponent>(cars[i]))
                {
                    transform->position = ToVec3(position);
                    transform->rotationDeg = EulerDegrees(rotation);
                    transform->scale = b.demolished ? Vec3{0.0f, 0.0f, 0.0f} : Vec3{tuning.car.halfExtentX * 2.0f, tuning.car.halfExtentY * 2.0f, tuning.car.halfExtentZ * 2.0f};
                }
                for (int w = 0; w < rl::kWheelCount; ++w)
                {
                    if (wheels[i][w] == kInvalidEntity)
                    {
                        continue;
                    }
                    auto* wheel = world.TryGet<components::TransformComponent>(wheels[i][w]);
                    if (wheel == nullptr)
                    {
                        continue;
                    }
                    const float side = (w & 1) ? 1.0f : -1.0f;
                    const float z = (w < 2) ? tuning.car.wheelFrontZ : tuning.car.wheelRearZ;
                    const float radius = 0.2f;
                    // the wheel hangs under its mount by the free suspension travel
                    const float drop = std::max(tuning.car.suspensionRest - radius - b.wheelCompression[w], 0.0f);
                    const dx::XMVECTOR local = dx::XMVectorSet(side * tuning.car.wheelMountX, tuning.car.wheelMountYOffset - drop, z, 0.0f);
                    wheel->position = ToVec3(dx::XMVectorAdd(position, dx::XMVector3Rotate(local, rotation)));
                    wheel->rotationDeg = EulerDegrees(rotation);
                    wheel->scale = b.demolished ? Vec3{0.0f, 0.0f, 0.0f} : Vec3{0.16f, radius, radius};
                }
            }

            {
                const rl::RlBallState& a = before.ball;
                const rl::RlBallState& b = now.ball;
                ballPosition = dx::XMVectorLerp(P(a.position), P(b.position), alpha);
                const dx::XMVECTOR rotation = dx::XMQuaternionSlerp(Q(a.rotation), Q(b.rotation), alpha);
                if (auto* transform = world.TryGet<components::TransformComponent>(ball))
                {
                    transform->position = ToVec3(ballPosition);
                    transform->rotationDeg = EulerDegrees(rotation);
                    const float r = tuning.ball.radius;
                    transform->scale = {r, r, r};
                }
            }

            UpdateCamera(world, deltaTime, now.cars[0], carPosition, carRotation, ballPosition);
        }

        // Behind the car looking over it at the ball (ball cam), or along the car's heading (chase cam). On a wall or the ceiling the
        // camera takes the surface as its floor.
        void UpdateCamera(World& world, const float deltaTime, const rl::RlCarState& car, dx::FXMVECTOR carPosition, dx::FXMVECTOR carRotation,
                          dx::FXMVECTOR ballPosition)
        {
            if (camera == kInvalidEntity)
            {
                return;
            }
            auto* cam = world.TryGet<components::CameraComponent>(camera);
            if (cam == nullptr)
            {
                return;
            }

            const dx::XMVECTOR worldUp = dx::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            const bool onSurface = car.wheelContacts >= 3;
            const dx::XMVECTOR targetUp = onSurface ? dx::XMVector3Normalize(P(car.surfaceNormal)) : worldUp;
            dx::XMVECTOR up = dx::XMVectorSet(cameraUp.x, cameraUp.y, cameraUp.z, 0.0f);
            const dx::XMVECTOR carForward = dx::XMVector3Rotate(dx::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), carRotation);
            const float kUp = 1.0f - std::exp(-deltaTime * 4.0f);
            const float kAim = 1.0f - std::exp(-deltaTime * (ballCam ? 9.0f : 6.0f));
            const auto onPlane = [](dx::FXMVECTOR v, dx::FXMVECTOR normal)
            {
                return dx::XMVectorSubtract(v, dx::XMVectorScale(normal, dx::XMVectorGetX(dx::XMVector3Dot(v, normal))));
            };

            if (!cameraReady)
            {
                up = targetUp;
            }
            else
            {
                up = dx::XMVector3Normalize(dx::XMVectorLerp(up, targetUp, kUp));
            }

            dx::XMVECTOR aim = onPlane(carForward, up);
            if (dx::XMVectorGetX(dx::XMVector3LengthSq(aim)) < 0.05f)
            {
                aim = dx::XMVectorSet(cameraForward.x, cameraForward.y, cameraForward.z, 0.0f);  // nose straight up or down: keep the heading
            }
            // the ball cam only makes sense with the car on a floor
            if (ballCam && dx::XMVectorGetY(up) > 0.8f)
            {
                const dx::XMVECTOR toBall = onPlane(dx::XMVectorSubtract(ballPosition, carPosition), up);
                if (dx::XMVectorGetX(dx::XMVector3LengthSq(toBall)) > 0.25f)
                {
                    aim = toBall;
                }
            }
            aim = dx::XMVector3Normalize(aim);

            dx::XMVECTOR forward = aim;
            if (cameraReady)
            {
                forward = dx::XMVectorSet(cameraForward.x, cameraForward.y, cameraForward.z, 0.0f);
                forward = dx::XMVector3Normalize(dx::XMVectorLerp(forward, aim, kAim));
            }
            forward = dx::XMVector3Normalize(onPlane(forward, up));
            cameraReady = true;
            cameraForward = ToVec3(forward);
            cameraUp = ToVec3(up);

            const float distance = 4.6f;
            const float height = 1.55f;
            dx::XMVECTOR eye = dx::XMVectorAdd(dx::XMVectorSubtract(carPosition, dx::XMVectorScale(forward, distance)), dx::XMVectorScale(up, height));
            eye = dx::XMVectorSetY(eye, std::clamp(dx::XMVectorGetY(eye), 0.4f, sim->Tuning().arena.height - 0.4f));
            dx::XMVECTOR lookAt = dx::XMVectorAdd(carPosition, dx::XMVectorAdd(dx::XMVectorScale(forward, 6.0f), dx::XMVectorScale(up, 0.7f)));
            if (ballCam && dx::XMVectorGetY(up) > 0.8f)
            {
                // aim between the front of the car and the ball
                lookAt = dx::XMVectorLerp(lookAt, ballPosition, 0.7f);
            }
            const dx::XMVECTOR view = dx::XMVector3Normalize(dx::XMVectorSubtract(lookAt, eye));
            const dx::XMVECTOR right = dx::XMVector3Normalize(dx::XMVector3Cross(up, view));
            const dx::XMVECTOR cameraUpAxis = dx::XMVector3Cross(view, right);
            dx::XMMATRIX basis;
            basis.r[0] = right;
            basis.r[1] = cameraUpAxis;
            basis.r[2] = view;
            basis.r[3] = dx::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
            cam->position = ToVec3(eye);
            cam->rotationDeg = EulerDegrees(dx::XMQuaternionRotationMatrix(basis));
            cam->fovYDeg = cameraFov;
            cam->isPrimary = true;
        }

        void BuildHud()
        {
            hud.clear();
            const rl::RlCarState& car = sim->State().cars[0];
            const float speed = std::sqrt(car.linearVelocity.x * car.linearVelocity.x + car.linearVelocity.y * car.linearVelocity.y +
                                          car.linearVelocity.z * car.linearVelocity.z);
            hud.push_back(Format("RL sandbox  tick %u  %s", sim->State().tick, ballCam ? "ball cam" : "chase cam"));
            hud.push_back(Format("speed %.1f m/s (%.0f km/h)%s  boost %.0f", speed, speed * 3.6f, car.supersonic ? "  SUPERSONIC" : "", car.boost));
            hud.push_back(Format("wheels %d/4 %s  jump %s  %s", car.wheelContacts, car.onGround ? "on surface" : "in air",
                                 car.canDodge ? "2nd jump/dodge ready" : (car.jumped ? "used" : "ready"), car.dodging ? "flipping" : ""));
            if (!sim->State().cars[0].demolished)
            {
                const rl::RlBallState& ballState = sim->State().ball;
                const float ballSpeed = std::sqrt(ballState.linearVelocity.x * ballState.linearVelocity.x + ballState.linearVelocity.y * ballState.linearVelocity.y +
                                                  ballState.linearVelocity.z * ballState.linearVelocity.z);
                hud.push_back(Format("ball %.1f m/s  last touch %s", ballSpeed, sim->State().lastBallToucher < 0 ? "-" : Format("car %d", sim->State().lastBallToucher).c_str()));
            }
            else
            {
                hud.push_back(Format("demolished, back in %.1f s", car.respawnTimer));
            }
            hud.push_back(padConnected ? (padInUse ? "gamepad: in use" : "gamepad: connected") : "gamepad: not found");
            hud.push_back("W/S gas+pitch  A/D steer  Q/E roll  Space jump  Shift boost  Ctrl drift/air roll  C camera  R reset");
            hud.push_back("pad: RT/LT gas  stick  A jump  B boost  X drift/air roll  LB/RB roll  Y camera");
            if (!status.empty())
            {
                hud.push_back(status);
            }
        }
    };

    RlSandboxSystem::RlSandboxSystem(input::InputManager& input)
        : impl_(std::make_unique<Impl>(input))
    {
    }

    RlSandboxSystem::~RlSandboxSystem() = default;

    void RlSandboxSystem::Update(World& world, const float deltaTime)
    {
        ZoneScoped;

        if (core::ServiceLocator::GetEditorRuntimeState().mode != editor::RuntimeMode::Play)
        {
            impl_->Stop();
            return;
        }
        impl_->Update(world, deltaTime);
    }

    std::vector<std::string> RlSandboxSystem::HudLines() const
    {
        return impl_->hud;
    }
}
