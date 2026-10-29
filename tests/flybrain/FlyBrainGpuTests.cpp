// FlyBrainGpuTests.cpp
// N0b (FlyDoom): the GPU backend of the fly brain against the CPU reference. Exit code 77 (skipped) when there is no DX12
// adapter with double precision in compute shaders (WARP is used when there is no hardware one).
//   - the built-in self-test of SetBackend(Auto) passes
//   - a synthetic graph of 40,000 neurons with tonic drives, Poisson input, stimuli that end mid-window, per-tick drive
//     changes and DOOM ticks of 28.57 ms: the same spikes as on the CPU (every spike, every step), the same spike count
//     of every neuron, the same membrane potentials, the same watch counts
//   - the state moves between the backends at any step: CPU -> GPU -> CPU gives the run of the CPU alone
//   - Advance does not wait for the GPU: CompletedStep() lags behind Step() until Synchronize
//   - the real connectome (when downloaded): visual input for 300 ms, the same spikes as on the CPU
//
//   myengine_flybrain_gpu_tests [--hardware-only]

#include <flybrain/Connectome.h>
#include <flybrain/FlyBrain.h>

#include <myengine/jobs/JobSystem.h>

#include "SyntheticGraph.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using namespace flybrain;

    constexpr int kSkipped = 77;
    bool gAllowWarp = true;

    struct Skip
    {
        std::string reason;
    };

    void Check(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::vector<SpikeEvent> SortedSpikes(const FlyBrain& brain)
    {
        std::vector<SpikeEvent> spikes;
        brain.RecentSpikes(spikes, 1u << 22);
        std::sort(spikes.begin(), spikes.end(), [](const SpikeEvent& a, const SpikeEvent& b) { return a.step != b.step ? a.step < b.step : a.neuron < b.neuron; });
        return spikes;
    }

    void CheckSameSpikes(const std::vector<SpikeEvent>& cpu, const std::vector<SpikeEvent>& gpu, const std::string& what)
    {
        size_t first = 0;
        while (first < cpu.size() && first < gpu.size() && cpu[first].neuron == gpu[first].neuron && cpu[first].step == gpu[first].step)
        {
            ++first;
        }
        if (first != cpu.size() || cpu.size() != gpu.size())
        {
            std::string detail = what + ": CPU " + std::to_string(cpu.size()) + " spikes, GPU " + std::to_string(gpu.size()) + ", first difference at #" + std::to_string(first);
            if (first < cpu.size())
            {
                detail += " CPU (step " + std::to_string(cpu[first].step) + ", neuron " + std::to_string(cpu[first].neuron) + ")";
            }
            if (first < gpu.size())
            {
                detail += " GPU (step " + std::to_string(gpu[first].step) + ", neuron " + std::to_string(gpu[first].neuron) + ")";
            }
            throw std::runtime_error(detail);
        }
    }

    GpuOptions Options()
    {
        GpuOptions options;
        options.allowWarp = gAllowWarp;
        options.selfTest = false;
        return options;
    }

    std::string gAdapter;

    void TestAvailableAndSelfTest()
    {
        test::SyntheticOptions small;
        small.neurons = 100;
        auto connectome = test::MakeSyntheticConnectome(small);
        FlyBrain brain(connectome);
        std::string error;
        if (brain.SetBackend(BackendChoice::Gpu, Options(), &error) != Backend::Gpu)
        {
            throw Skip{ error };
        }
        gAdapter = brain.GpuAdapterName();
        std::printf("  adapter: %s\n", gAdapter.c_str());

        FlyBrain checked(connectome);
        GpuOptions options = Options();
        options.selfTest = true;
        Check(checked.SetBackend(BackendChoice::Gpu, options, &error) == Backend::Gpu, "the self-test of the GPU backend passes: " + error);
        FlyBrain automatic(connectome);
        GpuOptions autoOptions = Options();
        autoOptions.selfTest = true;
        Check(automatic.SetBackend(BackendChoice::Auto, autoOptions) == Backend::Gpu, "Auto takes the GPU when it passes the self-test");
        BackendChoice choice;
        Check(ParseBackendChoice("gpu", choice) && choice == BackendChoice::Gpu && ParseBackendChoice("auto", choice) && !ParseBackendChoice("x", choice),
            "--flybrain values are parsed");
    }

    // The same scenario on any brain: what a DOOM tick does to the fly
    void Scenario(FlyBrain& brain, const Connectome& cn, const std::vector<float>& drive, const Group& poisson, std::vector<WatchId>& watches)
    {
        Group left;
        Group right;
        for (uint32_t i = 0; i < cn.NeuronCount(); ++i)
        {
            (cn.NeuronSide(i) == Side::Left ? left : right).neurons.push_back(i);
        }
        watches.push_back(brain.Watch(left, 1000.0f));
        watches.push_back(brain.Watch(right, 1000.0f));
        for (uint32_t i = 0; i < cn.NeuronCount(); ++i)
        {
            brain.SetDrive(i, drive[i]);
        }
        brain.StimulateRate(poisson, 60.0f, 33.3f); // ends in the middle of a window
        for (int tick = 0; tick < 14; ++tick)
        {
            // Per-tick drive changes, like photoreceptors following the frame
            for (uint32_t i = static_cast<uint32_t>(tick) % 97; i < cn.NeuronCount(); i += 97)
            {
                brain.SetDrive(i, drive[i] + static_cast<float>(tick % 5));
            }
            if (tick == 5)
            {
                brain.StimulateCurrent(poisson, 9.0f, 20.0f);
            }
            brain.AdvanceMs(1000.0 / 35.0);
        }
        brain.Advance(7);
        brain.Synchronize();
    }

    std::vector<float> RandomDrive(uint32_t n, uint64_t seed)
    {
        std::vector<float> drive(n, 0.0f);
        uint64_t rng = seed;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (test::Uniform(rng) < 0.05)
            {
                drive[i] = static_cast<float>(8.0 + 12.0 * test::Uniform(rng));
            }
        }
        return drive;
    }

    void TestSyntheticSameAsCpu()
    {
        test::SyntheticOptions options;
        options.neurons = 40000;
        options.meanDegree = 60;
        options.maxContacts = 6;
        auto connectome = test::MakeSyntheticConnectome(options);
        const std::vector<float> drive = RandomDrive(options.neurons, 5);
        Group poisson;
        for (uint32_t i = 3; i < options.neurons; i += 50)
        {
            poisson.neurons.push_back(i);
        }

        FlyBrain cpu(connectome);
        cpu.SetSpikeRingCapacity(1u << 22);
        cpu.SetParallel(true);
        std::vector<WatchId> cpuWatches;
        Scenario(cpu, *connectome, drive, poisson, cpuWatches);

        FlyBrain gpu(connectome);
        gpu.SetSpikeRingCapacity(1u << 22);
        Check(gpu.SetBackend(BackendChoice::Gpu, Options()) == Backend::Gpu, "GPU backend");
        std::vector<WatchId> gpuWatches;
        Scenario(gpu, *connectome, drive, poisson, gpuWatches);

        const std::vector<SpikeEvent> cpuSpikes = SortedSpikes(cpu);
        const std::vector<SpikeEvent> gpuSpikes = SortedSpikes(gpu);
        std::printf("  synthetic 40k: %zu spikes on the CPU, %zu on the GPU, steps %llu\n", cpuSpikes.size(), gpuSpikes.size(),
            static_cast<unsigned long long>(gpu.Step()));
        Check(cpuSpikes.size() > 20000, "the network is active");
        CheckSameSpikes(cpuSpikes, gpuSpikes, "synthetic graph");
        Check(cpu.TotalSpikes() == gpu.TotalSpikes(), "the same total of the whole brain");
        for (size_t w = 0; w < cpuWatches.size(); ++w)
        {
            Check(cpu.SpikeCount(cpuWatches[w], 400.0f) == gpu.SpikeCount(gpuWatches[w], 400.0f) && cpu.TotalSpikes(cpuWatches[w]) == gpu.TotalSpikes(gpuWatches[w]),
                "the same watch counts");
        }
        uint32_t differentVoltage = 0;
        for (uint32_t i = 0; i < options.neurons; ++i)
        {
            Check(cpu.NeuronSpikeCount(i) == gpu.NeuronSpikeCount(i), "the same spike count of neuron " + std::to_string(i));
            const float a = cpu.VoltageMv(i);
            const float b = gpu.VoltageMv(i);
            differentVoltage += std::memcmp(&a, &b, sizeof(a)) != 0 ? 1 : 0;
        }
        Check(differentVoltage == 0, std::to_string(differentVoltage) + " neurons have another membrane potential on the GPU");
        Check(cpu.AwakeCount() == gpu.AwakeCount(), "the same neurons are awake");
    }

    void TestSwitchBackends()
    {
        test::SyntheticOptions options;
        options.neurons = 5000;
        options.meanDegree = 40;
        options.maxContacts = 6;
        auto connectome = test::MakeSyntheticConnectome(options);
        const std::vector<float> drive = RandomDrive(options.neurons, 9);
        Group poisson;
        for (uint32_t i = 1; i < options.neurons; i += 40)
        {
            poisson.neurons.push_back(i);
        }
        auto run = [&](bool switching)
        {
            FlyBrain brain(connectome);
            brain.SetSpikeRingCapacity(1u << 20);
            for (uint32_t i = 0; i < options.neurons; ++i)
            {
                brain.SetDrive(i, drive[i]);
            }
            brain.StimulateRate(poisson, 70.0f);
            brain.Advance(511);
            if (switching)
            {
                Check(brain.SetBackend(BackendChoice::Gpu, Options()) == Backend::Gpu, "to the GPU");
            }
            brain.Advance(1013);
            if (switching)
            {
                Check(brain.SetBackend(BackendChoice::Cpu) == Backend::Cpu, "back to the CPU");
            }
            brain.Advance(499);
            std::vector<uint32_t> counts(options.neurons);
            for (uint32_t i = 0; i < options.neurons; ++i)
            {
                counts[i] = brain.NeuronSpikeCount(i);
            }
            return counts;
        };
        const std::vector<uint32_t> alone = run(false);
        const std::vector<uint32_t> switched = run(true);
        Check(alone == switched, "CPU -> GPU -> CPU gives the run of the CPU");
    }

    void TestAsync()
    {
        test::SyntheticOptions options;
        options.neurons = 20000;
        auto connectome = test::MakeSyntheticConnectome(options);
        FlyBrain brain(connectome);
        Check(brain.SetBackend(BackendChoice::Gpu, Options()) == Backend::Gpu, "GPU backend");
        Group all;
        for (uint32_t i = 0; i < options.neurons; i += 7)
        {
            all.neurons.push_back(i);
        }
        brain.StimulateCurrent(all, 9.0f);
        const auto start = std::chrono::steady_clock::now();
        brain.AdvanceMs(500.0);
        const double recordMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const uint64_t completedBefore = brain.CompletedStep();
        brain.Synchronize();
        const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("  async: 500 ms of the brain recorded in %.1f ms, done in %.1f ms; completed step %llu of %llu before Synchronize\n",
            recordMs, totalMs, static_cast<unsigned long long>(completedBefore), static_cast<unsigned long long>(brain.Step()));
        Check(brain.CompletedStep() == brain.Step(), "Synchronize brings CompletedStep to Step");
        Check(completedBefore <= brain.Step(), "the counts never run ahead of the simulation");
    }

    void TestConnectome()
    {
        const std::string path = std::string(MYENGINE_SOURCE_DIR) + "/assets/flybrain/malecns_v1.fbrain";
        if (!std::filesystem::exists(std::filesystem::u8path(path)))
        {
            std::printf("  real connectome: not downloaded, skipped\n");
            return;
        }
        std::string error;
        const auto connectome = Connectome::Load(path, &error);
        Check(connectome != nullptr, error);
        Group tonic;
        for (const char* type : { "L1", "L2", "L3", "L5" })
        {
            const Group group = connectome->Find(type);
            tonic.neurons.insert(tonic.neurons.end(), group.neurons.begin(), group.neurons.end());
        }
        const Group retina = connectome->Find("R1-R6");
        auto run = [&](FlyBrain& brain)
        {
            brain.StimulateCurrent(tonic, 12.0f);
            brain.StimulateRate(retina, 20.0f);
            brain.AdvanceMs(300.0);
            brain.Synchronize();
            return SortedSpikes(brain);
        };
        FlyBrain cpu(connectome);
        cpu.SetSpikeRingCapacity(1u << 22);
        cpu.SetParallel(true);
        FlyBrain gpu(connectome);
        gpu.SetSpikeRingCapacity(1u << 22);
        Check(gpu.SetBackend(BackendChoice::Gpu, Options(), &error) == Backend::Gpu, "GPU backend: " + error);
        const std::vector<SpikeEvent> a = run(cpu);
        const std::vector<SpikeEvent> b = run(gpu);
        std::printf("  MaleCNS, visual input 300 ms: %zu spikes on the CPU, %zu on the GPU\n", a.size(), b.size());
        CheckSameSpikes(a, b, "MaleCNS");
    }
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--hardware-only")
        {
            gAllowWarp = false;
        }
    }
    myengine::jobs::Initialize();
    int result = 0;
    try
    {
        TestAvailableAndSelfTest();
        std::cout << "available, self-test: ok\n";
        TestSyntheticSameAsCpu();
        std::cout << "synthetic graph, GPU = CPU spike for spike: ok\n";
        TestSwitchBackends();
        std::cout << "switching backends: ok\n";
        TestAsync();
        std::cout << "async: ok\n";
        TestConnectome();
        std::cout << "real connectome: ok\n";
        std::cout << "OK: GPU backend on " << gAdapter << ": the same spikes, counts and potentials as the CPU\n";
    }
    catch (const Skip& skip)
    {
        std::cout << "SKIPPED: " << skip.reason << '\n';
        result = kSkipped;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        result = 1;
    }
    myengine::jobs::Shutdown();
    return result;
}
