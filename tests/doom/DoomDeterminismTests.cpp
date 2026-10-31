// DoomDeterminismTests.cpp
//
// Runs the same headless DOOM input in two fresh processes and compares the simulation and frame
// hashes at every tic. The child trace is intentionally explicit so the first divergence identifies
// whether the simulation or the renderer changed first.

#ifndef _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#endif

#include <doom/DoomGame.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
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

    constexpr int kTraceTics = 2200; // 62.9 seconds at DOOM's 35 Hz, beyond the reported ~45 s.
    constexpr int kReplayWarmupTics = 10; // FlyLearnMeasure advances the game ten tics before attaching FlyPlayer.

    std::string Hex(const std::uint64_t value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
        return text;
    }

    std::uint64_t Mix(std::uint64_t hash, const std::uint64_t value)
    {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        return hash;
    }

    std::uint64_t WorldHash(doom::DoomGame& game)
    {
        const doom::GameInfo info = game.Info();
        std::uint64_t hash = 1469598103934665603ull;
        for (const int value : {info.gameTic, static_cast<int>(info.state), info.episode, info.map,
                                info.menuActive ? 1 : 0, info.paused ? 1 : 0, info.demoPlayback ? 1 : 0,
                                info.consolePlayer, info.skill, info.deathmatch, info.netGame ? 1 : 0})
        {
            hash = Mix(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
        }
        for (int player = 0; player < 4; ++player)
        {
            const doom::PlayerState state = game.Player(player);
            const auto add = [&](const std::int64_t value) { hash = Mix(hash, static_cast<std::uint64_t>(value)); };
            add(state.inGame ? 1 : 0);
            add(state.health);
            add(state.armor);
            add(static_cast<std::int64_t>(std::llround(state.x * 65536.0)));
            add(static_cast<std::int64_t>(std::llround(state.y * 65536.0)));
            add(static_cast<std::int64_t>(std::llround(state.z * 65536.0)));
            add(static_cast<std::int64_t>(std::llround(state.angleDegrees * 1048576.0)));
            add(state.readyWeapon);
            add(state.kills);
            add(state.items);
            add(state.secrets);
            add(state.frags);
            add(state.deaths);
            add(state.hasAttacker ? 1 : 0);
            add(static_cast<std::int64_t>(std::llround(state.attackerX * 65536.0)));
            add(static_cast<std::int64_t>(std::llround(state.attackerY * 65536.0)));
            doom::TicCmd command;
            if (game.GetTicCmd(player, command))
            {
                add(command.forwardMove);
                add(command.sideMove);
                add(command.angleTurn);
                add(command.buttons);
            }
            else
            {
                add(-1);
            }
        }
        return hash;
    }

    class FixedTicCmdSource final : public doom::ITicCmdSource
    {
    public:
        void BuildTicCmd(const int player, doom::TicCmd& cmd) override
        {
            if (player != 0)
            {
                return;
            }
            state_ = state_ * 1664525u + 1013904223u;
            cmd.forwardMove = (state_ & 0x1u) != 0 ? 50 : 25;
            cmd.sideMove = (state_ & 0x2u) != 0 ? 25 : 0;
            cmd.angleTurn = static_cast<std::int16_t>((state_ >> 16) & 0x07ffu) - 1024;
            cmd.buttons = (state_ & 0x80000000u) != 0 ? doom::kButtonAttack : 0;
        }

    private:
        std::uint32_t state_ = 0xD00D1234u;
    };

    struct ReplayLoadResult
    {
        std::vector<doom::TicCmd> commands;
        std::string error;
    };

    ReplayLoadResult LoadReplayCommands(const fs::path& trace)
    {
        ReplayLoadResult result;
        std::ifstream input(trace);
        if (!input)
        {
            result.error = "cannot open trace";
            return result;
        }
        result.commands.resize(kTraceTics);
        std::vector<bool> seen(kTraceTics, false);
        std::string line;
        while (std::getline(input, line))
        {
            if (line.find_first_not_of(" \t\r") == std::string::npos)
            {
                continue;
            }
            int tic = 0;
            int health = 0;
            double x = 0.0;
            double y = 0.0;
            double angle = 0.0;
            int forward = 0;
            int turn = 0;
            unsigned buttons = 0;
            char frame[32] = {};
            if (sscanf_s(line.c_str(), "%d hp %d pos %lf %lf ang %lf cmd %d %d %u frame %31s", &tic, &health, &x, &y, &angle,
                         &forward, &turn, &buttons, frame, static_cast<unsigned>(sizeof(frame))) != 9)
            {
                result.error = "malformed trace line";
                return result;
            }
            if (tic < 0)
            {
                result.error = "trace tic outside replay window";
                return result;
            }
            if (tic >= kTraceTics)
            {
                continue;
            }
            if (seen[static_cast<std::size_t>(tic)])
            {
                result.error = "duplicate trace tic";
                return result;
            }
            seen[static_cast<std::size_t>(tic)] = true;
            doom::TicCmd& command = result.commands[static_cast<std::size_t>(tic)];
            command.forwardMove = static_cast<std::int8_t>(forward);
            command.angleTurn = static_cast<std::int16_t>(turn);
            command.buttons = static_cast<std::uint8_t>(buttons);
        }
        if (input.bad())
        {
            result.error = "error while reading trace";
            return result;
        }
        for (int tic = 0; tic < kTraceTics; ++tic)
        {
            if (!seen[static_cast<std::size_t>(tic)])
            {
                std::ostringstream message;
                message << "trace is incomplete at tic " << tic;
                result.error = message.str();
                result.commands.clear();
                return result;
            }
        }
        return result;
    }

    class ReplayTicCmdSource final : public doom::ITicCmdSource
    {
    public:
        explicit ReplayTicCmdSource(std::vector<doom::TicCmd> commands) : commands_(std::move(commands)) {}

        void BuildTicCmd(const int player, doom::TicCmd& cmd) override
        {
            if (player == 0 && tic_ < commands_.size())
            {
                cmd = commands_[tic_++];
            }
        }

    private:
        std::vector<doom::TicCmd> commands_;
        std::size_t tic_ = 0;
    };

    struct ReplayTraceRequest
    {
        bool requested = false;
        fs::path path;
    };

    ReplayTraceRequest FindReplayTrace()
    {
        wchar_t env[MAX_PATH * 2] = {};
        const DWORD length = GetEnvironmentVariableW(L"DOOM_DETERMINISM_TRACE", env, static_cast<DWORD>(std::size(env)));
        if (length > 0)
        {
            return {true, fs::path(env)};
        }
        return {GetLastError() != ERROR_ENVVAR_NOT_FOUND, {}};
    }

    int RunChild(const fs::path& wad)
    {
        doom::DoomConfig config;
        config.iwad = wad;
        config.dataDirectory = fs::temp_directory_path() /
                               ("myengine_doom_determinism_child_" + std::to_string(GetCurrentProcessId()));
        config.extraArgs = {"-warp", "1", "1", "-skill", "4", "-nomusic"};
        std::string error;
        auto game = doom::DoomGame::Create(config, error);
        if (!game)
        {
            std::printf("CREATE_ERROR %s\n", error.c_str());
            return 1;
        }

        const ReplayTraceRequest replayRequest = FindReplayTrace();
        std::vector<doom::TicCmd> replayCommands;
        if (replayRequest.requested)
        {
            if (replayRequest.path.empty())
            {
                std::printf("REPLAY_ERROR empty DOOM_DETERMINISM_TRACE\n");
                return 2;
            }
            ReplayLoadResult replay = LoadReplayCommands(replayRequest.path);
            if (!replay.error.empty())
            {
                std::printf("REPLAY_ERROR %s: %s\n", replayRequest.path.string().c_str(), replay.error.c_str());
                return 2;
            }
            replayCommands = std::move(replay.commands);
        }
        for (int tic = 0; tic < (replayRequest.requested ? kReplayWarmupTics : 0); ++tic)
        {
            game->StepTic();
        }
        FixedTicCmdSource fixedSource;
        ReplayTicCmdSource replaySource(replayCommands);
        doom::ITicCmdSource* source = replayRequest.requested ? static_cast<doom::ITicCmdSource*>(&replaySource) : static_cast<doom::ITicCmdSource*>(&fixedSource);
        game->SetTicCmdSource(source);
        if (replayRequest.requested)
        {
            std::printf("REPLAY trace=%s commands=%zu warmup=%d\n", replayRequest.path.string().c_str(), replayCommands.size(), kReplayWarmupTics);
        }
        for (int tic = 0; tic < kTraceTics; ++tic)
        {
            game->StepTic();
            std::printf("TRACE tic=%d world=%s frame=%s\n", tic + 1, Hex(WorldHash(*game)).c_str(), Hex(game->FrameHash()).c_str());
            if (tic + 1 == 1576)
            {
                std::printf("PROBE tic=1576 pixel_77_105=%08x\n", game->Pixels()[105 * doom::kScreenWidth + 77]);
            }
        }
        return 0;
    }

    struct ChildTrace
    {
        int exitCode = -1;
        std::map<int, std::pair<std::string, std::string>> tics;
        std::string output;
    };

    ChildTrace RunProcess(const fs::path& exe, const fs::path& wad)
    {
        ChildTrace result;
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &security, 0))
        {
            return result;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        SetEnvironmentVariableW(L"FLYDOOM_WAD", wad.c_str());

        std::wstring command = L"\"" + exe.wstring() + L"\" --child";
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

        char buffer[8192];
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
            const std::size_t end = result.output.find('\n', pos);
            const std::string line = result.output.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            int tic = 0;
            char world[32] = {};
            char frame[32] = {};
            if (::sscanf_s(line.c_str(), "TRACE tic=%d world=%31s frame=%31s", &tic, world, static_cast<unsigned>(sizeof(world)), frame, static_cast<unsigned>(sizeof(frame))) == 3)
            {
                result.tics.emplace(tic, std::make_pair(world, frame));
            }
            if (end == std::string::npos)
            {
                break;
            }
            pos = end + 1;
        }
        return result;
    }

    fs::path FindWad()
    {
        wchar_t env[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"FLYDOOM_WAD", env, static_cast<DWORD>(std::size(env))) > 0)
        {
            return fs::path(env);
        }
#ifdef DOOM_TEST_WAD_DIR
        const fs::path candidate = fs::u8path(DOOM_TEST_WAD_DIR) / "freedoom1.wad";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec))
        {
            return candidate;
        }
#endif
        return {};
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path wad = FindWad();
    std::error_code ec;
    if (wad.empty() || !fs::is_regular_file(wad, ec))
    {
        std::printf("doom_determinism SKIPPED: no WAD\n");
        return 77;
    }
    if (argc > 1 && std::string(argv[1]) == "--child")
    {
        return RunChild(wad);
    }

    wchar_t module[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    const fs::path exe(module);
    const ChildTrace first = RunProcess(exe, wad);
    const ChildTrace second = RunProcess(exe, wad);
    if (first.exitCode != 0 || second.exitCode != 0)
    {
        std::printf("doom_determinism FAILED: child exit codes %d and %d\n", first.exitCode, second.exitCode);
        return 1;
    }
    if (first.tics.size() != kTraceTics || second.tics.size() != kTraceTics)
    {
        std::printf("doom_determinism FAILED: trace sizes %zu and %zu, expected %d\n", first.tics.size(), second.tics.size(), kTraceTics);
        return 1;
    }

    for (int tic = 1; tic <= kTraceTics; ++tic)
    {
        const auto a = first.tics.find(tic);
        const auto b = second.tics.find(tic);
        if (a == first.tics.end() || b == second.tics.end() || a->second != b->second)
        {
            const std::string aw = a == first.tics.end() ? "missing" : a->second.first;
            const std::string bw = b == second.tics.end() ? "missing" : b->second.first;
            const std::string af = a == first.tics.end() ? "missing" : a->second.second;
            const std::string bf = b == second.tics.end() ? "missing" : b->second.second;
            std::printf("doom_determinism FAILED: first divergence tic %d world %s vs %s frame %s vs %s\n", tic, aw.c_str(), bw.c_str(), af.c_str(), bf.c_str());
            return 1;
        }
    }
    std::printf("doom_determinism OK: %d tics, world/frame hashes identical\n", kTraceTics);
    return 0;
}
