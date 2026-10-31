// DoomFlyTests.cpp
// Tests of the fly that plays DOOM (game/doomfly): the eyes, the controls, the steering by the picture, the reinforcement, the reset of a brain that runs away,
// and a game of E1M1. Needs the connectome (tools/flybrain/fetch_malecns.ps1) and, for the game, a WAD (tools/doom/fetch_freedoom.ps1 or the shareware doom1.wad);
// without the connectome the test is skipped (exit code 77), without the WAD the scenarios of the game are not run.
//
// DOOM keeps its state in globals and starts once per process, so every scenario runs in a child process of this program (`--scenario <name>`) and prints lines
// "RESULT key=value". `--verbose` prints the whole output of the children.

#include <doomfly/FlyControls.h>
#include <doomfly/FlyPlayer.h>

#include <doom/DoomGame.h>
#include <myengine/jobs/JobSystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

    fs::path SourceDirectory()
    {
#ifdef FLY_SOURCE_DIR
        return fs::u8path(FLY_SOURCE_DIR);
#else
        return fs::current_path();
#endif
    }

    fs::path FbrainPath()
    {
        wchar_t env[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"FLYDOOM_FBRAIN", env, static_cast<DWORD>(sizeof(env) / sizeof(env[0]))) > 0)
        {
            return fs::path(env);
        }
        return SourceDirectory() / "assets" / "flybrain" / "malecns_v1.fbrain";
    }

    fs::path ControlsPath(const char* name)
    {
        return SourceDirectory() / "assets" / "flybrain" / name;
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

    constexpr int kW = doom::kScreenWidth;
    constexpr int kH = doom::kScreenHeight;

    std::uint32_t Gray(const float v)
    {
        const std::uint32_t c = static_cast<std::uint32_t>(std::min(255.0f, std::max(0.0f, v * 255.0f)));
        return (c << 16) | (c << 8) | c;
    }

    // a picture: the part of the width that is bright
    std::vector<std::uint32_t> HalfFrame(const float from, const float to, const float bright, const float dim)
    {
        std::vector<std::uint32_t> frame(static_cast<std::size_t>(kW) * kH);
        for (int y = 0; y < kH; ++y)
        {
            for (int x = 0; x < kW; ++x)
            {
                const float u = static_cast<float>(x) / static_cast<float>(kW);
                frame[static_cast<std::size_t>(y) * kW + x] = Gray(u >= from && u < to ? bright : dim);
            }
        }
        return frame;
    }

    std::shared_ptr<const flybrain::Connectome> LoadBrain()
    {
        std::string error;
        auto connectome = flybrain::Connectome::Load(FbrainPath().u8string(), &error);
        if (!connectome)
        {
            std::printf("  the connectome: %s\n", error.c_str());
        }
        return connectome;
    }

    std::unique_ptr<doomfly::FlyPlayer> MakePlayer(const std::shared_ptr<const flybrain::Connectome>& connectome, const char* controls, const std::uint64_t seed = 1,
                                                       const std::function<void(doomfly::FlyPlayerConfig&)>& tweak = nullptr)
    {
        doomfly::FlyPlayerConfig config;
        config.connectome = connectome;
        config.seed = seed;
        if (tweak)
        {
            tweak(config);
        }
        wchar_t motion[32] = {};
        if (GetEnvironmentVariableW(L"FLYDOOM_TEST_MOTION", motion, 32) > 0)
        {
            config.eyes.motionOnGainMv = config.eyes.motionOffGainMv = static_cast<float>(_wtof(motion));   // to find the gain
        }
        std::string error;
        if (!doomfly::FlyControls::Load(ControlsPath(controls), config.controls, error))
        {
            std::printf("  controls: %s\n", error.c_str());
            return nullptr;
        }
        auto player = doomfly::FlyPlayer::Create(config, error);
        if (!player)
        {
            std::printf("  player: %s\n", error.c_str());
        }
        return player;
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // the scenarios

    int ScenarioEyes()
    {
        auto connectome = LoadBrain();
        if (!connectome)
        {
            return 1;
        }
        doomfly::FlyEyes eyes;
        std::string error;
        if (!eyes.Build(*connectome, doomfly::EyesConfig{}, error))
        {
            std::printf("  eyes: %s\n", error.c_str());
            return 1;
        }
        const auto& info = eyes.Info();
        Result("receptors_left", info.receptors[0]);
        Result("receptors_right", info.receptors[1]);
        Result("columns_left", info.columns[0]);
        Result("columns_right", info.columns[1]);
        Result("covered_left", info.columnsCovered[0]);
        Result("covered_right", info.columnsCovered[1]);
        double minLeft = 9, maxLeft = -9, minRight = 9, maxRight = -9;
        for (const doomfly::Photoreceptor& receptor : eyes.Receptors())
        {
            (receptor.eye == 0 ? minLeft : minRight) = std::min<double>(receptor.eye == 0 ? minLeft : minRight, receptor.x);
            (receptor.eye == 0 ? maxLeft : maxRight) = std::max<double>(receptor.eye == 0 ? maxLeft : maxRight, receptor.x);
        }
        Result("left_x_min", minLeft);
        Result("left_x_max", maxLeft);
        Result("right_x_min", minRight);
        Result("right_x_max", maxRight);

        // a picture bright on the left: the receptors of the left eye see more light than the right eye, and the left of each eye more than its right
        const std::vector<std::uint32_t> left = HalfFrame(0.0f, 0.3f, 0.8f, 0.02f);
        std::vector<float> lum;
        eyes.SampleLuminance(left.data(), kW, kH, lum);
        double sum[2] = {}, count[2] = {};
        for (std::size_t i = 0; i < lum.size(); ++i)
        {
            sum[eyes.Receptors()[i].eye] += lum[i];
            count[eyes.Receptors()[i].eye] += 1;
        }
        Result("left_picture_left_eye", sum[0] / count[0]);
        Result("left_picture_right_eye", sum[1] / count[1]);
        // the response curve
        Result("response_0", eyes.ResponseMv(0.0f));
        Result("response_dim", eyes.ResponseMv(0.02f));
        Result("response_bright", eyes.ResponseMv(1.0f));
        return 0;
    }

    int ScenarioControls()
    {
        doomfly::FlyControls fly, bio;
        std::string error;
        Result("doomfly_ok", doomfly::FlyControls::Load(ControlsPath("controls_doomfly.json"), fly, error) ? "1" : "0");
        Result("bio_ok", doomfly::FlyControls::Load(ControlsPath("controls_bio.json"), bio, error) ? "1" : "0");
        Result("doomfly_turn_terms", static_cast<double>(fly.turn.terms.size()));
        Result("bio_turn_terms", static_cast<double>(bio.turn.terms.size()));
        Result("bio_fire_type", bio.fire.spikeTerms.empty() ? "" : bio.fire.spikeTerms[0].type);
        std::string bad;
        doomfly::FlyControls broken;
        Result("bad_json", doomfly::FlyControls::Parse("{ not json", broken, bad) ? "0" : "1");
        Result("bad_json_text", bad.empty() ? "0" : "1");
        Result("no_turn", doomfly::FlyControls::Parse("{\"name\":\"x\"}", broken, bad) ? "0" : "1");
        Result("bad_side", doomfly::FlyControls::Parse(
                               "{\"turn\":{\"terms\":[{\"type\":\"A\",\"side\":\"up\"}]},\"forward\":{\"terms\":[{\"type\":\"B\"}]},\"fire\":{\"terms\":[{\"type\":\"C\"}]}}", broken, bad)
                               ? "0"
                               : "1");
        // the connectome does not have such neurons: the player refuses to start
        auto connectome = LoadBrain();
        if (connectome)
        {
            doomfly::FlyPlayerConfig config;
            config.connectome = connectome;
            std::string text = "{\"turn\":{\"terms\":[{\"type\":\"NoSuchNeuron\"}]},\"forward\":{\"terms\":[{\"type\":\"DNp20\"}]},\"fire\":{\"terms\":[{\"type\":\"DNp20\"}]}}";
            doomfly::FlyControls::Parse(text, config.controls, bad);
            std::string error2;
            Result("unknown_neuron_refused", doomfly::FlyPlayer::Create(config, error2) == nullptr && !error2.empty() ? "1" : "0");
        }
        return 0;
    }

    // The picture steers: bright on the left, bright on the right, even
    int ScenarioSteer()
    {
        myengine::jobs::Initialize();
        auto connectome = LoadBrain();
        if (!connectome)
        {
            return 1;
        }
        struct Scene
        {
            const char* name;
            std::vector<std::uint32_t> frame;
        };
        const Scene scenes[] = {
            {"even", HalfFrame(0.0f, 1.0f, 0.2f, 0.2f)},
            {"left", HalfFrame(0.0f, 0.5f, 0.6f, 0.03f)},
            {"right", HalfFrame(0.5f, 1.0f, 0.6f, 0.03f)},
        };
        for (const Scene& scene : scenes)
        {
            auto player = MakePlayer(connectome, "controls_doomfly.json");
            if (!player)
            {
                return 1;
            }
            const auto begin = std::chrono::steady_clock::now();
            const doomfly::FlyStatus status = player->EvaluateFrameForTest(scene.frame.data(), 40);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            const std::string prefix = scene.name;
            Result((prefix + "_turn_hz").c_str(), status.turnHz);
            Result((prefix + "_turn_deg").c_str(), status.turnDegrees);
            Result((prefix + "_forward").c_str(), status.forwardUnits);
            Result((prefix + "_fire").c_str(), status.fire ? "1" : "0");
            Result((prefix + "_spikes").c_str(), status.spikesPerSecond);
            Result((prefix + "_speed").c_str(), 40.0 / 35.0 / seconds);
        }
        myengine::jobs::Shutdown();
        return 0;
    }

    // The biological set: silent under the picture (documented), the fly stands still
    int ScenarioBio()
    {
        myengine::jobs::Initialize();
        auto connectome = LoadBrain();
        if (!connectome)
        {
            return 1;
        }
        auto player = MakePlayer(connectome, "controls_bio.json");
        if (!player)
        {
            return 1;
        }
        const std::vector<std::uint32_t> frame = HalfFrame(0.0f, 0.5f, 0.6f, 0.03f);
        const doomfly::FlyStatus status = player->EvaluateFrameForTest(frame.data(), 40);
        double maxRate = 0.0;
        for (const doomfly::RateReading& reading : status.rates)
        {
            maxRate = std::max<double>(maxRate, reading.hz);
        }
        Result("bio_groups", static_cast<double>(status.rates.size()));
        Result("bio_max_rate", maxRate);
        Result("bio_turn_deg", status.turnDegrees);
        Result("bio_forward", status.forwardUnits);
        Result("bio_fire", status.fire ? "1" : "0");
        myengine::jobs::Shutdown();
        return 0;
    }

    // An object that comes at the fly (an expanding bright disc, 28 tics a loop) on the left, on the right, in the middle, and the same disc standing still
    // learning: the brain of the fly that learns (N3b: dopamine as a modulator, the antennal lobe silent, KCs see the columns) with its controls
    int ScenarioLoom(const bool learning = false)
    {
        myengine::jobs::Initialize();
        auto connectome = LoadBrain();
        if (!connectome)
        {
            return 1;
        }
        struct Case
        {
            const char* name;
            float centreX;
            bool grows;
        };
        const Case cases[] = {{"left", 80.0f, true}, {"right", 240.0f, true}, {"centre", 160.0f, true}, {"still", 240.0f, false}};
        for (const Case& c : cases)
        {
            auto player = learning ? MakePlayer(connectome, "controls_learn.json", 1, [](doomfly::FlyPlayerConfig& config) { config.learning.enabled = true; })
                                   : MakePlayer(connectome, "controls_doomfly.json");
            if (!player)
            {
                return 1;
            }
            std::vector<doomfly::FlyStatus> history;
            player->EvaluateSequenceForTest(
                [&](int tic, std::vector<std::uint32_t>& frame)
                {
                    const float radius = tic >= 84 ? 0.0f : (c.grows ? 3.0f + 2.4f * static_cast<float>(tic % 28) : 40.0f);   // after 84 tics the object is gone
                    for (int y = 0; y < kH; ++y)
                    {
                        for (int x = 0; x < kW; ++x)
                        {
                            const float dx = static_cast<float>(x) - c.centreX;
                            const float dy = static_cast<float>(y) - 84.0f;
                            frame[static_cast<std::size_t>(y) * kW + x] = Gray(dx * dx + dy * dy < radius * radius ? 0.9f : 0.1f);
                        }
                    }
                },
                84 + 56, &history);
            std::map<std::string, double> peak;
            double fireTics = 0, turnSum = 0, centralPeak = 0, turnMin = 0, turnMax = 0, centralSum = 0;
            for (std::size_t i = 14; i < 84; ++i)   // after the start, while the object comes
            {
                for (const doomfly::RateReading& reading : history[i].rates)
                {
                    peak[reading.name] = std::max(peak[reading.name], static_cast<double>(reading.hz));
                }
                fireTics += history[i].fire ? 1 : 0;
                turnSum += history[i].turnDegrees;
                turnMin = std::min<double>(turnMin, history[i].turnDegrees);
                turnMax = std::max<double>(turnMax, history[i].turnDegrees);
                centralPeak = std::max<double>(centralPeak, history[i].centralSpikesPerSecond);
                centralSum += history[i].centralSpikesPerSecond;
            }
            double afterCentral = 0.0;
            for (std::size_t i = 84 + 28; i < history.size(); ++i)
            {
                afterCentral += history[i].centralSpikesPerSecond;
            }
            const std::string prefix = std::string(learning ? "loomlearn_" : "loom_") + c.name + "_";
            Result((prefix + "central_after_mean").c_str(), afterCentral / 28.0);
            for (const char* name : {"LC4 L", "LC4 R", "LPLC2 L", "LPLC2 R", "DNp01", "DNa02 L", "DNa02 R", "DNp20 L", "DNp20 R", "DNpe017"})
            {
                Result((prefix + name).c_str(), peak[name]);
            }
            if (learning)
            {
                for (const char* name : {"MBON32 L", "MBON32 R", "MBON20 L", "MBON20 R"})
                {
                    Result((prefix + name).c_str(), peak[name]);
                }
            }
            Result((prefix + "fire_tics").c_str(), fireTics);
            Result((prefix + "turn_mean").c_str(), turnSum / 70.0);
            Result((prefix + "turn_min").c_str(), turnMin);
            Result((prefix + "turn_max").c_str(), turnMax);
            Result((prefix + "central_peak").c_str(), centralPeak);
            Result((prefix + "central_mean").c_str(), centralSum / 70.0);
            Result((prefix + "seizures").c_str(), history.back().seizures);
        }
        myengine::jobs::Shutdown();
        return 0;
    }

    // N3b: "view X + damage" through the whole chain of the fly that learns. X: a flickering pattern on the left half of the picture (its columns change, so
    // the KCs see it), Y: the same on the right half. Before: X; then 8 pairings of X with damage (into every PPL1); after: X and Y. Two arms, learning on
    // and off, the same seed: the only difference is the weights (the DANs fire a little at any picture, so the arms part already at the first X)
    int ScenarioLearnShift()
    {
        myengine::jobs::Initialize();
        auto connectome = LoadBrain();
        if (!connectome)
        {
            return 1;
        }
        auto picture = [](const bool left, const int tic, std::vector<std::uint32_t>& frame)
        {
            for (int y = 0; y < kH; ++y)
            {
                for (int x = 0; x < kW; ++x)
                {
                    const bool inside = left ? x < kW / 2 - 20 : x >= kW / 2 + 20;
                    const bool on = ((x / 16 + y / 16 + tic) % 2) == 0;
                    frame[static_cast<std::size_t>(y) * kW + x] = Gray(inside ? (on ? 0.85f : 0.15f) : 0.3f);
                }
            }
        };
        std::vector<std::uint32_t> frame(static_cast<std::size_t>(kW) * kH);
        for (const bool learning : {true, false})
        {
            auto player = MakePlayer(connectome, "controls_learn.json", 1, [learning](doomfly::FlyPlayerConfig& config) {
                config.learning.enabled = true;
                config.learning.learning = learning;
            });
            if (!player)
            {
                return 1;
            }
            int tic = 0;
            // the spikes of the MBONs the turn reads, by side, and the turn, over `tics` tics of a picture (left: X, right: Y; none: grey)
            auto show = [&](const int pictureSide, const int tics, double& mbonLeft, double& mbonRight, double& turn, const bool hurt = false)
            {
                mbonLeft = mbonRight = turn = 0.0;
                for (int t = 0; t < tics; ++t, ++tic)
                {
                    if (pictureSide == 0)
                    {
                        std::fill(frame.begin(), frame.end(), Gray(0.3f));
                    }
                    else
                    {
                        picture(pictureSide < 0, tic, frame);
                    }
                    const doomfly::FlyStatus status = player->EvaluateFrameForTest(frame.data(), 1, hurt && t % 7 == 0);
                    for (const doomfly::RateReading& r : status.rates)
                    {
                        if (r.name == "MBON32 L" || r.name == "MBON20 L")
                        {
                            mbonLeft += r.hz / tics;
                        }
                        if (r.name == "MBON32 R" || r.name == "MBON20 R")
                        {
                            mbonRight += r.hz / tics;
                        }
                    }
                    turn += status.turnDegrees / tics;
                }
            };
            double l = 0, r = 0, turn = 0;
            show(0, 35, l, r, turn);
            double xLeftBefore = 0, xRightBefore = 0, xTurnBefore = 0;
            show(-1, 70, xLeftBefore, xRightBefore, xTurnBefore);
            show(0, 35, l, r, turn);
            for (int pairing = 0; pairing < 8; ++pairing)
            {
                show(-1, 21, l, r, turn, true);
                show(0, 14, l, r, turn);
            }
            show(0, 35, l, r, turn);
            double xLeftAfter = 0, xRightAfter = 0, xTurnAfter = 0;
            show(-1, 70, xLeftAfter, xRightAfter, xTurnAfter);
            show(0, 35, l, r, turn);
            double yLeftAfter = 0, yRightAfter = 0, yTurnAfter = 0;
            show(1, 70, yLeftAfter, yRightAfter, yTurnAfter);
            const std::string prefix = learning ? "shift_on_" : "shift_off_";
            Result((prefix + "x_mbon_left_before").c_str(), xLeftBefore);
            Result((prefix + "x_mbon_left_after").c_str(), xLeftAfter);
            Result((prefix + "x_mbon_right_after").c_str(), xRightAfter);
            Result((prefix + "x_turn_before").c_str(), xTurnBefore);
            Result((prefix + "x_turn_after").c_str(), xTurnAfter);
            Result((prefix + "y_mbon_right_after").c_str(), yRightAfter);
            Result((prefix + "y_turn_after").c_str(), yTurnAfter);
            Result((prefix + "scale").c_str(), player->Status().learnedScale);
            Result((prefix + "seizures").c_str(), player->Status().seizures);
        }
        myengine::jobs::Shutdown();
        return 0;
    }

    // Damage: a current into PPL101 for 200 ms, and it stops
    int ScenarioDamage()
    {
        myengine::jobs::Initialize();
        auto connectome = LoadBrain();
        if (!connectome)
        {
            return 1;
        }
        auto player = MakePlayer(connectome, "controls_doomfly.json");
        if (!player)
        {
            return 1;
        }
        const std::vector<std::uint32_t> frame = HalfFrame(0.0f, 1.0f, 0.2f, 0.2f);
        const doomfly::FlyStatus first = player->EvaluateFrameForTest(frame.data(), 1, true);
        Result("dopamine_at_damage", first.dopamineMv);
        Result("damages", first.damages);
        const doomfly::FlyStatus later = player->EvaluateFrameForTest(frame.data(), 10, false);   // 11 tics = 314 ms after the damage
        Result("dopamine_later", later.dopamineMv);
        Result("damages_later", later.damages);
        myengine::jobs::Shutdown();
        return 0;
    }

    // A brain that runs away is made again
    int ScenarioSeizure()
    {
        myengine::jobs::Initialize();
        auto connectome = LoadBrain();
        if (!connectome)
        {
            return 1;
        }
        auto player = MakePlayer(connectome, "controls_doomfly.json");
        if (!player)
        {
            return 1;
        }
        const std::vector<std::uint32_t> frame = HalfFrame(0.0f, 1.0f, 0.2f, 0.2f);
        doomfly::FlyStatus status = player->EvaluateFrameForTest(frame.data(), 15, false);
        Result("rest_spikes", status.spikesPerSecond);
        Result("seizures_at_rest", status.seizures);
        // the lamina cells get 20 mV more than the tonic current: the whole brain runs away (1.7 million spikes a second at 20 mV in all)
        player->InjectCurrentForTest("L1", 25.0f);
        player->InjectCurrentForTest("L2", 25.0f);
        player->InjectCurrentForTest("L3", 25.0f);
        double peak = 0.0;
        double peakCentral = 0.0;
        double centralSumRun = 0.0;
        int centralTicks = 0;
        int ticks = 0;
        for (; ticks < 60 && status.seizures == 0; ++ticks)
        {
            status = player->EvaluateFrameForTest(frame.data(), 1, false);
            peak = std::max<double>(peak, status.spikesPerSecond);
            peakCentral = std::max<double>(peakCentral, status.centralSpikesPerSecond);
            centralSumRun += status.centralSpikesPerSecond;
            ++centralTicks;
        }
        Result("seizures", status.seizures);
        Result("resets", status.resets);
        Result("peak_spikes", peak);
        Result("peak_central", peakCentral);
        Result("mean_central", centralTicks > 0 ? centralSumRun / centralTicks : 0.0);
        Result("ticks_to_seizure", ticks);
        // after the reset the brain is at rest again
        status = player->EvaluateFrameForTest(frame.data(), 15, false);
        Result("after_spikes", status.spikesPerSecond);
        Result("seizures_after", status.seizures);
        myengine::jobs::Shutdown();
        return 0;
    }

    // E1M1, the fly plays for a few seconds
    // Two brains in one game (Fly vs Fly): "-deathmatch -players 2", the brain of player 0 and the brain of player 1, each sees through its own eyes, one tic of the game
    // when both have answered
    int ScenarioDuel(const fs::path& wad, const int tics)
    {
        myengine::jobs::Initialize();
        auto connectome = LoadBrain();
        if (!connectome)
        {
            return 1;
        }
        doom::DoomConfig config;
        config.iwad = wad;
        config.dataDirectory = fs::temp_directory_path() / "myengine_doom_fly_test";
        config.extraArgs = {"-warp", "1", "1", "-skill", "3", "-nomusic", "-nomonsters", "-deathmatch", "-players", "2"};
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
        auto first = MakePlayer(connectome, "controls_doomfly.json", 1, [](doomfly::FlyPlayerConfig& c) { c.playerIndex = 0; c.ownView = true; c.pace = false; });
        auto second = MakePlayer(connectome, "controls_doomfly.json", 1001, [](doomfly::FlyPlayerConfig& c) { c.playerIndex = 1; c.ownView = true; c.drivesScreens = false; c.pace = false; });
        if (!first || !second)
        {
            return 1;
        }
        struct Source final : doom::ITicCmdSource
        {
            doomfly::FlyPlayer* a;
            doomfly::FlyPlayer* b;
            void BuildTicCmd(int player, doom::TicCmd& cmd) override
            {
                if (player == 0) cmd = a->Command();
                if (player == 1) cmd = b->Command();
            }
        } source;
        source.a = first.get();
        source.b = second.get();
        game->SetTicCmdSource(&source);
        const doom::PlayerState a0 = game->Player(0);
        const doom::PlayerState b0 = game->Player(1);
        std::uint64_t hashA = 1469598103934665603ull, hashB = hashA;
        int waits = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (int tic = 0; tic < tics; ++tic)
        {
            first->Begin(*game, 1.0 / 35.0);
            second->Begin(*game, 1.0 / 35.0);
            int guard = 0;
            while (!(first->Poll(*game) & second->Poll(*game)))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                ++waits;
                if (++guard > 20000)
                {
                    std::printf("  a brain did not answer in 20 s\n");
                    return 1;
                }
            }
            game->StepTic();
            doom::TicCmd c0, c1;
            game->GetTicCmd(0, c0);
            game->GetTicCmd(1, c1);
            hashA = (hashA ^ static_cast<std::uint8_t>(c0.forwardMove)) * 1099511628211ull;
            hashA = (hashA ^ static_cast<std::uint16_t>(c0.angleTurn)) * 1099511628211ull;
            hashB = (hashB ^ static_cast<std::uint8_t>(c1.forwardMove)) * 1099511628211ull;
            hashB = (hashB ^ static_cast<std::uint16_t>(c1.angleTurn)) * 1099511628211ull;
            first->EndTic(*game);
            second->EndTic(*game);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        const doom::PlayerState a1 = game->Player(0);
        const doom::PlayerState b1 = game->Player(1);
        Result("tics_a", static_cast<double>(first->Status().tics));
        Result("tics_b", static_cast<double>(second->Status().tics));
        Result("moved_a", std::hypot(a1.x - a0.x, a1.y - a0.y));
        Result("moved_b", std::hypot(b1.x - b0.x, b1.y - b0.y));
        Result("apart", std::hypot(a1.x - b1.x, a1.y - b1.y));
        Result("hash_a", [&] { char text[32]; std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hashA)); return std::string(text); }());
        Result("hash_b", [&] { char text[32]; std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hashB)); return std::string(text); }());
        Result("eyes_a", first->EyesPicture().size() == static_cast<size_t>(doom::kScreenWidth) * doom::kScreenHeight ? "1" : "0");
        Result("eyes_b", second->EyesPicture().size() == static_cast<size_t>(doom::kScreenWidth) * doom::kScreenHeight ? "1" : "0");
        Result("eyes_differ", first->EyesPicture() != second->EyesPicture() ? "1" : "0");
        Result("speed", 1.0 / 35.0 * tics / seconds);
        Result("waits", waits);
        // the round is over: the level again, both players born again
        game->RestartLevel();
        for (int i = 0; i < 4; ++i)
        {
            game->StepTic();
        }
        Result("again_in0", game->Player(0).inGame ? "1" : "0");
        Result("again_in1", game->Player(1).inGame ? "1" : "0");
        Result("again_health", std::min(game->Player(0).health, game->Player(1).health));
        Result("again_frags", game->Player(0).frags + game->Player(1).frags);
        game->SetTicCmdSource(nullptr);
        first.reset();
        second.reset();
        game.reset();
        myengine::jobs::Shutdown();
        return 0;
    }

    int ScenarioPlay(const fs::path& wad, const std::uint64_t seed, const int tics)
    {
        myengine::jobs::Initialize();
        auto connectome = LoadBrain();
        if (!connectome)
        {
            return 1;
        }
        doom::DoomConfig config;
        config.iwad = wad;
        config.dataDirectory = fs::temp_directory_path() / "myengine_doom_fly_test";
        config.extraArgs = {"-warp", "1", "1", "-skill", "3", "-nomusic"};
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
        auto player = MakePlayer(connectome, "controls_doomfly.json", seed);
        if (!player)
        {
            return 1;
        }
        player->Attach(*game);
        std::uint64_t hash = 1469598103934665603ull;
        std::map<int, int> turns;
        double forwardSum = 0.0;
        int fires = 0;
        double startX = game->Player(0).x, startY = game->Player(0).y;
        double travelled = 0.0, lastX = startX, lastY = startY;
        const auto begin = std::chrono::steady_clock::now();
        for (int tic = 0; tic < tics; ++tic)
        {
            player->StepBlocking(*game);
            doom::TicCmd cmd;
            game->GetTicCmd(0, cmd);
            hash = (hash ^ static_cast<std::uint8_t>(cmd.forwardMove)) * 1099511628211ull;
            hash = (hash ^ static_cast<std::uint16_t>(cmd.angleTurn)) * 1099511628211ull;
            hash = (hash ^ cmd.buttons) * 1099511628211ull;
            turns[cmd.angleTurn]++;
            forwardSum += cmd.forwardMove;
            fires += (cmd.buttons & doom::kButtonAttack) != 0;
            const doom::PlayerState p = game->Player(0);
            travelled += std::hypot(p.x - lastX, p.y - lastY);
            lastX = p.x;
            lastY = p.y;
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        const doomfly::FlyStatus status = player->Status();
        Result("cmd_hash", [&] { char text[32]; std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash)); return std::string(text); }());
        Result("distinct_turns", static_cast<double>(turns.size()));
        Result("mean_forward", forwardSum / tics);
        Result("fires", fires);
        Result("travelled", travelled);
        Result("net_distance", std::hypot(lastX - startX, lastY - startY));
        Result("final_x", lastX);
        Result("final_y", lastY);
        Result("final_angle", game->Player(0).angleDegrees);
        Result("game_tic", game->Info().gameTic);
        Result("brain_speed", 1.0 / 35.0 * tics / seconds);
        Result("health", game->Player(0).health);
        Result("resets", status.resets);
        Result("tics", static_cast<double>(status.tics));
        player->Detach(*game);
        player.reset();
        game.reset();
        myengine::jobs::Shutdown();
        return 0;
    }

    // A real FlyPlayer owns the tic-command source here. A native intermission only observes BT_USE/BT_ATTACK
    // in that command, so this catches the regression where drivesScreens posts Enter but leaves buttons clear.
    int ScenarioIntermission(const fs::path& wad)
    {
        myengine::jobs::Initialize();
        auto connectome = LoadBrain();
        if (!connectome)
        {
            return 1;
        }
        doom::DoomConfig config;
        config.iwad = wad;
        config.dataDirectory = fs::temp_directory_path() / "myengine_doom_fly_intermission_test";
        config.extraArgs = {"-warp", "1", "1", "-skill", "3", "-nomusic", "-nomonsters"};
        std::string error;
        auto game = doom::DoomGame::Create(config, error);
        if (!game)
        {
            std::printf("  DOOM: %s\n", error.c_str());
            myengine::jobs::Shutdown();
            return 1;
        }
        for (int tic = 0; tic < 35; ++tic)
        {
            game->StepTic();
        }
        G_ExitLevel();
        game->StepTic();
        const bool entered = game->Info().state == doom::GameState::Intermission;
        auto player = MakePlayer(connectome, "controls_doomfly.json", 1, [](doomfly::FlyPlayerConfig& c) {
            c.pace = false;
            c.playerIndex = 0;
            c.ownView = true;
            c.drivesScreens = true;
        });
        if (!player)
        {
            game.reset();
            myengine::jobs::Shutdown();
            return 1;
        }
        player->Attach(*game);
        int usePulses = 0;
        int attackPulses = 0;
        constexpr int kMaxTics = 900;
        for (int tic = 0; tic < kMaxTics && game->Info().state == doom::GameState::Intermission; ++tic)
        {
            player->StepBlocking(*game);
            doom::TicCmd command;
            game->GetTicCmd(0, command);
            usePulses += (command.buttons & doom::kButtonUse) != 0 ? 1 : 0;
            attackPulses += (command.buttons & doom::kButtonAttack) != 0 ? 1 : 0;
        }
        const bool advanced = game->Info().state == doom::GameState::Level && game->Info().map == 2;
        Result("entered", entered ? "1" : "0");
        Result("use_pulses", usePulses);
        Result("attack_pulses", attackPulses);
        Result("advanced", advanced ? "1" : "0");
        Result("next_map", game->Info().map);
        Result("next_level", game->Info().state == doom::GameState::Level ? "1" : "0");
        player->Detach(*game);
        player.reset();
        game.reset();
        myengine::jobs::Shutdown();
        return entered && usePulses >= 3 && advanced ? 0 : 1;
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // the parent

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
        WaitForSingleObject(process.hProcess, 600000);
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
        if (g_verbose)
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

    std::string Str(const ChildResult& child, const char* key)
    {
        const auto it = child.values.find(key);
        return it == child.values.end() ? std::string() : it->second;
    }

    int RunAll(const fs::path& exe)
    {
        // 1. the controls (data)
        const ChildResult controls = RunChild(exe, "--scenario controls");
        CHECK(controls.exitCode == 0);
        CHECK(Str(controls, "doomfly_ok") == "1");
        CHECK(Str(controls, "bio_ok") == "1");
        CHECK(Num(controls, "doomfly_turn_terms") == 4.0);
        CHECK(Num(controls, "bio_turn_terms") == 4.0);
        CHECK(Str(controls, "bio_fire_type") == "MN9");
        CHECK(Str(controls, "bad_json") == "1");
        CHECK(Str(controls, "bad_json_text") == "1");
        CHECK(Str(controls, "no_turn") == "1");
        CHECK(Str(controls, "bad_side") == "1");
        CHECK(Str(controls, "unknown_neuron_refused") == "1");

        // 2. the eyes
        const ChildResult eyes = RunChild(exe, "--scenario eyes");
        CHECK(eyes.exitCode == 0);
        std::printf("  eyes: %.0f / %.0f photoreceptors (left / right), columns covered %.0f of %.0f / %.0f of %.0f\n", Num(eyes, "receptors_left"), Num(eyes, "receptors_right"),
                    Num(eyes, "covered_left"), Num(eyes, "columns_left"), Num(eyes, "covered_right"), Num(eyes, "columns_right"));
        CHECK(Num(eyes, "receptors_left") > 300.0);
        CHECK(Num(eyes, "receptors_right") > 600.0);
        CHECK(Num(eyes, "covered_left") > 100.0);
        CHECK(Num(eyes, "covered_right") > 250.0);
        // each eye looks at its part of the picture, the middle is seen by both
        CHECK(Num(eyes, "left_x_min") < 0.05 && Num(eyes, "left_x_max") > 0.45 && Num(eyes, "left_x_max") <= 0.6001);
        CHECK(Num(eyes, "right_x_min") >= 0.3999 && Num(eyes, "right_x_min") < 0.5 && Num(eyes, "right_x_max") > 0.95);
        // a picture bright on the left is seen by the left eye
        CHECK(Num(eyes, "left_picture_left_eye") > 2.0 * Num(eyes, "left_picture_right_eye"));
        CHECK(Num(eyes, "response_0") == 0.0);
        CHECK(std::fabs(Num(eyes, "response_dim") - 15.0) < 0.01);   // half of the saturation at the half luminance
        CHECK(Num(eyes, "response_bright") > 28.0 && Num(eyes, "response_bright") < 30.0);

        // 3. the picture steers the fly (the neurons of DOOMFLY)
        const ChildResult steer = RunChild(exe, "--scenario steer");
        CHECK(steer.exitCode == 0);
        std::printf("  steering (DNp20 R-L, Hz): even %.1f, light on the left %.1f, light on the right %.1f; turn %.2f / %.2f / %.2f degrees per tic; brain %.2fx real time\n",
                    Num(steer, "even_turn_hz"), Num(steer, "left_turn_hz"), Num(steer, "right_turn_hz"), Num(steer, "even_turn_deg"), Num(steer, "left_turn_deg"), Num(steer, "right_turn_deg"),
                    Num(steer, "left_speed"));
        // the sign: positive turns right. The light on the right turns the fly to the right, the light on the left to the left
        CHECK(Num(steer, "right_turn_hz") > Num(steer, "even_turn_hz") + 8.0);
        CHECK(Num(steer, "left_turn_hz") < Num(steer, "even_turn_hz") - 8.0);
        CHECK(Num(steer, "right_turn_deg") > 1.0);
        CHECK(Num(steer, "left_turn_deg") < -1.0);
        CHECK(std::fabs(Num(steer, "even_turn_deg")) < 1.0);
        CHECK(Num(steer, "left_spikes") > 100000.0 && Num(steer, "left_spikes") < 1000000.0);
        CHECK(Num(steer, "even_forward") > 5.0);   // DNpe017 walks

        // 4. the biological set is silent
        const ChildResult bio = RunChild(exe, "--scenario bio");
        CHECK(bio.exitCode == 0);
        CHECK(Num(bio, "bio_groups") >= 6.0);
        CHECK(Num(bio, "bio_max_rate") < 1.0);
        CHECK(Num(bio, "bio_turn_deg") == 0.0);
        CHECK(Num(bio, "bio_forward") == 0.0);
        CHECK(Str(bio, "bio_fire") == "0");

        // 5. damage
        const ChildResult damage = RunChild(exe, "--scenario damage");
        CHECK(damage.exitCode == 0);
        CHECK(Num(damage, "dopamine_at_damage") == 4.0);
        CHECK(Num(damage, "damages") == 1.0);
        CHECK(Num(damage, "dopamine_later") == 0.0);

        // 6. the brain that runs away
        const ChildResult seizure = RunChild(exe, "--scenario seizure");
        CHECK(seizure.exitCode == 0);
        std::printf("  runaway: %.0f spikes/s at rest, peak %.0f, reset after %.0f ticks, %.0f spikes/s after it\n", Num(seizure, "rest_spikes"), Num(seizure, "peak_spikes"),
                    Num(seizure, "ticks_to_seizure"), Num(seizure, "after_spikes"));
        CHECK(Num(seizure, "seizures_at_rest") == 0.0);
        CHECK(Num(seizure, "rest_spikes") < 600000.0);
        CHECK(Num(seizure, "seizures") == 1.0);
        CHECK(Num(seizure, "resets") >= 1.0);
        CHECK(Num(seizure, "peak_spikes") > 1000000.0);
        CHECK(Num(seizure, "peak_central") > 600000.0);
        CHECK(Num(seizure, "after_spikes") < 600000.0);
        CHECK(Num(seizure, "seizures_after") == 1.0);

        // 6b. an object comes at the fly (an expanding bright disc): what the looming path does in this model
        const ChildResult loom = RunChild(exe, "--scenario loom");
        CHECK(loom.exitCode == 0);
        std::printf("  looming (84 tics, then the object is gone): right: LC4 R %.1f Hz, giant fiber %.1f Hz, DNa02 L %.1f Hz, turn mean %.2f min %.1f degrees per tic, %.0f shot tics, %.0f runaway resets;"
                    " centre: turn mean %.2f; left: turn mean %.2f, %.0f shot tics; a disc that only stands: turn %.2f, %.0f shot tics\n",
                    Num(loom, "loom_right_LC4 R"), Num(loom, "loom_right_DNp01"), Num(loom, "loom_right_DNa02 L"), Num(loom, "loom_right_turn_mean"), Num(loom, "loom_right_turn_min"),
                    Num(loom, "loom_right_fire_tics"), Num(loom, "loom_right_seizures"), Num(loom, "loom_centre_turn_mean"), Num(loom, "loom_left_turn_mean"), Num(loom, "loom_left_fire_tics"),
                    Num(loom, "loom_still_turn_mean"), Num(loom, "loom_still_fire_tics"));
        // from the right and from the front: the central brain is thrown into its own state (DNa02 of the opposite side fires), the fly turns hard away and shoots
        CHECK(Num(loom, "loom_right_DNa02 L") > 10.0);
        CHECK(Num(loom, "loom_right_turn_min") <= -4.0);
        CHECK(Num(loom, "loom_right_turn_mean") < -0.8);
        CHECK(Num(loom, "loom_right_fire_tics") >= 1.0);
        CHECK(Num(loom, "loom_right_LC4 R") > 0.0);
        CHECK(Num(loom, "loom_centre_DNa02 L") > 10.0);
        CHECK(Num(loom, "loom_centre_turn_mean") < -0.8);
        CHECK(Num(loom, "loom_centre_fire_tics") >= 1.0);
        // the object that stands, and the object on the left: nothing (the left eye is silent in this model, docs/game/flydoom-fly.md)
        CHECK(Num(loom, "loom_still_fire_tics") == 0.0);
        CHECK(Num(loom, "loom_still_central_mean") == 0.0);
        CHECK(Num(loom, "loom_still_seizures") == 0.0);
        CHECK(Num(loom, "loom_left_fire_tics") == 0.0);
        CHECK(Num(loom, "loom_left_seizures") == 0.0);
        CHECK(Num(loom, "loom_right_turn_mean") < Num(loom, "loom_left_turn_mean") - 0.5);

        // 6c. the same with the brain of the fly that learns (N3b: dopamine a modulator, the antennal lobe silent, the KCs see the columns; controls_learn.json):
        // no storm and no reset; the turn away from an object on the right is weaker than in D1a (against the disc that stands, with the offset of these
        // controls) and an object on the left draws the fly towards it (the MBONs of that side answer it); controls_learn.json shoots at any picture
        const ChildResult loomLearn = RunChild(exe, "--scenario loom_learn");
        CHECK(loomLearn.exitCode == 0);
        std::printf("  looming, the brain that learns: right: turn mean %.2f min %.1f, %.0f shot tics, giant fiber %.1f Hz, %.0f resets, central %.0f spikes/s, MBON32 R %.0f Hz;"
                    " left: turn mean %.2f, %.0f shot tics; a disc that only stands: turn mean %.2f, %.0f shot tics\n",
                    Num(loomLearn, "loomlearn_right_turn_mean"), Num(loomLearn, "loomlearn_right_turn_min"), Num(loomLearn, "loomlearn_right_fire_tics"),
                    Num(loomLearn, "loomlearn_right_DNp01"), Num(loomLearn, "loomlearn_right_seizures"), Num(loomLearn, "loomlearn_right_central_mean"),
                    Num(loomLearn, "loomlearn_right_MBON32 R"), Num(loomLearn, "loomlearn_left_turn_mean"), Num(loomLearn, "loomlearn_left_fire_tics"),
                    Num(loomLearn, "loomlearn_still_turn_mean"), Num(loomLearn, "loomlearn_still_fire_tics"));
        CHECK(Num(loomLearn, "loomlearn_right_turn_min") <= -4.0);
        CHECK(Num(loomLearn, "loomlearn_right_turn_mean") < Num(loomLearn, "loomlearn_still_turn_mean") - 0.3);
        CHECK(Num(loomLearn, "loomlearn_centre_turn_mean") < Num(loomLearn, "loomlearn_still_turn_mean") - 0.3);
        CHECK(Num(loomLearn, "loomlearn_right_seizures") == 0.0 && Num(loomLearn, "loomlearn_centre_seizures") == 0.0);
        CHECK(Num(loomLearn, "loomlearn_right_MBON32 R") > 0.0);
        CHECK(Num(loomLearn, "loomlearn_still_MBON32 R") == 0.0);
        CHECK(Num(loomLearn, "loomlearn_still_central_mean") == 0.0);

        // 6d. "view X + damage" through the whole chain of the fly that learns: afterwards the MBONs of its side answer X less, and the fly turns less towards X
        // than the same brain without learning (MBON -> turn is our weight, controls_learn.json)
        const ChildResult shift = RunChild(exe, "--scenario learn_shift");
        CHECK(shift.exitCode == 0);
        std::printf("  view X + damage (learning on / off): MBONs of the left to X %.1f / %.1f Hz, turn at X %.2f / %.2f deg per tic, to Y %.2f / %.2f; mean weight %.4f / %.4f\n",
                    Num(shift, "shift_on_x_mbon_left_after"), Num(shift, "shift_off_x_mbon_left_after"), Num(shift, "shift_on_x_turn_after"), Num(shift, "shift_off_x_turn_after"),
                    Num(shift, "shift_on_y_turn_after"), Num(shift, "shift_off_y_turn_after"), Num(shift, "shift_on_scale"), Num(shift, "shift_off_scale"));
        CHECK(Num(shift, "shift_on_x_mbon_left_after") <= 0.8 * Num(shift, "shift_off_x_mbon_left_after"));
        CHECK(Num(shift, "shift_on_x_turn_after") > Num(shift, "shift_off_x_turn_after") + 0.5);
        CHECK(Num(shift, "shift_on_scale") < 1.0 && Num(shift, "shift_off_scale") == 1.0);
        CHECK(Num(shift, "shift_on_seizures") == 0.0 && Num(shift, "shift_off_seizures") == 0.0);

        // 7. the game
        const fs::path wad = FindWad();
        if (wad.empty())
        {
            std::printf("  (no WAD: the game of E1M1 is not played; tools/doom/fetch_freedoom.ps1)\n");
        }
        else
        {
            SetEnvironmentVariableW(L"FLYDOOM_WAD", wad.c_str());
            const ChildResult playA = RunChild(exe, "--scenario play --seed 1 --tics 140");
            const ChildResult playB = RunChild(exe, "--scenario play --seed 1 --tics 140");
            const ChildResult playC = RunChild(exe, "--scenario play --seed 2 --tics 140");
            CHECK(playA.exitCode == 0 && playB.exitCode == 0 && playC.exitCode == 0);
            std::printf("  E1M1, 140 tics: %.0f distinct turns, forward %.1f, %.0f shots, walked %.0f units (net %.0f), brain %.2fx real time\n", Num(playA, "distinct_turns"),
                        Num(playA, "mean_forward"), Num(playA, "fires"), Num(playA, "travelled"), Num(playA, "net_distance"), Num(playA, "brain_speed"));
            CHECK(Num(playA, "tics") == 140.0);
            CHECK(Num(playA, "distinct_turns") >= 5.0);
            CHECK(Num(playA, "mean_forward") > 3.0);
            CHECK(Num(playA, "travelled") > 100.0);
            CHECK(!Str(playA, "cmd_hash").empty());
            // the same seed, the same game
            CHECK(Str(playA, "cmd_hash") == Str(playB, "cmd_hash"));
            CHECK(Str(playA, "final_x") == Str(playB, "final_x") && Str(playA, "final_y") == Str(playB, "final_y"));
            CHECK(Str(playA, "game_tic") == Str(playB, "game_tic"));
            std::printf("  seed 1 and seed 1: %s, %s; seed 2: %s\n", Str(playA, "cmd_hash").c_str(), Str(playB, "cmd_hash").c_str(), Str(playC, "cmd_hash").c_str());
        }

        if (!wad.empty())
        {
            // 7b. a real FlyPlayer drives native intermission through BT_USE pulses, not keyboard Enter
            const ChildResult intermission = RunChild(exe, "--scenario intermission");
            CHECK(intermission.exitCode == 0);
            CHECK(Str(intermission, "entered") == "1");
            CHECK(Num(intermission, "use_pulses") >= 3.0);
            CHECK(Str(intermission, "advanced") == "1" && Num(intermission, "next_map") == 2.0 && Str(intermission, "next_level") == "1");

            // 8. two brains in one game
            const ChildResult duel = RunChild(exe, "--scenario duel --tics 100");
            CHECK(duel.exitCode == 0);
            std::printf("  duel, 100 tics: both played %.0f / %.0f tics, walked %.0f / %.0f units, apart %.0f, brains %.2fx real time\n", Num(duel, "tics_a"), Num(duel, "tics_b"),
                        Num(duel, "moved_a"), Num(duel, "moved_b"), Num(duel, "apart"), Num(duel, "speed"));
            CHECK(Num(duel, "tics_a") == 100.0 && Num(duel, "tics_b") == 100.0);
            CHECK(Num(duel, "moved_a") > 5.0);
            CHECK(Num(duel, "moved_b") > 5.0);
            CHECK(Num(duel, "apart") > 30.0);
            CHECK(Str(duel, "eyes_a") == "1" && Str(duel, "eyes_b") == "1");
            CHECK(Str(duel, "eyes_differ") == "1");
            CHECK(!Str(duel, "hash_a").empty() && Str(duel, "hash_a") != Str(duel, "hash_b"));
            CHECK(Str(duel, "again_in0") == "1" && Str(duel, "again_in1") == "1");
            CHECK(Num(duel, "again_health") == 100.0);
            CHECK(Num(duel, "again_frags") == 0.0);
        }

        std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "doom_fly OK" : "doom_fly FAILED", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
}

int main(int argc, char** argv)
{
    std::string scenario;
    std::uint64_t seed = 1;
    int tics = 140;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc)
        {
            scenario = argv[++i];
        }
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
        {
            seed = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (std::strcmp(argv[i], "--tics") == 0 && i + 1 < argc)
        {
            tics = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--verbose") == 0)
        {
            g_verbose = true;
        }
    }
    if (!scenario.empty())
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    }

    std::error_code ec;
    if (!fs::is_regular_file(FbrainPath(), ec))
    {
        std::printf("doom_fly SKIPPED: no connectome (tools/flybrain/fetch_malecns.ps1, or FLYDOOM_FBRAIN)\n");
        return 77;
    }

    if (!scenario.empty())
    {
        if (scenario == "eyes") return ScenarioEyes();
        if (scenario == "controls") return ScenarioControls();
        if (scenario == "steer") return ScenarioSteer();
        if (scenario == "bio") return ScenarioBio();
        if (scenario == "damage") return ScenarioDamage();
        if (scenario == "loom") return ScenarioLoom();
        if (scenario == "loom_learn") return ScenarioLoom(true);
        if (scenario == "learn_shift") return ScenarioLearnShift();
        if (scenario == "seizure") return ScenarioSeizure();
        if (scenario == "duel")
        {
            const fs::path wad = FindWad();
            return wad.empty() ? 77 : ScenarioDuel(wad, tics);
        }
        if (scenario == "play")
        {
            const fs::path wad = FindWad();
            if (wad.empty())
            {
                return 77;
            }
            return ScenarioPlay(wad, seed, tics);
        }
        if (scenario == "intermission")
        {
            const fs::path wad = FindWad();
            return wad.empty() ? 77 : ScenarioIntermission(wad);
        }
        std::printf("unknown scenario %s\n", scenario.c_str());
        return 2;
    }

    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    return RunAll(fs::path(path));
}
