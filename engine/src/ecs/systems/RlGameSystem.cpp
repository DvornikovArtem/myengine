// RlGameSystem.cpp

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include <DirectXMath.h>

#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/systems/RlGameSystem.h>
#include <myengine/editor/EditorState.h>
#include <myengine/input/InputManager.h>

#include <rl/RlMatch.h>

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
            float yaw;
            float roll;
            const float s = std::clamp(-m._32, -1.0f, 1.0f);
            const float pitch = std::asin(s);
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
            char buffer[200];
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

        std::string ClockText(float seconds, bool overtime)
        {
            const int total = static_cast<int>(std::max(seconds, 0.0f) + (overtime ? 0.0f : 0.999f));
            return Format("%s%d:%02d", overtime ? "+" : "", total / 60, total % 60);
        }
    }

    struct RlGameSystem::Impl
    {
        enum class Mode
        {
            Menu,
            Match,
        };

        input::InputManager& input;

        rl::RlTuning tuning;
        std::unique_ptr<rl::RlMatch> match;
        Mode mode = Mode::Menu;
        bool scanned = false;
        bool active = false;
        std::string status;

        int menuMode = 2;     // 0 practice, 1..4 players per team
        int botLevel = 1;

        EntityId arena = kInvalidEntity;
        EntityId ball = kInvalidEntity;
        EntityId camera = kInvalidEntity;
        EntityId marker = kInvalidEntity;
        EntityId cars[2][4] = {};
        EntityId wheels[2][4][rl::kWheelCount] = {};
        EntityId pads[rl::kPadCount] = {};

        float accumulator = 0.0f;
        bool jumpLatched = false;
        bool ballCam = true;
        bool cameraReady = false;
        Vec3 cameraForward{0.0f, 0.0f, 1.0f};
        Vec3 cameraUp{0.0f, 1.0f, 0.0f};
        float cameraFov = 95.0f;
        components::CameraComponent cameraHome;
        bool haveCameraHome = false;

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

        void SetVisible(World& world, EntityId entity, bool visible)
        {
            if (entity == kInvalidEntity)
            {
                return;
            }
            if (auto* renderer = world.TryGet<components::MeshRendererComponent>(entity))
            {
                renderer->visible = visible;
            }
        }

        void HideEverything(World& world)
        {
            SetVisible(world, ball, false);
            SetVisible(world, marker, false);
            for (int t = 0; t < 2; ++t)
            {
                for (int i = 0; i < 4; ++i)
                {
                    SetVisible(world, cars[t][i], false);
                    for (int w = 0; w < rl::kWheelCount; ++w)
                    {
                        SetVisible(world, wheels[t][i][w], false);
                    }
                }
            }
            for (EntityId pad : pads)
            {
                SetVisible(world, pad, false);
            }
        }

        void Stop(World* world)
        {
            if (active && world != nullptr)
            {
                if (haveCameraHome && camera != kInvalidEntity)
                {
                    if (auto* cam = world->TryGet<components::CameraComponent>(camera))
                    {
                        *cam = cameraHome;
                    }
                }
            }
            match.reset();
            mode = Mode::Menu;
            scanned = false;
            active = false;
            status.clear();
            hud.clear();
            accumulator = 0.0f;
            cameraReady = false;
            cameraUp = {0.0f, 1.0f, 0.0f};
            jumpLatched = false;
            haveCameraHome = false;
        }

        bool Scan(World& world)
        {
            scanned = true;
            arena = FindTag(world, "RlArena");
            ball = FindTag(world, "RlBall");
            camera = FindTag(world, "RlCamera");
            marker = FindTag(world, "RlPlayerMarker");
            const char* teamNames[2] = {"RlBlue", "RlOrange"};
            for (int t = 0; t < 2; ++t)
            {
                for (int i = 0; i < 4; ++i)
                {
                    cars[t][i] = FindTag(world, Format("%s%d", teamNames[t], i));
                    for (int w = 0; w < rl::kWheelCount; ++w)
                    {
                        wheels[t][i][w] = FindTag(world, Format("%s%dWheel%d", teamNames[t], i, w));
                    }
                }
            }
            for (int i = 0; i < rl::kPadCount; ++i)
            {
                pads[i] = FindTag(world, Format("RlPad%d", i));
            }
            if (arena == kInvalidEntity || ball == kInvalidEntity || cars[0][0] == kInvalidEntity)
            {
                return false;  // not a Rocket League scene
            }

            std::string error;
            const auto tuningPath = core::ServiceLocator::GetProjectContext().ResolveContentPath("assets/data/rl_tuning.json");
            if (!tuning.LoadFromFile(tuningPath.generic_u8string(), &error))
            {
                status = "rl_tuning.json: " + error + " (defaults)";
            }
            if (const auto* cam = camera != kInvalidEntity ? world.TryGet<components::CameraComponent>(camera) : nullptr)
            {
                cameraFov = cam->fovYDeg;
                cameraHome = *cam;
                haveCameraHome = true;
            }
            active = true;
            HideEverything(world);
            return true;
        }

        void StartMatch(World& world)
        {
            rl::RlMatchConfig config;
            config.mode = menuMode;
            config.humanMask = 1u;
            config.botLevel = botLevel;
            // a seed from the clock of the frame loop would break replays; the menu counter is enough for variety
            static uint64_t started = 0;
            config.seed = 0x5EED0000ull + ++started;
            match = std::make_unique<rl::RlMatch>(tuning, config);
            mode = Mode::Match;
            accumulator = 0.0f;
            cameraReady = false;
            HideEverything(world);
            const int teamSize = std::max(menuMode, 1);
            for (int t = 0; t < (menuMode == 0 ? 1 : 2); ++t)
            {
                for (int i = 0; i < teamSize; ++i)
                {
                    SetVisible(world, cars[t][i], true);
                    for (int w = 0; w < rl::kWheelCount; ++w)
                    {
                        SetVisible(world, wheels[t][i][w], true);
                    }
                }
            }
            SetVisible(world, ball, true);
            SetVisible(world, marker, true);
            for (EntityId pad : pads)
            {
                SetVisible(world, pad, true);
            }
            for (int i = 0; i < rl::kPadCount; ++i)
            {
                if (auto* transform = world.TryGet<components::TransformComponent>(pads[i]))
                {
                    float x, z;
                    rl::RlPadSpot(i, x, z);
                    transform->position = {x, 0.04f, z};
                }
            }
        }

        void ToMenu(World& world)
        {
            match.reset();
            mode = Mode::Menu;
            HideEverything(world);
            if (haveCameraHome && camera != kInvalidEntity)
            {
                if (auto* cam = world.TryGet<components::CameraComponent>(camera))
                {
                    *cam = cameraHome;
                }
            }
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
            if (!scanned && !Scan(world))
            {
                return;
            }
            if (!active)
            {
                return;
            }

            const bool windowActive = input.GetActiveWindowId() != 0;
            if (mode == Mode::Menu)
            {
                if (windowActive)
                {
                    for (int n = 1; n <= 4; ++n)
                    {
                        if (input.WasKeyPressed(static_cast<std::uint32_t>('0' + n)))
                        {
                            menuMode = n;
                        }
                    }
                    if (input.WasKeyPressed('P') || input.WasKeyPressed('0'))
                    {
                        menuMode = 0;
                    }
                    if (input.WasKeyPressed('L'))
                    {
                        botLevel = (botLevel + 1) % 3;
                    }
                    if (input.WasKeyPressed(VK_RETURN))
                    {
                        StartMatch(world);
                        return;
                    }
                }
                BuildMenuHud();
                return;
            }

            // ---- input of the player car
            rl::RlCarInput keyboard;
            bool ballCamPressed = false;
            bool toMenu = false;
            bool enter = false;
            if (windowActive)
            {
                keyboard = KeyboardInput();
                ballCamPressed = input.WasKeyPressed('C');
                toMenu = input.WasKeyPressed('M');
                enter = input.WasKeyPressed(VK_RETURN);
            }
            rl::RlCarInput pad;
            bool padBallCam = false;
            PollPad(deltaTime, pad, padBallCam);
            ballCamPressed = ballCamPressed || padBallCam;
            if (ballCamPressed)
            {
                ballCam = !ballCam;
            }
            if (toMenu || (enter && match->World().match.phase == rl::RlMatchPhase::Finished))
            {
                ToMenu(world);
                BuildMenuHud();
                return;
            }
            rl::RlCarInput player = padInUse ? pad : keyboard;
            if (padInUse)
            {
                player.roll = std::clamp(pad.roll + keyboard.roll, -1.0f, 1.0f);
            }

            // ---- fixed ticks
            accumulator += std::min(deltaTime, 0.1f);
            std::vector<rl::RlCarInput> inputs(static_cast<size_t>(match->SlotCount()));
            while (accumulator >= rl::kTickDt)
            {
                accumulator -= rl::kTickDt;
                inputs[0] = player;
                if (jumpLatched)
                {
                    inputs[0].jump = true;
                    jumpLatched = false;
                }
                match->Step(inputs.data());
            }

            ApplyToScene(world, deltaTime);
            BuildMatchHud();
        }

        void ApplyToScene(World& world, const float deltaTime)
        {
            const float alpha = std::clamp(accumulator / rl::kTickDt, 0.0f, 1.0f);
            const rl::RlWorldState& now = match->World();
            const rl::RlWorldState& before = match->PreviousWorld();
            const rl::RlTuning& t = match->Tuning();
            const int teamSize = std::max(menuMode, 1);

            dx::XMVECTOR playerPosition = dx::XMVectorZero();
            dx::XMVECTOR playerRotation = dx::XMQuaternionIdentity();

            for (int slot = 0; slot < match->SlotCount(); ++slot)
            {
                const rl::RlCarState& a = before.cars[slot];
                const rl::RlCarState& b = now.cars[slot];
                const bool jumped = a.demolished != b.demolished || before.match.kickoffs != now.match.kickoffs;  // no sliding from the park spot or the last kick-off
                const float s = jumped ? 1.0f : alpha;
                const dx::XMVECTOR position = dx::XMVectorLerp(P(a.position), P(b.position), s);
                const dx::XMVECTOR rotation = dx::XMQuaternionSlerp(Q(a.rotation), Q(b.rotation), s);
                if (slot == 0)
                {
                    playerPosition = position;
                    playerRotation = rotation;
                }
                const int team = match->Team(slot);
                const int index = slot % teamSize;
                const bool shown = !b.demolished;
                if (auto* transform = world.TryGet<components::TransformComponent>(cars[team][index]))
                {
                    transform->position = ToVec3(position);
                    transform->rotationDeg = EulerDegrees(rotation);
                    transform->scale = {t.car.halfExtentX * 2.0f, t.car.halfExtentY * 2.0f, t.car.halfExtentZ * 2.0f};
                }
                SetVisible(world, cars[team][index], shown);
                for (int w = 0; w < rl::kWheelCount; ++w)
                {
                    const EntityId wheelEntity = wheels[team][index][w];
                    SetVisible(world, wheelEntity, shown);
                    auto* wheel = world.TryGet<components::TransformComponent>(wheelEntity);
                    if (wheel == nullptr)
                    {
                        continue;
                    }
                    const float side = (w & 1) ? 1.0f : -1.0f;
                    const float z = (w < 2) ? t.car.wheelFrontZ : t.car.wheelRearZ;
                    const float radius = 0.2f;
                    // the wheel hangs under its mount by the free suspension travel
                    const float drop = std::max(t.car.suspensionRest - radius - b.wheelCompression[w], 0.0f);
                    const dx::XMVECTOR local = dx::XMVectorSet(side * t.car.wheelMountX, t.car.wheelMountYOffset - drop, z, 0.0f);
                    wheel->position = ToVec3(dx::XMVectorAdd(position, dx::XMVector3Rotate(local, rotation)));
                    wheel->rotationDeg = EulerDegrees(rotation);
                    wheel->scale = {0.16f, radius, radius};
                }
            }

            dx::XMVECTOR ballPosition;
            {
                const rl::RlBallState& a = before.ball;
                const rl::RlBallState& b = now.ball;
                const bool reset = before.match.kickoffs != now.match.kickoffs;
                const float s = reset ? 1.0f : alpha;
                ballPosition = dx::XMVectorLerp(P(a.position), P(b.position), s);
                const dx::XMVECTOR rotation = dx::XMQuaternionSlerp(Q(a.rotation), Q(b.rotation), s);
                if (auto* transform = world.TryGet<components::TransformComponent>(ball))
                {
                    transform->position = ToVec3(ballPosition);
                    transform->rotationDeg = EulerDegrees(rotation);
                    const float r = t.ball.radius;
                    transform->scale = {r, r, r};
                }
            }

            // the pads: lit while they wait for a car
            for (int i = 0; i < rl::kPadCount; ++i)
            {
                SetVisible(world, pads[i], now.match.pads[i].respawnLeft <= 0.0f);
            }
            if (auto* transform = world.TryGet<components::TransformComponent>(marker))
            {
                transform->position = {dx::XMVectorGetX(playerPosition), dx::XMVectorGetY(playerPosition) + 0.95f, dx::XMVectorGetZ(playerPosition)};
                transform->rotationDeg = {0.0f, 45.0f, 0.0f};
                transform->scale = {0.22f, 0.22f, 0.22f};
                SetVisible(world, marker, !now.cars[0].demolished);
            }

            UpdateCamera(world, deltaTime, now.cars[0], playerPosition, playerRotation, ballPosition);
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
            eye = dx::XMVectorSetY(eye, std::clamp(dx::XMVectorGetY(eye), 0.4f, match->Tuning().arena.height - 0.4f));
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

        void BuildMenuHud()
        {
            hud.clear();
            hud.push_back("=== ROCKET LEAGUE ===");
            const char* names[5] = {"practice (one car and the ball)", "1v1", "2v2", "3v3", "4v4"};
            hud.push_back(Format("mode: %s   (1-4 = players per team, P = practice)", names[menuMode]));
            const char* levels[3] = {"easy", "normal", "hard"};
            hud.push_back(Format("bots: %s   (L = change)", levels[botLevel]));
            hud.push_back("Enter - start      M - back to this menu      Esc - stop Play");
            hud.push_back("W/S gas+pitch  A/D steer  Q/E roll  Space jump (x2 + direction = dodge)  Shift boost  Ctrl drift/air roll  C camera");
            hud.push_back("pad: RT/LT gas  stick  A jump  B boost  X drift/air roll  LB/RB roll  Y camera");
            if (!status.empty())
            {
                hud.push_back(status);
            }
        }

        void BuildMatchHud()
        {
            hud.clear();
            const rl::RlWorldState& w = match->World();
            const rl::RlMatchState& m = w.match;
            const rl::RlCarState& car = w.cars[0];
            const char* teamName[2] = {"BLUE", "ORANGE"};

            if (menuMode == 0)
            {
                hud.push_back(Format("PRACTICE   goals %d", m.score[0]));
            }
            else
            {
                hud.push_back(Format("BLUE %d : %d ORANGE    %s%s", m.score[0], m.score[1], m.overtime != 0 ? "OVERTIME " : "", ClockText(m.clock, m.overtime != 0).c_str()));
            }
            switch (m.phase)
            {
            case rl::RlMatchPhase::Countdown:
                hud.push_back(Format("- %d -", static_cast<int>(std::ceil(m.phaseTimer))));
                break;
            case rl::RlMatchPhase::GoalScored:
            {
                std::string author = m.lastGoalScorer < 0 ? "" : Format("  (%s%s %d)", teamName[match->Team(m.lastGoalScorer)], m.lastGoalOwn != 0 ? " own goal" : "",
                                                                       m.lastGoalScorer % std::max(menuMode, 1) + 1);
                hud.push_back(Format("*** GOAL!  %s scores%s ***", teamName[m.lastGoalTeam], author.c_str()));
                break;
            }
            case rl::RlMatchPhase::Finished:
                hud.push_back(Format("FINAL  %s TEAM WINS  %d : %d   -   Enter / M for the menu", teamName[m.winner], m.score[0], m.score[1]));
                break;
            case rl::RlMatchPhase::Playing:
                if (m.overtime != 0 && m.clock < 3.0f)
                {
                    hud.push_back("OVERTIME! The next goal wins");
                }
                break;
            }

            const float speed = std::sqrt(car.linearVelocity.x * car.linearVelocity.x + car.linearVelocity.y * car.linearVelocity.y +
                                          car.linearVelocity.z * car.linearVelocity.z);
            hud.push_back(Format("boost %.0f   speed %.1f m/s (%.0f km/h)%s", car.boost, speed, speed * 3.6f, car.supersonic ? "  SUPERSONIC" : ""));
            if (car.demolished)
            {
                hud.push_back(Format("demolished, back in %.1f s", car.respawnTimer));
            }
            else
            {
                hud.push_back(Format("wheels %d/4 %s  %s  %s   camera: %s", car.wheelContacts, car.onGround ? "on surface" : "in air",
                                     car.canDodge ? "2nd jump/dodge ready" : (car.jumped ? "jump used" : "jump ready"), car.dodging ? "flipping" : "",
                                     ballCam ? "ball cam" : "chase"));
            }
            hud.push_back(padConnected ? (padInUse ? "gamepad: in use" : "gamepad: connected") : "gamepad: not found");
            hud.push_back("You = the blue car with the gold marker.   M menu   C camera");
        }
    };

    RlGameSystem::RlGameSystem(input::InputManager& input)
        : impl_(std::make_unique<Impl>(input))
    {
    }

    RlGameSystem::~RlGameSystem() = default;

    void RlGameSystem::Update(World& world, const float deltaTime)
    {
        ZoneScoped;

        if (core::ServiceLocator::GetEditorRuntimeState().mode != editor::RuntimeMode::Play)
        {
            if (impl_->active)
            {
                // the editor puts the scene back from its snapshot when Play stops; only our own state is dropped here
                impl_->Stop(nullptr);
            }
            else
            {
                impl_->scanned = false;
            }
            return;
        }
        impl_->Update(world, deltaTime);
    }

    std::vector<std::string> RlGameSystem::HudLines() const
    {
        return impl_->hud;
    }
}
