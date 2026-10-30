// FlyBrainSwarmTests.cpp
// N2 (FlyDoom): the brains of the swarm. Always, on synthetic graphs:
//   - replicas are independent brains: fly r of a batch of 8 fires exactly like a single brain with the inputs of fly r
//   - the batch on the GPU gives the spikes of the batch on the CPU, the tracked spike totals agree on both
// With the real connectome (assets/flybrain/malecns_v1.fbrain), the circuit cut out of it:
//   - 1,000..5,000 neurons with the looming inputs and the descending outputs
//   - no input: no spikes; brightness alone: no spikes (no self-sustained activity)
//   - looming on one side makes the Giant Fiber fire (escape) and steers; the other side steers the other way
//   - after the looming stops the swarm falls silent
//   - 32 flies at least as fast as real time on the GPU (the CPU figure is printed)
// Exit code 77 only when neither a GPU nor the connectome is there to test the second half: the first half always runs.

#include <flybrain/Circuit.h>
#include <flybrain/Connectome.h>
#include <flybrain/FlyBrain.h>
#include <flybrain/FlySwarmBrain.h>

#include <myengine/jobs/JobSystem.h>

#include "SyntheticGraph.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using namespace flybrain;

    void Check(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::vector<float> RandomDrive(uint32_t n, uint64_t seed)
    {
        std::vector<float> drive(n, 0.0f);
        uint64_t rng = seed;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (test::Uniform(rng) < 0.06)
            {
                drive[i] = static_cast<float>(8.0 + 12.0 * test::Uniform(rng));
            }
        }
        return drive;
    }

    std::vector<SpikeEvent> SortedSpikes(const FlyBrain& brain)
    {
        std::vector<SpikeEvent> spikes;
        brain.RecentSpikes(spikes, 1u << 22);
        std::sort(spikes.begin(), spikes.end(), [](const SpikeEvent& a, const SpikeEvent& b) { return a.step != b.step ? a.step < b.step : a.neuron < b.neuron; });
        return spikes;
    }

    bool gGpu = false;

    void TestReplicasAreIndependent()
    {
        test::SyntheticOptions options;
        options.neurons = 2000;
        options.meanDegree = 40;
        options.maxContacts = 6;
        auto connectome = test::MakeSyntheticConnectome(options);
        const uint32_t flies = 8;

        FlyBrain batch(connectome, ModelParams{}, flies);
        batch.SetParallel(true);
        Check(batch.UnitCount() == options.neurons * flies && batch.NeuronOfUnit(batch.Unit(7, 3)) == 7 && batch.ReplicaOfUnit(batch.Unit(7, 3)) == 3, "units");
        std::vector<std::vector<float>> drives;
        for (uint32_t fly = 0; fly < flies; ++fly)
        {
            drives.push_back(RandomDrive(options.neurons, 100 + fly));
            for (uint32_t i = 0; i < options.neurons; ++i)
            {
                batch.SetDrive(batch.Unit(i, fly), drives[fly][i]);
            }
        }
        batch.Advance(1503);

        uint64_t spikes = 0;
        for (uint32_t fly = 0; fly < flies; ++fly)
        {
            FlyBrain single(connectome);
            for (uint32_t i = 0; i < options.neurons; ++i)
            {
                single.SetDrive(i, drives[fly][i]);
            }
            single.Advance(1503);
            for (uint32_t i = 0; i < options.neurons; ++i)
            {
                Check(single.NeuronSpikeCount(i) == batch.NeuronSpikeCount(batch.Unit(i, fly)),
                    "fly " + std::to_string(fly) + " of the batch fires like a single brain (neuron " + std::to_string(i) + ")");
                Check(single.VoltageMv(i) == batch.VoltageMv(batch.Unit(i, fly)), "the same membrane potential in the batch");
            }
            spikes += single.TotalSpikes();
        }
        std::printf("  8 replicas = 8 single brains: %llu spikes, every neuron the same\n", static_cast<unsigned long long>(spikes));
        Check(spikes > 2000, "the networks are active");
    }

    void TestBatchOnGpu()
    {
        test::SyntheticOptions options;
        options.neurons = 3000;
        options.meanDegree = 40;
        options.maxContacts = 6;
        auto connectome = test::MakeSyntheticConnectome(options);
        const uint32_t flies = 16;
        auto run = [&](bool gpu, std::vector<uint32_t>& tracked)
        {
            FlyBrain batch(connectome, ModelParams{}, flies);
            batch.SetParallel(true);
            batch.SetSpikeRingCapacity(1u << 22);
            Group watched;
            for (uint32_t i = 0; i < options.neurons; i += 97)
            {
                watched.neurons.push_back(i);
            }
            for (uint32_t fly = 0; fly < flies; ++fly)
            {
                batch.Track(batch.ForReplica(watched, fly));
            }
            if (gpu)
            {
                GpuOptions gpuOptions;
                gpuOptions.selfTest = false;
                gpuOptions.allowWarp = true;
                Check(batch.SetBackend(BackendChoice::Gpu, gpuOptions) == Backend::Gpu, "GPU backend");
            }
            const std::vector<float> drive = RandomDrive(options.neurons, 7);
            for (uint32_t fly = 0; fly < flies; ++fly)
            {
                for (uint32_t i = 0; i < options.neurons; ++i)
                {
                    batch.SetDrive(batch.Unit(i, fly), drive[i] * (0.8f + 0.03f * fly));
                }
                for (uint32_t i = fly; i < options.neurons; i += 61)
                {
                    batch.SetPoissonRate(batch.Unit(i, fly), 40.0f + fly);
                }
            }
            for (int tick = 0; tick < 10; ++tick)
            {
                for (uint32_t fly = 0; fly < flies; fly += 3)
                {
                    batch.SetPoissonRate(batch.Unit((tick * 13 + fly) % options.neurons, fly), tick % 2 ? 0.0f : 90.0f);
                }
                batch.AdvanceMs(1000.0 / 35.0);
            }
            batch.Synchronize();
            tracked.clear();
            for (uint32_t fly = 0; fly < flies; ++fly)
            {
                for (uint32_t i : watched.neurons)
                {
                    const uint32_t unit = batch.Unit(i, fly);
                    Check(batch.TrackedSpikes(unit) == batch.NeuronSpikeCount(unit), "tracked total = spike count of the unit");
                    tracked.push_back(batch.TrackedSpikes(unit));
                }
            }
            return SortedSpikes(batch);
        };
        std::vector<uint32_t> cpuTracked;
        std::vector<uint32_t> gpuTracked;
        const std::vector<SpikeEvent> cpu = run(false, cpuTracked);
        const std::vector<SpikeEvent> gpu = run(true, gpuTracked);
        std::printf("  batch of 16 x 3000 neurons: %zu spikes on the CPU, %zu on the GPU\n", cpu.size(), gpu.size());
        Check(cpu.size() > 10000, "the batch is active");
        Check(cpu.size() == gpu.size(), "the same number of spikes on the GPU");
        for (size_t k = 0; k < cpu.size(); ++k)
        {
            Check(cpu[k].neuron == gpu[k].neuron && cpu[k].step == gpu[k].step, "spike " + std::to_string(k) + " of the batch differs on the GPU");
        }
        Check(cpuTracked == gpuTracked, "the same tracked totals");
    }

    double RunSwarm(FlySwarmBrain& swarm, double seconds, bool changing)
    {
        // Random looming that changes every DOOM tick: the swarm in a level
        uint64_t rng = 99;
        const auto start = std::chrono::steady_clock::now();
        for (double done = 0.0; done < seconds * 1000.0 - 1e-6; done += 1000.0 / 35.0)
        {
            if (changing)
            {
                for (uint32_t fly = 0; fly < swarm.FlyCount(); ++fly)
                {
                    SwarmInput input;
                    input.loomLeft = static_cast<float>(test::Uniform(rng) * 0.5);
                    input.loomRight = static_cast<float>(test::Uniform(rng) * 0.5);
                    input.brightness = 0.5f;
                    swarm.SetInput(fly, input);
                }
            }
            swarm.AdvanceMs(1000.0 / 35.0);
        }
        swarm.Synchronize();
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return seconds / wall;
    }

    void TestCircuit(const std::shared_ptr<const Connectome>& full)
    {
        CircuitReport report;
        std::string error;
        const auto circuit = ExtractCircuit(*full, CircuitSpec{}, &report, &error);
        Check(circuit != nullptr, "the circuit is cut out: " + error);
        std::printf("  circuit: %s\n", report.Text(15).c_str());
        Check(report.neurons >= 1000 && report.neurons <= 5000, "the circuit has 1,000..5,000 neurons");
        const std::string saved = std::string(MYENGINE_SOURCE_DIR) + "/assets/flybrain/swarm_circuit.fbrain";
        if (std::filesystem::exists(std::filesystem::u8path(saved)))
        {
            const auto file = Connectome::Load(saved, &error);
            Check(file != nullptr && file->NeuronCount() == circuit->NeuronCount() && file->EdgeCount() == circuit->EdgeCount(),
                "assets/flybrain/swarm_circuit.fbrain is the circuit of the current spec");
        }

        for (const bool gpu : { false, true })
        {
            if (gpu && !gGpu)
            {
                continue;
            }
            SwarmOptions options;
            options.backend = gpu ? BackendChoice::Gpu : BackendChoice::Cpu;
            options.gpu.selfTest = false;
            const uint32_t flies = 32;
            auto swarm = FlySwarmBrain::Create(circuit, flies, options, &error);
            Check(swarm != nullptr, "the swarm is made: " + error);
            const char* name = gpu ? "GPU" : "CPU";

            // At rest and with light only nothing fires
            swarm->AdvanceMs(300.0);
            swarm->Synchronize();
            Check(swarm->Brain().TotalSpikes() == 0, "no input: no spikes");
            for (uint32_t fly = 0; fly < flies; ++fly)
            {
                SwarmInput input;
                input.brightness = 1.0f;
                swarm->SetInput(fly, input);
            }
            swarm->AdvanceMs(300.0);
            swarm->Synchronize();
            Check(swarm->Brain().TotalSpikes() == 0, "brightness alone: no spikes");

            // Fly 0 sees something approach on the left, fly 1 on the right, the others nothing
            SwarmInput left;
            left.loomLeft = 1.0f;
            left.brightness = 1.0f;
            SwarmInput right;
            right.loomRight = 1.0f;
            right.brightness = 1.0f;
            swarm->SetInput(0, left);
            swarm->SetInput(1, right);
            swarm->AdvanceMs(300.0);
            swarm->Synchronize();
            const SwarmOutput a = swarm->Output(0);
            const SwarmOutput b = swarm->Output(1);
            std::printf("  %s, looming left:  turn %+.2f (steer L %.1f Hz, R %.1f Hz), forward %+.2f, escape %.2f (%u Giant Fiber spikes)\n", name,
                a.turn, a.steerLeftHz, a.steerRightHz, a.forward, a.escape, a.escapeSpikes);
            std::printf("  %s, looming right: turn %+.2f (steer L %.1f Hz, R %.1f Hz), forward %+.2f, escape %.2f (%u Giant Fiber spikes)\n", name,
                b.turn, b.steerLeftHz, b.steerRightHz, b.forward, b.escape, b.escapeSpikes);
            Check(a.escapeSpikes > 0 && b.escapeSpikes > 0, "looming makes the Giant Fiber fire");
            Check(a.turn * b.turn < 0.0f, "looming on the other side steers the other way");
            Check(swarm->Output(2).escapeSpikes == 0 && swarm->Output(2).turn == 0.0f, "a fly that sees nothing does nothing");

            // The looming stops: the descending outputs fall silent. A loop of 9 neurons (CL213 <-> GNG581, GNG584,
            // DNg74_a) keeps firing in the circuit after strong looming; it reaches none of the outputs (the doc)
            const uint64_t duringLooming = swarm->Brain().TotalSpikes();
            SwarmInput dark;
            swarm->SetInput(0, dark);
            swarm->SetInput(1, dark);
            swarm->AdvanceMs(300.0);
            swarm->Synchronize();
            const uint64_t before = swarm->Brain().TotalSpikes();
            const uint32_t escapes0 = swarm->Output(0).escapeSpikes;
            const uint32_t escapes1 = swarm->Output(1).escapeSpikes;
            swarm->AdvanceMs(400.0);
            swarm->Synchronize();
            const uint64_t after = swarm->Brain().TotalSpikes() - before;
            std::printf("  %s, after the looming: %llu spikes in 400 ms (%llu during 300 ms of looming); turn %+.3f %+.3f\n", name,
                static_cast<unsigned long long>(after), static_cast<unsigned long long>(duringLooming), swarm->Output(0).turn, swarm->Output(1).turn);
            Check(swarm->Output(0).escapeSpikes == escapes0 && swarm->Output(1).escapeSpikes == escapes1, "no Giant Fiber spikes after the looming");
            for (uint32_t fly : { 0u, 1u })
            {
                const SwarmOutput& out = swarm->Output(fly);
                Check(std::fabs(out.turn) < 0.05f && std::fabs(out.forward) < 0.05f && out.escape < 0.05f, "the outputs of the fly come back to rest");
            }
            Check(after * 20 < duringLooming, "what keeps firing after the looming is small");

            // Speed: 32 flies with looming that changes every tick
            const double speed = RunSwarm(*swarm, 1.0, true);
            std::printf("  %s: 32 flies at %.2fx real time (%u units, %llu spikes in the last second)\n", name, speed, swarm->Brain().UnitCount(),
                static_cast<unsigned long long>(swarm->Brain().Stats().spikes));
            if (gpu)
            {
                Check(speed >= 1.0, "32 flies on the GPU at least as fast as real time");
            }
        }
    }
}

int main()
{
    myengine::jobs::Initialize();
    int result = 0;
    try
    {
        TestReplicasAreIndependent();
        std::cout << "replicas: ok\n";
        {
            test::SyntheticOptions probe;
            probe.neurons = 50;
            FlyBrain brain(test::MakeSyntheticConnectome(probe));
            GpuOptions options;
            options.selfTest = false;
            options.allowWarp = true;
            gGpu = brain.SetBackend(BackendChoice::Gpu, options) == Backend::Gpu;
        }
        if (gGpu)
        {
            TestBatchOnGpu();
            std::cout << "batch on the GPU: ok\n";
        }
        const std::string path = std::string(MYENGINE_SOURCE_DIR) + "/assets/flybrain/malecns_v1.fbrain";
        if (std::filesystem::exists(std::filesystem::u8path(path)))
        {
            std::string error;
            const auto full = Connectome::Load(path, &error);
            Check(full != nullptr, error);
            TestCircuit(full);
            std::cout << "swarm circuit: ok\n";
        }
        else
        {
            std::cout << "the connectome is not downloaded: the circuit part is skipped\n";
            result = gGpu ? 0 : 77;
        }
        std::cout << "OK: swarm brains (replicas, GPU batch, circuit behaviour and speed)\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        result = 1;
    }
    myengine::jobs::Shutdown();
    return result;
}
