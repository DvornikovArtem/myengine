// The game mode of Rocket League (G1-flow) without a window or a GPU: the settings file (defaults, round trip, clamping, a broken file, names cut at a
// character), the script of the UI tests (parsing, the order and the time of the commands, taps), and RlGameSystem driven through its public commands
// the way the screens drive it: a match from the start to the results (the HUD data, the pause that stands still, the events of the feed, the winner and
// the MVP), the lobby of a private match, the controls that the settings screen edits and saves.

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
#include <myengine/game/RlGameSettings.h>
#include <myengine/input/InputManager.h>

#include <rl/RlControls.h>
#include <rl/RlTuning.h>

#include "../../engine/src/core/UiScript.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
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
        changed.audio.music = 0.25f;
        changed.invertPitch = true;
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
        Check(back.camera.fov == 95.0f && back.camera.distanceCm == 300.0f && back.camera.ballCamHold && back.audio.music == 0.25f && back.invertPitch,
              "the camera, the audio and the pitch come back");
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
        Check(longName.play.playerName.size() <= 15 && longName.play.playerName.size() % 2 == 0, "a long name is cut at a character");
        Check(!defaults.PlayerName().empty() && defaults.PlayerName().size() <= 15, "the name of the player is never empty and fits the protocol");

        // the camera of the menu and the numbers of the camera of the game
        rl::RlCameraTuning tuning;
        tuning.wallMargin = 0.77f;
        const game::RlCameraSettings fromTuning = game::RlCameraSettings::FromTuning(tuning);
        Check(fromTuning.distanceCm == 270.0f && fromTuning.heightCm == 100.0f, "the centimetres of the menu come from the metres of the tuning");
        game::RlCameraSettings custom = fromTuning;
        custom.distanceCm = 350.0f;
        custom.fov = 90.0f;
        const rl::RlCameraTuning applied = custom.ToTuning(tuning);
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
            for (const char* name : {"RlArena", "RlBall", "RlBlue0", "RlOrange0", "RlPodium"})
            {
                add(name);
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
        Check(rig.Until([&] { return status.hud.score[0] == 1; }, 200), "a goal for blue is counted");
        Check(rig.Until([&] { return status.hud.phase == rlui::HudPhase::GoalScored; }, 60) || status.hud.phase == rlui::HudPhase::GoalScored, "the phase is GoalScored");
        Check(status.hud.lastGoalTeam == 0 && status.hud.lastGoalSpeedKmh > 30.0f, "the last goal has its team and its speed in km/h");
        const std::vector<rlui::HudEvent> events = rig.system.TakeHudEvents();
        Check(std::any_of(events.begin(), events.end(), [](const rlui::HudEvent& e) { return e.type == rlui::HudEventType::Goal; }), "the goal reaches the feed");
        Check(rig.system.TakeHudEvents().empty(), "an event comes once");

        // the end of the match: the winner, the MVP, the stage
        Check(rig.Until([&] { return status.stage == sys::RlGameStage::Finished; }, 1200), "the match ends and the stage turns Finished");
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
}

int main()
{
    try
    {
        const fs::path source = fs::u8path(MYENGINE_SOURCE_DIR);
        TestSettings();
        TestScript();
        TestSystem(source);
    }
    catch (const std::exception& error)
    {
        std::cout << "FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "OK: " << g_checks << " checks\n";
    return 0;
}
