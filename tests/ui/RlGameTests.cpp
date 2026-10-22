// The game mode of Rocket League (G1-flow) without a window or a GPU: the settings file (defaults, round trip, clamping, a broken file, names cut at a
// character), the script of the UI tests (parsing, the order and the time of the commands, taps), and RlGameSystem driven through its public commands
// the way the screens drive it: a match from the start to the results (the HUD data, the pause that stands still, the events of the feed, the winner and
// the MVP), the lobby of a private match, the controls that the settings screen edits and saves.

#include <stb_image.h>
#include <myengine/audio/AudioSystem.h>
#include <myengine/core/ProjectContext.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/systems/RlGameSystem.h>
#include <myengine/editor/EditorState.h>
#include <myengine/game/RlCars.h>
#include <myengine/game/RlGameSettings.h>
#include <myengine/game/RlVideoWindowChange.h>
#include <myengine/input/InputManager.h>

#include <rl/RlControls.h>
#include <rl/RlName.h>
#include <rl/net/LobbySteam.h>
#include <rl/net/NetReliable.h>
#include <rl/RlTuning.h>

#include "../../engine/src/core/UiScript.h"

#include <DirectXMath.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

namespace
{
    using namespace myengine;
    namespace fs = std::filesystem;
    namespace sys = ecs::systems;
    namespace components = ecs::components;

    int g_checks = 0;

    void Check(const bool condition, const std::string& message)
    {
        ++g_checks;
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    fs::path TempFolder(const char* name)
    {
        const fs::path folder = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder, ec);
        return folder;
    }

    // ---------------------------------------------------------------- the settings

    void TestSettings()
    {
        std::cout << "settings: defaults, round trip, clamping, a broken file, a name cut at a character\n";
        const fs::path folder = TempFolder("myengine_game_settings_test");
        const fs::path file = folder / "game_settings.json";

        game::RlGameSettings defaults;
        Check(defaults.video.windowMode == 0 && defaults.video.vsync && defaults.video.shadows == 2 && defaults.video.msaa == 4, "the defaults of the video");
        Check(defaults.camera.fov == 110.0f && defaults.camera.distanceCm == 270.0f, "the defaults of the camera are those of Rocket League");
        Check(defaults.play.mode == 2 && defaults.play.botLevel == 1 && defaults.play.hostPort == 27015, "the defaults of the Play screen");

        game::RlGameSettings missing;
        std::string error;
        Check(missing.Load(folder / "nothing.json", &error), "a missing file is the first start, not an error");

        game::RlGameSettings changed;
        changed.video.windowMode = 1;
        changed.video.width = 2560;
        changed.video.height = 1440;
        changed.video.vsync = false;
        changed.video.fpsLimit = 144;
        changed.video.shadows = 1;
        changed.video.msaa = 1;
        changed.video.uiScalePercent = 110;
        changed.camera.fov = 95.0f;
        changed.camera.distanceCm = 300.0f;
        changed.camera.ballCamHold = true;
        changed.camera.wallTilt = 0.4f;
        changed.audio.music = 0.25f;
        changed.invertPitch = true;
        changed.car = "bulwark";
        changed.play.mode = 3;
        changed.play.joinAddress = "192.168.1.42:27015";
        changed.play.recent = {"10.0.0.2:27015", "192.168.1.42:27015"};
        changed.play.playerName = "\xD0\x90\xD1\x80\xD1\x82\xD1\x91\xD0\xBC";  // a Cyrillic name
        Check(changed.Save(file, &error), "the file is written: " + error);
        game::RlGameSettings back;
        Check(back.Load(file, &error), "the file is read: " + error);
        Check(back.video.windowMode == 1 && back.video.width == 2560 && back.video.height == 1440 && !back.video.vsync && back.video.fpsLimit == 144 &&
                  back.video.shadows == 1 && back.video.msaa == 1 && back.video.uiScalePercent == 110,
              "the video settings come back");
        Check(back.camera.wallTilt == 0.4f, "Wall tilt comes back");
        Check(back.camera.fov == 95.0f && back.camera.distanceCm == 300.0f && back.camera.ballCamHold && back.audio.music == 0.25f && back.invertPitch,
              "the camera, the audio and the pitch come back");
        Check(back.car == "bulwark", "the car of the garage comes back");
        Check(game::RlGameSettings{}.car == "pulse", "the first start has Pulse");
        Check(back.play.mode == 3 && back.play.joinAddress == "192.168.1.42:27015" && back.play.recent.size() == 2 && back.play.playerName == changed.play.playerName,
              "the Play screen comes back, with the Cyrillic name");
        Check(!fs::exists(file.string() + ".tmp"), "no half-written file is left");

        // values out of their ranges are put back into them, unknown keys and keys of the wrong kind are ignored
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << R"({"video": {"width": 99999, "height": 1, "shadows": 7, "uiScalePercent": 500, "vsync": "yes", "future": 1},
                       "camera": {"fov": 500, "distanceCm": 1, "angle": 30},
                       "audio": {"master": 4.0, "music": -1.0},
                       "play": {"mode": 9, "botLevel": -3, "hostPort": 70000, "recent": ["a","b","c","d","e","f","g"]},
                       "somethingNew": true})";
        }
        game::RlGameSettings clamped;
        Check(clamped.Load(file, &error), "a file with odd values is read: " + error);
        Check(clamped.video.width == 7680 && clamped.video.height == 360 && clamped.video.shadows == 2 && clamped.video.uiScalePercent == 125, "the video is clamped");
        Check(clamped.video.vsync, "a value of the wrong kind keeps the default");
        Check(clamped.camera.fov == 110.0f && clamped.camera.distanceCm == 100.0f && clamped.camera.angle == 0.0f, "the camera is clamped");
        Check(clamped.audio.master == 1.0f && clamped.audio.music == 0.0f, "the audio is clamped");
        Check(clamped.play.mode == 4 && clamped.play.botLevel == 0 && clamped.play.hostPort == 65535 && clamped.play.recent.size() == 5, "the Play screen is clamped");

        // a file that is not json leaves the settings as they were and says so
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "this is { not json";
        }
        game::RlGameSettings broken = changed;
        error.clear();
        Check(!broken.Load(file, &error) && !error.empty(), "a broken file is reported");
        Check(broken.video.width == 2560 && broken.play.mode == 3, "and the settings stay as they were");

        // a name that does not fit is cut between characters, not in the middle of one
        game::RlGameSettings longName;
        longName.play.playerName = "\xD0\x90\xD1\x80\xD1\x82\xD1\x91\xD0\xBC\xD0\x90\xD1\x80\xD1\x82\xD1\x91\xD0\xBC";  // 16 bytes of 8 two-byte letters
        longName.Clamp();
        Check(longName.play.playerName.size() == 20, "a name of 10 letters (20 bytes) fits the slot");
        for (int i = 0; i < 8; ++i)
        {
            longName.play.playerName += "\xD0\xB6";  // a Cyrillic letter, two bytes
        }
        longName.Clamp();
        Check(longName.play.playerName.size() <= rl::kPlayerNameBytes && longName.play.playerName.size() % 2 == 0, "a long name is cut at a character");
        game::RlGameSettings shortName;
        shortName.play.playerName = "  x ";
        shortName.Clamp();
        Check(shortName.play.playerName.empty(), "a nickname of one letter is not kept: the Windows user stays");
        game::RlGameSettings spaced;
        spaced.play.playerName = "  Ann  ";
        spaced.Clamp();
        Check(spaced.play.playerName == "Ann", "the spaces at the ends are cut");
        game::RlGameSettings odd;
        odd.play.playerName = "Ann\tB\xF0\x9F\x98\x80";
        odd.Clamp();
        Check(odd.play.playerName == "AnnB", "control characters and characters without a glyph are dropped");
        Check(!defaults.PlayerName().empty() && defaults.PlayerName().size() <= rl::kPlayerNameBytes, "the name of the player is never empty and fits the slot");

        // the nickname (G1n): the Steam profile when there is one, else the name of the file, else the Windows user; MYENGINE_PLAYER_NAME above all
        {
            _wputenv_s(L"MYENGINE_PLAYER_NAME", L"");
            std::string steamName;
            const bool steam = rl::net::SteamPersonaName(steamName);
            game::RlGameSettings named;
            named.play.playerName = "FromFile";
            if (steam)
            {
                Check(named.PlayerName() == steamName, "with Steam the nickname is the name of the profile");
            }
            else
            {
                Check(named.PlayerName() == "FromFile", "without Steam the name of the file");
                if (defaults.play.playerName.empty())
                {
                    Check(!defaults.PlayerName().empty() && defaults.PlayerName() == rl::SanitizePlayerName(defaults.PlayerName()), "the Windows user is cleaned like any name");
                }
            }
            _wputenv_s(L"MYENGINE_PLAYER_NAME", L"\u043f\u043a");
            Check(named.PlayerName() == "\xD0\xBF\xD0\xBA", "MYENGINE_PLAYER_NAME is the nickname of a test or of a second copy");
            _wputenv_s(L"MYENGINE_PLAYER_NAME", L"");
        }

        // the camera of the menu and the numbers of the camera of the game
        rl::RlCameraTuning tuning;
        tuning.wallMargin = 0.77f;
        const game::RlCameraSettings fromTuning = game::RlCameraSettings::FromTuning(tuning);
        Check(fromTuning.distanceCm == 270.0f && fromTuning.heightCm == 100.0f, "the centimetres of the menu come from the metres of the tuning");
        game::RlCameraSettings custom = fromTuning;
        custom.distanceCm = 350.0f;
        custom.fov = 90.0f;
        const rl::RlCameraTuning applied = custom.ToTuning(tuning);
        Check(fromTuning.wallTilt == 0.0f && defaults.camera.wallTilt == 0.0f, "the horizon is level by default");
        custom.wallTilt = 0.5f;
        Check(custom.ToTuning(tuning).rollFollow == 0.5f, "Wall tilt is the rollFollow of the camera");
        Check(applied.distance == 3.5f && applied.fov == 90.0f && applied.wallMargin == 0.77f, "the settings replace what the menu has a row for and keep the rest");
    }

    // ---------------------------------------------------------------- the script of the UI tests

    void TestScript()
    {
        std::cout << "UI script: parsing, the order of the commands, taps\n";
        core::UiScript script;
        std::string error;
        Check(!script.Load("abc key Enter", error) && !error.empty(), "a line without a time is refused");
        error.clear();
        const std::string text =
            "# a comment\n"
            "1.0 key Down\n"
            "0.5 key Enter down\n"
            "2.0 key Enter up\n"
            "1.5 click left\n"
            "1.5 mouse 100 200\n"
            "2.5 pad A tap\n"
            "3.0 shot some/where.tga\n"
            "3.1 game goal-blue\n"
            "3.2 note a line of text\n"
            "9.0 quit\n";
        Check(script.Load(text, error), "the script loads: " + error);

        std::vector<std::string> log;
        core::UiScript::Sink sink;
        sink.key = [&](const int code, const bool down) { log.push_back(std::string(down ? "down " : "up ") + std::to_string(code)); };
        sink.mouseButton = [&](const int button, const bool down) { log.push_back(std::string(down ? "mouse down " : "mouse up ") + std::to_string(button)); };
        sink.screenshot = [&](const std::string& path) { log.push_back("shot " + path); };
        sink.game = [&](const std::string& command) { log.push_back("game " + command); };
        sink.note = [&](const std::string& note) { log.push_back("note " + note); };
        sink.quit = [&]() { log.push_back("quit"); };

        script.Advance(0.4, sink);
        Check(log.empty(), "nothing is due before the first command");
        script.Advance(0.2, sink);  // 0.6: Enter down (the commands are sorted by their time)
        Check(log.size() == 1 && log[0] == "down 13", "the earliest command runs first, whatever its place in the file");
        rlui::RawInput held;
        script.ApplyTo(held);
        Check(held.keys[13] && !held.keys[0x28], "Enter is held, Down is not yet");
        script.Advance(0.5, sink);  // 1.1: Down (a tap)
        script.ApplyTo(held);
        Check(held.keys[0x28], "the tapped key is down in the frame of the command");
        script.Advance(0.2, sink);  // 1.3: the tap ends
        rlui::RawInput after;
        script.ApplyTo(after);
        Check(!after.keys[0x28] && after.keys[13], "the tap is released later, Enter is still held");
        script.Advance(0.3, sink);  // 1.6: click and mouse position
        rlui::RawInput mouse;
        script.ApplyTo(mouse);
        Check(mouse.mouse[0] && mouse.mousePosition.x == 100.0f && mouse.mousePosition.y == 200.0f, "the click and the pointer are in the frame");
        script.Advance(0.5, sink);  // 2.1: Enter up, the click is released
        rlui::RawInput released;
        script.ApplyTo(released);
        Check(!released.keys[13] && !released.mouse[0], "Enter and the click are released");
        script.Advance(1.2, sink);  // 3.3: pad, shot, game command, note
        Check(std::find(log.begin(), log.end(), "shot some/where.tga") != log.end(), "the screenshot request reaches the sink with its path");
        Check(std::find(log.begin(), log.end(), "game goal-blue") != log.end() && std::find(log.begin(), log.end(), "note a line of text") != log.end(), "game commands and notes too");
        Check(!script.Finished(), "the script is not over before the last command");
        script.Advance(10.0, sink);
        Check(log.back() == "quit" || std::find(log.begin(), log.end(), "quit") != log.end(), "the last command quits");
        Check(script.Finished(), "and the script is over");
    }

    // ---------------------------------------------------------------- RlGameSystem as the screens use it

    struct Rig
    {
        input::InputManager input;
        ecs::World world;
        sys::RlGameSystem system{input};

        Rig()
        {
            auto& tags = world;
            const auto add = [&](const char* name, const bool camera = false)
            {
                const ecs::EntityId entity = tags.CreateEntity();
                tags.Emplace<components::TagComponent>(entity).name = name;
                tags.Emplace<components::TransformComponent>(entity);
                if (camera)
                {
                    tags.Emplace<components::CameraComponent>(entity);
                }
                else
                {
                    tags.Emplace<components::MeshRendererComponent>(entity);
                }
            };
            add("RlCamera", true);
            for (const char* name : {"RlArena", "RlBall", "RlBlue0", "RlOrange0", "RlBlue0Lights", "RlOrange0Lights", "RlPodium"})
            {
                add(name);
            }
            for (const char* team : {"RlBlue0", "RlOrange0"})
            {
                for (int w = 0; w < 4; ++w)
                {
                    add((std::string(team) + "Wheel" + std::to_string(w)).c_str());
                }
            }
            core::ServiceLocator::GetEditorRuntimeState().mode = editor::RuntimeMode::Play;
        }

        void Frames(const int count, const float dt = 1.0f / 30.0f)
        {
            for (int i = 0; i < count; ++i)
            {
                system.Update(world, dt);
            }
        }

        bool Until(const std::function<bool()>& done, const int limit, const float dt = 1.0f / 30.0f)
        {
            for (int i = 0; i < limit; ++i)
            {
                system.Update(world, dt);
                if (done())
                {
                    return true;
                }
            }
            return false;
        }
    };

    // RL-B2: the wheels of the car of the bot (nobody holds its stick: the picture works from the state of the car) turn about their axle as the car rolls
    // forward, and the front ones turn with the steering. The rotation of a wheel entity is that of the body under the turn of the wheel, so the turn of
    // the wheel is the wheel's matrix times the inverse of the body's.
    void TestWheelsOfTheBot(Rig& rig)
    {
        namespace dx = DirectX;
        constexpr float kDegToRad = 0.01745329f;
        const auto entityOf = [&](const std::string& tag)
        {
            ecs::EntityId found = ecs::kInvalidEntity;
            rig.world.ForEach<components::TagComponent>([&](const ecs::EntityId entity, const components::TagComponent& t) { found = t.name == tag ? entity : found; });
            return found;
        };
        const auto rotationOf = [&](const ecs::EntityId entity)
        {
            const auto* transform = rig.world.TryGet<components::TransformComponent>(entity);
            return dx::XMMatrixRotationRollPitchYaw(transform->rotationDeg.x * kDegToRad, transform->rotationDeg.y * kDegToRad, transform->rotationDeg.z * kDegToRad);
        };
        const ecs::EntityId body = entityOf("RlOrange0");
        ecs::EntityId wheel[4];
        for (int w = 0; w < 4; ++w)
        {
            wheel[w] = entityOf("RlOrange0Wheel" + std::to_string(w));
            Check(wheel[w] != ecs::kInvalidEntity, "the rig has the wheels of the car of the bot");
        }
        // the turn of a wheel relative to the body: about X by `spin`, about the up by `steer` (Spin * Steer * Body = Wheel)
        const auto relative = [&](const int w, float& spin, float& steer)
        {
            dx::XMFLOAT4X4 m;
            dx::XMStoreFloat4x4(&m, dx::XMMatrixMultiply(rotationOf(wheel[w]), dx::XMMatrixInverse(nullptr, rotationOf(body))));
            steer = std::atan2(-m._13, m._11);  // where the wheel's axle (X) points: the spin leaves it alone, the steering swings it from X towards -Z
            spin = std::atan2(m._23, m._22);    // the top (Y) of the wheel tilts to the front (+Z) for a positive spin
        };
        const auto positionOf = [&](const ecs::EntityId entity) { return rig.world.TryGet<components::TransformComponent>(entity)->position; };

        float spinSum[4] = {};
        float last[4] = {};
        float steerFront = 0.0f;
        float steerRear = 0.0f;
        int rolling = 0;
        int forward = 0;
        components::Vec3 lastPosition = positionOf(body);
        for (int i = 0; i < 360; ++i)
        {
            rig.Frames(1, 1.0f / 120.0f);
            const components::Vec3 position = positionOf(body);
            const float dx2 = position.x - lastPosition.x;
            const float dz2 = position.z - lastPosition.z;
            const float speed = std::sqrt(dx2 * dx2 + dz2 * dz2) * 120.0f;
            lastPosition = position;
            for (int w = 0; w < 4; ++w)
            {
                float spin = 0.0f;
                float steer = 0.0f;
                relative(w, spin, steer);
                float step = spin - last[w];
                last[w] = spin;
                if (i == 0)
                {
                    continue;
                }
                while (step > 3.14159265f)
                {
                    step -= 6.2831853f;
                }
                while (step < -3.14159265f)
                {
                    step += 6.2831853f;
                }
                spinSum[w] += step;
                if (w == 2 && speed > 3.0f)
                {
                    ++rolling;
                    forward += step > 0.0f ? 1 : 0;
                }
                if (w < 2)
                {
                    steerFront = std::max(steerFront, std::fabs(steer));
                }
                else
                {
                    steerRear = std::max(steerRear, std::fabs(steer));
                }
            }
        }
        std::cout << "  wheels of the bot: spin " << spinSum[0] << " " << spinSum[1] << " " << spinSum[2] << " " << spinSum[3] << " rad, rolling frames " << rolling
                  << ", forward " << forward << ", front steer up to " << steerFront << ", rear " << steerRear << "\n";
        Check(rolling > 60, "the car of the bot drove (the wheels have something to turn with)");
        for (int w = 0; w < 4; ++w)
        {
            Check(spinSum[w] > 10.0f, "the wheels of a car that drives forward turn forward (positive spin, many radians)");
        }
        Check(forward * 100 >= rolling * 90, "the wheel turns forward in nearly every frame of rolling");
        Check(steerRear < 0.001f, "the rear wheels do not steer");
    }

    void TestSystem(const fs::path& source)
    {
        std::cout << "RlGameSystem: a match from the start to the results, the pause, the events, the lobby, the controls\n";
        const fs::path appdata = TempFolder("myengine_game_system_test");
        _putenv_s("APPDATA", appdata.string().c_str());                    // the user files of the test go here, not to the folder of the player
        _putenv_s("LOCALAPPDATA", appdata.string().c_str());
        _putenv_s("MYENGINE_RL_MATCH_SECONDS", "6");                        // a match of six seconds
        core::ServiceLocator::GetProjectContext().InitializeDefault();
        audio::AudioConfig config;
        config.useDevice = false;
        audio::GetAudioSystem().Initialize(config);
        audio::GetAudioSystem().LoadBank(source / "assets/data/audio_bank.json");

        Rig rig;
        rig.system.SetGameMode(true);
        rig.system.SetOnlineOptions({});
        rig.Frames(3);
        Check(rig.system.Status().stage == sys::RlGameStage::Menu, "the game starts in the menu stage");
        Check(rig.system.HudLines().empty(), "the game mode has no text overlay");

        // ---- a 1v1 match against a bot
        sys::RlGameMatchSetup setup;
        setup.mode = 1;
        setup.botLevel = 1;
        setup.playerName = "Tester";
        rig.system.StartMatch(setup);
        rig.Frames(2);
        const sys::RlGameStatus& status = rig.system.Status();
        Check(status.stage == sys::RlGameStage::Playing && !status.online, "the match is on");
        Check(status.hud.players.size() == 2 && status.hud.players[0].name == "Tester" && !status.hud.players[0].bot, "the table has the player with his name");
        Check(status.hud.players[1].bot && status.hud.players[1].name == "Comet" && status.hud.players[1].team == 1, "and a bot of the other team");
        Check(status.hud.phase == rlui::HudPhase::Countdown && status.hud.clock > 5.9f, "it starts with the countdown and the whole clock");

        Check(rig.Until([&] { return status.hud.phase == rlui::HudPhase::Playing; }, 400), "the countdown ends");
        TestWheelsOfTheBot(rig);
        // the pause stands still
        rig.system.SetPaused(true);
        rig.Frames(1);
        const float clockAtPause = status.hud.clock;
        Check(status.stage == sys::RlGameStage::Paused, "the stage is Paused");
        rig.Frames(60);
        Check(status.hud.clock == clockAtPause, "an offline match does not run in the pause");
        rig.system.SetPaused(false);
        rig.Frames(30);
        Check(status.stage == sys::RlGameStage::Playing && status.hud.clock < clockAtPause, "and goes on after it");

        // a goal: the score, the banner data, the event of the feed
        rig.system.TakeHudEvents();
        rig.system.DebugCommand("goal-blue");
        bool sawReplay = false;
        bool matchWaitedInReplay = false;
        float clockInReplay = -1.0f;
        const auto watch = [&]
        {
            if (status.hud.replay.active)
            {
                sawReplay = true;
                if (clockInReplay < 0.0f)
                {
                    clockInReplay = status.hud.clock;
                }
                matchWaitedInReplay = status.hud.clock == clockInReplay;
            }
        };
        Check(rig.Until([&] { watch(); return status.hud.score[0] == 1; }, 200), "a goal for blue is counted");
        Check(rig.Until([&] { watch(); return status.hud.phase == rlui::HudPhase::GoalScored; }, 60) || status.hud.phase == rlui::HudPhase::GoalScored, "the phase is GoalScored");
        Check(status.hud.lastGoalTeam == 0 && status.hud.lastGoalSpeedKmh > 30.0f, "the last goal has its team and its speed in km/h");
        const std::vector<rlui::HudEvent> events = rig.system.TakeHudEvents();
        Check(std::any_of(events.begin(), events.end(), [](const rlui::HudEvent& e) { return e.type == rlui::HudEventType::Goal; }), "the goal reaches the feed");
        Check(rig.system.TakeHudEvents().empty(), "an event comes once");

        // the end of the match: the winner, the MVP, the stage
        Check(rig.Until([&] { watch(); return status.stage == sys::RlGameStage::Finished; }, 1200), "the match ends and the stage turns Finished");
        Check(sawReplay, "the replay of the goal played (HudState::replay)");
        Check(matchWaitedInReplay, "the clock of the offline match stood still during the replay");
        Check(status.winner == 0, "blue leads: blue wins");
        Check(status.mvp == 0, "the MVP is the best player of the winning team");
        Check(status.hud.phase == rlui::HudPhase::Finished, "the table says Finished");
        rig.system.LeaveMatch();
        rig.Frames(2);
        Check(status.stage == sys::RlGameStage::Menu && status.hud.players.empty(), "leaving brings the menu back and clears the table");

        // the match can be played again
        rig.system.StartMatch(setup);
        rig.Frames(2);
        Check(status.stage == sys::RlGameStage::Playing && status.hud.score[0] == 0 && status.hud.clock > 5.9f, "a new match starts from the start");
        rig.system.RestartMatch();
        rig.Frames(2);
        Check(status.stage == sys::RlGameStage::Playing && status.hud.clock > 5.9f, "restart plays the same setup again");
        rig.system.LeaveMatch();
        rig.Frames(2);

        // ---- the training: free play, a pack of exercises, the bot as the player, the pause, the results file, a pack that is not there
        {
            sys::RlGameMatchSetup training;
            training.kind = sys::RlGameMatchSetup::Kind::Training;
            training.mode = 0;
            rig.system.StartMatch(training);
            rig.Frames(3);
            Check(status.stage == sys::RlGameStage::Playing && !status.online, "free play is on: a stage Playing, nothing online");
            Check(status.hud.training.active && status.hud.training.phase == rlui::HudTrainingPhase::FreePlay && status.hud.players.size() == 1, "the HUD is the training one, with the player only");
            rig.Frames(30);
            Check(status.hud.training.elapsed > 0.5f, "the time of the session runs");
            rig.system.SetPaused(true);
            rig.Frames(1);
            const float pausedAt = status.hud.training.elapsed;
            rig.Frames(30);
            Check(status.stage == sys::RlGameStage::Paused && status.hud.training.elapsed == pausedAt, "the pause stops the training");
            rig.system.SetPaused(false);
            rig.system.LeaveMatch();
            rig.Frames(2);
            Check(status.stage == sys::RlGameStage::Menu && !status.hud.training.active, "leaving the training brings the menu back and clears the HUD");

            // a pack: striker, the exercise is shown, the bot drives it to a success, the results are written, the next exercise comes by itself
            const fs::path results = appdata / "myengine" / "training.json";
            const fs::path resultsShipping = appdata / "RocketArena" / "training.json";
            fs::remove(results);
            fs::remove(resultsShipping);
            training.trainingPack = "striker";
            rig.system.StartMatch(training);
            rig.Frames(3);
            Check(status.hud.training.active && status.hud.training.pack == "Striker" && status.hud.training.count == 8 && status.hud.training.index == 0, "the pack starts with its first exercise");
            Check(status.hud.training.phase == rlui::HudTrainingPhase::Ready || status.hud.training.phase == rlui::HudTrainingPhase::Running, "Ready first, then Running");
            Check(!status.hud.training.exercise.empty() && !status.hud.training.hint.empty() && status.hud.training.attempts == 3, "the name, the hint and the attempts of the exercise");
            rig.system.DebugCommand("autopilot");
            Check(rig.Until([&] { return status.hud.training.phase == rlui::HudTrainingPhase::Success; }, 600), "the bot scores the first exercise");
            Check(status.hud.training.elapsed > 1.0f && status.hud.training.lastHitKmh > 10.0f && status.hud.training.newBest, "the time, the speed of the hit and the record are in the HUD data");
            Check(fs::exists(results) || fs::exists(resultsShipping), "the results are written into the folder of the player after the attempt");
            Check(rig.Until([&] { return status.hud.training.index == 1; }, 200), "a success moves on to the next exercise by itself");
            rig.system.DebugCommand("next");
            rig.Frames(2);
            Check(status.hud.training.index == 2, "the key of the next exercise");
            rig.system.DebugCommand("previous");
            rig.Frames(2);
            Check(status.hud.training.index == 1, "the key of the previous exercise");
            rig.system.RestartMatch();
            rig.Frames(3);
            Check(status.hud.training.index == 0 && status.stage == sys::RlGameStage::Playing, "restart begins the pack again");
            rig.system.LeaveMatch();
            rig.Frames(2);

            // a kick-off: two cars and the countdown of the match in the HUD
            training.trainingPack = "kickoff";
            rig.system.StartMatch(training);
            rig.Frames(3);
            Check(status.hud.training.pack == "Kick-off" && status.hud.phase == rlui::HudPhase::Countdown, "a kick-off exercise has the countdown of the match");
            rig.system.LeaveMatch();
            rig.Frames(2);

            // a pack that is not there fails with the reason
            training.trainingPack = "nonsense";
            rig.system.StartMatch(training);
            rig.Frames(3);
            Check(status.stage == sys::RlGameStage::Failed && status.error.find("nonsense") != std::string::npos, "a pack that is not there ends in Failed with a reason that names it");
            rig.system.LeaveMatch();
            rig.Frames(2);
            Check(status.stage == sys::RlGameStage::Menu, "and the menu comes back");
        }

        // ---- the garage: the car of the player on the podium, in focus in the garage (a change is at once, the paint of the team), and in the match
        {
            const game::RlCarCatalog& cars = game::Cars();
            Check(cars.Cars().size() == 6, "the game reads the six cars");
            const auto entityOf = [&](const char* tag)
            {
                ecs::EntityId found = ecs::kInvalidEntity;
                rig.world.ForEach<components::TagComponent>([&](const ecs::EntityId entity, const components::TagComponent& t) { found = t.name == tag ? entity : found; });
                return found;
            };
            const auto meshOf = [&](const char* tag)
            {
                const auto* renderer = rig.world.TryGet<components::MeshRendererComponent>(entityOf(tag));
                return renderer != nullptr ? renderer->meshPath : std::string();
            };
            const auto paintOf = [&](const char* tag)
            {
                const auto* renderer = rig.world.TryGet<components::MeshRendererComponent>(entityOf(tag));
                return renderer != nullptr ? renderer->materialPath : std::string();
            };
            rig.system.SetSelectedCar("vector");
            Check(rig.system.SelectedCar() == "vector", "the car of the player is set");
            rig.system.SetSelectedCar("no-such-car");
            Check(rig.system.SelectedCar() == "pulse", "a key that is not in the data is Pulse");
            rig.system.SetSelectedCar("vector");
            rig.Frames(4);
            Check(meshOf("RlBlue0") == cars.Find("vector")->bodyMesh, "the podium of the menu shows the car of the player");

            // the garage: the car in focus replaces the body at once, in the paint of the preview team
            sys::RlGarageView view;
            view.active = true;
            view.car = "bulwark";
            view.team = 1;
            rig.system.SetGarageView(view);
            rig.Frames(4);
            Check(meshOf("RlOrange0") == cars.Find("bulwark")->bodyMesh && paintOf("RlOrange0") == cars.Find("bulwark")->material[1], "the garage shows Bulwark in orange");
            Check(meshOf("RlOrange0Lights") == cars.Find("bulwark")->lightsMesh, "with its own lights");
            view.car = "slate";
            view.team = 0;
            rig.system.SetGarageView(view);
            rig.Frames(4);
            Check(meshOf("RlBlue0") == cars.Find("slate")->bodyMesh && paintOf("RlBlue0") == cars.Find("slate")->material[0], "the next car in focus is on the podium the next frame, in blue");
            rig.system.RotateGarageCar(40.0f);
            rig.Frames(2);
            rig.system.SetGarageView({});
            rig.Frames(4);
            Check(meshOf("RlBlue0") == cars.Find("vector")->bodyMesh, "leaving the garage shows the car of the player again (the choice was not changed by the viewing)");

            // the match: the car of the local player is the one of the garage, the others are Pulse
            rig.system.SetSelectedCar("dart");
            sys::RlGameMatchSetup match;
            match.mode = 1;
            match.botLevel = 0;
            rig.system.StartMatch(match);
            rig.Frames(6);
            Check(status.stage == sys::RlGameStage::Playing, "the match is on");
            Check(meshOf("RlBlue0") == cars.Find("dart")->bodyMesh && paintOf("RlBlue0") == cars.Find("dart")->material[0], "the car of the player in the match is Dart in blue");
            // the bot drives the car the match drew for it (G4i): the body on the field is the one of that car, the table says which
            Check(status.carIds.size() == 2 && status.carIds[0] == "dart", "the table knows the cars of the two slots");
            const game::RlCarDef* botCar = status.carIds.size() == 2 ? cars.Find(status.carIds[1]) : nullptr;
            Check(botCar != nullptr && meshOf("RlOrange0") == botCar->bodyMesh, "the bot drives the car the match drew for it");
            Check(status.hud.players.size() == 2 && status.hud.players[0].car == "Dart" && botCar != nullptr && status.hud.players[1].car == botCar->name, "the table has the names of the cars");
            rig.system.LeaveMatch();
            rig.Frames(4);
            Check(meshOf("RlBlue0") == cars.Find("dart")->bodyMesh, "after the match the podium shows the car of the player");
            rig.system.SetSelectedCar("pulse");
        }

        // ---- a private match: the host waits in the lobby
        sys::RlGameMatchSetup host;
        host.kind = sys::RlGameMatchSetup::Kind::Host;
        host.mode = 2;
        host.port = 47871;
        host.playerName = "Host";
        rig.system.StartMatch(host);
        rig.Frames(3);
        Check(status.stage == sys::RlGameStage::Lobby && status.isHost && status.online, "the host is in the lobby");
        Check(status.lobby.size() == 4 && status.lobby[0].you && status.lobby[0].host && status.lobby[0].name == "Host", "the lobby has the four slots, the first is the host");
        Check(status.lobby[1].bot && status.lobby[2].team == 1, "the free slots are bots and the orange team starts at the third");
        rig.system.LeaveMatch();
        rig.Frames(2);
        Check(status.stage == sys::RlGameStage::Menu, "leaving the lobby brings the menu back");

        // a private match on the transport of a lobby service (LAN, Steam): the factory is called once and the system does not keep it
        {
            rl::net::LoopbackHub hub;
            int made = 0;
            sys::RlGameMatchSetup lobbyHost = host;
            lobbyHost.transportFactory = [&](const rl::net::NetSimParams& sim) {
                ++made;
                return rl::net::MakeLoopbackTransport(hub, sim, 5);
            };
            rig.system.StartMatch(lobbyHost);
            rig.Frames(3);
            Check(made == 1, "the transport comes from the factory of the lobby, once");
            Check(status.stage == sys::RlGameStage::Lobby && status.isHost && status.lobby.size() == 4, "the host of a lobby transport waits in the lobby of the match");
            Check(!status.setup.transportFactory, "the system forgot the factory (it holds the lobby service)");
            rig.system.LeaveMatch();
            rig.Frames(2);
            Check(status.stage == sys::RlGameStage::Menu, "leaving brings the menu back");

            sys::RlGameMatchSetup refusing = host;
            refusing.transportFactory = [&](const rl::net::NetSimParams&) { return std::unique_ptr<rl::net::INetTransport>(); };
            rig.system.StartMatch(refusing);
            rig.Frames(3);
            Check(status.stage == sys::RlGameStage::Failed && !status.error.empty(), "a factory that gives no transport fails with a reason");
            rig.system.LeaveMatch();
            rig.Frames(2);
        }

        // a port that is taken fails with the reason
        sys::RlGameMatchSetup first = host;
        first.port = 47872;
        rig.system.StartMatch(first);
        rig.Frames(3);
        Check(status.stage == sys::RlGameStage::Lobby, "the first host listens");
        Rig second;
        second.system.SetGameMode(true);
        second.Frames(3);
        sys::RlGameMatchSetup clash = first;
        second.system.StartMatch(clash);
        second.Frames(3);
        Check(second.system.Status().stage == sys::RlGameStage::Failed && second.system.Status().error.find("47872") != std::string::npos,
              "a port that is taken ends in Failed with a reason that names it");
        second.system.LeaveMatch();
        second.Frames(2);
        rig.system.LeaveMatch();
        rig.Frames(2);

        // ---- the controls that the screens edit
        rl::RlControls& controls = rig.system.Controls();
        Check(controls.ButtonTokens(rl::RlButtonAction::Menu).front() == "Esc", "Esc is the pause by default");
        Check(controls.SetButton(rl::RlButtonAction::Jump, {"F", "PadA"}), "a binding is changed");
        Check(rig.system.SaveControls().empty(), "the controls are saved");
        const fs::path userFile = appdata / "myengine" / "rl_controls.json";
        const fs::path userFileShipping = appdata / "RocketArena" / "rl_controls.json";
        Check(fs::exists(userFile) || fs::exists(userFileShipping), "into the user folder");
        const std::string reset = rig.system.ResetControls();
        (void)reset;
        Check(rig.system.Controls().ButtonTokens(rl::RlButtonAction::Jump).front() == "MouseRight", "the reset brings the shipped bindings back");
        Check(!fs::exists(userFile) && !fs::exists(userFileShipping), "and removes the file of the player");

        // the camera of the menu replaces the numbers of the tuning
        rl::RlCameraTuning camera = rig.system.CameraSettings();
        camera.fov = 80.0f;
        rig.system.SetCameraSettings(camera, true);
        Check(rig.system.CameraSettings().fov == 80.0f, "the camera settings are the player's");
    }

    // The ball and the cars of an online match are on the screen for the host and for the guest (the guest plays orange). The match used to begin inside
    // the frame in which the menu (the podium) was still being updated: the podium hid everything the match had just shown, and the ball stayed
    // invisible for good. The test runs a host and a guest through a loopback hub, frame by frame, the way the lobby screens drive them.
    void TestOnlineVisuals()
    {
        std::cout << "RlGameSystem: the ball and the cars of an online match are visible for the host and for the guest" << std::endl;
        const fs::path appdata = TempFolder("myengine_game_online_visuals_test");
        _putenv_s("APPDATA", appdata.string().c_str());
        _putenv_s("LOCALAPPDATA", appdata.string().c_str());

        for (int mode = 1; mode <= 2; ++mode)
        {
            rl::net::LoopbackHub hub;
            Rig hostRig;
            Rig guestRig;
            hostRig.system.SetGameMode(true);
            guestRig.system.SetGameMode(true);
            hostRig.Frames(2);
            guestRig.Frames(2);

            sys::RlGameMatchSetup host;
            host.kind = sys::RlGameMatchSetup::Kind::Host;
            host.mode = mode;
            host.port = 47873;
            host.playerName = "Host";
            host.transportFactory = [&](const rl::net::NetSimParams& sim) { return rl::net::MakeLoopbackTransport(hub, sim, 1); };
            hostRig.system.StartMatch(host);
            hostRig.Frames(3);
            Check(hostRig.system.Status().stage == sys::RlGameStage::Lobby, "the host waits in the lobby");

            sys::RlGameMatchSetup join;
            join.kind = sys::RlGameMatchSetup::Kind::Join;
            join.mode = mode;
            join.address = "47873";
            join.playerName = "Guest";
            join.transportFactory = [&](const rl::net::NetSimParams& sim) { return rl::net::MakeLoopbackTransport(hub, sim, 2); };
            guestRig.system.StartMatch(join);

            const auto step = [&](const int frames)
            {
                for (int i = 0; i < frames; ++i)
                {
                    hostRig.system.Update(hostRig.world, 1.0f / 60.0f);
                    guestRig.system.Update(guestRig.world, 1.0f / 60.0f);
                }
            };
            // the guest is in the lobby of the host, the host starts the match
            bool inLobby = false;
            for (int i = 0; i < 600 && !inLobby; ++i)
            {
                step(1);
                inLobby = guestRig.system.Status().stage == sys::RlGameStage::Lobby && hostRig.system.Status().lobby.size() >= 2;
            }
            Check(inLobby, "the guest is in the lobby of the host");
            hostRig.system.StartHostedMatch();
            bool playing = false;
            for (int i = 0; i < 1200 && !playing; ++i)
            {
                step(1);
                playing = hostRig.system.Status().stage == sys::RlGameStage::Playing && guestRig.system.Status().stage == sys::RlGameStage::Playing;
            }
            Check(playing, "the match is on for both");
            step(30);

            const auto visible = [](Rig& rig, const char* tag)
            {
                ecs::EntityId found = ecs::kInvalidEntity;
                rig.world.ForEach<components::TagComponent>([&](const ecs::EntityId entity, const components::TagComponent& t) { found = t.name == tag ? entity : found; });
                const auto* renderer = rig.world.TryGet<components::MeshRendererComponent>(found);
                return renderer != nullptr && renderer->visible;
            };
            for (Rig* rig : {&hostRig, &guestRig})
            {
                const char* who = rig == &hostRig ? "host" : "guest (orange)";
                Check(visible(*rig, "RlBall"), (std::string("the ball is visible for the ") + who).c_str());
                Check(visible(*rig, "RlBlue0"), (std::string("the blue car is visible for the ") + who).c_str());
                Check(visible(*rig, "RlOrange0"), (std::string("the orange car is visible for the ") + who).c_str());
                Check(!visible(*rig, "RlPodium"), (std::string("the podium of the menu is gone for the ") + who).c_str());
            }
            Check(guestRig.system.Status().stage == sys::RlGameStage::Playing && !guestRig.system.Status().isHost, "the guest plays as a client");
            guestRig.system.LeaveMatch();
            hostRig.system.LeaveMatch();
            guestRig.Frames(2);
            hostRig.Frames(2);
        }
    }

    // RL-B3: the host of a private match is gone (it crashed and says nothing, or it leaves). The guest gets a Failed stage with the reason, the screens show
    // it once and acknowledge it, and then the menu works as ever: nothing throws the player back to the main screen, an offline match and a new lobby start.
    // The other way round (a guest goes) is no failure for the host. One scenario = one host, one guest, a loopback hub, frame by frame like the lobby screens.
    // The lobby list always has a row for every seat of the match (a bot or an empty one in the seats nobody took): the players are the rows with a name and no bot
    int HumansIn(const sys::RlGameStatus& status)
    {
        return static_cast<int>(std::count_if(status.lobby.begin(), status.lobby.end(), [](const sys::RlLobbySlot& slot) { return !slot.name.empty() && !slot.bot; }));
    }

    void RunHostLost(const bool inMatch, const bool hostCrashes, const int port)
    {
        const std::string title = std::string("the host is gone ") + (inMatch ? "in the match" : "in the lobby") + (hostCrashes ? ", silently" : ", leaving");
        std::cout << "  " << title << std::endl;
        rl::net::LoopbackHub hub;
        Rig hostRig;
        Rig guestRig;
        hostRig.system.SetGameMode(true);
        guestRig.system.SetGameMode(true);
        hostRig.Frames(2);
        guestRig.Frames(2);

        sys::RlGameMatchSetup host;
        host.kind = sys::RlGameMatchSetup::Kind::Host;
        host.mode = 1;
        host.port = static_cast<std::uint16_t>(port);
        host.playerName = "Host";
        host.transportFactory = [&](const rl::net::NetSimParams& sim) { return rl::net::MakeLoopbackTransport(hub, sim, 1); };
        hostRig.system.StartMatch(host);
        hostRig.Frames(3);
        sys::RlGameMatchSetup join;
        join.kind = sys::RlGameMatchSetup::Kind::Join;
        join.mode = 1;
        join.address = std::to_string(port);
        join.playerName = "Guest";
        join.transportFactory = [&](const rl::net::NetSimParams& sim) { return rl::net::MakeLoopbackTransport(hub, sim, 2); };
        guestRig.system.StartMatch(join);

        bool hostAlive = true;
        bool guestAlive = true;
        const auto step = [&](const int frames, const bool slow = false)
        {
            for (int i = 0; i < frames; ++i)
            {
                if (hostAlive)
                {
                    hostRig.system.Update(hostRig.world, 1.0f / 60.0f);
                }
                if (guestAlive)
                {
                    guestRig.system.Update(guestRig.world, 1.0f / 60.0f);
                }
                if (slow)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));  // the network clock of the game is the real one: a silence of five seconds takes five
                }
            }
        };
        const auto stageOf = [](Rig& rig) { return rig.system.Status().stage; };
        const auto waitFor = [&](const std::function<bool()>& done, const int frames, const bool slow = false)
        {
            for (int i = 0; i < frames && !done(); ++i)
            {
                step(1, slow);
            }
            return done();
        };
        Check(waitFor([&] { return stageOf(guestRig) == sys::RlGameStage::Lobby && HumansIn(hostRig.system.Status()) >= 2; }, 600), "the guest is in the lobby of the host");
        if (inMatch)
        {
            hostRig.system.StartHostedMatch();
            Check(waitFor([&] { return stageOf(hostRig) == sys::RlGameStage::Playing && stageOf(guestRig) == sys::RlGameStage::Playing; }, 1200), "the match is on for both");
            step(30);
        }

        // the host goes: with its goodbye, or without one (the guest finds out when nothing has come for five seconds)
        if (hostCrashes)
        {
            hostAlive = false;
        }
        else
        {
            hostRig.system.LeaveMatch();
            step(3);
            hostAlive = false;
        }
        Check(waitFor([&] { return stageOf(guestRig) == sys::RlGameStage::Failed; }, 900, true), "the guest's stage turns Failed");
        Check(!guestRig.system.Status().error.empty(), "and says why");
        step(20);
        Check(stageOf(guestRig) == sys::RlGameStage::Failed, "the failure waits for the screen that shows it");

        // the screen showed it: the failure is over, it does not come back by itself
        guestRig.system.AcknowledgeFailure();
        Check(stageOf(guestRig) == sys::RlGameStage::Menu && guestRig.system.Status().error.empty(), "an acknowledged failure is the menu again, without the error");
        step(120);
        Check(stageOf(guestRig) == sys::RlGameStage::Menu && guestRig.system.Status().error.empty(), "and the menu stays the menu (no network object is left to fail again)");
        guestRig.system.AcknowledgeFailure();
        Check(stageOf(guestRig) == sys::RlGameStage::Menu, "acknowledging when nothing failed changes nothing");

        // everything starts again: a match against bots, a new lobby of this player, another guest in it
        sys::RlGameMatchSetup offline;
        offline.mode = 1;
        offline.botLevel = 1;
        guestRig.system.StartMatch(offline);
        Check(waitFor([&] { return stageOf(guestRig) == sys::RlGameStage::Playing; }, 120), "an offline match starts after the failure");
        guestRig.system.LeaveMatch();
        step(3);
        Check(stageOf(guestRig) == sys::RlGameStage::Menu, "and ends in the menu");

        sys::RlGameMatchSetup again;
        again.kind = sys::RlGameMatchSetup::Kind::Host;
        again.mode = 1;
        again.port = static_cast<std::uint16_t>(port + 1);
        again.playerName = "Guest";
        again.transportFactory = [&](const rl::net::NetSimParams& sim) { return rl::net::MakeLoopbackTransport(hub, sim, 3); };
        guestRig.system.StartMatch(again);
        Check(waitFor([&] { return stageOf(guestRig) == sys::RlGameStage::Lobby; }, 120), "a new lobby opens after the failure");
        guestRig.system.LeaveMatch();
        step(3);
        Check(stageOf(guestRig) == sys::RlGameStage::Menu, "and closes into the menu");
    }

    // A guest who goes is not a failure of the host: the lobby loses a name, the match goes on, no error appears
    void RunGuestLost(const bool inMatch, const bool guestCrashes, const int port)
    {
        const std::string title = std::string("the guest is gone ") + (inMatch ? "in the match" : "in the lobby") + (guestCrashes ? ", silently" : ", leaving");
        std::cout << "  " << title << std::endl;
        rl::net::LoopbackHub hub;
        Rig hostRig;
        Rig guestRig;
        hostRig.system.SetGameMode(true);
        guestRig.system.SetGameMode(true);
        hostRig.Frames(2);
        guestRig.Frames(2);
        sys::RlGameMatchSetup host;
        host.kind = sys::RlGameMatchSetup::Kind::Host;
        host.mode = 1;
        host.port = static_cast<std::uint16_t>(port);
        host.playerName = "Host";
        host.transportFactory = [&](const rl::net::NetSimParams& sim) { return rl::net::MakeLoopbackTransport(hub, sim, 1); };
        hostRig.system.StartMatch(host);
        hostRig.Frames(3);
        sys::RlGameMatchSetup join;
        join.kind = sys::RlGameMatchSetup::Kind::Join;
        join.mode = 1;
        join.address = std::to_string(port);
        join.playerName = "Guest";
        join.transportFactory = [&](const rl::net::NetSimParams& sim) { return rl::net::MakeLoopbackTransport(hub, sim, 2); };
        guestRig.system.StartMatch(join);

        bool guestAlive = true;
        const auto step = [&](const int frames, const bool slow = false)
        {
            for (int i = 0; i < frames; ++i)
            {
                hostRig.system.Update(hostRig.world, 1.0f / 60.0f);
                if (guestAlive)
                {
                    guestRig.system.Update(guestRig.world, 1.0f / 60.0f);
                }
                if (slow)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
            }
        };
        const auto waitFor = [&](const std::function<bool()>& done, const int frames, const bool slow = false)
        {
            for (int i = 0; i < frames && !done(); ++i)
            {
                step(1, slow);
            }
            return done();
        };
        const sys::RlGameStatus& hostStatus = hostRig.system.Status();
        Check(waitFor([&] { return guestRig.system.Status().stage == sys::RlGameStage::Lobby && HumansIn(hostStatus) >= 2; }, 600), "the guest is in the lobby of the host");
        if (inMatch)
        {
            hostRig.system.StartHostedMatch();
            Check(waitFor([&] { return hostStatus.stage == sys::RlGameStage::Playing && guestRig.system.Status().stage == sys::RlGameStage::Playing; }, 1200), "the match is on for both");
            step(30);
        }
        if (guestCrashes)
        {
            guestAlive = false;
        }
        else
        {
            guestRig.system.LeaveMatch();
            step(3);
            guestAlive = false;
        }
        bool failed = false;
        const auto watch = [&] { failed = failed || hostStatus.stage == sys::RlGameStage::Failed || !hostStatus.error.empty(); return false; };
        waitFor(watch, guestCrashes ? 420 : 150, true);  // the clock of the network is the real one; a silent guest is dropped after five seconds, one that says goodbye is not heard at once
        Check(!failed, "the host never fails because a guest went");
        Check(hostStatus.stage == (inMatch ? sys::RlGameStage::Playing : sys::RlGameStage::Lobby), "the host stays in its lobby / match");
        if (!inMatch)
        {
            Check(HumansIn(hostStatus) == 1, "the lobby lost the name of the guest");
        }
        hostRig.system.LeaveMatch();
        step(3);
        Check(hostStatus.stage == sys::RlGameStage::Menu, "the host leaves into the menu");
    }

    void TestHostLost()
    {
        std::cout << "RlGameSystem: a host that is gone is one error, shown once; a guest that is gone is no error" << std::endl;
        const fs::path appdata = TempFolder("myengine_game_host_lost_test");
        _putenv_s("APPDATA", appdata.string().c_str());
        _putenv_s("LOCALAPPDATA", appdata.string().c_str());
        RunHostLost(false, false, 47910);
        RunHostLost(true, false, 47912);
        RunHostLost(false, true, 47914);
        RunHostLost(true, true, 47916);
        RunGuestLost(false, false, 47920);
        RunGuestLost(true, false, 47922);
        RunGuestLost(true, true, 47924);
    }

    // A demolished car is parked under the map until it comes back. The camera of the player must stay where the car was blown up, not follow it
    // down (the player looked at the arena from below, "under the textures", for the three seconds of the respawn).
    void TestDemolishCamera()
    {
        std::cout << "RlGameSystem: the camera stays over the field while the car of the player is demolished" << std::endl;
        const fs::path appdata = TempFolder("myengine_game_demolish_camera_test");
        _putenv_s("APPDATA", appdata.string().c_str());
        _putenv_s("LOCALAPPDATA", appdata.string().c_str());
        Rig rig;
        rig.system.SetGameMode(true);
        rig.Frames(2);
        sys::RlGameMatchSetup offline;
        offline.kind = sys::RlGameMatchSetup::Kind::Offline;
        offline.mode = 1;
        offline.playerName = "Player";
        rig.system.StartMatch(offline);
        rig.Frames(2);
        rig.system.MatchViewBegins();
        Check(rig.Until([&] { return rig.system.Status().stage == sys::RlGameStage::Playing; }, 600), "the offline match is on");
        Check(rig.Until([&] { return rig.system.Status().hud.phase == rlui::HudPhase::Playing; }, 60 * 20, 1.0f / 60.0f), "the countdown is over");   // the flight, the countdown
        const auto cameraY = [&]
        {
            float y = 1000.0f;
            rig.world.ForEach<components::CameraComponent>([&](const ecs::EntityId, const components::CameraComponent& camera) { y = camera.position.y; });
            return y;
        };
        Check(cameraY() > 0.2f, "the camera is over the field before");
        rig.system.DebugCommand("demolish");
        float lowest = 1000.0f;
        bool sawDown = false;
        for (int i = 0; i < 60 * 5; ++i)
        {
            rig.Frames(1, 1.0f / 60.0f);
            lowest = std::min(lowest, cameraY());
            sawDown = sawDown || rig.system.Status().hud.demolished;
        }
        Check(sawDown, "the car of the player was demolished");
        Check(lowest > 0.2f, "the camera never went under the floor while the car was parked");
        rig.system.LeaveMatch();
        rig.Frames(2);
    }

    // The ball stays visible in every way to play: the practice, a match against bots, the free play and a pack of exercises of the training, and
    // through a goal with its celebration and its replay (the director of the replay moves the live ball out of the way and has to bring it back).
    void TestBallVisibleEverywhere()
    {
        std::cout << "RlGameSystem: the ball is visible in the practice, the match, the training, the goal and the replay" << std::endl;
        const fs::path appdata = TempFolder("myengine_game_ball_everywhere_test");
        _putenv_s("APPDATA", appdata.string().c_str());
        _putenv_s("LOCALAPPDATA", appdata.string().c_str());
        struct Case
        {
            const char* name;
            sys::RlGameMatchSetup::Kind kind;
            int mode;
            const char* pack;
        };
        const Case cases[] = {
            {"the practice", sys::RlGameMatchSetup::Kind::Offline, 0, ""},
            {"a match 1v1", sys::RlGameMatchSetup::Kind::Offline, 1, ""},
            {"a match 2v2", sys::RlGameMatchSetup::Kind::Offline, 2, ""},
            {"the free play", sys::RlGameMatchSetup::Kind::Training, 1, ""},
            {"a pack of exercises", sys::RlGameMatchSetup::Kind::Training, 1, "striker"},
        };
        for (const Case& c : cases)
        {
            Rig rig;
            rig.system.SetGameMode(true);
            rig.Frames(2);
            sys::RlGameMatchSetup setup;
            setup.kind = c.kind;
            setup.mode = c.mode;
            setup.trainingPack = c.pack;
            setup.playerName = "Player";
            rig.system.StartMatch(setup);
            rig.Frames(2);
            rig.system.MatchViewBegins();
            const bool on = rig.Until([&] { return rig.system.Status().stage == sys::RlGameStage::Playing; }, 600, 1.0f / 60.0f);
            Check(on, (std::string(c.name) + " starts").c_str());
            const auto ballVisible = [&]
            {
                ecs::EntityId found = ecs::kInvalidEntity;
                rig.world.ForEach<components::TagComponent>([&](const ecs::EntityId entity, const components::TagComponent& t) { found = t.name == "RlBall" ? entity : found; });
                const auto* renderer = rig.world.TryGet<components::MeshRendererComponent>(found);
                return renderer != nullptr && renderer->visible;
            };
            rig.Frames(60 * 8, 1.0f / 60.0f);
            Check(ballVisible(), (std::string("the ball is visible in ") + c.name).c_str());
            if (c.kind == sys::RlGameMatchSetup::Kind::Offline && c.mode > 0)
            {
                rig.Until([&] { return rig.system.Status().hud.phase == rlui::HudPhase::Playing; }, 60 * 20, 1.0f / 60.0f);
                rig.system.DebugCommand("goal-blue");
                int hidden = 0;
                for (int i = 0; i < 60 * 25; ++i)
                {
                    rig.Frames(1, 1.0f / 60.0f);
                    // (the results of a finished match show the podium instead of the field)
                    hidden += (rig.system.Status().stage != sys::RlGameStage::Playing || ballVisible()) ? 0 : 1;
                }
                Check(hidden == 0, (std::string("the ball is visible through the goal and its replay in ") + c.name).c_str());
            }
            rig.system.LeaveMatch();
            rig.Frames(2);
        }
    }

    // The floor of the arena has no white spot where the kick-off camera of the back spots stands: a dot painted in the turf two and a half metres
    // from that camera blew up into a white blur at the corner of the screen. The texture covers the field (2 x halfWidth by 2 x halfLength, +z at the
    // top, 20 pixels per metre); the pixels around (0, +-38.7), where the dots were, are turf.
    void TestFloorHasNoSpotsAtTheKickoffCamera(const fs::path& source)
    {
        std::cout << "arena floor: no white spot in front of the goals" << std::endl;
        const std::string path = (source / "assets" / "textures" / "rl2" / "arena_floor.tga").string();
        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
        Check(pixels != nullptr && width > 1000 && height > 1000, "the floor texture loads");
        if (pixels == nullptr)
        {
            return;
        }
        const float ppm = static_cast<float>(height) / 102.4f;   // halfLength 51.2 m
        const float halfWidthMetres = static_cast<float>(width) / ppm * 0.5f;
        int brightest = 0;
        for (const float z : {-38.7f, 38.7f})
        {
            const int cx = static_cast<int>(halfWidthMetres * ppm);
            const int cy = static_cast<int>((51.2f - z) * ppm);
            for (int dy = -12; dy <= 12; ++dy)
            {
                for (int dx = -12; dx <= 12; ++dx)
                {
                    const unsigned char* p = pixels + 4 * ((cy + dy) * width + cx + dx);
                    brightest = std::max(brightest, static_cast<int>(p[0]));
                }
            }
        }
        std::cout << "  the brightest red in the 0.6 m squares in front of the goals: " << brightest << std::endl;
        Check(brightest < 150, "the turf in front of the goals is turf, not a white dot");
        stbi_image_free(pixels);
    }

    // ---------------------------------------------------------------- the video window: chosen, applied, kept or reverted

    // The window mode and the resolution of Settings > Video wait for APPLY: the steps only choose, APPLY hands the window ONE change and opens the question,
    // Keep / Revert / the end of the time answer it, leaving with a choice asks, Discard throws the choice away without a touch of the window.
    void TestVideoWindowChange()
    {
        using game::RlVideoWindowChange;
        using game::RlWindowChoice;

        // the way the screen drives it: every change of the window that reaches the display is counted
        int windowChanges = 0;
        RlWindowChoice shown{};
        RlVideoWindowChange w;
        const auto apply = [&]() -> bool
        {
            if (!w.Apply())
            {
                return false;
            }
            ++windowChanges;
            shown = w.Applied();
            return true;
        };
        const auto revert = [&]()
        {
            if (w.Revert())
            {
                ++windowChanges;
                shown = w.Applied();
            }
        };

        w.Set({0, 1600, 900});
        shown = w.Applied();
        Check(!w.Dirty() && !w.Keeping(), "the window that was loaded is applied: nothing is chosen, no question");
        Check(!apply() && windowChanges == 0, "APPLY with nothing chosen does nothing");

        // steps of the resolution and a window mode only choose
        w.SetPendingSize(1280, 720);
        w.SetPendingSize(1366, 768);
        w.SetPendingSize(1920, 1080);
        w.SetPendingMode(2);
        Check(windowChanges == 0 && shown.width == 1600 && shown.mode == 0, "steps and clicks do not touch the window");
        Check(w.Dirty() && w.Pending().width == 1920 && w.Pending().mode == 2, "the chosen values are shown");
        Check(w.Applied().width == 1600 && w.Applied().mode == 0, "and the applied ones are the old ones");

        // APPLY: both at once, one change, then the question
        Check(apply(), "APPLY applies a chosen window");
        Check(windowChanges == 1 && shown.mode == 2 && shown.width == 1920 && shown.height == 1080, "the window changed once, to the mode and the size together");
        Check(w.Keeping() && std::abs(w.Remaining() - RlVideoWindowChange::kKeepSeconds) < 1e-4f, "the question of Keep / Revert is open for 15 s");
        Check(!w.Dirty(), "nothing is chosen any more");
        Check(!apply() && windowChanges == 1, "APPLY while the question is open does nothing");
        w.SetPendingMode(0);
        w.SetPendingSize(1280, 720);
        Check(!w.Dirty() && w.Pending().mode == 2, "a choice cannot be made while the question is open");

        // Keep
        w.Keep();
        Check(!w.Keeping() && w.Applied().mode == 2 && w.Applied().width == 1920, "Keep leaves the new window");
        Check(!w.Revert() && windowChanges == 1, "a Revert after Keep has nothing to bring back");
        Check(!w.Tick(100.0f) && windowChanges == 1, "the time of a closed question does not run");

        // Revert
        w.SetPendingMode(0);
        w.SetPendingSize(1600, 900);
        Check(apply() && windowChanges == 2, "the next APPLY");
        Check(!w.Tick(5.0f) && w.Keeping() && std::abs(w.Remaining() - 10.0f) < 1e-3f, "5 s later 10 s are left");
        revert();
        Check(windowChanges == 3 && shown.mode == 2 && shown.width == 1920, "Revert brings the old window back, once");
        Check(!w.Keeping() && !w.Dirty() && w.Pending().mode == 2 && w.Pending().width == 1920, "and the chosen values are the old ones again");

        // the time is up: the same as Revert
        w.SetPendingSize(1280, 720);
        Check(apply() && windowChanges == 4, "APPLY again");
        Check(!w.Tick(14.0f) && w.Keeping(), "14 s: still waiting");
        Check(w.Tick(1.5f), "past 15 s the question ends");
        windowChanges += 1;
        shown = w.Applied();
        Check(!w.Keeping() && shown.width == 1920 && !w.Dirty() && w.Pending().width == 1920, "the time out is a Revert: the old window and the chosen values");
        Check(!w.Revert(), "and it was one answer, not two");

        // Discard: the window is not touched, because nothing was applied
        const int before = windowChanges;
        w.SetPendingMode(1);
        w.SetPendingSize(1366, 768);
        Check(w.Dirty(), "a choice again");
        w.Discard();
        Check(!w.Dirty() && w.Pending().mode == 2 && w.Pending().width == 1920 && windowChanges == before, "Discard returns the chosen values to the applied ones and applies nothing");

        // Borderless takes the desktop: its size is not a choice
        w.Set({1, 2560, 1440});
        w.SetPendingSize(1280, 720);
        Check(!w.Dirty(), "a size is no change of a Borderless window");
        w.SetPendingMode(0);
        Check(w.Dirty(), "but the mode is");
        w.SetPendingMode(1);
        Check(!w.Dirty(), "back to Borderless: nothing is chosen again");

        // Set() (the settings were loaded, a reset) ends everything
        w.SetPendingMode(2);
        Check(apply() && w.Keeping(), "the question is open");
        w.Set({0, 1600, 900});
        Check(!w.Keeping() && !w.Dirty() && w.Applied().mode == 0, "Set starts from the applied window with no question");
        Check(game::SameWindow({1, 1, 1}, {1, 2, 2}) && !game::SameWindow({0, 1, 1}, {0, 2, 2}) && !game::SameWindow({0, 1, 1}, {2, 1, 1}), "the same window: Borderless by the mode, the others by the size too");
    }
}

int main()
{
    try
    {
        const fs::path source = fs::u8path(MYENGINE_SOURCE_DIR);
        TestSettings();
        TestVideoWindowChange();
        TestScript();
        TestSystem(source);
        TestOnlineVisuals();
        TestHostLost();
        TestDemolishCamera();
        TestBallVisibleEverywhere();
        TestFloorHasNoSpotsAtTheKickoffCamera(source);
    }
    catch (const std::exception& error)
    {
        std::cout << "FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "OK: " << g_checks << " checks\n";
    return 0;
}
