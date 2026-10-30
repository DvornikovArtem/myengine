// FlyVision.cpp (D1): what the motion and looming neurons answer to, with the input of the columns (manual tool)
//   myengine_doomfly_vision [gpu|cpu] [onGain] [offGain] [sustained 0|1]
#include <doomfly/FlyEyes.h>
#include <flybrain/FlyBrain.h>

#include <myengine/jobs/JobSystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>

using namespace flybrain;

namespace
{
    constexpr int kW = 320;
    constexpr int kH = 200;
    constexpr int kViewH = 168;

    uint32_t Gray(float v)
    {
        const uint32_t c = static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, v * 255.0f)));
        return (c << 16) | (c << 8) | c;
    }

    struct Scene
    {
        const char* name;
        std::function<float(int, int, int)> luminance;   // x, y, tic
    };

    struct Column
    {
        uint32_t neuron;
        int column;
        bool on;
    };
}

int main(int argc, char** argv)
{
    myengine::jobs::Initialize();
    std::string error;
    auto connectome = Connectome::Load("assets/flybrain/malecns_v1.fbrain", &error);
    if (!connectome)
    {
        std::printf("%s\n", error.c_str());
        return 77;
    }
    const std::string backend = argc > 1 ? argv[1] : "gpu";
    const float onGain = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 0.0f;
    const float offGain = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 0.0f;
    const bool sustained = argc > 4 && std::atoi(argv[4]) != 0;
    const char* onTypes[] = {"Mi1", "Tm3", "Mi4", "Mi9"};
    const char* offTypes[] = {"Tm1", "Tm2", "Tm4", "Tm9"};

    doomfly::EyesConfig config;
    doomfly::FlyEyes eyes;
    if (!eyes.Build(*connectome, config, error))
    {
        std::printf("eyes: %s\n", error.c_str());
        return 1;
    }
    std::vector<Column> columns;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (const char* type : pass == 0 ? std::vector<const char*>(std::begin(onTypes), std::end(onTypes)) : std::vector<const char*>(std::begin(offTypes), std::end(offTypes)))
        {
            size_t used = 0;
            for (uint32_t n : connectome->Find(type).neurons)
            {
                const int column = eyes.ColumnOf(*connectome, n);
                if (column >= 0)
                {
                    columns.push_back({n, column, pass == 0});
                    ++used;
                }
            }
            std::printf("%s: %zu neurons in columns\n", type, used);
        }
    }

    const Scene scenes[] = {
        {"static", [](int, int, int) { return 0.3f; }},
        {"loom left", [](int x, int y, int t) { const float r = 3.0f + 2.4f * static_cast<float>(t % 28); const float dx = static_cast<float>(x - 80), dy = static_cast<float>(y - 84); return dx * dx + dy * dy < r * r ? 0.02f : 0.5f; }},
        {"loom right", [](int x, int y, int t) { const float r = 3.0f + 2.4f * static_cast<float>(t % 28); const float dx = static_cast<float>(x - 240), dy = static_cast<float>(y - 84); return dx * dx + dy * dy < r * r ? 0.02f : 0.5f; }},
        {"loom centre", [](int x, int y, int t) { const float r = 3.0f + 2.4f * static_cast<float>(t % 28); const float dx = static_cast<float>(x - 160), dy = static_cast<float>(y - 84); return dx * dx + dy * dy < r * r ? 0.02f : 0.5f; }},
        {"bright loom L", [](int x, int y, int t) { const float r = 3.0f + 2.4f * static_cast<float>(t % 28); const float dx = static_cast<float>(x - 80), dy = static_cast<float>(y - 84); return dx * dx + dy * dy < r * r ? 0.9f : 0.1f; }},
        {"bright loom C", [](int x, int y, int t) { const float r = 3.0f + 2.4f * static_cast<float>(t % 28); const float dx = static_cast<float>(x - 160), dy = static_cast<float>(y - 84); return dx * dx + dy * dy < r * r ? 0.9f : 0.1f; }},
        {"bright loom R", [](int x, int y, int t) { const float r = 3.0f + 2.4f * static_cast<float>(t % 28); const float dx = static_cast<float>(x - 240), dy = static_cast<float>(y - 84); return dx * dx + dy * dy < r * r ? 0.9f : 0.1f; }},
        {"bar right", [](int x, int, int t) { const int p = (t * 5) % 340; return x >= p - 24 && x < p ? 0.9f : 0.1f; }},
        {"bar left", [](int x, int, int t) { const int p = 340 - (t * 5) % 340; return x >= p && x < p + 24 ? 0.9f : 0.1f; }},
    };

    struct Watched
    {
        const char* label;
        Group group;
    };
    std::vector<Watched> watched;
    auto add = [&](const char* label, const char* type, Side side) { watched.push_back({label, connectome->Find(type, side)}); };
    auto addPrefix = [&](const char* label, const char* prefix, Side side) { watched.push_back({label, connectome->FindPrefix(prefix, side)}); };
    addPrefix("T4 L", "T4", Side::Left);
    addPrefix("T4 R", "T4", Side::Right);
    addPrefix("T5 L", "T5", Side::Left);
    addPrefix("T5 R", "T5", Side::Right);
    add("LPLC2 L", "LPLC2", Side::Left);
    add("LPLC2 R", "LPLC2", Side::Right);
    add("LC4 L", "LC4", Side::Left);
    add("LC4 R", "LC4", Side::Right);
    add("DNp01 L", "DNp01", Side::Left);
    add("DNp01 R", "DNp01", Side::Right);
    add("DNp20 L", "DNp20", Side::Left);
    add("DNp20 R", "DNp20", Side::Right);
    add("DNpe017", "DNpe017", Side::Any);
    add("DNa02 L", "DNa02", Side::Left);
    add("DNa02 R", "DNa02", Side::Right);
    add("DNp09", "DNp09", Side::Any);
    add("MDN", "MDN", Side::Any);
    std::printf("motion/looming input: on gain %.1f, off gain %.1f, sustained %d; backend %s\n", onGain, offGain, sustained ? 1 : 0, backend.c_str());
    const Group central = connectome->FindSuperclass("cb_intrinsic");
    std::printf("%-14s", "scene");
    for (const Watched& w : watched) std::printf(" %8s", w.label);
    std::printf(" %9s %9s\n", "spikes/s", "central/s");

    for (const Scene& scene : scenes)
    {
        FlyBrain brain(connectome);
        brain.SetParallel(true);
        if (backend != "cpu")
        {
            std::string e;
            BackendChoice choice = BackendChoice::Gpu;
            if (brain.SetBackend(choice, {}, &e) != Backend::Gpu) std::printf("(GPU: %s)\n", e.c_str());
        }
        brain.StimulateCurrent(eyes.Lamina(), 12.0f);
        std::vector<WatchId> ids;
        for (const Watched& w : watched) ids.push_back(brain.Watch(w.group, 2000.0f));
        const WatchId centralWatch = brain.Watch(central, 2000.0f);

        std::vector<uint32_t> frame(kW * kH);
        std::vector<float> lum, columnLum, slow;
        std::vector<float> lastReceptor(eyes.Receptors().size(), -1.0f), lastDrive(columns.size(), -1.0f);
        uint64_t spikes0 = 0;
        const int tics = 70;
        for (int tic = 0; tic < tics; ++tic)
        {
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x) frame[y * kW + x] = Gray(scene.luminance(x, y * 168 / 200 < kViewH ? y : kViewH - 1, tic));
            eyes.SampleLuminance(frame.data(), kW, kH, lum);
            for (size_t i = 0; i < lum.size(); ++i) brain.SetDrive(eyes.Receptors()[i].neuron, eyes.ResponseMv(lum[i]));
            eyes.SampleColumns(frame.data(), kW, kH, columnLum);
            if (slow.empty()) slow = columnLum;
            const float alpha = 1.0f - std::exp(-28.57f / 100.0f);
            for (size_t i = 0; i < columns.size(); ++i)
            {
                const int c = columns[i].column;
                const float contrast = std::clamp((columnLum[c] - slow[c]) / (slow[c] + 0.05f), -1.0f, 1.0f);
                float drive = 0.0f;
                if (columns[i].on) drive = onGain * std::max(contrast, 0.0f) + (sustained ? onGain * 0.3f * columnLum[c] : 0.0f);
                else drive = offGain * std::max(-contrast, 0.0f) + (sustained ? offGain * 0.3f * (1.0f - columnLum[c]) : 0.0f);
                if (std::fabs(drive - lastDrive[i]) > 0.05f)
                {
                    lastDrive[i] = drive;
                    brain.SetDrive(columns[i].neuron, drive);
                }
            }
            for (size_t c = 0; c < columnLum.size(); ++c) slow[c] += (columnLum[c] - slow[c]) * alpha;
            brain.AdvanceMs(1000.0 / 35.0);
            if (tic == 14) spikes0 = brain.TotalSpikes();
        }
        std::printf("%-14s", scene.name);
        for (size_t i = 0; i < ids.size(); ++i) std::printf(" %8.1f", brain.RateHz(ids[i], 1500.0f));
        std::printf(" %9.0f %9.0f\n", static_cast<double>(brain.TotalSpikes() - spikes0) / ((tics - 15) / 35.0), static_cast<double>(brain.SpikeCount(centralWatch, 1500.0f)) / 1.5);
    }
    myengine::jobs::Shutdown();
    return 0;
}
