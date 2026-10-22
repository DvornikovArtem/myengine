// RlGameSystem.cpp

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>

#include <DirectXMath.h>

#include <myengine/audio/AudioSystem.h>
#include <myengine/core/AppPaths.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/systems/RlGameSystem.h>
#include <myengine/editor/EditorState.h>
#include <myengine/fx/ParticleSystem.h>
#include <myengine/game/RlFeedback.h>
#include <myengine/game/RlTrainingStore.h>
#include <myengine/input/InputManager.h>
#include <myengine/ui/rl/RlUiInput.h>

#include <rl/RlCamera.h>
#include <rl/RlControls.h>
#include <rl/RlMatch.h>
#include <rl/RlNet.h>
#include <rl/RlTraining.h>
#include <rl/RlTrainingAutopilot.h>
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
        EntityId lights[2][4] = {};     // the headlights and tail lights of the car (a mesh of their own)
        EntityId boosts[2][4] = {};     // the flame of the boost
        EntityId pads[rl::kPadCount] = {};
        EntityId orbs[rl::kPadCount] = {};  // the glowing ball over a big pad
        EntityId podium = kInvalidEntity;   // the plinth of the menu
        Vec3 boostBaseScale[2][4] = {};

        float accumulator = 0.0f;
        rl::RlInputLatch latch;      // what was pressed since the last tick, even if it was let go again before the frame ended
        bool ballCam = true;
        rl::RlCameraState cameraState;
        components::CameraComponent cameraHome;
        bool haveCameraHome = false;
        float viewAspect = 16.0f / 9.0f;   // the window: RL's field of view is the horizontal one, the camera component takes the vertical one

        rl::RlControls controls = rl::RlControls::Defaults();
        bool previousDown[static_cast<int>(rl::RlButtonAction::Count)] = {};
        bool scoreboard = false;

        // XInput: an unplugged pad is looked for once a second, polling it every frame is slow
        bool padConnected = false;
        float padRetry = 0.0f;
        bool padInUse = false;

        std::vector<std::string> hud;
        rl::RlCarInput lastPlayer;  // what the player holds, shown in the overlay

        // The sounds and effects of the match (docs/game/rl-feedback.md)
        std::unique_ptr<game::RlFeedback> feedback;
        float effectTime = 0.0f;  // the clock of the flicker of the boost flames

        // The game mode (RlGameFlow): the commands wait here until the next Update, because they change the scene
        bool gameMode = false;
        RlGameStatus status_;
        enum class Command
        {
            Start,
            StartHosted,
            Leave,
            Restart,
        };
        struct Pending
        {
            Command command = Command::Start;
            RlGameMatchSetup setup;
        };
        std::vector<Pending> pending;
        bool paused = false;
        bool pausePressed = false;
        bool podiumShown = false;
        float podiumTime = 0.0f;
        int podiumTeam = 0;
        float finishedAge = 0.0f;       // seconds since the match turned Finished
        float boostHold = 0.0f;         // the boost ring pulses for a moment after the charge fell
        float previousBoost = 0.0f;
        int seenEventHead = 0;
        std::deque<std::uint64_t> seenEvents;   // the events pushed to the HUD already (a rollback shows the same one again)
        std::vector<rlui::HudEvent> hudEvents;
        bool ballCamHold = false;
        bool invertPitch = false;
        rl::RlCameraTuning cameraSettings;
        bool haveCameraSettings = false;
        std::string playerName = "Player";
        bool autoNetDone = false;        // --rl-host / --rl-join in the game mode start the network at once
        bool previewActive = false;      // Settings > Camera: the scene shows the practice field seen through the settings
        // The training (G4g): in the game mode it takes the place of the match; the keys of the exercise are read from the keys that went down this frame
        std::unique_ptr<rl::RlTraining> training;
        std::unique_ptr<rl::RlTrainingAutopilot> autopilot;   // the debug command "autopilot": a bot drives the car
        bool autopilotOn = false;
        rl::RlRawInput trainingPressed;
        bool previewWanted = false;
        float previewTime = 0.0f;
        float connectingFor = 0.0f;
        std::filesystem::path tuningSavePath;  // where the RL Tuning window writes (the user folder in a shipping build)

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
            SetVisible(world, podium, false);
            for (int t = 0; t < 2; ++t)
            {
                for (int i = 0; i < 4; ++i)
                {
                    SetVisible(world, cars[t][i], false);
                    SetVisible(world, lights[t][i], false);
                    SetVisible(world, boosts[t][i], false);
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
            for (EntityId orb : orbs)
            {
                SetVisible(world, orb, false);
            }
            podiumShown = false;
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
            if (feedback != nullptr)
            {
                feedback->EndMatch();
                feedback->StopMenuMusic(0.2f);
            }
            mode = Mode::Menu;
            scanned = false;
            active = false;
            status.clear();
            hud.clear();
            accumulator = 0.0f;
            cameraState = rl::RlCameraState{};
            latch.Clear();
            haveCameraHome = false;
            auto& runtimeState = core::ServiceLocator::GetEditorRuntimeState();
            runtimeState.gameCapturesMouse = false;
            runtimeState.gameHidesCursor = false;
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
            if (feedback != nullptr)
            {
                feedback->SetTuning(tuning);
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
            // a shipping build keeps the assets next to the exe, where nothing may be written: the player's copy goes to the user folder
            const std::filesystem::path& target = tuningSavePath.empty() ? tuningPath : tuningSavePath;
            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            std::ofstream file(target, std::ios::binary | std::ios::trunc);
            const std::string text = tuning.ToJson() + '\n';
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.close();
            if (!file)
            {
                return "cannot write " + target.generic_u8string();
            }
            tuningPath = target;
            tuningStamp = FileStamp();  // our own write is not a change to read back
            return "saved to " + target.generic_u8string();
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
                    lights[t][i] = FindTag(world, Format("%s%dLights", teamNames[t], i));
                    boosts[t][i] = FindTag(world, Format("%s%dBoost", teamNames[t], i));
                    boostBaseScale[t][i] = {1.0f, 1.0f, 1.0f};
                    if (const auto* transform = boosts[t][i] != kInvalidEntity ? world.TryGet<components::TransformComponent>(boosts[t][i]) : nullptr)
                    {
                        boostBaseScale[t][i] = transform->scale;
                    }
                }
            }
            for (int i = 0; i < rl::kPadCount; ++i)
            {
                pads[i] = FindTag(world, Format("RlPad%d", i));
                orbs[i] = FindTag(world, Format("RlPadOrb%d", i));
            }
            podium = FindTag(world, "RlPodium");
            if (arena == kInvalidEntity || ball == kInvalidEntity || cars[0][0] == kInvalidEntity)
            {
                return false;  // not a Rocket League scene
            }

            std::string error;
            tuningPath = core::ServiceLocator::GetProjectContext().ResolveContentPath("assets/data/rl_tuning.json");
            tuningSavePath.clear();
            if (core::apppaths::IsShipping())
            {
                // the player's own copy (saved by the RL Tuning window) is the one that is read, when there is one
                tuningSavePath = core::apppaths::UserDataDirectory() / "rl_tuning.json";
                std::error_code exists;
                if (std::filesystem::exists(tuningSavePath, exists))
                {
                    tuningPath = tuningSavePath;
                }
            }
            tuning = rl::RlTuning{};
            if (!tuning.LoadFromFile(tuningPath.generic_u8string(), &error))
            {
                status = "rl_tuning.json: " + error + " (defaults)";
            }
            // MYENGINE_RL_MATCH_SECONDS=<n>: a short match, for the tests of the results screen
            {
                char* seconds = nullptr;
                std::size_t secondsLength = 0;
                if (_dupenv_s(&seconds, &secondsLength, "MYENGINE_RL_MATCH_SECONDS") == 0 && seconds != nullptr && std::atof(seconds) > 0.0)
                {
                    tuning.match.matchSeconds = static_cast<float>(std::atof(seconds));
                }
                std::free(seconds);
            }
            if (!haveCameraSettings)
            {
                cameraSettings = tuning.camera;
            }
            feedback = std::make_unique<game::RlFeedback>(tuning, &audio::GetAudioSystem(), &fx::GetParticleSystem());
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
            if (gameMode)
            {
                EnterPodium(world, 0);
            }
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
            config.names[0] = playerName;
            match = std::make_unique<rl::RlMatch>(tuning, config);
            mode = Mode::Match;
            accumulator = 0.0f;
            cameraState = rl::RlCameraState{};
            BeginMatchVisuals(world);
            if (feedback != nullptr)
            {
                feedback->StartMatch(0);
            }
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
                float x, z;
                const bool big = rl::RlPadSpot(i, x, z);
                SetVisible(world, orbs[i], big);
                // a scene of the old kind (cubes for pads) has no authored position: put the pad where the game has it
                if (orbs[i] == kInvalidEntity)
                {
                    if (auto* transform = world.TryGet<components::TransformComponent>(pads[i]))
                    {
                        transform->position = {x, 0.04f, z};
                    }
                }
            }
        }

        void ToMenu(World& world)
        {
            match.reset();
            if (feedback != nullptr)
            {
                feedback->EndMatch();
                feedback->PlayMenuMusic();
            }
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

        // The file of the player: %APPDATA%/myengine/rl_controls.json, in a shipping build %LOCALAPPDATA%/RocketArena/rl_controls.json
        static std::filesystem::path UserControlsPath() { return core::apppaths::UserDataDirectory() / "rl_controls.json"; }

        // the shipped bindings, then the file of the player
        void LoadControls()
        {
            std::string error;
            const std::string shipped = core::ServiceLocator::GetProjectContext().ResolveContentPath("assets/data/rl_controls.json").generic_u8string();
            const std::string user = UserControlsPath().generic_u8string();
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
            if (!gameMode)
            {
                core::ServiceLocator::GetEditorRuntimeState().gameCapturesMouse = true;
            }
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

            if (gameMode)
            {
                trainingPressed = pressed;
                UpdateGame(world, deltaTime, state, justPressed, ballCamPressed, menuPressed);
                return;
            }

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
            // A press shorter than a frame (a tap of a key, a click) is not lost: the events of the frame are kept until a tick takes them.
            latch.Press(justPressed.car);

            // ---- fixed ticks
            accumulator += std::min(deltaTime, 0.1f);
            std::vector<rl::RlCarInput> inputs(static_cast<size_t>(match->SlotCount()));
            while (accumulator >= rl::kTickDt)
            {
                accumulator -= rl::kTickDt;
                inputs[0] = latch.Take(player);
                match->Step(inputs.data());
                if (feedback != nullptr)
                {
                    feedback->OnTick(match->PreviousWorld(), match->World());
                }
            }

            ApplyToScene(world, deltaTime);
            BuildMatchHud();
        }

        void ApplyToScene(World& world, const float deltaTime)
        {
            // the match (or the network), or the training when there is no match
            const rl::RlMatch* view = View();
            float alpha = std::clamp(accumulator / rl::kTickDt, 0.0f, 1.0f);
            const rl::RlWorldState* nowWorld = view != nullptr ? &view->World() : &training->World();
            const rl::RlWorldState* beforeWorld = view != nullptr ? &view->PreviousWorld() : &training->PreviousWorld();
            const rl::RlTuning& t = view != nullptr ? view->Tuning() : training->Tuning();
            const rl::RlSim& sim = view != nullptr ? view->Sim() : training->Sim();
            const int slotCount = view != nullptr ? view->SlotCount() : nowWorld->carCount;
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
            const int teamSize = std::max(slotCount / 2, 1);

            dx::XMVECTOR playerPosition = dx::XMVectorZero();
            dx::XMVECTOR playerRotation = dx::XMQuaternionIdentity();

            effectTime += deltaTime;
            for (int slot = 0; slot < slotCount; ++slot)
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
                const int team = view != nullptr ? view->Team(slot) : slot;   // a training: the player is blue, the bot of a kick-off orange
                const int index = slot % teamSize;
                const bool shown = !b.demolished;
                const Vec3 eulerRotation = EulerDegrees(rotation);
                // The meshes of the pack carry their own scale (the scene has it); a scene of the old kind (boxes) is scaled to the hitbox
                const bool oldCubes = lights[team][index] == kInvalidEntity;
                for (const EntityId bodyPart : {cars[team][index], lights[team][index]})
                {
                    if (auto* transform = bodyPart != kInvalidEntity ? world.TryGet<components::TransformComponent>(bodyPart) : nullptr)
                    {
                        transform->position = ToVec3(position);
                        transform->rotationDeg = eulerRotation;
                        if (oldCubes)
                        {
                            transform->scale = {t.car.halfExtentX * 2.0f, t.car.halfExtentY * 2.0f, t.car.halfExtentZ * 2.0f};
                        }
                    }
                    SetVisible(world, bodyPart, shown);
                }
                // the flame of the boost: while the charge falls (the car of the player: while the button is held and there is charge)
                const bool boosting = shown && b.boost > 0.0f && (b.boost < a.boost - 1e-4f || (slot == localSlot && lastPlayer.boost && a.boost > 0.0f));
                if (auto* transform = boosts[team][index] != kInvalidEntity ? world.TryGet<components::TransformComponent>(boosts[team][index]) : nullptr)
                {
                    const dx::XMVECTOR nozzle = dx::XMVector3Rotate(dx::XMVectorSet(0.0f, -0.06f, -0.74f, 0.0f), rotation);
                    transform->position = ToVec3(dx::XMVectorAdd(position, nozzle));
                    transform->rotationDeg = eulerRotation;
                    const float flicker = 1.0f + 0.16f * std::sin(effectTime * 55.0f + static_cast<float>(slot)) + 0.08f * std::sin(effectTime * 23.0f);
                    const Vec3& base = boostBaseScale[team][index];
                    transform->scale = {base.x * flicker, base.y * flicker, base.z * (0.85f + 0.3f * flicker)};
                }
                SetVisible(world, boosts[team][index], boosting);
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
                    const float compression = a.wheelCompression[w] + (b.wheelCompression[w] - a.wheelCompression[w]) * s;
                    const float drop = (frontWheel ? t.car.suspensionRestFront : t.car.suspensionRestRear) - compression;
                    const dx::XMVECTOR local = dx::XMVectorSet(side * (frontWheel ? t.car.wheelMountXFront : t.car.wheelMountXRear), t.car.wheelMountYOffset - drop, z, 0.0f);
                    wheel->position = ToVec3(dx::XMVectorAdd(position, dx::XMVector3Rotate(local, rotation)));
                    wheel->rotationDeg = eulerRotation;
                    if (oldCubes)
                    {
                        wheel->scale = {0.16f, radius, radius};
                    }
                }
            }

            // a training with one car: the orange car of the kick-off exercises is not there
            for (int slot = view != nullptr ? 2 : slotCount; slot < 2; ++slot)
            {
                for (const EntityId part : {cars[slot][0], lights[slot][0], boosts[slot][0]})
                {
                    SetVisible(world, part, false);
                }
                for (int w = 0; w < rl::kWheelCount; ++w)
                {
                    SetVisible(world, wheels[slot][0][w], false);
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
                    if (orbs[0] == kInvalidEntity && orbs[3] == kInvalidEntity)
                    {
                        const float r = t.ball.radius;
                        transform->scale = {r, r, r};
                    }
                }
            }

            // the pads: lit while they wait for a car (a big pad keeps its plate and loses the orb)
            for (int i = 0; i < rl::kPadCount; ++i)
            {
                // a training has pads only in a kick-off (the match behind it); the free play and the drills have none to take
                const bool ready = now.match.pads[i].respawnLeft <= 0.0f && (view != nullptr || now.match.mode != 0);
                if (orbs[i] != kInvalidEntity)
                {
                    SetVisible(world, orbs[i], ready);
                    SetVisible(world, pads[i], true);
                }
                else
                {
                    SetVisible(world, pads[i], ready);
                }
            }
            if (auto* transform = marker != kInvalidEntity ? world.TryGet<components::TransformComponent>(marker) : nullptr)
            {
                transform->position = {dx::XMVectorGetX(playerPosition), dx::XMVectorGetY(playerPosition) + 0.95f, dx::XMVectorGetZ(playerPosition)};
                transform->rotationDeg = {0.0f, 45.0f, 0.0f};
                transform->scale = {0.22f, 0.22f, 0.22f};
                SetVisible(world, marker, !now.cars[localSlot].demolished);
            }

            UpdateCamera(world, deltaTime, now.cars[localSlot], playerPosition, playerRotation, ballPosition, t, sim);
            if (feedback != nullptr && !previewActive)
            {
                feedback->Frame(deltaTime, before, now, alpha);
            }
        }

        // The camera of Rocket League (game/rl RlCamera): behind the car, over it at the ball (ball cam), the up follows the surface, and it
        // is pulled in so that it stays inside the arena.
        void UpdateCamera(World& world, const float deltaTime, const rl::RlCarState& car, dx::FXMVECTOR carPosition, dx::FXMVECTOR carRotation,
                          dx::FXMVECTOR ballPosition, const rl::RlTuning& arenaTuning, const rl::RlSim& sim)
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
            // the player's own camera (Settings > Camera) when the game mode has set it, else the numbers of rl_tuning.json
            const rl::RlCameraTuning& cameraTuning = haveCameraSettings ? cameraSettings : arenaTuning.camera;
            const rl::RlCameraView cameraView = rl::RlCameraUpdate(cameraTuning, cameraState, sim, in, std::min(deltaTime, 0.1f));

            dx::XMMATRIX basis;
            basis.r[0] = dx::XMVectorSet(cameraView.right.x, cameraView.right.y, cameraView.right.z, 0.0f);
            basis.r[1] = dx::XMVectorSet(cameraView.up.x, cameraView.up.y, cameraView.up.z, 0.0f);
            basis.r[2] = dx::XMVectorSet(cameraView.forward.x, cameraView.forward.y, cameraView.forward.z, 0.0f);
            basis.r[3] = dx::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
            cam->position = {cameraView.position.x, cameraView.position.y, cameraView.position.z};
            cam->rotationDeg = EulerDegrees(dx::XMQuaternionRotationMatrix(basis));
            // RL's field of view is the horizontal one; the camera component takes the vertical one
            cam->fovYDeg = 2.0f * std::atan(std::tan(cameraTuning.fov * 0.5f / kRadToDeg) / viewAspect) * kRadToDeg;
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
            // a tap shorter than a frame is not lost on the network either: the frame's input carries what was pressed since the last one
            latch.Press(justPressed.car);
            player = latch.Take(player);
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
                    BuildOnlineWaitHud(clientState == rl::RlNetClientState::Lobby     ? "joined; waiting for the host to start the match"
                                       : clientState == rl::RlNetClientState::Syncing ? "the match is on: getting its state from the host ..."
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
                BeginOnlineFeedback();
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

        // ---- the game mode (RlGameFlow) ------------------------------------------------------------------------------------------------

        static std::string KeyName(const rl::RlControls& controls, const rl::RlButtonAction action, const bool pad)
        {
            for (const std::string& token : controls.ButtonTokens(action))
            {
                const bool isPad = token.rfind("Pad", 0) == 0;
                if (isPad == pad)
                {
                    return rlui::TokenDisplayName(token);
                }
            }
            return {};
        }

        // The podium of the menu: the car of a team on the plinth, the camera that circles it (docs/game/rl-ui.md). The car stays on the right of the
        // screen, the menu is on the left.
        void EnterPodium(World& world, const int team)
        {
            HideEverything(world);
            podiumShown = true;
            podiumTeam = std::clamp(team, 0, 1);
            SetVisible(world, podium, true);
            SetVisible(world, cars[podiumTeam][0], true);
            SetVisible(world, lights[podiumTeam][0], true);
            for (int w = 0; w < rl::kWheelCount; ++w)
            {
                SetVisible(world, wheels[podiumTeam][0][w], true);
            }
            cameraState = rl::RlCameraState{};
        }

        void UpdatePodium(World& world, const float deltaTime)
        {
            if (!podiumShown)
            {
                EnterPodium(world, podiumTeam);
            }
            podiumTime += deltaTime;
            constexpr float kPlinth = 0.26f;   // the height of the plinth (tools/rl_assets/podium.py)
            constexpr float kRest = 0.40f;     // the centre of the hitbox over the floor when the car stands on its wheels
            constexpr float kTwoPi = 6.2831853f;
            const int team = podiumTeam;
            const dx::XMVECTOR carPosition = dx::XMVectorSet(0.0f, kPlinth + kRest, 0.0f, 0.0f);
            const dx::XMVECTOR carRotation = dx::XMQuaternionRotationRollPitchYaw(0.0f, 215.0f / kRadToDeg, 0.0f);
            const Vec3 euler = EulerDegrees(carRotation);
            for (const EntityId part : {cars[team][0], lights[team][0]})
            {
                if (auto* transform = part != kInvalidEntity ? world.TryGet<components::TransformComponent>(part) : nullptr)
                {
                    transform->position = ToVec3(carPosition);
                    transform->rotationDeg = euler;
                }
            }
            for (int w = 0; w < rl::kWheelCount; ++w)
            {
                if (auto* wheel = wheels[team][0][w] != kInvalidEntity ? world.TryGet<components::TransformComponent>(wheels[team][0][w]) : nullptr)
                {
                    const bool front = w < 2;
                    const float side = (w & 1) ? 1.0f : -1.0f;
                    const dx::XMVECTOR local = dx::XMVectorSet(side * (front ? 0.259f : 0.295f), front ? -0.26755f : -0.25055f, front ? 0.37374f : -0.47626f, 0.0f);
                    wheel->position = ToVec3(dx::XMVectorAdd(carPosition, dx::XMVector3Rotate(local, carRotation)));
                    wheel->rotationDeg = euler;
                }
            }
            if (auto* transform = podium != kInvalidEntity ? world.TryGet<components::TransformComponent>(podium) : nullptr)
            {
                transform->position = {0.0f, 0.0f, 0.0f};
                transform->rotationDeg = {0.0f, 0.0f, 0.0f};
            }

            // the camera: a full turn in 40 s, it rises and sinks by about 5 degrees every 20 s
            if (auto* cam = camera != kInvalidEntity ? world.TryGet<components::CameraComponent>(camera) : nullptr)
            {
                const float angle = 0.9f + podiumTime * kTwoPi / 40.0f;
                const float radius = 6.6f;
                const float height = 1.75f + std::sin(podiumTime * kTwoPi / 20.0f) * 0.5f;
                const dx::XMVECTOR eye = dx::XMVectorSet(std::sin(angle) * radius, height, std::cos(angle) * radius, 0.0f);
                const dx::XMVECTOR worldUp = dx::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                dx::XMVECTOR target = dx::XMVectorSet(0.0f, 0.62f, 0.0f, 0.0f);
                dx::XMVECTOR forward = dx::XMVector3Normalize(dx::XMVectorSubtract(target, eye));
                dx::XMVECTOR right = dx::XMVector3Normalize(dx::XMVector3Cross(worldUp, forward));
                // look to the left of the car, so that it stands on the right of the picture
                target = dx::XMVectorSubtract(target, dx::XMVectorScale(right, 1.25f * std::min(viewAspect / (16.0f / 9.0f), 1.3f)));
                forward = dx::XMVector3Normalize(dx::XMVectorSubtract(target, eye));
                right = dx::XMVector3Normalize(dx::XMVector3Cross(worldUp, forward));
                const dx::XMVECTOR up = dx::XMVector3Cross(forward, right);
                dx::XMMATRIX basis;
                basis.r[0] = dx::XMVectorSetW(right, 0.0f);
                basis.r[1] = dx::XMVectorSetW(up, 0.0f);
                basis.r[2] = dx::XMVectorSetW(forward, 0.0f);
                basis.r[3] = dx::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
                cam->position = ToVec3(eye);
                cam->rotationDeg = EulerDegrees(dx::XMQuaternionRotationMatrix(basis));
                cam->fovYDeg = 36.0f;
                cam->isPrimary = true;
            }
        }

        // Ends the match, the lobby or the connection and clears what the screens read
        void StopMatchState(World& world)
        {
            (void)world;
            previewActive = false;
            match.reset();
            training.reset();
            autopilot.reset();
            autopilotOn = false;
            StopOnline();
            if (feedback != nullptr)
            {
                feedback->EndMatch();
            }
            mode = Mode::Menu;
            accumulator = 0.0f;
            cameraState = rl::RlCameraState{};
            latch.Clear();
            paused = false;
            pausePressed = false;
            finishedAge = 0.0f;
            boostHold = 0.0f;
            previousBoost = 0.0f;
            seenEventHead = 0;
            seenEvents.clear();
            hudEvents.clear();
            status_.hud = rlui::HudState{};
            status_.lobby.clear();
            status_.error.clear();
            status_.statusText.clear();
            status_.winner = -1;
            status_.mvp = -1;
            status_.overtimePlayed = false;
            status_.matchSeconds = 0.0;
            status_.online = false;
            status_.isHost = true;
            status_.localSlot = 0;
            status_.stage = RlGameStage::Menu;
        }

        // A match that failed on the network: the screens show the reason
        void FailNetwork(World& world, const std::string& reason)
        {
            StopMatchState(world);
            status_.error = reason;
            status_.stage = RlGameStage::Failed;
            EnterPodium(world, 0);
            if (feedback != nullptr)
            {
                feedback->PlayMenuMusic();
            }
        }

        bool StartNetwork(const RlGameMatchSetup& setup, std::string& error)
        {
            rl::net::NetSimParams sim;
            if (!online.netSim.empty() && !rl::net::NetSimParams::Parse(online.netSim, sim, &error))
            {
                return false;
            }
            static std::uint64_t started = 0;
            ++started;
            if (setup.transportFactory)
            {
                transport = setup.transportFactory(sim);
                status_.setup.transportFactory = nullptr;  // used once; it holds the lobby service
                if (!transport)
                {
                    error = "The lobby could not make the transport of the match";
                    return false;
                }
            }
            else
            {
                transport = rl::net::MakeUdpTransport(sim, static_cast<std::uint64_t>(GetCurrentProcessId()) * 2654435761u + started);
            }
            onlineEpoch = std::chrono::steady_clock::now();
            if (setup.kind == RlGameMatchSetup::Kind::Host)
            {
                rl::RlNetHostOptions options;
                options.match.mode = std::clamp(setup.mode, 1, 4);
                options.match.botLevel = setup.botLevel;
                options.match.seed = 0x5EED0000ull + static_cast<std::uint64_t>(GetCurrentProcessId()) + started;
                options.match.names[0] = playerName;
                netHost = std::make_unique<rl::RlNetHost>(*transport, tuning, options);
                if (!netHost->Listen(setup.port, error))
                {
                    error = "Cannot listen on port " + std::to_string(setup.port) + ": " + error;
                    netHost.reset();
                    transport->Close();
                    transport.reset();
                    return false;
                }
                localSlot = 0;
                menuMode = options.match.mode;
            }
            else
            {
                netClient = std::make_unique<rl::RlNetClient>(*transport, tuning);
                if (!netClient->Connect(setup.address, playerName, error))
                {
                    netClient.reset();
                    transport->Close();
                    transport.reset();
                    return false;
                }
            }
            return true;
        }

        void StartGameMatch(World& world, const RlGameMatchSetup& wanted)
        {
            RlGameMatchSetup setup = wanted;
            if (!setup.playerName.empty())
            {
                playerName = setup.playerName;
            }
            setup.playerName = playerName;
            status_.setup = setup;
            status_.online = setup.kind == RlGameMatchSetup::Kind::Host || setup.kind == RlGameMatchSetup::Kind::Join;
            status_.isHost = setup.kind != RlGameMatchSetup::Kind::Join;
            status_.error.clear();
            if (setup.kind == RlGameMatchSetup::Kind::Training)
            {
                StartTraining(world, setup);
                return;
            }
            if (setup.kind == RlGameMatchSetup::Kind::Offline)
            {
                menuMode = std::clamp(setup.mode, 0, 4);
                botLevel = std::clamp(setup.botLevel, 0, 2);
                StartMatch(world);
                localSlot = 0;
                status_.localSlot = 0;
                paused = false;
                status_.stage = RlGameStage::Playing;
                if (feedback != nullptr)
                {
                    feedback->SetOwnSlot(0);
                }
                return;
            }
            std::string error;
            if (!StartNetwork(setup, error))
            {
                FailNetwork(world, error);
                return;
            }
            status_.stage = setup.kind == RlGameMatchSetup::Kind::Host ? RlGameStage::Lobby : RlGameStage::Connecting;
            status_.statusText = setup.kind == RlGameMatchSetup::Kind::Host ? Format("Hosting on UDP port %u", static_cast<unsigned>(setup.port))
                                                                            : "Connecting to " + setup.address + " ...";
            EnterPodium(world, 0);
        }

        void ProcessCommands(World& world)
        {
            std::vector<Pending> commands;
            commands.swap(pending);
            for (Pending& c : commands)
            {
                switch (c.command)
                {
                case Command::Start:
                    StopMatchState(world);
                    StartGameMatch(world, c.setup);
                    break;
                case Command::StartHosted:
                    if (netHost != nullptr && !netHost->Started())
                    {
                        netHost->StartMatch(OnlineNow());
                    }
                    break;
                case Command::Leave:
                    StopMatchState(world);
                    EnterPodium(world, 0);
                    if (feedback != nullptr)
                    {
                        feedback->PlayMenuMusic();
                    }
                    break;
                case Command::Restart:
                {
                    const RlGameMatchSetup setup = status_.setup;
                    StopMatchState(world);
                    StartGameMatch(world, setup);
                    break;
                }
                }
            }
        }

        void BuildLobby()
        {
            status_.lobby.clear();
            if (netHost != nullptr)
            {
                const int teamSize = std::clamp(status_.setup.mode, 1, 4);
                const std::vector<rl::RlNetPeerInfo> peers = netHost->Peers();
                for (int slot = 0; slot < teamSize * 2; ++slot)
                {
                    RlLobbySlot entry;
                    entry.team = slot < teamSize ? 0 : 1;
                    if (slot == 0)
                    {
                        entry.name = playerName;
                        entry.you = true;
                        entry.host = true;
                    }
                    else
                    {
                        const auto it = std::find_if(peers.begin(), peers.end(), [slot](const rl::RlNetPeerInfo& peer) { return peer.slot == slot; });
                        if (it != peers.end())
                        {
                            entry.name = it->name.empty() ? "Player" : it->name;
                            entry.pingMs = static_cast<int>(std::lround(it->rttMs));
                        }
                        else
                        {
                            entry.bot = status_.setup.fillWithBots;
                            entry.name = status_.setup.fillWithBots ? "Bot" : "";
                        }
                    }
                    status_.lobby.push_back(entry);
                }
                status_.statusText = Format("Hosting on UDP port %u · %d of %d players joined", static_cast<unsigned>(status_.setup.port), 1 + static_cast<int>(peers.size()),
                                            teamSize * 2);
            }
            else if (netClient != nullptr)
            {
                RlLobbySlot entry;
                entry.name = playerName;
                entry.you = true;
                entry.team = netClient->Slot() < 0 ? 0 : (netClient->Slot() < std::max(status_.setup.mode, 1) ? 0 : 1);
                status_.lobby.push_back(entry);
                status_.statusText = "Joined · waiting for the host to start the match";
            }
        }

        // The network of the game mode for one frame; true when the online match is on the field. The stages Connecting / Lobby / Failed are set here.
        bool NetStep(World& world, const rl::RlCarInput& player)
        {
            const double now = OnlineNow();
            if (netHost != nullptr)
            {
                netHost->SetLocalInput(player);
                netHost->Update(now);
                if (!netHost->Started())
                {
                    status_.stage = RlGameStage::Lobby;
                    BuildLobby();
                    // --rl-host: the match starts when the guests are in
                    if (online.role == RlOnlineOptions::Role::Host && netHost->ClientCount() >= online.expectedClients)
                    {
                        netHost->StartMatch(now);
                    }
                    return false;
                }
            }
            else if (netClient != nullptr)
            {
                netClient->SetLocalInput(player);
                netClient->Update(now);
                const rl::RlNetClientState clientState = netClient->State();
                if (clientState == rl::RlNetClientState::Failed || clientState == rl::RlNetClientState::Disconnected)
                {
                    FailNetwork(world, netClient->Error().empty() ? "The connection was closed" : netClient->Error());
                    return false;
                }
                if (clientState != rl::RlNetClientState::Playing)
                {
                    if (clientState == rl::RlNetClientState::Lobby)
                    {
                        status_.stage = RlGameStage::Lobby;
                        BuildLobby();
                    }
                    else
                    {
                        status_.stage = RlGameStage::Connecting;
                        status_.statusText = "Connecting to " + status_.setup.address + " ...";
                    }
                    return false;
                }
                localSlot = netClient->Slot();
            }
            else
            {
                return false;
            }
            if (!onlineVisuals && View() != nullptr)
            {
                menuMode = View()->SlotCount() == 1 ? 0 : View()->SlotCount() / 2;
                BeginMatchVisuals(world);
                mode = Mode::Match;
                cameraState = rl::RlCameraState{};
                onlineVisuals = true;
                BeginOnlineFeedback();
                paused = false;
                status_.stage = RlGameStage::Playing;
                status_.localSlot = localSlot;
                finishedAge = 0.0f;
            }
            return onlineVisuals;
        }

        void BeginOnlineFeedback()
        {
            if (feedback == nullptr)
            {
                return;
            }
            feedback->StartMatch(netClient != nullptr ? netClient->Slot() : 0);
            auto observer = [this](std::uint32_t, const rl::RlMatch& m) { feedback->OnTick(m.PreviousWorld(), m.World()); };
            if (netHost != nullptr)
            {
                netHost->SetTickObserver(observer);
            }
            if (netClient != nullptr)
            {
                netClient->SetTickObserver(observer);
            }
        }

        // The HUD of the match and what the screens read about it
        void BuildGameStatus(const float deltaTime, const bool boostHeld)
        {
            const rl::RlMatch* view = View();
            if (view == nullptr)
            {
                return;
            }
            const rl::RlWorldState& w = view->World();
            const rl::RlMatchState& m = w.match;
            const int slot = std::clamp(localSlot, 0, std::max(view->SlotCount() - 1, 0));
            const rl::RlCarState& car = w.cars[slot];
            rlui::HudState& h = status_.hud;
            h.phase = static_cast<rlui::HudPhase>(static_cast<int>(m.phase));
            h.phaseTimer = m.phaseTimer;
            h.clock = m.clock;
            h.overtime = m.overtime != 0;
            h.mode = m.mode;
            h.score[0] = m.score[0];
            h.score[1] = m.score[1];
            h.boost = car.boost;
            if (car.boost < previousBoost - 1e-3f || (boostHeld && car.boost > 0.0f))
            {
                boostHold = 0.12f;
            }
            else
            {
                boostHold = std::max(0.0f, boostHold - deltaTime);
            }
            previousBoost = car.boost;
            h.boosting = boostHold > 0.0f && !car.demolished;
            h.demolished = car.demolished;
            h.respawnTimer = car.respawnTimer;
            h.lastGoalTeam = m.lastGoalTeam;
            h.lastGoalScorer = m.lastGoalScorer;
            h.lastGoalOwn = m.lastGoalOwn != 0;
            h.lastGoalSpeedKmh = m.lastGoalSpeed * 3.6f;
            h.ballCam = ballCam;
            h.ballCamKey = status_.ballCamKey.empty() ? "Space" : status_.ballCamKey;
            h.localPlayer = slot;
            h.players.resize(static_cast<size_t>(view->SlotCount()));
            for (int i = 0; i < view->SlotCount(); ++i)
            {
                rlui::HudPlayer& p = h.players[static_cast<size_t>(i)];
                p.name = m.names[i];
                p.team = view->Team(i);
                p.bot = view->IsBot(i);
                p.score = m.points[i];
                p.goals = m.goals[i];
                p.assists = m.assists[i];
                p.saves = m.saves[i];
                p.shots = m.shots[i];
                p.demos = m.demos[i];
                p.pingMs = -1;
            }
            h.pingMs = -1;
            if (netClient != nullptr)
            {
                h.pingMs = static_cast<int>(std::lround(netClient->Stats().rttMs));
                h.players[static_cast<size_t>(slot)].pingMs = h.pingMs;
            }
            else if (netHost != nullptr)
            {
                h.pingMs = 0;
                h.players[0].pingMs = 0;
                for (const rl::RlNetPeerInfo& peer : netHost->Peers())
                {
                    if (peer.slot >= 0 && peer.slot < view->SlotCount())
                    {
                        h.players[static_cast<size_t>(peer.slot)].pingMs = static_cast<int>(std::lround(peer.rttMs));
                    }
                }
            }
            status_.localSlot = slot;
            status_.ballCam = ballCam;
            status_.matchSeconds = static_cast<double>(w.tick) * static_cast<double>(rl::kTickDt);
        }

        // The events of the ring of the match that the HUD has not seen: a rollback can bring the same one again, so each is remembered by its tick
        void CollectEvents()
        {
            const rl::RlMatch* view = View();
            if (view == nullptr)
            {
                return;
            }
            const rl::RlMatchState& m = view->World().match;
            const int first = std::max(m.eventHead - rl::kEventRing, 0);
            for (int n = first; n < m.eventHead; ++n)
            {
                const rl::RlMatchEvent& e = m.events[n % rl::kEventRing];
                const std::uint64_t key = (static_cast<std::uint64_t>(e.tick) << 24) ^ (static_cast<std::uint64_t>(e.type & 15) << 16) ^
                                          (static_cast<std::uint64_t>(e.slot & 255) << 8) ^ static_cast<std::uint64_t>(e.other & 255);
                if (std::find(seenEvents.begin(), seenEvents.end(), key) != seenEvents.end())
                {
                    continue;
                }
                seenEvents.push_back(key);
                if (seenEvents.size() > 96)
                {
                    seenEvents.pop_front();
                }
                rlui::HudEvent out;
                out.player = e.slot;
                out.other = -1;
                switch (static_cast<rl::RlMatchEventType>(e.type))
                {
                case rl::RlMatchEventType::Goal: out.type = rlui::HudEventType::Goal; break;
                case rl::RlMatchEventType::OwnGoal: out.type = rlui::HudEventType::OwnGoal; break;
                case rl::RlMatchEventType::Assist: out.type = rlui::HudEventType::Assist; break;
                case rl::RlMatchEventType::Save: out.type = rlui::HudEventType::Save; break;
                case rl::RlMatchEventType::Shot: out.type = rlui::HudEventType::Shot; break;
                case rl::RlMatchEventType::Demo:
                    out.type = rlui::HudEventType::Demo;
                    out.other = e.other;
                    break;
                default: continue;
                }
                hudEvents.push_back(out);
            }
            seenEventHead = m.eventHead;
        }

        void FinishMatchIfOver(World& world, const float deltaTime)
        {
            const rl::RlMatch* view = View();
            if (view == nullptr || status_.stage == RlGameStage::Finished)
            {
                return;
            }
            const rl::RlMatchState& m = view->World().match;
            if (m.phase != rl::RlMatchPhase::Finished)
            {
                finishedAge = 0.0f;
                return;
            }
            finishedAge += deltaTime;
            if (finishedAge < 1.6f)
            {
                return;
            }
            status_.winner = m.winner;
            status_.overtimePlayed = m.overtime != 0;
            // the MVP: the best player of the winning team (points, then goals, then the lower slot)
            int mvp = -1;
            for (int slot = 0; slot < view->SlotCount(); ++slot)
            {
                if (view->Team(slot) != m.winner)
                {
                    continue;
                }
                if (mvp < 0 || m.points[slot] > m.points[mvp] || (m.points[slot] == m.points[mvp] && m.goals[slot] > m.goals[mvp]))
                {
                    mvp = slot;
                }
            }
            status_.mvp = mvp;
            status_.stage = RlGameStage::Finished;
            paused = false;
            EnterPodium(world, m.winner < 0 ? 0 : m.winner);
            if (feedback != nullptr)
            {
                feedback->EndMatch();
                feedback->PlayMenuMusic();
            }
        }

        // ---- the training (G4g, docs/game/rl-training.md): in the game mode free play and the packs of exercises take the place of the match

        // Begins free play (no pack) or a pack of exercises; a pack that is not there fails like a network that is not there
        void StartTraining(World& world, const RlGameMatchSetup& setup)
        {
            training = std::make_unique<rl::RlTraining>(tuning);
            game::AttachTrainingResults(*training);
            autopilot.reset();
            autopilotOn = false;
            if (!setup.trainingPack.empty())
            {
                const std::vector<rl::RlTrainingPack> packs = game::LoadTrainingPacks();
                const auto it = std::find_if(packs.begin(), packs.end(), [&](const rl::RlTrainingPack& pack) { return pack.id == setup.trainingPack; });
                if (it == packs.end() || !training->StartPack(*it, setup.trainingExercise))
                {
                    training.reset();
                    status_.error = "The training pack \"" + setup.trainingPack + "\" is not in assets/data/training.";
                    status_.stage = RlGameStage::Failed;
                    EnterPodium(world, 0);
                    return;
                }
            }
            mode = Mode::Match;
            accumulator = 0.0f;
            cameraState = rl::RlCameraState{};
            menuMode = 1;  // the blue car and the orange one (hidden while the exercise has no bot): BeginMatchVisuals shows both
            BeginMatchVisuals(world);
            if (feedback != nullptr)
            {
                feedback->StartMatch(0);
                feedback->SetOwnSlot(0);
            }
            localSlot = 0;
            status_.localSlot = 0;
            paused = false;
            status_.stage = RlGameStage::Playing;
        }

        // The keys of the training: R / N / P / T / C / B and the D-pad (the car takes the keys of the controls, the pad buttons of the
        // controls are the face buttons, the bumpers and the sticks' roll)
        void TrainingKeys()
        {
            const auto key = [&](const char c) { return trainingPressed.keys[static_cast<unsigned char>(c)]; };
            const auto pad = [&](const rl::RlPadButton button) { return (trainingPressed.padButtons & (1u << static_cast<unsigned>(button))) != 0; };
            if (training->Mode() == rl::RlTrainingMode::FreePlay)
            {
                if (key('R') || pad(rl::PadDpadUp))
                {
                    training->ResetBallToCar();
                }
                if (key('T') || pad(rl::PadDpadDown))
                {
                    training->BallInAir();
                }
                if (key('C') || pad(rl::PadDpadLeft))
                {
                    training->ResetCar();
                }
                if (key('B') || pad(rl::PadLeftStick))
                {
                    training->SetInfiniteBoost(!training->InfiniteBoost());
                }
                return;
            }
            if (key('R') || pad(rl::PadDpadUp))
            {
                training->Retry();
            }
            if (key('N') || pad(rl::PadDpadRight))
            {
                training->Next();
            }
            if (key('P') || pad(rl::PadDpadLeft))
            {
                training->Previous();
            }
        }

        // The commands of the tests in a training: the bot drives, the keys of the exercise
        void RunTrainingCommands()
        {
            std::vector<std::string> commands;
            commands.swap(debugCommands);
            for (const std::string& command : commands)
            {
                if (command == "autopilot")
                {
                    autopilot = std::make_unique<rl::RlTrainingAutopilot>(tuning, 2, 1);
                    autopilotOn = true;
                }
                else if (command == "autopilot-off")
                {
                    autopilotOn = false;
                }
                else if (command == "next")
                {
                    training->Next();
                }
                else if (command == "previous")
                {
                    training->Previous();
                }
                else if (command == "retry")
                {
                    training->Retry();
                }
                else if (command == "boost")
                {
                    training->MutableSim().SetCarBoost(0, 100.0f);
                }
            }
        }

        // One frame of the training: the keys, the ticks, the scene, the HUD
        void UpdateTraining(World& world, const float deltaTime, const rl::RlCarInput& player)
        {
            RunTrainingCommands();
            if (!paused)
            {
                TrainingKeys();
                accumulator += std::min(deltaTime, 0.1f);
                while (accumulator >= rl::kTickDt)
                {
                    accumulator -= rl::kTickDt;
                    rl::RlCarInput drive = latch.Take(player);
                    if (autopilotOn && autopilot != nullptr)
                    {
                        if (training->Phase() == rl::RlTrainingPhase::Ready)
                        {
                            autopilot->Reset();
                            drive = rl::RlCarInput{};
                        }
                        else
                        {
                            if (const rl::RlTrainingExercise* exercise = training->CurrentExercise())
                            {
                                autopilot->SetStyleFor(*exercise);
                            }
                            drive = autopilot->Think(training->World());
                        }
                    }
                    training->Step(drive);
                    if (feedback != nullptr)
                    {
                        feedback->OnTick(training->PreviousWorld(), training->World());
                    }
                }
            }
            ApplyToScene(world, paused ? 0.0001f : deltaTime);
            BuildTrainingStatus(deltaTime, lastPlayer.boost);
        }

        void BuildTrainingStatus(const float deltaTime, const bool boostHeld)
        {
            const rl::RlWorldState& w = training->World();
            const rl::RlCarState& car = w.cars[0];
            const rl::RlTrainingInfo info = training->Info();
            const rl::RlTrainingExercise* exercise = training->CurrentExercise();
            rlui::HudState& h = status_.hud;
            // a kick-off has the countdown of its match (3 2 1 GO); the other exercises wait with GET READY instead
            const bool countdown = exercise != nullptr && exercise->kickoff;
            h.phase = countdown ? static_cast<rlui::HudPhase>(static_cast<int>(w.match.phase)) : rlui::HudPhase::Playing;
            h.phaseTimer = countdown ? w.match.phaseTimer : 0.0f;
            h.clock = 0.0f;
            h.overtime = false;
            h.mode = 0;
            h.score[0] = h.score[1] = 0;
            h.boost = car.boost;
            if (car.boost < previousBoost - 1e-3f || (boostHeld && car.boost > 0.0f))
            {
                boostHold = 0.12f;
            }
            else
            {
                boostHold = std::max(0.0f, boostHold - deltaTime);
            }
            previousBoost = car.boost;
            h.boosting = boostHold > 0.0f && !car.demolished;
            h.demolished = car.demolished;
            h.respawnTimer = car.respawnTimer;
            h.ballCam = ballCam;
            h.ballCamKey = status_.ballCamKey.empty() ? "Space" : status_.ballCamKey;
            h.localPlayer = 0;
            h.players.assign(1, rlui::HudPlayer{});
            h.players[0].name = playerName;
            h.pingMs = -1;
            status_.localSlot = 0;
            status_.ballCam = ballCam;
            status_.matchSeconds = static_cast<double>(w.tick) * static_cast<double>(rl::kTickDt);

            rlui::HudTraining& t = h.training;
            t.active = true;
            t.phase = info.mode == rl::RlTrainingMode::FreePlay ? rlui::HudTrainingPhase::FreePlay : static_cast<rlui::HudTrainingPhase>(static_cast<int>(info.phase));
            t.pack = info.packName;
            t.exercise = info.exerciseName;
            t.hint = info.hint;
            t.index = info.exerciseIndex;
            t.count = info.exerciseCount;
            t.attempt = info.attempt;
            t.attempts = info.attempts;
            t.timeLeft = info.timeLeft;
            t.timeLimit = info.timeLimit;
            t.elapsed = info.elapsed;
            t.bannerTime = info.bannerTime;
            switch (info.missReason)
            {
            case rl::RlTrainingMiss::TimeUp: t.missReason = "TIME UP"; break;
            case rl::RlTrainingMiss::OwnGoal: t.missReason = "OWN GOAL"; break;
            case rl::RlTrainingMiss::LostKickoff: t.missReason = "LOST KICK-OFF"; break;
            case rl::RlTrainingMiss::None: t.missReason.clear(); break;
            }
            t.outOfAttempts = info.outOfAttempts;
            t.newBest = info.newBest;
            t.hits = info.hits;
            t.lastHitKmh = info.lastHitSpeed * 3.6f;
            t.bestHitKmh = info.bestHitSpeed * 3.6f;
            t.maxTouchHeight = info.maxTouchHeight;
            t.infiniteBoost = info.infiniteBoost;
            t.recordTime = info.best.bestTime;
            t.recordHitKmh = info.best.bestHitSpeed * 3.6f;
            t.done = info.packDoneSuccesses;
        }

        void UpdateGame(World& world, const float deltaTime, const rl::RlControlState& state, const rl::RlControlState& justPressed, const bool ballCamPressed,
                        const bool menuPressed)
        {
            ProcessCommands(world);
            auto& runtimeState = core::ServiceLocator::GetEditorRuntimeState();
            status_.padConnected = padConnected;
            status_.scoreboardHeld = scoreboard;
            status_.ballCamKey = KeyName(controls, rl::RlButtonAction::BallCam, padInUse);
            status_.pauseKey = KeyName(controls, rl::RlButtonAction::Menu, padInUse);

            // --rl-host / --rl-join with --game: the network starts by itself, as if the player had chosen it on the Play screen
            if (!autoNetDone && online.role != RlOnlineOptions::Role::None)
            {
                autoNetDone = true;
                RlGameMatchSetup setup;
                setup.kind = online.role == RlOnlineOptions::Role::Host ? RlGameMatchSetup::Kind::Host : RlGameMatchSetup::Kind::Join;
                setup.mode = std::clamp(online.playersPerTeam, 1, 4);
                setup.botLevel = botLevel;
                setup.port = online.port;
                setup.address = online.address;
                StopMatchState(world);
                StartGameMatch(world, setup);
            }

            const bool onField = status_.stage == RlGameStage::Playing || status_.stage == RlGameStage::Paused;
            if (previewWanted && status_.stage == RlGameStage::Menu && !previewActive)
            {
                previewActive = true;
            }
            else if ((!previewWanted || status_.stage != RlGameStage::Menu) && previewActive)
            {
                previewActive = false;
                if (status_.stage == RlGameStage::Menu)
                {
                    EndPreview(world);
                }
            }
            runtimeState.gameCapturesMouse = onField && !paused;
            runtimeState.gameHidesCursor = onField && !paused;

            rl::RlCarInput player = state.car;
            rl::RlCarInput tapped = justPressed.car;
            if (invertPitch)
            {
                player.pitch = -player.pitch;
                tapped.pitch = -tapped.pitch;
            }
            if (paused || !onField)
            {
                player = rl::RlCarInput{};
                tapped = rl::RlCarInput{};
                latch.Clear();
            }

            if (!onField)
            {
                // the menus: the podium (the network, if any, keeps going)
                if ((netHost != nullptr || netClient != nullptr) && status_.stage != RlGameStage::Finished)
                {
                    NetStep(world, rl::RlCarInput{});  // the match may start while a menu is shown: the next frame is the match
                }
                if (status_.stage == RlGameStage::Finished && View() != nullptr)
                {
                    BuildGameStatus(deltaTime, false);
                }
                if (previewActive)
                {
                    UpdatePreview(world, deltaTime);
                }
                else
                {
                    UpdatePodium(world, deltaTime);
                }
                return;
            }

            if (menuPressed && !paused)
            {
                pausePressed = true;
            }
            if (ballCamHold)
            {
                ballCam = state.Down(rl::RlButtonAction::BallCam);
            }
            else if (ballCamPressed)
            {
                ballCam = !ballCam;
            }
            lastPlayer = player;
            latch.Press(tapped);

            if (netHost != nullptr || netClient != nullptr)
            {
                player = latch.Take(player);
                if (!NetStep(world, player))
                {
                    if (status_.stage == RlGameStage::Failed || status_.stage == RlGameStage::Lobby || status_.stage == RlGameStage::Connecting)
                    {
                        UpdatePodium(world, deltaTime);
                    }
                    return;
                }
            }
            else if (training != nullptr)
            {
                UpdateTraining(world, deltaTime, player);
                return;
            }
            else if (match != nullptr)
            {
                if (!paused)
                {
                    accumulator += std::min(deltaTime, 0.1f);
                    std::vector<rl::RlCarInput> inputs(static_cast<size_t>(match->SlotCount()));
                    while (accumulator >= rl::kTickDt)
                    {
                        accumulator -= rl::kTickDt;
                        inputs[0] = latch.Take(player);
                        match->Step(inputs.data());
                        if (feedback != nullptr)
                        {
                            feedback->OnTick(match->PreviousWorld(), match->World());
                        }
                    }
                }
            }
            else
            {
                return;
            }

            RunDebugCommands();
            ApplyToScene(world, paused ? 0.0001f : deltaTime);
            BuildGameStatus(deltaTime, lastPlayer.boost);
            CollectEvents();
            FinishMatchIfOver(world, deltaTime);
        }

        // The commands of the tests: run in the next Update of a match
        std::vector<std::string> debugCommands;

        void RunDebugCommands()
        {
            std::vector<std::string> commands;
            commands.swap(debugCommands);
            const rl::RlMatch* view = View();
            if (view == nullptr || match == nullptr)
            {
                return;  // an online match is not touched
            }
            for (const std::string& command : commands)
            {
                rl::RlSim& sim = match->MutableSim();
                const float line = tuning.arena.halfLength;
                if (command == "goal-blue")
                {
                    sim.ResetBall({0.0f, 1.5f, line - 4.0f}, {0.0f, 0.0f, 40.0f});  // into the orange goal: blue scores (the last toucher decides the author)
                }
                else if (command == "goal-orange")
                {
                    sim.ResetBall({0.0f, 1.5f, -(line - 4.0f)}, {0.0f, 0.0f, -40.0f});
                }
                else if (command == "demolish")
                {
                    // the car of the player is run into by an enemy at full speed
                    const int enemy = match->SlotCount() > 1 ? match->SlotCount() - 1 : -1;
                    if (enemy >= 0)
                    {
                        const rl::RlCarState& car = match->World().cars[0];
                        sim.SetCarPose(enemy, {car.position.x + 6.0f, car.position.y, car.position.z}, {0.0f, -0.70710678f, 0.0f, 0.70710678f}, {-30.0f, 0.0f, 0.0f});
                    }
                }
                else if (command == "boost")
                {
                    sim.SetCarBoost(0, 100.0f);
                }
                else if (command.rfind("time", 0) == 0)
                {
                    // the clock is the state of the match: a snapshot with another clock is the way to move it
                    // (not available here: use MYENGINE_RL_MATCH_SECONDS for a short match)
                }
            }
        }

        // ---- the preview of the camera settings (Settings > Camera): a practice match in the arena that drives by itself, seen through the numbers of the menu

        void UpdatePreview(World& world, const float deltaTime)
        {
            if (match == nullptr)
            {
                rl::RlMatchConfig config;
                config.mode = 0;
                config.humanMask = 1u;
                config.seed = 77;
                config.names[0] = playerName;
                match = std::make_unique<rl::RlMatch>(tuning, config);
                std::vector<rl::RlCarInput> idle(static_cast<size_t>(match->SlotCount()));
                for (int t = 0; t < static_cast<int>(rl::kTickRate * 3.4f); ++t)
                {
                    match->Step(idle.data());  // past the countdown
                }
                match->MutableSim().ResetBall({0.0f, 1.2f, 14.0f});
                BeginMatchVisuals(world);
                podiumShown = false;
                cameraState = rl::RlCameraState{};
                accumulator = 0.0f;
                previewTime = 0.0f;
            }
            previewTime += deltaTime;
            // drives a slow figure of eight round the ball
            rl::RlCarInput drive;
            drive.throttle = 0.55f;
            drive.steer = std::sin(previewTime * 0.9f) * 0.75f;
            accumulator += std::min(deltaTime, 0.1f);
            while (accumulator >= rl::kTickDt)
            {
                accumulator -= rl::kTickDt;
                std::vector<rl::RlCarInput> inputs(static_cast<size_t>(match->SlotCount()));
                inputs[0] = drive;
                match->Step(inputs.data());
            }
            localSlot = 0;
            ApplyToScene(world, deltaTime);
        }

        void EndPreview(World& world)
        {
            match.reset();
            mode = Mode::Menu;
            accumulator = 0.0f;
            cameraState = rl::RlCameraState{};
            EnterPodium(world, podiumTeam);
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
        return impl_->gameMode ? std::vector<std::string>{} : impl_->hud;
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

    // ---- the game mode ----------------------------------------------------------------------------------------------------------------

    void RlGameSystem::SetGameMode(const bool enabled)
    {
        impl_->gameMode = enabled;
    }

    bool RlGameSystem::GameMode() const
    {
        return impl_->gameMode;
    }

    const RlGameStatus& RlGameSystem::Status() const
    {
        return impl_->status_;
    }

    void RlGameSystem::StartMatch(const RlGameMatchSetup& setup)
    {
        Impl::Pending command;
        command.command = Impl::Command::Start;
        command.setup = setup;
        impl_->pending.push_back(command);
    }

    void RlGameSystem::StartHostedMatch()
    {
        Impl::Pending command;
        command.command = Impl::Command::StartHosted;
        impl_->pending.push_back(command);
    }

    void RlGameSystem::LeaveMatch()
    {
        Impl::Pending command;
        command.command = Impl::Command::Leave;
        impl_->pending.push_back(command);
    }

    void RlGameSystem::RestartMatch()
    {
        Impl::Pending command;
        command.command = Impl::Command::Restart;
        impl_->pending.push_back(command);
    }

    void RlGameSystem::SetPaused(const bool paused)
    {
        Impl& d = *impl_;
        if (d.status_.stage != RlGameStage::Playing && d.status_.stage != RlGameStage::Paused)
        {
            return;
        }
        // an online match goes on behind the pause screen; the car just gets no input
        d.paused = paused;
        d.status_.stage = paused ? RlGameStage::Paused : RlGameStage::Playing;
        d.latch.Clear();
    }

    std::vector<rlui::HudEvent> RlGameSystem::TakeHudEvents()
    {
        std::vector<rlui::HudEvent> events;
        events.swap(impl_->hudEvents);
        return events;
    }

    bool RlGameSystem::TakePausePressed()
    {
        const bool pressed = impl_->pausePressed;
        impl_->pausePressed = false;
        return pressed;
    }

    rl::RlControls& RlGameSystem::Controls()
    {
        return impl_->controls;
    }

    std::string RlGameSystem::SaveControls()
    {
        std::string error;
        const std::filesystem::path path = Impl::UserControlsPath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        return impl_->controls.SaveToFile(path.generic_u8string(), &error) ? std::string() : error;
    }

    std::string RlGameSystem::ResetControls()
    {
        std::error_code ec;
        std::filesystem::remove(Impl::UserControlsPath(), ec);
        impl_->LoadControls();
        return impl_->status.empty() ? std::string() : impl_->status;
    }

    void RlGameSystem::SetCameraSettings(const rl::RlCameraTuning& camera, const bool ballCamHold)
    {
        impl_->cameraSettings = camera;
        impl_->haveCameraSettings = true;
        impl_->ballCamHold = ballCamHold;
    }

    const rl::RlCameraTuning& RlGameSystem::CameraSettings() const
    {
        return impl_->haveCameraSettings ? impl_->cameraSettings : impl_->tuning.camera;
    }

    void RlGameSystem::SetInvertPitch(const bool invert)
    {
        impl_->invertPitch = invert;
    }

    void RlGameSystem::SetVolumes(const float master, const float effects, const float music)
    {
        if (impl_->feedback != nullptr)
        {
            impl_->feedback->SetVolumes(master, effects, music);
        }
    }

    void RlGameSystem::PlayUiSound(const char* eventName)
    {
        if (eventName != nullptr)
        {
            audio::GetAudioSystem().PlayEvent(eventName);
        }
    }

    void RlGameSystem::SetPlayerName(const std::string& name)
    {
        impl_->playerName = name.empty() ? "Player" : name;
    }

    void RlGameSystem::DebugCommand(const std::string& command)
    {
        impl_->debugCommands.push_back(command);
    }

    void RlGameSystem::SetViewAspect(const float aspect)
    {
        impl_->viewAspect = std::clamp(aspect, 1.0f, 4.0f);
    }

    void RlGameSystem::SetCameraPreview(const bool enabled)
    {
        impl_->previewWanted = enabled;
    }
}
