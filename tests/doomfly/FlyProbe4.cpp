// FlyProbe4.cpp: which descending neurons answer to the picture, and on which side (manual tool for the controls of the fly player, N1)
#include <doomfly/FlyEyes.h>
#include <flybrain/FlyBrain.h>

#include <myengine/jobs/JobSystem.h>

#include <algorithm>
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

    uint32_t Gray(float v)
    {
        const uint32_t c = static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, v * 255.0f)));
        return (c << 16) | (c << 8) | c;
    }
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
    doomfly::EyesConfig config;
    config.tonicMv = argc > 1 ? static_cast<float>(std::atof(argv[1])) : 12.0f;
    config.maxDriveMv = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 30.0f;
    const float seconds = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 2.0f;
    doomfly::FlyEyes eyes;
    if (!eyes.Build(*connectome, config, error))
    {
        std::printf("eyes: %s\n", error.c_str());
        return 1;
    }

    // every descending neuron: rate in four scenes
    struct Scene
    {
        const char* name;
        std::function<float(int, int)> luminance;
    };
    const float dim = 0.03f;
    const Scene scenes[] = {
        {"dark", [](int, int) { return 0.0f; }},
        {"bright", [](int, int) { return 0.8f; }},
        {"left", [dim](int x, int) { return x < 160 ? 0.6f : dim; }},
        {"right", [dim](int x, int) { return x >= 160 ? 0.6f : dim; }},
    };
    const Group dns = connectome->FindSuperclass("descending_neuron");
    std::printf("descending neurons: %zu; tonic %.1f mV, max drive %.1f mV, %.1f s per scene\n", dns.Size(), config.tonicMv, config.maxDriveMv, seconds);
    std::vector<std::vector<float>> rates(4, std::vector<float>(dns.Size()));
    std::vector<double> total(4);
    for (int s = 0; s < 4; ++s)
    {
        FlyBrain brain(connectome);
        brain.SetParallel(true);
        brain.StimulateCurrent(eyes.Lamina(), config.tonicMv);
        std::vector<uint32_t> frame(kW * kH);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) frame[y * kW + x] = Gray(scenes[s].luminance(x, y));
        std::vector<float> lum;
        eyes.SampleLuminance(frame.data(), kW, kH, lum);
        for (size_t i = 0; i < lum.size(); ++i) brain.SetDrive(eyes.Receptors()[i].neuron, eyes.ResponseMv(lum[i]));
        brain.AdvanceMs(500.0);
        std::vector<uint32_t> before(dns.Size());
        for (size_t i = 0; i < dns.Size(); ++i) before[i] = brain.NeuronSpikeCount(dns.neurons[i]);
        const uint64_t spikes0 = brain.TotalSpikes();
        brain.AdvanceMs(seconds * 1000.0);
        for (size_t i = 0; i < dns.Size(); ++i) rates[s][i] = static_cast<float>(brain.NeuronSpikeCount(dns.neurons[i]) - before[i]) / seconds;
        total[s] = static_cast<double>(brain.TotalSpikes() - spikes0) / seconds;
        std::printf("scene %-7s total %.0f spikes/s\n", scenes[s].name, total[s]);
    }

    // per type and side: mean rate per scene
    struct Row
    {
        std::string type;
        Side side;
        float mean[4];
        int count;
    };
    std::map<std::string, Row> rows;
    for (size_t i = 0; i < dns.Size(); ++i)
    {
        const uint32_t n = dns.neurons[i];
        const std::string key = std::string(connectome->Type(n)) + (connectome->NeuronSide(n) == Side::Left ? "/L" : connectome->NeuronSide(n) == Side::Right ? "/R" : "/-");
        Row& row = rows[key];
        row.type = std::string(connectome->Type(n));
        row.side = connectome->NeuronSide(n);
        row.count++;
        for (int s = 0; s < 4; ++s) row.mean[s] = (row.count == 1 ? 0.0f : row.mean[s]) + rates[s][i];
    }
    std::vector<Row> list;
    for (auto& r : rows)
    {
        for (int s = 0; s < 4; ++s) r.second.mean[s] /= r.second.count;
        list.push_back(r.second);
    }
    // the most active
    std::sort(list.begin(), list.end(), [](const Row& a, const Row& b) { return std::max(a.mean[1], std::max(a.mean[2], a.mean[3])) > std::max(b.mean[1], std::max(b.mean[2], b.mean[3])); });
    std::printf("%-14s %3s %4s | %7s %7s %7s %7s\n", "type", "n", "side", "dark", "bright", "left", "right");
    for (size_t i = 0; i < std::min<size_t>(list.size(), 40); ++i)
        std::printf("%-14s %3d %4s | %7.1f %7.1f %7.1f %7.1f\n", list[i].type.c_str(), list[i].count, ToString(list[i].side), list[i].mean[0], list[i].mean[1], list[i].mean[2], list[i].mean[3]);
    // pairs with a difference between the sides that follows the light
    std::printf("-- pairs L/R: (R-L) for the left scene, for the right scene; a steering pair has them of opposite signs\n");
    std::map<std::string, std::pair<const Row*, const Row*>> pairs;
    for (const Row& r : list)
    {
        if (r.side == Side::Left) pairs[r.type].first = &r;
        if (r.side == Side::Right) pairs[r.type].second = &r;
    }
    struct Pair
    {
        std::string type;
        float left, right, level;
    };
    std::vector<Pair> pl;
    for (auto& p : pairs)
        if (p.second.first && p.second.second)
        {
            const float rl = p.second.second->mean[2] - p.second.first->mean[2];
            const float rr = p.second.second->mean[3] - p.second.first->mean[3];
            pl.push_back({p.first, rl, rr, std::max(p.second.first->mean[3], p.second.second->mean[3])});
        }
    std::sort(pl.begin(), pl.end(), [](const Pair& a, const Pair& b) { return (a.right - a.left) > (b.right - b.left); });
    for (size_t i = 0; i < std::min<size_t>(pl.size(), 15); ++i) std::printf("%-14s R-L left-scene %7.1f right-scene %7.1f  (difference %7.1f)\n", pl[i].type.c_str(), pl[i].left, pl[i].right, pl[i].right - pl[i].left);
    std::printf("...\n");
    for (size_t i = pl.size() > 6 ? pl.size() - 6 : 0; i < pl.size(); ++i) std::printf("%-14s R-L left-scene %7.1f right-scene %7.1f  (difference %7.1f)\n", pl[i].type.c_str(), pl[i].left, pl[i].right, pl[i].right - pl[i].left);
    myengine::jobs::Shutdown();
    return 0;
}
