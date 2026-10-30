// FlyProbe3.cpp: what the fly brain does with synthetic pictures: the rates of the descending neurons for a dark frame, a uniform one, a spot on
// the left / right / in the middle, a wall that grows (manual tool for the controls of the fly player, N1)
#include <doomfly/FlyEyes.h>
#include <flybrain/FlyBrain.h>

#include <myengine/jobs/JobSystem.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

using namespace flybrain;

namespace
{
    constexpr int kW = 320;
    constexpr int kH = 200;

    uint32_t Gray(float v)
    {
        const uint32_t c = static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, v * 255.0f)));
        return (c << 16) | (c << 8) | c;
    }

    std::vector<uint32_t> Frame(const std::function<float(int, int)>& luminance)
    {
        std::vector<uint32_t> frame(kW * kH);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) frame[y * kW + x] = Gray(luminance(x, y));
        return frame;
    }
}

int main(int argc, char** argv)
{
    myengine::jobs::Initialize();
    std::string error;
    auto connectome = Connectome::Load(argc > 1 ? argv[1] : "assets/flybrain/malecns_v1.fbrain", &error);
    if (!connectome)
    {
        std::printf("%s\n", error.c_str());
        return 77;
    }
    const float tonic = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 12.0f;
    doomfly::EyesConfig config;
    config.tonicMv = tonic;
    doomfly::FlyEyes eyes;
    if (!eyes.Build(*connectome, config, error))
    {
        std::printf("eyes: %s\n", error.c_str());
        return 1;
    }
    const auto& info = eyes.Info();
    std::printf("receptors L %u (no column %u), R %u (no column %u); columns L %u covered %u, R %u covered %u\n", info.receptors[0], info.withoutColumn[0], info.receptors[1],
                info.withoutColumn[1], info.columns[0], info.columnsCovered[0], info.columns[1], info.columnsCovered[1]);

    struct Scene
    {
        const char* name;
        std::function<float(int, int)> luminance;
    };
    const float dim = 0.03f;
    const Scene scenes[] = {
        {"black", [](int, int) { return 0.0f; }},
        {"dim", [dim](int, int) { return dim; }},
        {"gray", [](int, int) { return 0.2f; }},
        {"bright", [](int, int) { return 0.8f; }},
        {"spot left", [dim](int x, int y) { return (x > 30 && x < 90 && y > 50 && y < 120) ? 0.9f : dim; }},
        {"spot right", [dim](int x, int y) { return (x > 230 && x < 290 && y > 50 && y < 120) ? 0.9f : dim; }},
        {"spot centre", [dim](int x, int y) { return (x > 130 && x < 190 && y > 50 && y < 120) ? 0.9f : dim; }},
        {"left half", [dim](int x, int) { return x < 160 ? 0.6f : dim; }},
        {"right half", [dim](int x, int) { return x >= 160 ? 0.6f : dim; }},
    };

    const Group dnaR = connectome->Find("DNa02", Side::Right), dnaL = connectome->Find("DNa02", Side::Left);
    const Group dn1R = connectome->Find("DNa01", Side::Right), dn1L = connectome->Find("DNa01", Side::Left);
    const Group p9 = connectome->Find("DNp09"), mdn = connectome->Find("MDN"), gf = connectome->Find("DNp01"), mn9 = connectome->Find("MN9");
    const Group p20R = connectome->Find("DNp20", Side::Right), p20L = connectome->Find("DNp20", Side::Left), pe = connectome->Find("DNpe017");
    const Group lplc2 = connectome->Find("LPLC2"), lc4 = connectome->Find("LC4");
    const Group hsn = connectome->FindPrefix("HS");

    std::printf("%-12s %7s %7s | %6s %6s %6s %6s | %6s %6s %6s %6s | %6s %6s %6s %6s | %8s %6s %s\n", "scene", "x", "speed", "a2L", "a2R", "a1L", "a1R", "p9", "mdn", "gf", "mn9", "p20L",
                "p20R", "pe17", "lplc2", "spk/s", "lc4", "(Hz per neuron, 2 s)");
    for (const Scene& scene : scenes)
    {
        FlyBrain brain(connectome);
        brain.SetParallel(true);
        brain.StimulateCurrent(eyes.Lamina(), tonic);
        const std::vector<uint32_t> frame = Frame(scene.luminance);
        std::vector<float> lum;
        eyes.SampleLuminance(frame.data(), kW, kH, lum);
        for (size_t i = 0; i < lum.size(); ++i) brain.SetDrive(eyes.Receptors()[i].neuron, eyes.ResponseMv(lum[i]));
        const WatchId wa2L = brain.Watch(dnaL), wa2R = brain.Watch(dnaR), wa1L = brain.Watch(dn1L), wa1R = brain.Watch(dn1R), wp9 = brain.Watch(p9), wmdn = brain.Watch(mdn),
                      wgf = brain.Watch(gf), wmn9 = brain.Watch(mn9), wp20L = brain.Watch(p20L), wp20R = brain.Watch(p20R), wpe = brain.Watch(pe), wlp = brain.Watch(lplc2), wlc4 = brain.Watch(lc4);
        const auto start = std::chrono::steady_clock::now();
        brain.AdvanceMs(500.0);
        const uint64_t spikes0 = brain.TotalSpikes();
        brain.AdvanceMs(2000.0);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("%-12s %7.2f %6.2fx | %6.1f %6.1f %6.1f %6.1f | %6.1f %6.1f %6.1f %6.1f | %6.1f %6.1f %6.1f %6.1f | %8.0f %6.1f\n", scene.name, 0.0, 2.5 / seconds,
                    brain.RateHz(wa2L, 2000.0f), brain.RateHz(wa2R, 2000.0f), brain.RateHz(wa1L, 2000.0f), brain.RateHz(wa1R, 2000.0f), brain.RateHz(wp9, 2000.0f), brain.RateHz(wmdn, 2000.0f),
                    brain.RateHz(wgf, 2000.0f), brain.RateHz(wmn9, 2000.0f), brain.RateHz(wp20L, 2000.0f), brain.RateHz(wp20R, 2000.0f), brain.RateHz(wpe, 2000.0f), brain.RateHz(wlp, 2000.0f),
                    (brain.TotalSpikes() - spikes0) / 2.0, brain.RateHz(wlc4, 2000.0f));
    }
    myengine::jobs::Shutdown();
    return 0;
}
