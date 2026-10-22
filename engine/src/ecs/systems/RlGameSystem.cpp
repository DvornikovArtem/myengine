// RlGameSystem.cpp

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

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

#include <rl/RlCamera.h>
#include <rl/RlControls.h>
#include <rl/RlMatch.h>
#include <rl/RlNet.h>
#include <rl/net/NetReliable.h>

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
        rl::RlCameraState cameraState;
        components::CameraComponent cameraHome;
        bool haveCameraHome = false;

        rl::RlControls controls = rl::RlControls::Defaults();
        bool previousDown[static_cast<int>(rl::RlButtonAction::Count)] = {};
        bool scoreboard = false;

        // XInput: an unplugged pad is looked for once a second, polling it every frame is slow
        bool padConnected = false;
        float padRetry = 0.0f;
        bool padInUse = false;

        std::vector<std::string> hud;
        rl::RlCarInput lastPlayer;  // what the player holds, shown in the overlay

        // The online game (G5). The match then lives in netHost or netClient; `match` stays empty.
        RlOnlineOptions online;
        std::unique_ptr<rl::net::INetTransport> transport;
        std::unique_ptr<rl::RlNetHost> netHost;
        std::unique_ptr<rl::RlNetClient> netClient;
        bool onlineVisuals = false;
        int localSlot = 0;
        std::string onlineStatus;
        std::chrono::steady_clock::time_point onlineEpoch = std::chrono::steady_clock::now();

        // rl_tuning.json is read again when it changes on disk (and the RL Tuning window edits the same numbers and writes them back)
        std::filesystem::path tuningPath;
        std::filesystem::file_time_type tuningStamp{};
        float tuningPoll = 0.0f;
        std::string tuningMessage;

        explicit Impl(input::InputManager& inputManager)
            : input(inputManager)
        {
        }

        // The match that is shown: the offline one, the host's or the client's own simulation
        const rl::RlMatch* View() const
        {
            if (match != nullptr)
            {
                return match.get();
            }
            if (netHost != nullptr)
            {
                return netHost->Match();
            }
            return netClient != nullptr ? netClient->Match() : nullptr;
        }

        double OnlineNow() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - onlineEpoch).count(); }

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
            StopOnline();
            mode = Mode::Menu;
            scanned = false;
            active = false;
            status.clear();
            hud.clear();
            accumulator = 0.0f;
            cameraState = rl::RlCameraState{};
            jumpLatched = false;
            haveCameraHome = false;
            core::ServiceLocator::GetEditorRuntimeState().gameCapturesMouse = false;
        }

        std::filesystem::file_time_type FileStamp() const
        {
            std::error_code ec;
            const auto stamp = std::filesystem::last_write_time(tuningPath, ec);
            return ec ? std::filesystem::file_time_type{} : stamp;
        }

        // The numbers changed (by the window or by the file): the running match takes them at once.
        void ApplyTuning()
        {
            if (match != nullptr)
            {
                match->SetTuning(tuning);
                tuning = match->Tuning();  // what stays fixed in a running match is put back
            }
        }

        std::string ReloadTuning()
        {
            std::string error;
            rl::RlTuning loaded;
            if (!loaded.LoadFromFile(tuningPath.generic_u8string(), &error))
            {
                return error;
            }
            tuning = loaded;
            tuningStamp = FileStamp();
            ApplyTuning();
            return "rl_tuning.json read again";
        }

        std::string SaveTuning()
        {
            std::ofstream file(tuningPath, std::ios::binary | std::ios::trunc);
            const std::string text = tuning.ToJson() + '\n';
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.close();
            if (!file)
            {
                return "cannot write " + tuningPath.generic_u8string();
            }
            tuningStamp = FileStamp();  // our own write is not a change to read back
            return "saved to " + tuningPath.generic_u8string();
        }

        void PollTuningFile(const float deltaTime)
        {
            tuningPoll += deltaTime;
            if (tuningPoll < 0.5f)
            {
                return;
            }
            tuningPoll = 0.0f;
            const auto stamp = FileStamp();
            if (stamp != tuningStamp && stamp != std::filesystem::file_time_type{})
            {
                tuningMessage = ReloadTuning();
            }
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
            tuningPath = core::ServiceLocator::GetProjectContext().ResolveContentPath("assets/data/rl_tuning.json");
            tuning = rl::RlTuning{};
            if (!tuning.LoadFromFile(tuningPath.generic_u8string(), &error))
            {
                status = "rl_tuning.json: " + error + " (defaults)";
            }
            tuningStamp = FileStamp();
            tuningMessage.clear();
            if (const auto* cam = camera != kInvalidEntity ? world.TryGet<components::CameraComponent>(camera) : nullptr)
            {
                cameraHome = *cam;
                haveCameraHome = true;
            }
            LoadControls();
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
            cameraState = rl::RlCameraState{};
            BeginMatchVisuals(world);
        }

        // Shows the cars of the mode in menuMode, the ball, the marker and the pads
        void BeginMatchVisuals(World& world)
        {
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

        // the shipped bindings, then the file of the player (%APPDATA%/myengine/rl_controls.json)
        void LoadControls()
        {
            std::string error;
            const std::string shipped = core::ServiceLocator::GetProjectContext().ResolveContentPath("assets/data/rl_controls.json").generic_u8string();
            std::string user;
            wchar_t buffer[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
            if (length > 0 && length < std::size(buffer))
            {
                user = (std::filesystem::path(buffer) / L"myengine" / L"rl_controls.json").generic_u8string();
            }
            rl::RlControls loaded;
            if (loaded.LoadLayered(shipped, user, &error))
            {
                controls = loaded;
            }
            else
            {
                controls = rl::RlControls::Defaults();
                status = error + " (default controls)";
            }
        }

        // The devices of this frame: the keys and buttons that are down, and (second snapshot) the ones that went down since the last frame,
        // which is how a click shorter than a frame is not lost.
        void BuildRaw(const float deltaTime, const bool windowActive, rl::RlRawInput& raw, rl::RlRawInput& pressed)
        {
            if (windowActive)
            {
                for (int code = 0; code < 256; ++code)
                {
                    raw.keys[code] = input.IsKeyDown(static_cast<std::uint32_t>(code));
                    pressed.keys[code] = input.WasKeyPressed(static_cast<std::uint32_t>(code));
                }
                const core::MouseButton buttons[3] = {core::MouseButton::Left, core::MouseButton::Right, core::MouseButton::Middle};
                for (int i = 0; i < 3; ++i)
                {
                    raw.mouse[i] = input.IsMouseDown(buttons[i]);
                    pressed.mouse[i] = input.WasMousePressed(buttons[i]);
                }
            }

            padInUse = false;
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
            raw.padConnected = true;
            const XINPUT_GAMEPAD& g = state.Gamepad;
            raw.padAxes[rl::PadLeftX] = static_cast<float>(g.sThumbLX) / 32767.0f;
            raw.padAxes[rl::PadLeftY] = static_cast<float>(g.sThumbLY) / 32767.0f;
            raw.padAxes[rl::PadRightX] = static_cast<float>(g.sThumbRX) / 32767.0f;
            raw.padAxes[rl::PadRightY] = static_cast<float>(g.sThumbRY) / 32767.0f;
            raw.padAxes[rl::PadLeftTrigger] = static_cast<float>(g.bLeftTrigger) / 255.0f;
            raw.padAxes[rl::PadRightTrigger] = static_cast<float>(g.bRightTrigger) / 255.0f;
            const struct
            {
                WORD mask;
                rl::RlPadButton button;
            } map[] = {{XINPUT_GAMEPAD_A, rl::PadA},
                       {XINPUT_GAMEPAD_B, rl::PadB},
                       {XINPUT_GAMEPAD_X, rl::PadX},
                       {XINPUT_GAMEPAD_Y, rl::PadY},
                       {XINPUT_GAMEPAD_LEFT_SHOULDER, rl::PadLB},
                       {XINPUT_GAMEPAD_RIGHT_SHOULDER, rl::PadRB},
                       {XINPUT_GAMEPAD_BACK, rl::PadBack},
                       {XINPUT_GAMEPAD_START, rl::PadStart},
                       {XINPUT_GAMEPAD_LEFT_THUMB, rl::PadLeftStick},
                       {XINPUT_GAMEPAD_RIGHT_THUMB, rl::PadRightStick},
                       {XINPUT_GAMEPAD_DPAD_UP, rl::PadDpadUp},
                       {XINPUT_GAMEPAD_DPAD_DOWN, rl::PadDpadDown},
                       {XINPUT_GAMEPAD_DPAD_LEFT, rl::PadDpadLeft},
                       {XINPUT_GAMEPAD_DPAD_RIGHT, rl::PadDpadRight}};
            for (const auto& m : map)
            {
                if ((g.wButtons & m.mask) != 0)
                {
                    raw.padButtons |= 1u << m.button;
                }
            }
            padInUse = g.wButtons != 0 || g.bLeftTrigger > 12 || g.bRightTrigger > 12 || std::abs(static_cast<int>(g.sThumbLX)) > 6500 ||
                       std::abs(static_cast<int>(g.sThumbLY)) > 6500;
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
            core::ServiceLocator::GetEditorRuntimeState().gameCapturesMouse = true;
            PollTuningFile(deltaTime);

            const bool windowActive = input.GetActiveWindowId() != 0;
            rl::RlRawInput raw;
            rl::RlRawInput pressed;
            BuildRaw(deltaTime, windowActive, raw, pressed);
            const rl::RlControlState state = controls.Evaluate(raw);
            const rl::RlControlState justPressed = controls.Evaluate(pressed);
            const auto edge = [&](rl::RlButtonAction action)
            {
                const int i = static_cast<int>(action);
                const bool now = state.buttons[i] || justPressed.buttons[i];
                const bool fired = now && !previousDown[i];
                previousDown[i] = state.buttons[i];
                return fired;
            };
            const bool ballCamPressed = edge(rl::RlButtonAction::BallCam);
            const bool menuPressed = edge(rl::RlButtonAction::Menu);
            edge(rl::RlButtonAction::Scoreboard);
            scoreboard = state.Down(rl::RlButtonAction::Scoreboard);

            if (online.role != RlOnlineOptions::Role::None)
            {
                UpdateOnline(world, deltaTime, state, justPressed, windowActive, ballCamPressed, menuPressed);
                return;
            }

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
                    if (input.WasKeyPressed(VK_RETURN) || (padConnected && (raw.padButtons & (1u << rl::PadA)) != 0 && windowActive))
                    {
                        StartMatch(world);
                        return;
                    }
                }
                BuildMenuHud();
                return;
            }

            if (ballCamPressed)
            {
                ballCam = !ballCam;
            }
            const bool enter = windowActive && input.WasKeyPressed(VK_RETURN);
            if (menuPressed || (enter && match->World().match.phase == rl::RlMatchPhase::Finished))
            {
                ToMenu(world);
                BuildMenuHud();
                return;
            }
            rl::RlCarInput player = state.car;
            lastPlayer = player;
            if (justPressed.car.jump)
            {
                jumpLatched = true;  // a click shorter than a frame still jumps
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
            const rl::RlMatch* view = View();
            float alpha = std::clamp(accumulator / rl::kTickDt, 0.0f, 1.0f);
            const rl::RlWorldState* nowWorld = &view->World();
            const rl::RlWorldState* beforeWorld = &view->PreviousWorld();
            rl::RlWorldState clientWorld;
            if (netClient != nullptr)
            {
                // already interpolated between its last two ticks, with the fading correction of a rollback
                clientWorld = netClient->RenderWorld();
                nowWorld = beforeWorld = &clientWorld;
                alpha = 0.0f;
            }
            else if (netHost != nullptr)
            {
                alpha = netHost->TickFraction();
            }
            const rl::RlWorldState& now = *nowWorld;
            const rl::RlWorldState& before = *beforeWorld;
            const rl::RlTuning& t = view->Tuning();
            const int teamSize = std::max(view->SlotCount() / 2, 1);

            dx::XMVECTOR playerPosition = dx::XMVectorZero();
            dx::XMVECTOR playerRotation = dx::XMQuaternionIdentity();

            for (int slot = 0; slot < view->SlotCount(); ++slot)
            {
                const rl::RlCarState& a = before.cars[slot];
                const rl::RlCarState& b = now.cars[slot];
                const bool jumped = a.demolished != b.demolished || before.match.kickoffs != now.match.kickoffs;  // no sliding from the park spot or the last kick-off
                const float s = jumped ? 1.0f : alpha;
                const dx::XMVECTOR position = dx::XMVectorLerp(P(a.position), P(b.position), s);
                const dx::XMVECTOR rotation = dx::XMQuaternionSlerp(Q(a.rotation), Q(b.rotation), s);
                if (slot == localSlot)
                {
                    playerPosition = position;
                    playerRotation = rotation;
                }
                const int team = view->Team(slot);
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
                    const bool frontWheel = w < 2;
                    const float side = (w & 1) ? 1.0f : -1.0f;
                    const float z = frontWheel ? t.car.wheelFrontZ : t.car.wheelRearZ;
                    const float radius = frontWheel ? t.car.wheelRadiusFront : t.car.wheelRadiusRear;
                    // the wheel hangs under its mount by the length of the spring
                    const float drop = (frontWheel ? t.car.suspensionRestFront : t.car.suspensionRestRear) - b.wheelCompression[w];
                    const dx::XMVECTOR local = dx::XMVectorSet(side * (frontWheel ? t.car.wheelMountXFront : t.car.wheelMountXRear), t.car.wheelMountYOffset - drop, z, 0.0f);
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
                SetVisible(world, marker, !now.cars[localSlot].demolished);
            }

            UpdateCamera(world, deltaTime, now.cars[localSlot], playerPosition, playerRotation, ballPosition, *view);
        }

        // The camera of Rocket League (game/rl RlCamera): behind the car, over it at the ball (ball cam), the up follows the surface, and it
        // is pulled in so that it stays inside the arena.
        void UpdateCamera(World& world, const float deltaTime, const rl::RlCarState& car, dx::FXMVECTOR carPosition, dx::FXMVECTOR carRotation,
                          dx::FXMVECTOR ballPosition, const rl::RlMatch& view)
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
            rl::RlCameraInput in;
            const Vec3 p = ToVec3(carPosition);
            in.carPosition = {p.x, p.y, p.z};
            dx::XMFLOAT4 q;
            dx::XMStoreFloat4(&q, carRotation);
            in.carRotation = {q.x, q.y, q.z, q.w};
            in.onSurface = car.wheelContacts >= 3;
            in.surfaceNormal = car.surfaceNormal;
            const Vec3 b = ToVec3(ballPosition);
            in.ballPosition = {b.x, b.y, b.z};
            in.ballCam = ballCam;
            const rl::RlCameraTuning& cameraTuning = view.Tuning().camera;
            const rl::RlCameraView cameraView = rl::RlCameraUpdate(cameraTuning, cameraState, view.Sim(), in, std::min(deltaTime, 0.1f));

            dx::XMMATRIX basis;
            basis.r[0] = dx::XMVectorSet(cameraView.right.x, cameraView.right.y, cameraView.right.z, 0.0f);
            basis.r[1] = dx::XMVectorSet(cameraView.up.x, cameraView.up.y, cameraView.up.z, 0.0f);
            basis.r[2] = dx::XMVectorSet(cameraView.forward.x, cameraView.forward.y, cameraView.forward.z, 0.0f);
            basis.r[3] = dx::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
            cam->position = {cameraView.position.x, cameraView.position.y, cameraView.position.z};
            cam->rotationDeg = EulerDegrees(dx::XMQuaternionRotationMatrix(basis));
            // RL's field of view is the horizontal one; the camera component takes the vertical one (16:9 assumed)
            constexpr float kAspect = 16.0f / 9.0f;
            cam->fovYDeg = 2.0f * std::atan(std::tan(cameraTuning.fov * 0.5f / kRadToDeg) / kAspect) * kRadToDeg;
            cam->isPrimary = true;
        }

        // one line with the main bindings, from the loaded controls
        std::string Describe() const
        {
            const auto join = [](const std::vector<std::string>& tokens)
            {
                std::string out;
                for (size_t i = 0; i < tokens.size() && i < 2; ++i)
                {
                    out += (i > 0 ? "/" : "") + tokens[i];
                }
                return out;
            };
            return "boost " + join(controls.ButtonTokens(rl::RlButtonAction::Boost)) + "   jump " + join(controls.ButtonTokens(rl::RlButtonAction::Jump)) +
                   "   ball cam " + join(controls.ButtonTokens(rl::RlButtonAction::BallCam)) + "   powerslide/air roll " +
                   join(controls.ButtonTokens(rl::RlButtonAction::Powerslide)) + "   scores " + join(controls.ButtonTokens(rl::RlButtonAction::Scoreboard)) +
                   "   menu " + join(controls.ButtonTokens(rl::RlButtonAction::Menu));
        }

        // ---- online (G5) --------------------------------------------------------------------------------------------------------------

        void StopOnline()
        {
            netHost.reset();
            netClient.reset();
            if (transport != nullptr)
            {
                transport->Close();
                transport.reset();
            }
            onlineVisuals = false;
            localSlot = 0;
            onlineStatus.clear();
        }

        // The network of this window: UDP with the simulated lag of --net-sim
        bool StartOnline()
        {
            rl::net::NetSimParams sim;
            std::string error;
            if (!online.netSim.empty() && !rl::net::NetSimParams::Parse(online.netSim, sim, &error))
            {
                onlineStatus = error;
                return false;
            }
            transport = rl::net::MakeUdpTransport(sim, static_cast<uint64_t>(GetCurrentProcessId()) * 2654435761u);
            if (online.role == RlOnlineOptions::Role::Host)
            {
                rl::RlNetHostOptions options;
                options.match.mode = std::clamp(online.playersPerTeam, 1, 4);
                options.match.botLevel = botLevel;
                options.match.seed = 0x5EED0000ull + static_cast<uint64_t>(GetCurrentProcessId());
                netHost = std::make_unique<rl::RlNetHost>(*transport, tuning, options);
                if (!netHost->Listen(online.port, error))
                {
                    onlineStatus = "cannot listen on port " + std::to_string(online.port) + ": " + error;
                    netHost.reset();
                    return false;
                }
                localSlot = 0;
                menuMode = options.match.mode;
                onlineStatus = Format("hosting on UDP port %u", static_cast<unsigned>(online.port));
            }
            else
            {
                netClient = std::make_unique<rl::RlNetClient>(*transport, tuning);
                if (!netClient->Connect(online.address, "player", error))
                {
                    onlineStatus = error;
                    netClient.reset();
                    return false;
                }
                onlineStatus = "connecting to " + online.address;
            }
            return true;
        }

        void BuildOnlineWaitHud(const std::string& line)
        {
            hud.clear();
            hud.push_back("=== ROCKET LEAGUE ONLINE ===");
            hud.push_back(line);
            hud.push_back("M - back to the menu      Esc - stop Play");
            if (!onlineStatus.empty())
            {
                hud.push_back(onlineStatus);
            }
        }

        void UpdateOnline(World& world, const float deltaTime, const rl::RlControlState& state, const rl::RlControlState& justPressed, const bool windowActive,
                          const bool ballCamPressed, const bool menuPressed)
        {
            if (netHost == nullptr && netClient == nullptr && !StartOnline())
            {
                BuildOnlineWaitHud("cannot start the network");
                if (menuPressed)
                {
                    online = RlOnlineOptions{};
                    StopOnline();
                    BuildMenuHud();
                }
                return;
            }
            if (menuPressed)
            {
                // back to the menu: the offline game
                online = RlOnlineOptions{};
                StopOnline();
                ToMenu(world);
                BuildMenuHud();
                return;
            }
            if (ballCamPressed)
            {
                ballCam = !ballCam;
            }

            rl::RlCarInput player = state.car;
            lastPlayer = player;
            if (justPressed.car.jump)
            {
                jumpLatched = true;
            }
            if (jumpLatched)
            {
                player.jump = true;
                jumpLatched = false;
            }
            const double now = OnlineNow();

            if (netHost != nullptr)
            {
                netHost->SetLocalInput(player);
                netHost->Update(now);
                if (!netHost->Started())
                {
                    const bool enter = windowActive && input.WasKeyPressed(VK_RETURN);
                    if (netHost->ClientCount() >= online.expectedClients || enter)
                    {
                        netHost->StartMatch(now);
                    }
                    else
                    {
                        BuildOnlineWaitHud(Format("waiting for %d player(s): %d joined   (Enter - start now with bots)", online.expectedClients, netHost->ClientCount()));
                        return;
                    }
                }
            }
            else
            {
                netClient->SetLocalInput(player);
                netClient->Update(now);
                const rl::RlNetClientState clientState = netClient->State();
                if (clientState == rl::RlNetClientState::Failed || clientState == rl::RlNetClientState::Disconnected)
                {
                    HideEverything(world);
                    onlineVisuals = false;
                    BuildOnlineWaitHud("cannot play: " + netClient->Error());
                    return;
                }
                if (clientState != rl::RlNetClientState::Playing)
                {
                    BuildOnlineWaitHud(clientState == rl::RlNetClientState::Lobby ? "joined; waiting for the host to start the match"
                                                                                   : "connecting to " + online.address + " ...");
                    return;
                }
                localSlot = netClient->Slot();
            }

            if (!onlineVisuals && View() != nullptr)
            {
                menuMode = View()->SlotCount() == 1 ? 0 : View()->SlotCount() / 2;
                BeginMatchVisuals(world);
                mode = Mode::Match;
                cameraState = rl::RlCameraState{};
                onlineVisuals = true;
            }
            if (!onlineVisuals)
            {
                return;
            }
            ApplyToScene(world, deltaTime);
            BuildMatchHud();
            const rl::RlNetStats& stats = netHost != nullptr ? netHost->Stats() : netClient->Stats();
            if (netClient != nullptr)
            {
                hud.push_back(Format("NET client slot %d   ping %.0f ms   tick %u   lead %d   corrections/s %d   (rollbacks %llu, hard %llu)", localSlot, stats.rttMs, stats.tick,
                                     stats.lead, stats.rollbacksLastSecond, static_cast<unsigned long long>(stats.rollbacks), static_cast<unsigned long long>(stats.hardCorrections)));
            }
            else
            {
                hud.push_back(Format("NET host   guests %d   ping %.0f ms   tick %u   late inputs %llu", netHost->ClientCount(), stats.rttMs, stats.tick,
                                     static_cast<unsigned long long>(stats.lateInputs)));
            }
        }

        void BuildMenuHud()
        {
            hud.clear();
            hud.push_back("=== ROCKET LEAGUE ===");
            const char* names[5] = {"practice (one car and the ball)", "1v1", "2v2", "3v3", "4v4"};
            hud.push_back(Format("mode: %s   (1-4 = players per team, P = practice)", names[menuMode]));
            const char* levels[3] = {"easy", "normal", "hard"};
            hud.push_back(Format("bots: %s   (L = change)", levels[botLevel]));
            hud.push_back("Enter - start      M - back to this menu      Esc - stop Play      (the controls are in assets/data/rl_controls.json)");
            hud.push_back("W/S gas (pitch in the air)  A/D steer (yaw)  Q/E roll  jump twice + direction = dodge");
            hud.push_back(Describe());
            if (!status.empty())
            {
                hud.push_back(status);
            }
        }

        void BuildMatchHud()
        {
            hud.clear();
            const rl::RlMatch* view = View();
            const rl::RlWorldState& w = view->World();
            const rl::RlMatchState& m = w.match;
            const rl::RlCarState& car = w.cars[localSlot];
            const int teamSize = std::max(view->SlotCount() / 2, 1);
            const char* teamName[2] = {"BLUE", "ORANGE"};

            if (view->SlotCount() == 1)
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
                std::string author = m.lastGoalScorer < 0 ? "" : Format("  (%s%s %d)", teamName[view->Team(m.lastGoalScorer)], m.lastGoalOwn != 0 ? " own goal" : "",
                                                                       m.lastGoalScorer % teamSize + 1);
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
            hud.push_back(Format("input: gas %.1f steer %.1f pitch %.1f roll %.1f %s%s%s%s", lastPlayer.throttle, lastPlayer.steer, lastPlayer.pitch, lastPlayer.roll,
                                 lastPlayer.boost ? " BOOST" : "", lastPlayer.jump ? " JUMP" : "", lastPlayer.handbrake ? " POWERSLIDE" : "", lastPlayer.airRoll ? " AIRROLL" : ""));
            hud.push_back(Describe());
            if (scoreboard)
            {
                hud.push_back("--- SCOREBOARD ---");
                for (int slot = 0; slot < view->SlotCount(); ++slot)
                {
                    hud.push_back(Format("%-6s %s %d   goals %d   hits %d", teamName[view->Team(slot)], slot == localSlot ? "you   " : (view->IsBot(slot) ? "bot   " : "human "),
                                         slot % teamSize + 1, m.goals[slot], m.touches[slot]));
                }
            }
            if (!status.empty())
            {
                hud.push_back(status);
            }
        }
    };

    RlGameSystem::RlGameSystem(input::InputManager& input)
        : impl_(std::make_unique<Impl>(input))
    {
    }

    RlGameSystem::~RlGameSystem() = default;

    void RlGameSystem::SetOnlineOptions(const RlOnlineOptions& options)
    {
        impl_->online = options;
    }

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

    rl::RlTuning* RlGameSystem::EditableTuning()
    {
        return impl_->active ? &impl_->tuning : nullptr;
    }

    void RlGameSystem::TuningEdited()
    {
        impl_->ApplyTuning();
    }

    std::string RlGameSystem::SaveTuning()
    {
        return impl_->SaveTuning();
    }

    std::string RlGameSystem::ReloadTuning()
    {
        impl_->tuningMessage = impl_->ReloadTuning();
        return impl_->tuningMessage;
    }
}
