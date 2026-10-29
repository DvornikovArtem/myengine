// FlyBrainBench.cpp
// N0 (FlyDoom): how fast the fly brain runs compared with real time. Not part of ctest: it takes minutes.
//
//   myengine_flybrain_bench [--fbrain <path> | --synthetic] [--threads 1,2,4,8] [--repeats 3] [--seconds 1] [--warmup 0.2]
//                           [--scenarios silent,rest,vision20,vision50] [--degree 153] [--probe <type>|rest|vision20]
//
// Method of LR1: a warm-up, then N runs, the median. A run is a fresh brain: warm-up of simulated time, then the measured
// simulated seconds. Speed = simulated time / wall time (1.0 = real time).
// Scenarios (DOOMFLY baseline): silent - no input; rest - tonic 12 mV into L1/L2/L3/L5 (the lamina is silent without it);
// vision20 / vision50 - rest plus Poisson input into all R1-R6 at 20 / 50 Hz. On the synthetic graph: 5 % of the neurons
// get the tonic current, 2 % the Poisson input, --degree sets the mean number of targets. Diagnostic scenario awake: every
// neuron gets 8 mV and fires (with --degree 0 it measures the neuron step alone).

#include <flybrain/Connectome.h>
#include <flybrain/FlyBrain.h>

#include <myengine/jobs/JobSystem.h>

#include "SyntheticGraph.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using namespace flybrain;

    std::vector<std::string> Split(const std::string& text)
    {
        std::vector<std::string> parts;
        std::stringstream stream(text);
        std::string part;
        while (std::getline(stream, part, ','))
        {
            if (!part.empty())
            {
                parts.push_back(part);
            }
        }
        return parts;
    }

    double WorkingSetMb()
    {
        PROCESS_MEMORY_COUNTERS counters{};
        GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
        return double(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }

    struct Inputs
    {
        Group tonic;
        Group retina;
    };

    struct Result
    {
        double speed = 0.0; // simulated / wall
        double spikesPerSecond = 0.0; // per simulated second
        double meanRateHz = 0.0;
        double integrateShare = 0.0;
        double deliverShare = 0.0;
        double edgesPerSecond = 0.0; // delivered synapses per simulated second
        double updatesPerWindow = 0.0;
    };

    Result RunOnce(const std::shared_ptr<const Connectome>& connectome, const Inputs& inputs, const std::string& scenario,
        bool parallel, double warmupSeconds, double seconds)
    {
        FlyBrain brain(connectome);
        brain.SetParallel(parallel);
        brain.SetSpikeRingCapacity(0);
        if (scenario == "awake")
        {
            // Diagnostics: every neuron fires tonically (8 mV, ~23 Hz): the cost of a neuron step without sleeping neurons
            Group all;
            for (uint32_t i = 0; i < connectome->NeuronCount(); ++i)
            {
                all.neurons.push_back(i);
            }
            brain.StimulateCurrent(all, 8.0f);
        }
        else if (scenario != "silent")
        {
            brain.StimulateCurrent(inputs.tonic, 12.0f);
        }
        if (scenario == "vision20" || scenario == "vision50")
        {
            brain.StimulateRate(inputs.retina, scenario == "vision20" ? 20.0f : 50.0f);
        }
        brain.AdvanceMs(warmupSeconds * 1000.0);
        brain.ResetStats();

        const auto start = std::chrono::steady_clock::now();
        brain.AdvanceMs(seconds * 1000.0);
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        const BrainStats& stats = brain.Stats();
        Result result;
        result.speed = seconds / wall;
        result.spikesPerSecond = double(stats.spikes) / seconds;
        result.meanRateHz = result.spikesPerSecond / connectome->NeuronCount();
        result.integrateShare = stats.integrateSeconds / wall;
        result.deliverShare = stats.deliverSeconds / wall;
        result.edgesPerSecond = double(stats.deliveredEdges) / seconds;
        result.updatesPerWindow = stats.windows ? double(stats.neuronWindowUpdates) / double(stats.windows) : 0.0;
        return result;
    }
}

int main(int argc, char** argv)
{
    std::string fbrain = std::string(MYENGINE_SOURCE_DIR) + "/assets/flybrain/malecns_v1.fbrain";
    bool synthetic = false;
    uint32_t syntheticDegree = 153;
    std::vector<std::string> threads = { "1", "2", "4", "8" };
    std::vector<std::string> scenarios = { "silent", "rest", "vision20", "vision50" };
    int repeats = 3;
    double seconds = 1.0;
    double warmup = 0.2;
    std::string probeType;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (arg == "--fbrain") fbrain = next();
        else if (arg == "--synthetic") synthetic = true;
        else if (arg == "--degree") syntheticDegree = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--threads") threads = Split(next());
        else if (arg == "--scenarios") scenarios = Split(next());
        else if (arg == "--repeats") repeats = std::max(1, std::stoi(next()));
        else if (arg == "--seconds") seconds = std::stod(next());
        else if (arg == "--warmup") warmup = std::stod(next());
        else if (arg == "--probe") probeType = next();
        else
        {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            return 2;
        }
    }

    std::shared_ptr<const Connectome> connectome;
    Inputs inputs;
    const auto loadStart = std::chrono::steady_clock::now();
    if (synthetic)
    {
        test::SyntheticOptions options;
        options.neurons = 166700;
        options.meanDegree = syntheticDegree;
        options.maxContacts = 30;
        connectome = test::MakeSyntheticConnectome(options);
        for (uint32_t i = 0; i < options.neurons; ++i)
        {
            if (i % 20 == 0) inputs.tonic.neurons.push_back(i);
            if (i % 50 == 7) inputs.retina.neurons.push_back(i);
        }
    }
    else
    {
        std::string error;
        connectome = Connectome::Load(fbrain, &error);
        if (connectome == nullptr)
        {
            std::fprintf(stderr, "cannot load %s: %s (download it with tools/flybrain/fetch_malecns.ps1 or use --synthetic)\n", fbrain.c_str(), error.c_str());
            return 77;
        }
        for (const char* type : { "L1", "L2", "L3", "L5" })
        {
            const Group group = connectome->Find(type);
            inputs.tonic.neurons.insert(inputs.tonic.neurons.end(), group.neurons.begin(), group.neurons.end());
        }
        inputs.retina = connectome->Find("R1-R6");
    }
    const double loadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - loadStart).count();

    std::printf("connectome: %s\n", synthetic ? "synthetic (MaleCNS scale)" : fbrain.c_str());
    std::printf("neurons %u, edges %llu, image %.1f MB, loaded in %.2f s; tonic group %zu, retina group %zu\n",
        connectome->NeuronCount(), static_cast<unsigned long long>(connectome->EdgeCount()), connectome->MemoryBytes() / 1048576.0,
        loadSeconds, inputs.tonic.Size(), inputs.retina.Size());
    {
        FlyBrain probe(connectome);
        std::printf("simulation state %.1f MB; window %u steps of %.2f ms\n", probe.StateBytes() / 1048576.0, probe.StepsPerWindow(), probe.Params().dtMs);
    }
    if (!probeType.empty())
    {
        // Diagnostics: one type at 150 Hz for the first 0.5 s of 1 s (or the scenario input with --probe rest/vision20); spikes by superclass
        myengine::jobs::Initialize();
        FlyBrain brain(connectome);
        brain.SetParallel(true);
        if (probeType == "rest" || probeType == "vision20")
        {
            brain.StimulateCurrent(inputs.tonic, 12.0f);
            if (probeType == "vision20")
            {
                brain.StimulateRate(inputs.retina, 20.0f);
            }
        }
        else
        {
            brain.StimulateRate(connectome->Find(probeType), 150.0f, 500.0f);
        }
        std::vector<double> rateAt;
        for (int k = 0; k < 10; ++k)
        {
            const uint64_t before = brain.TotalSpikes();
            brain.AdvanceMs(100.0);
            rateAt.push_back(double(brain.TotalSpikes() - before) * 10.0);
        }
        std::printf("probe %s: spikes per second of the whole brain, every 100 ms:", probeType.c_str());
        for (double r : rateAt)
        {
            std::printf(" %.0f", r);
        }
        std::printf("\n");
        std::map<std::string, std::pair<uint64_t, uint32_t>> bySuperclass; // spikes, neurons
        uint32_t silent = 0;
        for (uint32_t i = 0; i < connectome->NeuronCount(); ++i)
        {
            auto& entry = bySuperclass[std::string(connectome->Superclass(i))];
            entry.first += brain.NeuronSpikeCount(i);
            ++entry.second;
            silent += brain.NeuronSpikeCount(i) == 0 ? 1 : 0;
        }
        std::printf("neurons that never fired: %u of %u\n| superclass | neurons | mean rate, Hz |\n| --- | --- | --- |\n", silent, connectome->NeuronCount());
        for (const auto& [name, entry] : bySuperclass)
        {
            std::printf("| %s | %u | %.2f |\n", name.c_str(), entry.second, double(entry.first) / entry.second);
        }
        myengine::jobs::Shutdown();
        return 0;
    }
    std::printf("method: warm-up %.2f s simulated, %.2f s measured, median of %d runs\n\n", warmup, seconds, repeats);
    std::printf("| scenario | threads | x real time (median) | min..max | spikes/s | mean rate, Hz | synapses/s | neurons/window | integrate | deliver |\n");
    std::printf("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n");

    for (const std::string& threadText : threads)
    {
        const int threadCount = std::max(1, std::stoi(threadText));
        const bool parallel = threadCount > 1;
        if (parallel)
        {
            // The main thread runs jobs while it waits, so threadCount - 1 workers make threadCount threads
            myengine::jobs::Initialize(static_cast<uint32_t>(threadCount - 1));
        }
        for (const std::string& scenario : scenarios)
        {
            RunOnce(connectome, inputs, scenario, parallel, warmup, std::min(seconds, 0.1)); // warm-up run (caches, page faults)
            std::vector<Result> results;
            for (int r = 0; r < repeats; ++r)
            {
                results.push_back(RunOnce(connectome, inputs, scenario, parallel, warmup, seconds));
            }
            std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) { return a.speed < b.speed; });
            const Result& median = results[results.size() / 2];
            std::printf("| %s | %d | %.2f | %.2f..%.2f | %.0f | %.3f | %.3g | %.0f | %.0f %% | %.0f %% |\n", scenario.c_str(), threadCount,
                median.speed, results.front().speed, results.back().speed, median.spikesPerSecond, median.meanRateHz, median.edgesPerSecond,
                median.updatesPerWindow, 100.0 * median.integrateShare, 100.0 * median.deliverShare);
            std::fflush(stdout);
        }
        if (parallel)
        {
            myengine::jobs::Shutdown();
        }
    }
    std::printf("\npeak working set %.0f MB\n", WorkingSetMb());
    return 0;
}
