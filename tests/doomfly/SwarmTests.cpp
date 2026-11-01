// SwarmTests.cpp
// Tests of the swarm of enemy flies (game/doomfly FlySwarm, game/doom DoomSwarm, docs/game/flydoom-fly.md, "Swarm"):
//   sprites  the pictures of the fly: two frames of flight that differ, the death, the patch format of DOOM (needs nothing)
//   brain    the circuit of the swarm (assets/flybrain/swarm_circuit.fbrain): what a fly does at looming of 0 .. 1 on the left and on the right
//   game     a game of E1M1 with no monsters of DOOM and a swarm: the waves, the flies come, bite, are shot dead, the next wave comes
// DOOM keeps its state in globals and starts once per process, so every scenario runs in a child process of this program (`--scenario <name>`) and prints
// "RESULT key=value" lines; `--verbose` prints the whole output of the children. Without the circuit the scenarios of the brain and the game are skipped, without
// a WAD the game is skipped (the sprites always run). SWARM_SHEET=<file.ppm> makes the sprites scenario write a sheet of the pictures to look at.

#include <doomfly/FlySwarm.h>

#include <doom/DoomGame.h>
#include <doom/DoomSwarm.h>
#include <flybrain/FlySwarmBrain.h>
#include <myengine/jobs/JobSystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

namespace
{
    namespace fs = std::filesystem;

    int g_failures = 0;
    int g_checks = 0;
    bool g_verbose = false;

    void Check(const bool ok, const char* what, const char* file, const int line)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("  FAIL %s (%s:%d)\n", what, file, line);
        }
    }
#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)

    void Result(const char* key, const std::string& value)
    {
        std::printf("RESULT %s=%s\n", key, value.c_str());
        std::fflush(stdout);
    }

    void Result(const char* key, const double value)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%.4f", value);
        Result(key, std::string(text));
    }

    fs::path SourceDirectory()
    {
#ifdef FLY_SOURCE_DIR
        return fs::u8path(FLY_SOURCE_DIR);
#else
        return fs::current_path();
#endif
    }

    fs::path CircuitPath()
    {
        wchar_t env[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"FLYDOOM_SWARM_CIRCUIT", env, static_cast<DWORD>(sizeof(env) / sizeof(env[0]))) > 0)
        {
            return fs::path(env);
        }
        return SourceDirectory() / "assets" / "flybrain" / "swarm_circuit.fbrain";
    }

    fs::path FindWad()
    {
        std::error_code ec;
        wchar_t env[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"FLYDOOM_WAD", env, static_cast<DWORD>(sizeof(env) / sizeof(env[0]))) > 0)
        {
            return fs::path(env);
        }
        wchar_t local[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(sizeof(local) / sizeof(local[0]))) > 0)
        {
            const fs::path shareware = fs::path(local) / L"FlyDoom" / L"wads" / L"doom1.wad";
            if (fs::is_regular_file(shareware, ec))
            {
                return shareware;
            }
        }
        for (const char* name : {"doom1.wad", "freedoom1.wad"})
        {
            const fs::path candidate = SourceDirectory() / "assets" / "doom" / name;
            if (fs::is_regular_file(candidate, ec))
            {
                return candidate;
            }
        }
        return {};
    }

    // a WAD of assets/doom by its name (doom1.wad, freedoom1.wad), the shareware one also from %LOCALAPPDATA%/FlyDoom/wads
    fs::path FindNamedWad(const std::string& name)
    {
        std::error_code ec;
        const fs::path candidate = SourceDirectory() / "assets" / "doom" / name;
        if (fs::is_regular_file(candidate, ec))
        {
            return candidate;
        }
        wchar_t local[MAX_PATH * 2] = {};
        if (name == "doom1.wad" && GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(sizeof(local) / sizeof(local[0]))) > 0)
        {
            const fs::path shareware = fs::path(local) / L"FlyDoom" / L"wads" / L"doom1.wad";
            if (fs::is_regular_file(shareware, ec))
            {
                return shareware;
            }
        }
        return {};
    }

    // ---------------------------------------------------------------- sprites

    int ScenarioSprites()
    {
        namespace swarm = doom::swarm;
        std::vector<std::vector<unsigned char>> frames;
        std::vector<int> opaque;
        for (int frame = 0; frame < swarm::kSpriteFrames; ++frame)
        {
            std::vector<unsigned char> rgba(static_cast<std::size_t>(swarm::kSpriteWidth) * swarm::kSpriteHeight * 4);
            swarm::DrawSprite(frame, rgba.data());
            int count = 0;
            for (std::size_t i = 3; i < rgba.size(); i += 4)
            {
                count += rgba[i] != 0 ? 1 : 0;
            }
            opaque.push_back(count);
            frames.push_back(std::move(rgba));
        }
        for (int frame = 0; frame < swarm::kSpriteFrames; ++frame)
        {
            Result(("opaque_" + std::to_string(frame)).c_str(), opaque[static_cast<std::size_t>(frame)]);
        }
        std::size_t differing = 0;
        for (std::size_t i = 0; i < frames[0].size(); ++i)
        {
            differing += frames[0][i] != frames[1][i] ? 1 : 0;
        }
        Result("wings_differ", static_cast<double>(differing));
        // the last frame lies on the floor: its pixels are in the lower part of the picture
        int highest = swarm::kSpriteHeight;
        for (int y = 0; y < swarm::kSpriteHeight; ++y)
        {
            for (int x = 0; x < swarm::kSpriteWidth; ++x)
            {
                if (frames[10][static_cast<std::size_t>((y * swarm::kSpriteWidth + x) * 4 + 3)] != 0)
                {
                    highest = std::min(highest, y);
                }
            }
        }
        Result("splat_top_row", highest);

        wchar_t sheetPath[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"SWARM_SHEET", sheetPath, static_cast<DWORD>(sizeof(sheetPath) / sizeof(sheetPath[0]))) > 0)
        {
            const fs::path sheet(sheetPath);
            // a sheet of the frames, 4 times bigger, on a grey ground (PPM)
            const int scale = 4;
            const int columns = 6;
            const int rows = 2;
            const int width = columns * swarm::kSpriteWidth * scale;
            const int height = rows * swarm::kSpriteHeight * scale;
            std::vector<unsigned char> image(static_cast<std::size_t>(width) * height * 3, 90);
            for (int frame = 0; frame < swarm::kSpriteFrames; ++frame)
            {
                const int ox = (frame % columns) * swarm::kSpriteWidth * scale;
                const int oy = (frame / columns) * swarm::kSpriteHeight * scale;
                for (int y = 0; y < swarm::kSpriteHeight * scale; ++y)
                {
                    for (int x = 0; x < swarm::kSpriteWidth * scale; ++x)
                    {
                        const unsigned char* pixel = &frames[static_cast<std::size_t>(frame)][static_cast<std::size_t>(((y / scale) * swarm::kSpriteWidth + x / scale) * 4)];
                        if (pixel[3] != 0)
                        {
                            unsigned char* out = &image[static_cast<std::size_t>(((oy + y) * width + ox + x) * 3)];
                            out[0] = pixel[0];
                            out[1] = pixel[1];
                            out[2] = pixel[2];
                        }
                    }
                }
            }
            std::ofstream file(sheet, std::ios::binary);
            file << "P6\n" << width << " " << height << "\n255\n";
            file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        }
        return 0;
    }

    // ---------------------------------------------------------------- brain

    std::shared_ptr<const flybrain::Connectome> LoadCircuit()
    {
        std::string error;
        auto circuit = flybrain::Connectome::Load(CircuitPath().u8string(), &error);
        if (!circuit)
        {
            std::printf("  the circuit: %s\n", error.c_str());
        }
        return circuit;
    }

    // Looming of a level on one side for 300 ms (the last 100 ms are read), 8 flies at once with the levels 0, 0.1, 0.25, 0.5, 1 on the left and on the right
    int ScenarioBrain()
    {
        myengine::jobs::Initialize();
        auto circuit = LoadCircuit();
        if (!circuit)
        {
            return 1;
        }
        flybrain::SwarmOptions options;
        options.backend = flybrain::BackendChoice::Cpu;
        std::string error;
        auto brain = flybrain::FlySwarmBrain::Create(circuit, 12, options, &error);
        if (!brain)
        {
            std::printf("  the swarm brain: %s\n", error.c_str());
            return 1;
        }
        const float levels[6] = {0.0f, 0.1f, 0.25f, 0.5f, 0.8f, 1.0f};
        // flies 0..5: the player on the left at these levels, 6..11: on the right
        for (int fly = 0; fly < 12; ++fly)
        {
            flybrain::SwarmInput input;
            input.brightness = 0.3f;
            (fly < 6 ? input.loomLeft : input.loomRight) = levels[fly % 6];
            brain->SetInput(static_cast<std::uint32_t>(fly), input);
        }
        std::uint32_t before[12] = {};
        const auto begin = std::chrono::steady_clock::now();
        for (int tic = 0; tic < 11; ++tic)   // 11 tics = 314 ms
        {
            if (tic == 1)
            {
                for (int fly = 0; fly < 12; ++fly)
                {
                    before[fly] = brain->Output(static_cast<std::uint32_t>(fly)).escapeSpikes;
                }
            }
            brain->AdvanceMs(1000.0 / 35.0);
        }
        brain->Synchronize();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        for (int fly = 0; fly < 12; ++fly)
        {
            const flybrain::SwarmOutput& out = brain->Output(static_cast<std::uint32_t>(fly));
            char key[48];
            std::snprintf(key, sizeof(key), "%s_%02d_turn", fly < 6 ? "left" : "right", static_cast<int>(levels[fly % 6] * 100.0f));
            Result(key, out.turn);
            std::snprintf(key, sizeof(key), "%s_%02d_escape_spikes", fly < 6 ? "left" : "right", static_cast<int>(levels[fly % 6] * 100.0f));
            Result(key, static_cast<double>(out.escapeSpikes - before[fly]));
        }
        Result("cpu_ms_per_tic", seconds * 1000.0 / 11.0);
        myengine::jobs::Shutdown();
        return 0;
    }

    // ---------------------------------------------------------------- game

    // The human of the test: turns to the nearest fly and fires when it is on the line. Stands still when `stand` is set.
    class TestPlayer final : public doom::ITicCmdSource
    {
    public:
        doomfly::FlySwarm* swarm = nullptr;
        doom::DoomGame* game = nullptr;
        bool shoot = false;
        int fired = 0;

        void BuildTicCmd(const int player, doom::TicCmd& cmd) override
        {
            if (player != 0)
            {
                return;
            }
            cmd = doom::TicCmd{};
            if (!shoot || swarm == nullptr || game == nullptr)
            {
                return;
            }
            const doom::PlayerState me = game->Player(0);
            double best = 1.0e9;
            double bestAngle = 0.0;
            bool found = false;
            for (const doomfly::FlySwarm::FlyView& fly : swarm->Flies())
            {
                if (!fly.alive || !fly.sees)
                {
                    continue;
                }
                const double dx = fly.x - me.x;
                const double dy = fly.y - me.y;
                const double distance = std::hypot(dx, dy);
                if (distance < best)
                {
                    best = distance;
                    bestAngle = std::atan2(dy, dx) * 180.0 / 3.14159265358979;
                    found = true;
                }
            }
            if (!found)
            {
                return;
            }
            double difference = bestAngle - me.angleDegrees;
            while (difference > 180.0) difference -= 360.0;
            while (difference < -180.0) difference += 360.0;
            const double turn = std::clamp(difference, -8.0, 8.0);
            cmd.angleTurn = static_cast<std::int16_t>(turn * 65536.0 / 360.0);
            if (std::fabs(difference) < 5.0)
            {
                cmd.buttons |= doom::kButtonAttack;
                ++fired;
            }
        }
    };

    // N6: DOOM starts with the swarm on the WAD (on Freedoom the renaming of the sprites of the Lost Soul once took the wall patch SKULFACE and R_InitTextures
    // stopped), and the first wave comes
    int ScenarioStart(const fs::path& wad)
    {
        myengine::jobs::Initialize();
        auto circuit = LoadCircuit();
        if (!circuit)
        {
            return 1;
        }
        doom::DoomConfig config;
        config.iwad = wad;
        config.dataDirectory = fs::temp_directory_path() / "myengine_doom_swarm_test";
        config.extraArgs = {"-warp", "1", "1", "-skill", "3", "-nomusic", "-nomonsters", doom::swarm::kStartArgument};
        std::string error;
        auto game = doom::DoomGame::Create(config, error);
        Result("doom_started", game ? 1.0 : 0.0);
        if (!game)
        {
            std::printf("  DOOM: %s\n", error.c_str());
            myengine::jobs::Shutdown();
            return 0;
        }
        doomfly::SwarmConfig swarmConfig;
        swarmConfig.circuit = circuit;
        swarmConfig.seed = 7;
        auto swarm = doomfly::FlySwarm::Create(swarmConfig, error);
        if (!swarm)
        {
            std::printf("  the swarm: %s\n", error.c_str());
            return 1;
        }
        TestPlayer human;
        game->SetTicCmdSource(&human);
        int firstWaveTic = -1;
        int alive = 0;
        for (int tic = 0; tic < 300 && firstWaveTic < 0; ++tic)
        {
            swarm->StepTic(*game);   // steps the game too
            if (swarm->Stats().wave == 1 && swarm->Stats().flies > 0)
            {
                firstWaveTic = tic;
                for (const auto& fly : swarm->Flies())
                {
                    alive += fly.alive ? 1 : 0;
                }
            }
        }
        Result("first_wave_tic", firstWaveTic);
        Result("wave1_size", swarm->Stats().waveSize);
        Result("wave1_alive", alive);
        game->SetTicCmdSource(nullptr);
        swarm.reset();
        myengine::jobs::Shutdown();
        game.reset();
        return 0;
    }

    // `flies` and `backend` for a measurement of a big swarm (--flies 32 --wave 7 --backend gpu); the checks of the waves are for the defaults
    int ScenarioGame(const fs::path& wad, const int maxTics, const int flies, const int startWave, const flybrain::BackendChoice backend)
    {
        myengine::jobs::Initialize();
        auto circuit = LoadCircuit();
        if (!circuit)
        {
            return 1;
        }
        doom::DoomConfig config;
        config.iwad = wad;
        config.dataDirectory = fs::temp_directory_path() / "myengine_doom_swarm_test";
        config.extraArgs = {"-warp", "1", "1", "-skill", "3", "-nomusic", "-nomonsters", doom::swarm::kStartArgument};
        std::string error;
        auto game = doom::DoomGame::Create(config, error);
        if (!game)
        {
            std::printf("  DOOM: %s\n", error.c_str());
            return 1;
        }
        for (int i = 0; i < 10; ++i)
        {
            game->StepTic();
        }
        doomfly::SwarmConfig swarmConfig;
        swarmConfig.circuit = circuit;
        swarmConfig.maxFlies = flies;
        swarmConfig.startWave = startWave;
        swarmConfig.backend = backend;
        swarmConfig.seed = 7;
        auto swarm = doomfly::FlySwarm::Create(swarmConfig, error);
        if (!swarm)
        {
            std::printf("  the swarm: %s\n", error.c_str());
            return 1;
        }
        TestPlayer human;
        human.swarm = swarm.get();
        human.game = game.get();
        game->SetTicCmdSource(&human);

        // phase 1: the human stands and does not shoot: the first wave comes and bites
        const doom::PlayerState start = game->Player(0);
        int firstWaveTic = -1;
        int tic = 0;
        double spawnDistance = 0.0;
        double distanceAfter = 0.0;
        for (; tic < 500; ++tic)
        {
            swarm->StepTic(*game);
            const doomfly::SwarmStats& stats = swarm->Stats();
            if (firstWaveTic < 0 && stats.wave == 1 && stats.flies > 0)
            {
                firstWaveTic = tic;
                double sum = 0.0;
                int n = 0;
                for (const auto& fly : swarm->Flies())
                {
                    sum += fly.distance;
                    ++n;
                }
                spawnDistance = n > 0 ? sum / n : 0.0;
            }
            if (firstWaveTic >= 0 && g_verbose && (tic - firstWaveTic) % 20 == 0)
            {
                const doom::PlayerState me = game->Player(0);
                std::printf("  tic %d: player %.0f,%.0f z %.0f\n", tic, me.x, me.y, me.z);
                for (const auto& fly : swarm->Flies())
                {
                    std::printf("    fly %d %s at %.0f,%.0f z %.0f heading %.0f distance %.0f loom %.2f/%.2f turn %+.2f\n", fly.slot, fly.alive ? "alive" : "dead", fly.x, fly.y, fly.z,
                                fly.heading, fly.distance, fly.input.loomLeft, fly.input.loomRight, fly.output.turn);
                }
            }
            if (firstWaveTic >= 0 && tic == firstWaveTic + 160)
            {
                double sum = 0.0;
                int n = 0;
                for (const auto& fly : swarm->Flies())
                {
                    if (fly.alive)
                    {
                        sum += fly.distance;
                        ++n;
                    }
                }
                distanceAfter = n > 0 ? sum / n : 0.0;
                break;
            }
        }
        const doomfly::SwarmStats afterStand = swarm->Stats();
        Result("first_wave_tic", firstWaveTic);
        Result("wave1_size", afterStand.waveSize);
        Result("spawn_distance", spawnDistance);
        Result("distance_after_160", distanceAfter);
        Result("bites_standing", afterStand.bites);
        Result("health_after_standing", game->Player(0).health);
        Result("start_x", start.x);
        Result("brain_ms_per_tic", afterStand.brainMsPerTic);

        // phase 2: the human turns to the nearest fly and fires: they die, the next wave comes
        human.shoot = true;
        int killedAt = -1;
        int wave2Tic = -1;
        int wave2Size = 0;
        int wave3Tic = -1;
        int wave3Size = 0;
        int minHealth = game->Player(0).health;
        for (; tic < maxTics; ++tic)
        {
            swarm->StepTic(*game);
            const doomfly::SwarmStats& stats = swarm->Stats();
            minHealth = std::min(minHealth, game->Player(0).health);
            if (killedAt < 0 && stats.killed >= stats.waveSize && stats.wave == 1)
            {
                killedAt = tic;
            }
            if (stats.wave >= 2 && wave2Tic < 0 && stats.flies > 0)
            {
                wave2Tic = tic;
                wave2Size = stats.waveSize;
            }
            if (stats.wave >= 3 && stats.flies > 0)
            {
                wave3Tic = tic;
                wave3Size = stats.waveSize;
                break;
            }
        }
        const doomfly::SwarmStats end = swarm->Stats();
        Result("wave1_cleared_tic", killedAt);
        Result("wave2_tic", wave2Tic);
        Result("wave2_size", wave2Size);
        Result("wave3_tic", wave3Tic);
        Result("wave3_size", wave3Size);
        Result("killed", end.killed);
        Result("kills_of_doom", game->Player(0).kills);
        Result("leaps", end.leaps);
        Result("bites", end.bites);
        Result("bite_damage", end.biteDamage);
        Result("shots", human.fired);
        Result("min_health", minHealth);
        Result("tics", tic);
        Result("brain_ms_per_tic_end", end.brainMsPerTic);
        game->SetTicCmdSource(nullptr);
        swarm.reset();

        // phase 3: a fly that is hurt and lives (a fist does 2..20, the flies have 5) must not wake up the AI of a Lost Soul: it would charge at the player and play a
        // sound that the shareware WAD does not have (the game stops with "W_GetNumForName: dssklatk not found"). Flies of our own here, driven by hand.
        const doom::PlayerState me = game->Player(0);
        double floorZ = 0.0;
        double ceilingZ = 0.0;
        int handle = -1;
        for (int attempt = 0; attempt < 200 && handle < 0; ++attempt)
        {
            const double angle = attempt * 0.7;
            const double x = me.x + std::cos(angle) * (60.0 + attempt);
            const double y = me.y + std::sin(angle) * (60.0 + attempt);
            if (doom::swarm::SpotOk(x, y, floorZ, ceilingZ))
            {
                handle = doom::swarm::Spawn(x, y, std::min(floorZ + 30.0, ceilingZ - 60.0), 0.0, 100);
            }
        }
        Result("hurt_fly_spawned", handle >= 0 ? 1.0 : 0.0);
        if (handle >= 0)
        {
            doom::swarm::Hurt(handle, 3);
            doom::swarm::Hurt(handle, 3);
            double x0 = 0.0, y0 = 0.0, driven = 0.0;
            doom::swarm::Fly fly;
            for (int i = 0; i < 70; ++i)
            {
                if (i == 10 && doom::swarm::Get(handle, fly))
                {
                    x0 = fly.x;
                    y0 = fly.y;
                }
                doom::swarm::Drive(handle, 0.0, 2.0, 0.0, 0.0, i / 2 % 2);   // east, 2 units a tic
                game->StepTic();
            }
            if (doom::swarm::Get(handle, fly))
            {
                driven = std::hypot(fly.x - x0, fly.y - y0);
                Result("hurt_fly_health", fly.health);
                Result("hurt_fly_alive", fly.alive ? 1.0 : 0.0);
            }
            Result("hurt_fly_driven", driven);   // 60 tics at 2 units: about 120 (the walls may stop it); a charge of a Lost Soul would be 20 units a tic
        }
        myengine::jobs::Shutdown();
        game.reset();
        return 0;
    }

    // ---------------------------------------------------------------- the parent

    struct ChildResult
    {
        int exitCode = -1;
        std::map<std::string, std::string> values;
        std::string output;
        double seconds = 0.0;
    };

    ChildResult RunChild(const fs::path& exe, const std::string& arguments)
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
        std::wstring command = L"\"" + exe.wstring() + L"\" " + std::wstring(arguments.begin(), arguments.end());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        const auto begin = std::chrono::steady_clock::now();
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
        WaitForSingleObject(process.hProcess, 900000);
        DWORD code = 0;
        GetExitCodeProcess(process.hProcess, &code);
        result.exitCode = static_cast<int>(code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
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
        if (g_verbose || result.exitCode != 0)
        {
            std::printf("---- %s (exit %d, %.1f s)\n%s\n----\n", arguments.c_str(), result.exitCode, result.seconds, result.output.c_str());
        }
        return result;
    }

    double Num(const ChildResult& child, const char* key)
    {
        const auto it = child.values.find(key);
        return it == child.values.end() ? std::nan("") : std::atof(it->second.c_str());
    }

    int RunAll(const fs::path& exe)
    {
        std::error_code ec;
        const bool haveCircuit = fs::is_regular_file(CircuitPath(), ec);
        const bool haveWad = !FindWad().empty();

        const ChildResult sprites = RunChild(exe, "--scenario sprites");
        CHECK(sprites.exitCode == 0);
        std::printf("sprites: wings differ in %.0f bytes, opaque pixels of the frames %.0f %.0f ... %.0f\n", Num(sprites, "wings_differ"), Num(sprites, "opaque_0"),
                    Num(sprites, "opaque_1"), Num(sprites, "opaque_10"));
        for (int frame = 0; frame < 11; ++frame)
        {
            CHECK(Num(sprites, ("opaque_" + std::to_string(frame)).c_str()) > 120.0);
        }
        CHECK(Num(sprites, "wings_differ") > 100.0);
        CHECK(Num(sprites, "splat_top_row") >= 30.0);   // lies on the floor

        if (!haveCircuit)
        {
            std::printf("doom_fly_swarm: no circuit of the swarm (%s): the brain and the game are skipped\n", CircuitPath().string().c_str());
        }
        else
        {
            const ChildResult brain = RunChild(exe, "--scenario brain");
            CHECK(brain.exitCode == 0);
            std::printf("brain: looming on the left 0/25/100 %%: turn %+.2f %+.2f %+.2f, escape spikes %.0f %.0f %.0f; on the right: turn %+.2f %+.2f %+.2f, escape spikes %.0f %.0f %.0f\n",
                        Num(brain, "left_00_turn"), Num(brain, "left_25_turn"), Num(brain, "left_100_turn"), Num(brain, "left_00_escape_spikes"),
                        Num(brain, "left_25_escape_spikes"), Num(brain, "left_100_escape_spikes"), Num(brain, "right_00_turn"), Num(brain, "right_25_turn"),
                        Num(brain, "right_100_turn"), Num(brain, "right_00_escape_spikes"), Num(brain, "right_25_escape_spikes"), Num(brain, "right_100_escape_spikes"));
            CHECK(Num(brain, "left_00_escape_spikes") == 0.0 && Num(brain, "right_00_escape_spikes") == 0.0);   // no looming: nothing happens
            CHECK(Num(brain, "left_100_escape_spikes") > 20.0 && Num(brain, "right_100_escape_spikes") > 20.0);   // looming: the giant fibre jumps
            CHECK(Num(brain, "left_100_turn") > 0.2);   // looming on the left turns the fly to the right, away from it
            CHECK(Num(brain, "left_100_escape_spikes") >= Num(brain, "left_25_escape_spikes"));
        }
        for (const char* name : {"doom1.wad", "freedoom1.wad"})
        {
            if (!haveCircuit || FindNamedWad(name).empty())
            {
                std::printf("doom_fly_swarm: the start on %s is skipped (%s)\n", name, !haveCircuit ? "no circuit" : "no such WAD in assets/doom: tools/doom/fetch_freedoom.ps1");
                continue;
            }
            const ChildResult start = RunChild(exe, std::string("--scenario start --wad-name ") + name);
            CHECK(start.exitCode == 0);
            std::printf("start on %s: DOOM started %.0f, wave 1 of %.0f flies at tic %.0f, %.0f alive\n", name, Num(start, "doom_started"), Num(start, "wave1_size"),
                        Num(start, "first_wave_tic"), Num(start, "wave1_alive"));
            CHECK(Num(start, "doom_started") == 1.0);
            CHECK(Num(start, "first_wave_tic") >= 0.0);
            CHECK(Num(start, "wave1_size") == 8.0 && Num(start, "wave1_alive") == 8.0);
        }
        if (!haveCircuit || !haveWad)
        {
            std::printf("doom_fly_swarm: the game is skipped (%s)\n", !haveCircuit ? "no circuit" : "no WAD: tools/doom/fetch_freedoom.ps1");
        }
        else
        {
            const ChildResult game = RunChild(exe, "--scenario game --tics 3500");
            CHECK(game.exitCode == 0);
            std::printf("game: wave 1 of %.0f flies at tic %.0f from %.0f units, %.0f units 160 tics later; standing: %.0f bites, health %.0f; then shooting: wave 1 cleared at tic %.0f, wave 2 "
                        "(%.0f flies) at tic %.0f, wave 3 (%.0f flies) at tic %.0f; killed %.0f (DOOM counts %.0f), leaps %.0f, shots %.0f, lowest health %.0f; brain %.1f ms a tic\n",
                        Num(game, "wave1_size"), Num(game, "first_wave_tic"), Num(game, "spawn_distance"), Num(game, "distance_after_160"), Num(game, "bites_standing"),
                        Num(game, "health_after_standing"), Num(game, "wave1_cleared_tic"), Num(game, "wave2_size"), Num(game, "wave2_tic"), Num(game, "wave3_size"), Num(game, "wave3_tic"), Num(game, "killed"),
                        Num(game, "kills_of_doom"), Num(game, "leaps"), Num(game, "shots"), Num(game, "min_health"), Num(game, "brain_ms_per_tic_end"));
            CHECK(Num(game, "first_wave_tic") > 20.0 && Num(game, "first_wave_tic") < 80.0);
            CHECK(Num(game, "wave1_size") == 8.0);
            CHECK(Num(game, "distance_after_160") < Num(game, "spawn_distance") * 0.5);   // they fly to the player
            CHECK(Num(game, "bites_standing") > 0.0);
            CHECK(Num(game, "health_after_standing") < 100.0);
            CHECK(Num(game, "wave1_cleared_tic") > 0.0);                                  // they die of the shots
            CHECK(Num(game, "kills_of_doom") >= 8.0);                                     // DOOM itself counts the kills of the player
            CHECK(Num(game, "wave2_tic") > Num(game, "wave1_cleared_tic"));               // the next wave comes after the clear field
            CHECK(Num(game, "wave2_size") == 12.0);
            CHECK(Num(game, "wave3_tic") > Num(game, "wave2_tic") && Num(game, "wave3_size") == 16.0);   // the waves grow to the size of the swarm and the slots of the corpses are used again
            CHECK(Num(game, "leaps") > 0.0);                                              // the giant fibre jumps when the weapon points at a fly
            CHECK(Num(game, "hurt_fly_spawned") == 1.0 && Num(game, "hurt_fly_alive") == 1.0 && Num(game, "hurt_fly_health") == 94.0);   // hurt, alive
            CHECK(Num(game, "hurt_fly_driven") < 200.0);                                  // and still driven by the host: no charge of a Lost Soul
        }
        std::printf("doom_fly_swarm: %d checks, %d failed\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
}

int main(const int argc, char** argv)
{
    std::string scenario;
    std::string wadName;
    int tics = 3500;
    int flies = 16;
    int startWave = 1;
    flybrain::BackendChoice backend = flybrain::BackendChoice::Cpu;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc)
        {
            scenario = argv[++i];
        }
        else if (std::strcmp(argv[i], "--tics") == 0 && i + 1 < argc)
        {
            tics = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--flies") == 0 && i + 1 < argc)
        {
            flies = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--wave") == 0 && i + 1 < argc)
        {
            startWave = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
        {
            flybrain::ParseBackendChoice(argv[++i], backend);
        }
        else if (std::strcmp(argv[i], "--wad-name") == 0 && i + 1 < argc)
        {
            wadName = argv[++i];
        }
        else if (std::strcmp(argv[i], "--verbose") == 0)
        {
            g_verbose = true;
        }
    }
    if (!scenario.empty())
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        if (scenario == "sprites") return ScenarioSprites();
        if (scenario == "brain") return ScenarioBrain();
        if (scenario == "start")
        {
            const fs::path wad = FindNamedWad(wadName);
            return wad.empty() ? 77 : ScenarioStart(wad);
        }
        if (scenario == "game")
        {
            const fs::path wad = FindWad();
            return wad.empty() ? 77 : ScenarioGame(wad, tics, flies, startWave, backend);
        }
        std::printf("unknown scenario %s\n", scenario.c_str());
        return 2;
    }
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    return RunAll(fs::path(path));
}
