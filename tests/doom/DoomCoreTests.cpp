// DoomCoreTests.cpp
// Headless tests of the DOOM of the engine (game/doom): DoomGame over doomgeneric with Freedoom, no window and no sound.
//
// DOOM keeps its state in globals and can be started once per process, so the test starts itself again for every scenario
// (`--scenario <name>`) and reads what the child printed: lines "RESULT key=value". Without the WAD the test is skipped (exit code 77):
// run tools/doom/fetch_freedoom.ps1 once. `--wad <path>` names another WAD, `--verbose` prints the output of the children.

#include <doom/DoomGame.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" void G_ExitLevel(void);

namespace
{
    namespace fs = std::filesystem;

    int g_failures = 0;
    int g_checks = 0;
    bool g_verbose = false;

    void Check(bool ok, const char* what, const char* file, int line)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("  FAIL %s (%s:%d)\n", what, file, line);
        }
    }
#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)

    constexpr double kTic = 1.0 / 35.0;

    // The two files the hashes of DEMO1 are pinned for (the size tells which one it is): Freedoom 0.13.0 Phase 1, which tools/doom/fetch_freedoom.ps1
    // downloads, and the shareware doom1.wad v1.9 (4 196 020 bytes)
    constexpr std::uintmax_t kFreedoom1Size = 28795076;
    constexpr std::uintmax_t kShareware1Size = 4196020;

    void Result(const char* key, const std::string& value)
    {
        std::printf("RESULT %s=%s\n", key, value.c_str());
        std::fflush(stdout);
    }

    void Result(const char* key, double value)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%.4f", value);
        Result(key, std::string(text));
    }

    std::string Hex(std::uint64_t value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
        return text;
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // The scenarios: one DOOM per process

    std::unique_ptr<doom::DoomGame> Start(const fs::path& wad, std::vector<std::string> args)
    {
        doom::DoomConfig config;
        config.iwad = wad;
        config.dataDirectory = fs::temp_directory_path() / "myengine_doom_core_test";
        config.extraArgs = std::move(args);
        std::string error;
        auto game = doom::DoomGame::Create(config, error);
        if (!game)
        {
            std::printf("  Create failed: %s\n", error.c_str());
        }
        return game;
    }

    void Run(doom::DoomGame& game, int tics)
    {
        for (int i = 0; i < tics; ++i)
        {
            game.Advance(kTic);
        }
    }

    // 35 tics of the demo DEMO1: the frame is the same in every run
    int ScenarioDemo(const fs::path& wad)
    {
        auto game = Start(wad, {"-playdemo", "demo1", "-nomusic"});
        if (!game)
        {
            return 1;
        }
        Run(*game, 35);
        Result("hash", Hex(game->FrameHash()));
        Result("tic", game->Info().gameTic);
        Result("demo", game->Info().demoPlayback ? "1" : "0");
        Result("frames", static_cast<double>(game->FrameCount()));
        Result("title", game->WindowTitle());
        return 0;
    }

    struct Spot
    {
        double x, y, angle;
    };

    Spot Where(doom::DoomGame& game)
    {
        const doom::PlayerState p = game.Player(game.Info().consolePlayer);
        return {p.x, p.y, p.angleDegrees};
    }

    void ReportSpot(const char* prefix, const Spot& s)
    {
        Result((std::string(prefix) + "_x").c_str(), s.x);
        Result((std::string(prefix) + "_y").c_str(), s.y);
        Result((std::string(prefix) + "_angle").c_str(), s.angle);
    }

    // The mouse movement of one tic is the sum of the events that came in it: eight events of 5 turn the player as one event of 40 does (DOOM kept only the last)
    int ScenarioMouse(const fs::path& wad)
    {
        auto game = Start(wad, {"-warp", "1", "1", "-skill", "3", "-nomonsters", "-nomusic"});
        if (!game)
        {
            return 1;
        }
        Run(*game, 35);
        auto turnOf = [&](const std::function<void()>& post)
        {
            const Spot before = Where(*game);
            post();
            Run(*game, 1);
            const Spot after = Where(*game);
            Run(*game, 3);
            double delta = after.angle - before.angle;
            while (delta > 180.0) delta -= 360.0;
            while (delta < -180.0) delta += 360.0;
            return delta;
        };
        Result("one_event", turnOf([&] { game->PostMouse(0, 40, 0); }));
        Result("eight_events", turnOf([&] { for (int i = 0; i < 8; ++i) game->PostMouse(0, 5, 0); }));
        Result("back_and_forth", turnOf([&] { game->PostMouse(0, 30, 0); game->PostMouse(0, -30, 0); }));
        Result("hundred_events", turnOf([&] { for (int i = 0; i < 100; ++i) game->PostMouse(0, 1, 0); }));
        Result("hundred_one", turnOf([&] { game->PostMouse(0, 100, 0); }));
        Result("still", turnOf([&] {}));
        return 0;
    }

    // E1M1, skill 3: the player stands; W (the up arrow of DOOM) moves it; the mouse turns it; Esc opens the menu
    int ScenarioInput(const fs::path& wad)
    {
        auto game = Start(wad, {"-warp", "1", "1", "-skill", "3", "-nomonsters", "-nomusic"});
        if (!game)
        {
            return 1;
        }
        Run(*game, 35);
        Result("level", game->Info().state == doom::GameState::Level ? "1" : "0");
        Result("menu0", game->Info().menuActive ? "1" : "0");
        const Spot still0 = Where(*game);
        Run(*game, 35);
        const Spot still1 = Where(*game);
        ReportSpot("still", still1);
        Result("still_moved", std::hypot(still1.x - still0.x, still1.y - still0.y));

        game->PostKey(doom::key::kUp, true);
        Run(*game, 35);
        game->PostKey(doom::key::kUp, false);
        Run(*game, 5);
        const Spot walked = Where(*game);
        Result("walk_distance", std::hypot(walked.x - still1.x, walked.y - still1.y));

        for (int i = 0; i < 10; ++i)
        {
            game->PostMouse(0, 40, 0);
            game->Advance(kTic);
        }
        Run(*game, 3);
        const Spot turned = Where(*game);
        double delta = turned.angle - walked.angle;
        while (delta > 180.0) delta -= 360.0;
        while (delta < -180.0) delta += 360.0;
        Result("turn_degrees", delta);

        game->PostKey(doom::key::kEscape, true);
        Run(*game, 2);
        game->PostKey(doom::key::kEscape, false);
        Run(*game, 2);
        Result("menu1", game->Info().menuActive ? "1" : "0");
        Result("frames", static_cast<double>(game->FrameCount()));
        Result("pixels", game->Pixels() != nullptr ? "1" : "0");
        return 0;
    }

    // Native contract coverage only: this deliberately bypasses DoomGameFlow and posts kUse directly.
    // A UI frame can be much shorter than a DOOM tic, so the Flow's intermission CONTINUE action must
    // keep its use edge until a tic is actually consumed, then release it. Frontend routing is proved
    // separately by the bounded hidden-desktop GUI evidence run.
    int ScenarioIntermission(const fs::path& wad)
    {
        auto game = Start(wad, {"-warp", "1", "1", "-skill", "3", "-nomonsters", "-nomusic"});
        if (!game)
        {
            return 1;
        }
        Run(*game, 35);
        G_ExitLevel();
        game->Advance(kTic);
        Result("entered", game->Info().state == doom::GameState::Intermission ? "1" : "0");

        // Let the real intermission stat stages reach the normal continuation point.
        Run(*game, 280);
        Result("stats", game->Info().state == doom::GameState::Intermission ? "1" : "0");
        for (int press = 0; press < 3; ++press)
        {
            const int startTic = game->Info().gameTic;
            bool keyDown = false;
            bool consumed = false;
            for (int frame = 0; frame < 240 && !consumed; ++frame)
            {
                if (!keyDown)
                {
                    game->PostKey(doom::key::kUse, true);
                    keyDown = true;
                }
                game->Advance(1.0 / 240.0);
                if (game->Info().gameTic != startTic)
                {
                    game->PostKey(doom::key::kUse, false);
                    keyDown = false;
                    consumed = true;
                }
            }
            if (keyDown)
            {
                game->PostKey(doom::key::kUse, false);
            }
            for (int frame = 0; frame < 100; ++frame)
            {
                game->Advance(1.0 / 240.0);
            }
            Result((std::string("continue") + std::to_string(press + 1)).c_str(), consumed ? "1" : "0");
        }
        Result("next_map", game->Info().map);
        Result("next_level", game->Info().state == doom::GameState::Level ? "1" : "0");
        return 0;
    }

    // A source of tic commands instead of the keyboard: the interface that a brain or a bot uses (tasks N1, D1)
    class Runner final : public doom::ITicCmdSource
    {
    public:
        void BuildTicCmd(int player, doom::TicCmd& cmd) override
        {
            if (player == 0)
            {
                ++calls;
                cmd.forwardMove = 40;
                cmd.angleTurn = 256;
            }
        }
        int calls = 0;
    };

    int ScenarioHook(const fs::path& wad)
    {
        auto game = Start(wad, {"-warp", "1", "1", "-skill", "3", "-nomonsters", "-nomusic"});
        if (!game)
        {
            return 1;
        }
        Run(*game, 10);
        Runner runner;
        game->SetTicCmdSource(&runner);
        const Spot before = Where(*game);
        Run(*game, 30);
        const Spot after = Where(*game);
        doom::TicCmd last;
        const bool got = game->GetTicCmd(0, last);
        game->SetTicCmdSource(nullptr);
        Result("calls", runner.calls);
        Result("moved", std::hypot(after.x - before.x, after.y - before.y));
        Result("last_forward", got ? last.forwardMove : -1);
        double delta = after.angle - before.angle;
        while (delta > 180.0) delta -= 360.0;
        while (delta < -180.0) delta += 360.0;
        Result("turned", delta);
        doom::ViewBuffer view;
        Result("view0", game->RenderView(0, view) && view.width == doom::kScreenWidth && view.height == doom::kScreenHeight ? "1" : "0");
        Result("view1", game->RenderView(1, view) ? "1" : "0");
        return 0;
    }

    // Two players in one process (D1): "-deathmatch -players 2"; player 1 is driven by the source of commands, player 0 stands still
    class SecondPlayer final : public doom::ITicCmdSource
    {
    public:
        void BuildTicCmd(int player, doom::TicCmd& cmd) override
        {
            if (player == 1)
            {
                ++calls;
                cmd.forwardMove = 40;
                cmd.angleTurn = 200;
            }
        }
        int calls = 0;
    };

    std::uint64_t HashOf(const doom::ViewBuffer& view)
    {
        std::uint64_t hash = 1469598103934665603ull;
        for (const std::uint32_t pixel : view.pixels)
        {
            hash = (hash ^ pixel) * 1099511628211ull;
        }
        return hash;
    }

    int ScenarioPlayers(const fs::path& wad)
    {
        auto game = Start(wad, {"-warp", "1", "1", "-skill", "3", "-nomonsters", "-nomusic", "-deathmatch", "-players", "2"});
        if (!game)
        {
            return 1;
        }
        Run(*game, 10);
        SecondPlayer second;
        game->SetTicCmdSource(&second);
        const doom::PlayerState p0Before = game->Player(0);
        const doom::PlayerState p1Before = game->Player(1);
        Run(*game, 40);
        const doom::PlayerState p0After = game->Player(0);
        const doom::PlayerState p1After = game->Player(1);
        game->SetTicCmdSource(nullptr);
        Result("in0", p0Before.inGame ? "1" : "0");
        Result("in1", p1Before.inGame ? "1" : "0");
        Result("in2", game->Player(2).inGame ? "1" : "0");
        Result("apart", std::hypot(p0Before.x - p1Before.x, p0Before.y - p1Before.y));
        Result("p0_moved", std::hypot(p0After.x - p0Before.x, p0After.y - p0Before.y));
        Result("p1_moved", std::hypot(p1After.x - p1Before.x, p1After.y - p1Before.y));
        Result("calls", second.calls);
        Result("deathmatch", game->Info().deathmatch);
        doom::ViewBuffer v0;
        doom::ViewBuffer v1;
        const bool got0 = game->RenderView(0, v0);
        const bool got1 = game->RenderView(1, v1);
        Result("view0", got0 ? "1" : "0");
        Result("view1", got1 && v1.width == doom::kScreenWidth && v1.height == doom::kScreenHeight ? "1" : "0");
        Result("views_differ", got0 && got1 && HashOf(v0) != HashOf(v1) ? "1" : "0");
        // the second render pass must not break the picture DOOM draws next
        Run(*game, 2);
        Result("frags0", game->Player(0).frags);
        Result("frags1", game->Player(1).frags);
        Result("alive", game->Info().state == doom::GameState::Level ? "1" : "0");
        return 0;
    }

    // The menu: Esc, then the "Quit game" with the keys of DOOM (up from the first item reaches it) and "y"
    int ScenarioQuit(const fs::path& wad)
    {
        auto game = Start(wad, {"-warp", "1", "1", "-nomusic"});
        if (!game)
        {
            return 1;
        }
        Run(*game, 10);
        auto press = [&](int key)
        {
            game->PostKey(key, true);
            Run(*game, 2);
            game->PostKey(key, false);
            Run(*game, 2);
        };
        press(doom::key::kEscape);
        press(doom::key::kUp);          // wraps to the last item of the main menu: Quit Game
        press(doom::key::kEnter);
        press('y');
        Run(*game, 5);
        Result("finished", game->IsFinished() ? "1" : "0");
        Result("error", game->ErrorMessage().empty() ? "0" : "1");
        return 0;
    }

    // A sound device that only counts: what DOOM gives to the host
    class RecordingAudio final : public doom::IDoomAudioHost
    {
    public:
        int RegisterSfx(const char*, const std::int16_t* samples, int frames, int rate) override
        {
            ++registered;
            sfxFrames += frames;
            sfxRate = rate;
            for (int i = 0; i < frames; ++i)
            {
                sfxPeak = std::max(sfxPeak, std::abs(static_cast<int>(samples[i])));
            }
            return registered;
        }
        bool StartSfx(int sfx, int channel, float volume, float pan) override
        {
            ++started;
            lastSfx = sfx;
            lastChannel = channel;
            maxVolume = std::max(maxVolume, volume);
            minPan = std::min(minPan, pan);
            maxPan = std::max(maxPan, pan);
            playing[channel] = true;
            return true;
        }
        void UpdateSfx(int, float, float) override {}
        void StopSfx(int channel) override { playing[channel] = false; }
        bool IsSfxPlaying(int channel) override { return playing[channel]; }
        void PlayMusic(const std::int16_t* frames, std::size_t count, int rate, bool loop) override
        {
            ++musicStarts;
            musicFrames = count;
            musicRate = rate;
            musicLoop = loop;
            musicPeak = 0;
            for (std::size_t i = 0; i < count * 2; ++i)
            {
                musicPeak = std::max(musicPeak, std::abs(static_cast<int>(frames[i])));
            }
        }
        void StopMusic() override { ++musicStops; }

        int registered = 0;
        long long sfxFrames = 0;
        int sfxRate = 0;
        int sfxPeak = 0;
        int started = 0;
        int lastSfx = 0;
        int lastChannel = 0;
        float maxVolume = 0.0f;
        float minPan = 0.0f;
        float maxPan = 0.0f;
        bool playing[64] = {};
        int musicStarts = 0;
        int musicStops = 0;
        std::size_t musicFrames = 0;
        int musicRate = 0;
        bool musicLoop = false;
        int musicPeak = 0;
    };

    // The sound effects and the music of E1M1: decoded from the WAD, played through the host; the music is rendered by the OPL emulation
    int ScenarioAudio(const fs::path& wad)
    {
        RecordingAudio audio;
        doom::DoomConfig config;
        config.iwad = wad;
        config.dataDirectory = fs::temp_directory_path() / "myengine_doom_core_test";
        config.extraArgs = {"-warp", "1", "1", "-skill", "3", "-nomonsters"};
        config.audio = &audio;
        config.music = GetEnvironmentVariableW(L"FLYDOOM_TEST_NOMUSIC", nullptr, 0) == 0;
        std::string error;
        auto game = doom::DoomGame::Create(config, error);
        if (!game)
        {
            std::printf("  Create failed: %s\n", error.c_str());
            return 1;
        }
        Run(*game, 20);
        // the shots of the pistol (the fire key of DOOM) and the door of the start room
        game->PostKey(doom::key::kFire, true);
        Run(*game, 3);
        game->PostKey(doom::key::kFire, false);
        Run(*game, 20);
        // the music is rendered on a worker thread: the tics go on until it arrives (not more than 30 s of real time)
        const auto begin = GetTickCount64();
        while (audio.musicStarts == 0 && GetTickCount64() - begin < 30000)
        {
            Sleep(20);
            game->Advance(kTic);
        }
        Result("registered", audio.registered);
        Result("sfx_peak", audio.sfxPeak);
        Result("sfx_rate", audio.sfxRate);
        Result("started", audio.started);
        Result("max_volume", audio.maxVolume);
        Result("music_starts", audio.musicStarts);
        Result("music_seconds", audio.musicRate > 0 ? static_cast<double>(audio.musicFrames) / audio.musicRate : 0.0);
        Result("music_rate", audio.musicRate);
        Result("music_loop", audio.musicLoop ? "1" : "0");
        Result("music_peak", audio.musicPeak);
        Result("render_ms", static_cast<double>(GetTickCount64() - begin));
        return 0;
    }

    // A WAD that is not a WAD: DOOM stops with I_Error, the process lives
    int ScenarioBadWad(const fs::path&)
    {
        const fs::path bad = fs::temp_directory_path() / "myengine_doom_bad.wad";
        {
            std::ofstream file(bad, std::ios::binary);
            file << "this is not a WAD file at all, only some text to fill the header";
        }
        doom::DoomConfig config;
        config.iwad = bad;
        config.dataDirectory = fs::temp_directory_path() / "myengine_doom_core_test";
        std::string error;
        auto game = doom::DoomGame::Create(config, error);
        Result("created", game ? "1" : "0");
        Result("has_error", error.empty() ? "0" : "1");
        // DOOM has been started (and has failed): it cannot be started again in this process, and that is said
        std::string again;
        auto second = doom::DoomGame::Create(config, again);
        Result("second", second ? "1" : "0");
        Result("second_error", again.empty() ? "0" : "1");
        std::error_code ec;
        fs::remove(bad, ec);
        return 0;
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // The parent: starts the scenarios

    struct ChildResult
    {
        int exitCode = -1;
        std::map<std::string, std::string> values;
        std::string output;
    };

    ChildResult RunChild(const fs::path& exe, const std::string& scenario, const fs::path& wad)
    {
        ChildResult result;
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &security, 0))
        {
            return result;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

        // The WAD goes to the child by the environment: the path may have letters that the narrow command line cannot hold
        SetEnvironmentVariableW(L"FLYDOOM_WAD", wad.c_str());
        std::wstring command = L"\"" + exe.wstring() + L"\" --scenario " + std::wstring(scenario.begin(), scenario.end());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        {
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return result;
        }
        CloseHandle(writeEnd);

        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        {
            result.output.append(buffer, read);
        }
        CloseHandle(readEnd);
        WaitForSingleObject(process.hProcess, 120000);
        DWORD code = 0;
        GetExitCodeProcess(process.hProcess, &code);
        result.exitCode = static_cast<int>(code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);

        std::size_t pos = 0;
        while (pos < result.output.size())
        {
            std::size_t end = result.output.find('\n', pos);
            if (end == std::string::npos)
            {
                end = result.output.size();
            }
            std::string line = result.output.substr(pos, end - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            {
                line.pop_back();
            }
            if (line.rfind("RESULT ", 0) == 0)
            {
                const std::size_t eq = line.find('=');
                if (eq != std::string::npos)
                {
                    result.values[line.substr(7, eq - 7)] = line.substr(eq + 1);
                }
            }
            pos = end + 1;
        }
        if (g_verbose)
        {
            std::printf("---- %s (exit %d)\n%s\n----\n", scenario.c_str(), result.exitCode, result.output.c_str());
        }
        return result;
    }

    double Num(const ChildResult& child, const char* key)
    {
        const auto it = child.values.find(key);
        return it == child.values.end() ? std::nan("") : std::atof(it->second.c_str());
    }

    std::string Str(const ChildResult& child, const char* key)
    {
        const auto it = child.values.find(key);
        return it == child.values.end() ? std::string() : it->second;
    }

    fs::path FindWad(const std::string& argument)
    {
        std::error_code ec;
        if (!argument.empty())
        {
            return fs::path(argument);
        }
        wchar_t env[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"FLYDOOM_WAD", env, static_cast<DWORD>(sizeof(env) / sizeof(env[0]))) > 0)
        {
            return fs::path(env);
        }
        // The shareware DOOM of the player (the original data), then Freedoom of the repository
        wchar_t local[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(sizeof(local) / sizeof(local[0]))) > 0)
        {
            const fs::path shareware = fs::path(local) / L"FlyDoom" / L"wads" / L"doom1.wad";
            if (fs::is_regular_file(shareware, ec))
            {
                return shareware;
            }
        }
#ifdef DOOM_TEST_WAD_DIR
        for (const char* name : {"doom1.wad", "freedoom1.wad"})
        {
            const fs::path candidate = fs::u8path(DOOM_TEST_WAD_DIR) / name;
            if (fs::is_regular_file(candidate, ec))
            {
                return candidate;
            }
        }
#endif
        return {};
    }

    int RunAll(const fs::path& exe, const fs::path& wad)
    {
        std::error_code ec;
        const std::uintmax_t wadSize = fs::file_size(wad, ec);
        const bool knownWad = wadSize == kFreedoom1Size || wadSize == kShareware1Size;
        const char* pinnedHash = wadSize == kFreedoom1Size ? "6103f206f6aa6f10" : "f330e87839f0d106";
        std::printf("WAD: %s%s\n", wad.string().c_str(),
                    wadSize == kFreedoom1Size ? " (Freedoom 0.13.0 Phase 1)" : wadSize == kShareware1Size ? " (shareware doom1.wad 1.9)" : " (another file: the pinned hash is not checked)");

        // 1. the demo: the same frame in two processes, the demo plays
        const ChildResult demoA = RunChild(exe, "demo", wad);
        const ChildResult demoB = RunChild(exe, "demo", wad);
        CHECK(demoA.exitCode == 0 && demoB.exitCode == 0);
        std::printf("  demo hash %s, tic %s, frames %s, title \"%s\"\n", Str(demoA, "hash").c_str(), Str(demoA, "tic").c_str(), Str(demoA, "frames").c_str(), Str(demoA, "title").c_str());
        CHECK(!Str(demoA, "hash").empty());
        CHECK(Str(demoA, "hash") == Str(demoB, "hash"));
        CHECK(Str(demoA, "demo") == "1");
        CHECK(Num(demoA, "tic") >= 30.0);
        CHECK(Num(demoA, "frames") >= 30.0);
        CHECK(Str(demoA, "hash") != "0000000000000000");
        if (knownWad)
        {
            // 35 tics of DEMO1 of Freedoom 0.13.0: if this changes, the frame, the timing or the game logic changed
            CHECK(Str(demoA, "hash") == pinnedHash);
        }

        // 2. the input: the player is where it was, W moves it, the mouse turns it, Esc opens the menu
        const ChildResult input = RunChild(exe, "input", wad);
        CHECK(input.exitCode == 0);
        CHECK(Str(input, "level") == "1");
        CHECK(Str(input, "menu0") == "0");
        CHECK(Num(input, "still_moved") < 1.0);
        std::printf("  walk %.1f units, mouse turn %.1f degrees\n", Num(input, "walk_distance"), Num(input, "turn_degrees"));
        CHECK(Num(input, "walk_distance") > 20.0);
        CHECK(std::fabs(Num(input, "turn_degrees")) > 5.0);
        CHECK(Str(input, "menu1") == "1");
        CHECK(Str(input, "pixels") == "1");

        // 2b. the mouse: the movement of all the events of a tic adds up (the host posts at the frame rate, several events per tic)
        const ChildResult mouse = RunChild(exe, "mouse", wad);
        CHECK(mouse.exitCode == 0);
        std::printf("  mouse: 1 event of 40 -> %.2f degrees, 8 events of 5 -> %.2f, 100 events of 1 -> %.2f (100 in one: %.2f)\n", Num(mouse, "one_event"), Num(mouse, "eight_events"),
                    Num(mouse, "hundred_events"), Num(mouse, "hundred_one"));
        CHECK(std::fabs(Num(mouse, "one_event")) > 1.0);
        CHECK(std::fabs(Num(mouse, "eight_events") - Num(mouse, "one_event")) < 0.01);
        CHECK(std::fabs(Num(mouse, "hundred_events") - Num(mouse, "hundred_one")) < 0.01);
        CHECK(std::fabs(Num(mouse, "back_and_forth")) < 0.01);
        CHECK(std::fabs(Num(mouse, "still")) < 0.01);

        // 2c. native intermission contract: real level exit, sub-tic cadence, three direct native edges, E1M2.
        // This is not a DoomGameFlow/rlui routing regression test; the frontend seam is GUI-only today.
        const ChildResult intermission = RunChild(exe, "intermission", wad);
        CHECK(intermission.exitCode == 0);
        CHECK(Str(intermission, "entered") == "1");
        CHECK(Str(intermission, "stats") == "1");
        CHECK(Str(intermission, "continue1") == "1" && Str(intermission, "continue2") == "1" && Str(intermission, "continue3") == "1");
        CHECK(Num(intermission, "next_map") == 2.0 && Str(intermission, "next_level") == "1");

        // 3. the source of tic commands (the point of extension for a brain)
        const ChildResult hook = RunChild(exe, "hook", wad);
        CHECK(hook.exitCode == 0);
        std::printf("  hook: %.0f calls, moved %.1f, turned %.1f degrees\n", Num(hook, "calls"), Num(hook, "moved"), Num(hook, "turned"));
        CHECK(Num(hook, "calls") >= 25.0);
        CHECK(Num(hook, "moved") > 20.0);
        CHECK(Num(hook, "last_forward") == 40.0);
        CHECK(std::fabs(Num(hook, "turned")) > 5.0);
        CHECK(Str(hook, "view0") == "1");
        CHECK(Str(hook, "view1") == "0");

        // 3b. two players in one process: both in the game apart, the second one driven by a source, its own view through its eyes
        const ChildResult players = RunChild(exe, "players", wad);
        CHECK(players.exitCode == 0);
        std::printf("  players: apart %.0f, p0 moved %.1f, p1 moved %.1f, views differ %s\n", Num(players, "apart"), Num(players, "p0_moved"), Num(players, "p1_moved"), Str(players, "views_differ").c_str());
        CHECK(Str(players, "in0") == "1");
        CHECK(Str(players, "in1") == "1");
        CHECK(Str(players, "in2") == "0");
        CHECK(Num(players, "deathmatch") >= 1.0);
        CHECK(Num(players, "apart") > 30.0);
        CHECK(Num(players, "p0_moved") < 1.0);
        CHECK(Num(players, "p1_moved") > 20.0);
        CHECK(Num(players, "calls") >= 35.0);
        CHECK(Str(players, "view0") == "1");
        CHECK(Str(players, "view1") == "1");
        CHECK(Str(players, "views_differ") == "1");
        CHECK(Str(players, "alive") == "1");

        // 4. Quit game from the menu ends the game without ending the process
        const ChildResult quit = RunChild(exe, "quit", wad);
        CHECK(quit.exitCode == 0);
        CHECK(Str(quit, "finished") == "1");
        CHECK(Str(quit, "error") == "0");

        // 5. the sound: the effects are decoded and played, the music is rendered by the OPL emulation
        const ChildResult audio = RunChild(exe, "audio", wad);
        CHECK(audio.exitCode == 0);
        std::printf("  audio: %.0f sounds, %.0f started, music %.1f s at %.0f Hz (rendered in %.0f ms), peak %.0f\n", Num(audio, "registered"), Num(audio, "started"),
                    Num(audio, "music_seconds"), Num(audio, "music_rate"), Num(audio, "render_ms"), Num(audio, "music_peak"));
        CHECK(Num(audio, "registered") >= 1.0);
        CHECK(Num(audio, "sfx_peak") > 1000.0);
        CHECK(Num(audio, "sfx_rate") >= 8000.0);
        CHECK(Num(audio, "started") >= 1.0);
        CHECK(Num(audio, "max_volume") > 0.0);
        CHECK(Num(audio, "music_starts") >= 1.0);
        CHECK(Num(audio, "music_seconds") > 5.0);
        CHECK(Num(audio, "music_rate") == 49716.0);
        CHECK(Str(audio, "music_loop") == "1");
        CHECK(Num(audio, "music_peak") > 500.0);

        // 6. a file that is not a WAD: an error text, no exit() of the process, and no second start
        const ChildResult bad = RunChild(exe, "badwad", wad);
        CHECK(bad.exitCode == 0);
        CHECK(Str(bad, "created") == "0");
        CHECK(Str(bad, "has_error") == "1");
        CHECK(Str(bad, "second") == "0");
        CHECK(Str(bad, "second_error") == "1");

        std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "doom_core OK" : "doom_core FAILED", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
}

int main(int argc, char** argv)
{
    std::string scenario;
    std::string wadArgument;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc)
        {
            scenario = argv[++i];
        }
        else if (std::strcmp(argv[i], "--wad") == 0 && i + 1 < argc)
        {
            wadArgument = argv[++i];
        }
        else if (std::strcmp(argv[i], "--verbose") == 0)
        {
            g_verbose = true;
        }
    }

    if (!scenario.empty())
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);   // what DOOM printed before a crash is not lost in the pipe
    }

    const fs::path wad = FindWad(wadArgument);
    std::error_code ec;
    if (wad.empty() || !fs::is_regular_file(wad, ec))
    {
        std::printf("doom_core SKIPPED: no WAD (run tools/doom/fetch_freedoom.ps1, or set FLYDOOM_WAD / --wad)\n");
        return 77;
    }

    if (!scenario.empty())
    {
        if (scenario == "demo") return ScenarioDemo(wad);
        if (scenario == "input") return ScenarioInput(wad);
        if (scenario == "intermission") return ScenarioIntermission(wad);
        if (scenario == "mouse") return ScenarioMouse(wad);
        if (scenario == "hook") return ScenarioHook(wad);
        if (scenario == "players") return ScenarioPlayers(wad);
        if (scenario == "quit") return ScenarioQuit(wad);
        if (scenario == "badwad") return ScenarioBadWad(wad);
        if (scenario == "audio") return ScenarioAudio(wad);
        std::printf("unknown scenario %s\n", scenario.c_str());
        return 2;
    }

    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    return RunAll(fs::path(path), wad);
}
