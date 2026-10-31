// WallSenseTests.cpp
// N4: our layer WallSense (game/doomfly/WallSense.h, not the connectome) without a game or a brain: the pictures before a touch are remembered, the same
// picture later brings the stop and the turn away from the half that saw the wall, another picture does not; a memory per place; the file of the trained fly
// (the codes as bytes) gives the same answers; a layer that does not learn keeps its memory.

#include <doomfly/WallSense.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool ok, const std::string& what)
    {
        std::printf("  %s: %s\n", ok ? "ok" : "FAILED", what.c_str());
        g_failures += ok ? 0 : 1;
    }

    // 16 x 12 columns over the picture
    void Columns(std::vector<float>& x, std::vector<float>& y)
    {
        for (int r = 0; r < 12; ++r)
        {
            for (int c = 0; c < 16; ++c)
            {
                x.push_back((c + 0.5f) / 16.0f);
                y.push_back((r + 0.5f) / 12.0f);
            }
        }
    }

    // A picture of a corridor: no wall close, the light by the rows
    std::vector<float> Corridor(float jitter)
    {
        std::vector<float> light;
        for (int r = 0; r < 12; ++r)
        {
            for (int c = 0; c < 16; ++c)
            {
                light.push_back(0.1f + 0.06f * r + 0.05f * std::cos(0.7f * c + jitter));
            }
        }
        return light;
    }

    // A picture: a dark wall with stripes of its own close on the left (side 0) or the right (side 1), the other half the corridor; `jitter` moves it a little
    std::vector<float> Picture(int side, float stripes, float jitter)
    {
        std::vector<float> light = Corridor(jitter);
        for (int r = 0; r < 12; ++r)
        {
            for (int c = 0; c < 16; ++c)
            {
                if (side == 0 ? c < 8 : c >= 8)
                {
                    light[static_cast<size_t>(r) * 16 + c] = 0.2f + 0.15f * std::sin(stripes * (r + jitter)) + 0.01f * c;
                }
            }
        }
        return light;
    }

    // Walks towards the wall on the left five times: the corridor, then the wall for 20 tics, then the touch
    void Teach(doomfly::WallSense& sense)
    {
        for (int round = 0; round < 5; ++round)
        {
            for (int t = 0; t < 40; ++t)
            {
                sense.Tick(Corridor(0.1f * t), false, true);
            }
            for (int t = 0; t < 20; ++t)
            {
                sense.Tick(Picture(0, 1.3f, 0.05f * t), false, true);
            }
            for (int t = 0; t < 10; ++t)
            {
                sense.Tick(Picture(0, 1.3f, 1.0f), true, true);
            }
        }
    }
}

int main()
{
    std::vector<float> x, y;
    Columns(x, y);
    doomfly::WallSenseConfig config;
    config.enabled = true;

    std::printf("WallSense: learning\n");
    doomfly::WallSense sense;
    sense.Build(config, x, y);
    sense.SetPlace("test E1M1");
    sense.Tick(Picture(0, 1.3f, 0.3f), false, false);
    Check(sense.Chance() == 0.0f && sense.Brake() == 0.0f, "an empty memory: no chance, no brake");
    Teach(sense);
    Check(sense.Positives() > 0 && sense.Remembered() > sense.Positives(), "pictures before the touches and others are remembered (" + std::to_string(sense.Positives()) + " of " +
                                                                               std::to_string(sense.Remembered()) + ")");
    sense.Tick(Corridor(0.35f), false, false);
    sense.Tick(Corridor(0.45f), false, false);
    const float corridor = sense.Chance();
    sense.Tick(Picture(0, 1.3f, 0.33f), false, false);
    const float wall = sense.Chance();
    std::printf("  chance: the wall on the left %.2f, the corridor %.2f\n", wall, corridor);
    Check(wall >= config.avoidFrom && corridor < config.avoidUntil, "the wall that came before a touch is seen coming, the corridor is not");
    Check(sense.Brake() == 1.0f && sense.TurnDegrees() > 0.0f, "it stops and turns right, away from the wall on the left");
    const uint32_t rememberedBeforeReset = sense.Remembered();
    sense.ResetTransient();
    Check(sense.Remembered() == rememberedBeforeReset && sense.Brake() == 0.0f, "reset keeps learned pictures and clears the steering state");
    for (int t = 0; t < 3; ++t)
    {
        sense.Tick(Corridor(0.5f), false, false);
    }
    Check(sense.Brake() == 0.0f, "the corridor again: it walks");

    std::printf("WallSense: places\n");
    sense.SetPlace("test E1M2");
    sense.Tick(Picture(0, 1.3f, 0.33f), false, false);
    Check(sense.Chance() == 0.0f && sense.Places() == 2, "another map has its own memory, empty");
    sense.SetPlace("test E1M1");
    sense.Tick(Picture(0, 1.3f, 0.33f), false, false);
    Check(sense.Chance() >= config.avoidFrom, "back on the first map the memory is there");

    std::printf("WallSense: the file of the trained fly\n");
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "myengine_wallsense_test.bin";
    std::string error;
    Check(sense.Save(path.u8string(), &error), "save " + error);
    doomfly::WallSense loaded;
    loaded.Build(config, x, y);
    Check(loaded.Load(path.u8string(), &error), "load " + error);
    loaded.SetPlace("test E1M1");
    loaded.Tick(Picture(0, 1.3f, 0.33f), false, false);
    Check(std::fabs(loaded.Chance() - wall) < 0.05f && loaded.Positives() == sense.Positives(), "the loaded memory (bytes) answers as the one that learned");
    doomfly::WallSense merged;
    merged.Build(config, x, y);
    merged.SetPlace("other E1M5");
    for (int t = 0; t < 30; ++t)
    {
        merged.Tick(Picture(1, 0.7f, 0.05f * t), t >= 20, true);
    }
    const std::filesystem::path other = std::filesystem::temp_directory_path() / "myengine_wallsense_test2.bin";
    Check(merged.Save(other.u8string(), &error), "save another place");
    doomfly::WallSense both;
    both.Build(config, x, y);
    Check(both.Load(path.u8string(), &error, true) && both.Load(other.u8string(), &error, true) && both.Places() == 2, "two files put together keep both places");
    std::filesystem::remove(path);
    std::filesystem::remove(other);

    std::printf("WallSense: not learning\n");
    doomfly::WallSenseConfig frozenConfig = config;
    frozenConfig.learning = false;
    doomfly::WallSense frozen;
    frozen.Build(frozenConfig, x, y);
    frozen.SetPlace("test E1M1");
    Teach(frozen);
    Check(frozen.Remembered() == 0 && frozen.Brake() == 0.0f, "a layer that does not learn remembers nothing and never brakes");

    doomfly::WallSense lifeBoundary;
    lifeBoundary.Build(config, x, y);
    lifeBoundary.SetPlace("test E1M1");
    for (int t = 0; t < 10; ++t) lifeBoundary.Tick(Corridor(0.1f * t), false, true);
    lifeBoundary.ResetTransient();
    lifeBoundary.Tick(Picture(0, 1.3f, 0.3f), true, false);
    Check(lifeBoundary.Positives() == 0, "a touch after respawn does not label pictures from the previous life");

    doomfly::WallSense bundled;
    bundled.Build(config, x, y);
    const std::filesystem::path trained = std::filesystem::path(FLY_SOURCE_DIR) / "assets/flybrain/fly_trained.bin";
    Check(bundled.Load(trained.u8string(), &error), "bundled trained memory loads: " + error);
    Check(bundled.Places() == 18, "bundled memory has the 18 episode-one places");
    for (const char* wad : { "doom1", "freedoom1" })
    {
        for (int map = 1; map <= 9; ++map)
        {
            const std::string place = std::string(wad) + " E1M" + std::to_string(map);
            bundled.SetPlace(place);
            Check(bundled.Remembered() > 0 && bundled.Positives() > 0, "trained samples are active for " + place);
        }
    }

    std::printf(g_failures == 0 ? "OK: WallSense remembers what came before a touch, per place, in its file\n" : "FAILED: %d checks\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
