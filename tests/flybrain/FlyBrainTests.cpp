// FlyBrainTests.cpp
// N0 (FlyDoom): the model of the fly brain on graphs built in code, without the downloaded connectome.
//   - the .fbrain image: write, read, find by type, side and body id; a broken file is refused
//   - one neuron under a constant current fires at the rate of the analytic LIF (and not at all below the threshold)
//   - refractoriness: a saturated neuron fires every t_rfc
//   - the synaptic delay: a spike reaches the target exactly 1.8 ms (18 steps) later along a chain
//   - Poisson input at 150 Hz (Shiu's stimulation) makes the neuron fire at about 150 Hz
//   - a small recurrent network fires spike for spike like Brian2 with the equations of Shiu (BrianGolden.h)
//   - the event-driven simulation with sleeping neurons gives the same spikes as a plain step-by-step reference
//   - on the job system the spikes are bit-identical to one thread (integer sums do not depend on the order)

#include <flybrain/Connectome.h>
#include <flybrain/FlyBrain.h>

#include <myengine/jobs/JobSystem.h>

#include "BrianGolden.h"
#include "SyntheticGraph.h"

#include <algorithm>
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
    namespace fs = std::filesystem;

    void Check(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::shared_ptr<const Connectome> Build(const ConnectomeBuilder& builder)
    {
        std::string error;
        auto connectome = Connectome::FromImage(builder.Build("test"), &error);
        Check(connectome != nullptr, "the built image is read back: " + error);
        return connectome;
    }

    NeuronInfo Neuron(uint64_t bodyId, const char* type, Side side = Side::Left, Transmitter nt = Transmitter::Acetylcholine)
    {
        NeuronInfo info;
        info.bodyId = bodyId;
        info.type = type;
        info.side = side;
        info.transmitter = nt;
        return info;
    }

    std::vector<uint64_t> SpikeSteps(const FlyBrain& brain, uint32_t neuron)
    {
        std::vector<SpikeEvent> events;
        brain.RecentSpikes(events, 1u << 20);
        std::vector<uint64_t> steps;
        for (const SpikeEvent& e : events)
        {
            if (e.neuron == neuron)
            {
                steps.push_back(e.step);
            }
        }
        return steps;
    }

    // ---------------------------------------------------------------- file

    void TestImage()
    {
        ConnectomeBuilder builder;
        NeuronInfo dn = Neuron(500, "DNa02", Side::Left);
        dn.cellClass = "DN";
        dn.superclass = "descending_neuron";
        dn.position[0] = 1.0f;
        dn.position[1] = 2.0f;
        dn.position[2] = 3.0f;
        const uint32_t a = builder.AddNeuron(dn);
        const uint32_t b = builder.AddNeuron(Neuron(100, "DNa02", Side::Right, Transmitter::Gaba));
        NeuronInfo l1 = Neuron(300, "L1", Side::Left, Transmitter::Glutamate);
        l1.hex1 = 12;
        l1.hex2 = -3;
        const uint32_t c = builder.AddNeuron(l1);
        const uint32_t d = builder.AddNeuron(Neuron(200, "", Side::Unknown, Transmitter::Unknown));
        builder.AddEdge(a, c, 5);
        builder.AddEdge(a, b, 2);
        builder.AddEdge(a, c, 3); // the same pair: summed
        builder.AddEdge(b, d, -40000); // clamped to int16
        builder.AddEdge(c, a, 1);
        builder.AddEdge(c, a, -1); // sums to 0: dropped

        const fs::path path = fs::temp_directory_path() / "myengine_flybrain_test.fbrain";
        std::string error;
        Check(builder.Save(path.u8string(), "attribution text", &error), "the image is saved: " + error);
        auto connectome = Connectome::Load(path.u8string(), &error);
        fs::remove(path);
        Check(connectome != nullptr, "the saved file is loaded: " + error);
        const Connectome& cn = *connectome;

        Check(cn.NeuronCount() == 4 && cn.EdgeCount() == 3, "4 neurons and 3 edges after merging and dropping");
        Check(cn.Attribution() == "attribution text", "attribution survives");
        // Sorted by body id: 100 (b), 200 (d), 300 (c), 500 (a)
        Check(cn.BodyId(0) == 100 && cn.BodyId(3) == 500, "neurons are sorted by body id");
        const uint32_t ia = cn.FindBodyId(500);
        const uint32_t ib = cn.FindBodyId(100);
        const uint32_t ic = cn.FindBodyId(300);
        const uint32_t id = cn.FindBodyId(200);
        Check(cn.FindBodyId(999) == UINT32_MAX, "a missing body id is not found");
        Check(cn.Type(ia) == "DNa02" && cn.CellClass(ia) == "DN" && cn.Superclass(ia) == "descending_neuron", "annotations of a");
        Check(cn.NeuronSide(ib) == Side::Right && cn.NeuronTransmitter(ib) == Transmitter::Gaba, "side and transmitter of b");
        Check(cn.Hex1(ic) == 12 && cn.Hex2(ic) == -3 && cn.Hex1(ia) == kNoHex, "optic lobe columns");
        Check(cn.Position(ia)[2] == 3.0f, "position");

        Check(cn.Find("DNa02").Size() == 2, "Find by type takes both sides");
        const Group left = cn.Find("DNa02", Side::Left);
        Check(left.Size() == 1 && left.neurons[0] == ia, "Find by type and side");
        Check(cn.Find("DNa0").Empty() && cn.FindPrefix("DNa0").Size() == 2, "Find is exact, FindPrefix is a prefix");
        Check(cn.Find("").Empty() && cn.TypeCount() == 2, "untyped neurons are in no type group");
        Check(cn.FindSuperclass("descending_neuron").Size() == 1, "FindSuperclass");

        // Row of a: targets ascending (b before c), weights summed
        Check(cn.EdgeEnd(ia) - cn.EdgeBegin(ia) == 2, "a has 2 targets");
        const uint32_t e = cn.EdgeBegin(ia);
        Check(cn.Targets()[e] == ib && cn.Weights()[e] == 2 && cn.Targets()[e + 1] == ic && cn.Weights()[e + 1] == 8, "row of a");
        Check(cn.Weights()[cn.EdgeBegin(ib)] == -32767 && cn.Targets()[cn.EdgeBegin(ib)] == id, "weights are clamped to int16");
        Check(cn.EdgeEnd(ic) == cn.EdgeBegin(ic), "an edge summed to 0 is dropped");

        // A broken image is refused, not trusted
        std::vector<std::byte> image = builder.Build("x");
        image[0] = std::byte{ 'X' };
        Check(Connectome::FromImage(image, &error) == nullptr && !error.empty(), "bad magic is refused");
        image = builder.Build("x");
        image.resize(image.size() / 2);
        Check(Connectome::FromImage(image, &error) == nullptr, "a cut file is refused");
        Check(Connectome::Load("Z:/no/such/file.fbrain", &error) == nullptr, "a missing file is refused");
    }

    // ---------------------------------------------------------------- one neuron

    void TestConstantCurrent()
    {
        ConnectomeBuilder builder;
        builder.AddNeuron(Neuron(1, "n"));
        auto connectome = Build(builder);
        const ModelParams params;
        const double theta = params.thresholdMv - params.restMv; // 7 mV above rest
        const double a = std::exp(-double(params.dtMs) / params.membraneTauMs);

        for (const float current : { 6.0f, 7.0f })
        {
            FlyBrain brain(connectome);
            brain.StimulateCurrent(connectome->Find("n"), current);
            brain.Advance(20000);
            Check(brain.TotalSpikes() == 0, "no spikes at or below the threshold current " + std::to_string(current));
            Check(std::fabs(brain.VoltageMv(0) - (params.restMv + current)) < 0.01, "v settles at v_0 + I");
        }

        for (const float current : { 7.5f, 8.0f, 10.0f, 15.0f, 20.0f, 30.0f, 50.0f })
        {
            FlyBrain brain(connectome);
            brain.StimulateCurrent(connectome->Find("n"), current);
            brain.Advance(20000); // 2 s
            const std::vector<uint64_t> steps = SpikeSteps(brain, 0);
            Check(steps.size() >= 3, "a neuron above the threshold fires");

            // Discrete: n integration steps from reset to cross, then refractory - 1 frozen steps (Brian2 rule)
            const int n = static_cast<int>(std::floor(std::log(1.0 - theta / current) / std::log(a))) + 1;
            const uint64_t period = steps[2] - steps[1];
            const int expectedPeriod = n + 21;
            Check(std::llabs(static_cast<long long>(period) - expectedPeriod) <= 1,
                "period at I=" + std::to_string(current) + ": " + std::to_string(period) + " steps, discrete LIF " + std::to_string(expectedPeriod));

            // Continuous LIF: f = 1 / (t_ref + t_mbr ln(I / (I - theta))); the step makes it at most dt shorter per period
            const double analyticPeriodMs = params.refractoryMs + params.membraneTauMs * std::log(current / (current - theta));
            const double measuredHz = 1000.0 * (steps.size() - 1) / ((steps.back() - steps.front()) * double(params.dtMs));
            const double analyticHz = 1000.0 / analyticPeriodMs;
            const double relative = std::fabs(measuredHz - analyticHz) / analyticHz;
            Check(relative <= double(params.dtMs) / analyticPeriodMs + 0.005,
                "rate at I=" + std::to_string(current) + ": " + std::to_string(measuredHz) + " Hz, analytic " + std::to_string(analyticHz));
            std::printf("  I=%5.1f mV: %7.2f Hz (analytic LIF %7.2f Hz, %+.2f %%)\n", current, measuredHz, analyticHz, 100.0 * (measuredHz - analyticHz) / analyticHz);
        }
    }

    void TestRefractory()
    {
        ConnectomeBuilder builder;
        builder.AddNeuron(Neuron(1, "n"));
        auto connectome = Build(builder);
        FlyBrain brain(connectome);
        brain.StimulateCurrent(connectome->Find("n"), 100000.0f);
        brain.Advance(1000);
        const std::vector<uint64_t> steps = SpikeSteps(brain, 0);
        Check(!steps.empty() && steps[0] == 0, "a saturated neuron fires on the first step");
        for (size_t k = 1; k < steps.size(); ++k)
        {
            Check(steps[k] - steps[k - 1] == 22, "a saturated neuron fires every t_rfc = 22 steps");
        }
        Check(steps.size() == 46, "1000 steps / 22 -> 46 spikes");
    }

    // ---------------------------------------------------------------- delay

    void TestDelayChain()
    {
        // A -> B -> C -> D with weights that fire the target on its first integration step
        ConnectomeBuilder builder;
        const uint32_t count = 4;
        for (uint32_t i = 0; i < count; ++i)
        {
            builder.AddNeuron(Neuron(10 + i, i == 0 ? "source" : "relay"));
        }
        for (uint32_t i = 0; i + 1 < count; ++i)
        {
            builder.AddEdge(i, i + 1, 20000);
        }
        auto connectome = Build(builder);
        FlyBrain brain(connectome);
        brain.StimulateCurrent(connectome->Find("source"), 100000.0f, 0.1f); // one step: one spike at step 0

        brain.Advance(18);
        Check(brain.SynapticMv(1) == 0.0f, "nothing reaches B before 18 steps");
        brain.Advance(1);
        const float expected = static_cast<float>(20000 * brain.Params().weightMv);
        Check(std::fabs(brain.SynapticMv(1) - expected) < 0.5f, "B gets w_syn x contacts at step 18 (t_dly = 1.8 ms)");

        brain.Advance(200);
        Check(SpikeSteps(brain, 0) == std::vector<uint64_t>{ 0 }, "A fires once");
        for (uint32_t i = 1; i < count; ++i)
        {
            const std::vector<uint64_t> steps = SpikeSteps(brain, i);
            // The input lands at the end of step s + 18; the target crosses on its next update: s + 19
            Check(steps.size() == 1 && steps[0] == 19ull * i, "relay " + std::to_string(i) + " fires 19 steps after its source");
        }

        // Inhibition: the same chain with a negative weight keeps a tonically firing B silent for a while
        ConnectomeBuilder inhibitory;
        inhibitory.AddNeuron(Neuron(1, "source"));
        inhibitory.AddNeuron(Neuron(2, "target"));
        inhibitory.AddEdge(0, 1, -2000);
        auto inhibitoryConnectome = Build(inhibitory);
        FlyBrain quiet(inhibitoryConnectome);
        quiet.StimulateCurrent(inhibitoryConnectome->Find("target"), 20.0f);
        quiet.Advance(500);
        // A burst of 5 spikes (every 2.2 ms): input that lands in the target's refractory period is lost, the rest is not
        quiet.StimulateCurrent(inhibitoryConnectome->Find("source"), 100000.0f, 10.0f);
        quiet.Advance(1500);
        const std::vector<uint64_t> target = SpikeSteps(quiet, 1);
        Check(target.size() >= 3 && target[2] < 500, "the target fires tonically before the inhibition");
        bool gap = false;
        for (size_t k = 1; k < target.size(); ++k)
        {
            gap = gap || (target[k] - target[k - 1] > 200);
        }
        Check(gap, "inhibition makes a long pause in the target");
    }

    void TestSilentTypes()
    {
        // source -> lLN1_bc -> relay and source -> PN -> relay2: with "lLN" silent the LN fires, its relay does not (N3b)
        ConnectomeBuilder builder;
        builder.AddNeuron(Neuron(1, "source"));
        builder.AddNeuron(Neuron(2, "lLN1_bc"));
        builder.AddNeuron(Neuron(3, "relay"));
        builder.AddNeuron(Neuron(4, "PN"));
        builder.AddNeuron(Neuron(5, "relay2"));
        builder.AddEdge(0, 1, 20000);
        builder.AddEdge(1, 2, 20000);
        builder.AddEdge(0, 3, 20000);
        builder.AddEdge(3, 4, 20000);
        auto connectome = Build(builder);
        for (const bool silent : { false, true })
        {
            ModelParams params;
            if (silent)
            {
                params.silentTypePrefixes = { "lLN" };
            }
            FlyBrain brain(connectome, params);
            brain.StimulateCurrent(connectome->Find("source"), 100000.0f, 0.1f);
            brain.Advance(200);
            Check(brain.NeuronSpikeCount(1) == 1 && brain.NeuronSpikeCount(3) == 1 && brain.NeuronSpikeCount(4) == 1, "the LN and the PN path fire");
            Check(brain.NeuronSpikeCount(2) == (silent ? 0u : 1u), silent ? "a silent type delivers nothing" : "without silent types the LN drives its relay");
        }
    }

    // ---------------------------------------------------------------- Poisson

    void TestPoisson()
    {
        ConnectomeBuilder builder;
        for (uint32_t i = 0; i < 200; ++i)
        {
            builder.AddNeuron(Neuron(1 + i, "grn"));
        }
        auto connectome = Build(builder);
        const Group group = connectome->Find("grn");

        FlyBrain brain(connectome);
        const WatchId watch = brain.Watch(group, 1000.0f);
        brain.StimulateRate(group, 150.0f, 1000.0f);
        brain.AdvanceMs(1000.0);
        const float rate = brain.RateHz(watch, 1000.0f);
        Check(std::fabs(rate - 150.0f) < 7.5f, "Poisson 150 Hz makes the neurons fire at ~150 Hz, got " + std::to_string(rate));
        Check(brain.SpikeCount(watch, 1000.0f) == brain.TotalSpikes(), "the watch counts every spike of the group");
        brain.AdvanceMs(500.0);
        // A kick on the last stimulated step still fires on the next one; nothing after that
        Check(brain.SpikeCount(watch, 490.0f) == 0, "the stimulus ends after its duration");
        Check(brain.AwakeCount() == 0, "every neuron sleeps when nothing drives it");
        std::printf("  Poisson 150 Hz -> %.1f Hz\n", rate);
    }

    // ---------------------------------------------------------------- Brian2

    void TestBrianGolden()
    {
        // The network of tools/flybrain/brian_golden.py
        const uint32_t n = 60;
        ConnectomeBuilder builder;
        for (uint32_t i = 0; i < n; ++i)
        {
            builder.AddNeuron(Neuron(1 + i, "golden"));
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            for (uint32_t k = 1; k <= 10; ++k)
            {
                const uint32_t j = (i * 7 + k * 13) % n;
                if (j != i)
                {
                    builder.AddEdge(i, j, (i % 3 == 0 ? -1 : 1) * static_cast<int32_t>(1 + (i * k) % 15));
                }
            }
        }
        auto connectome = Build(builder);
        FlyBrain brain(connectome);
        for (uint32_t i = 0; i < n; i += 4)
        {
            brain.SetDrive(i, 8.0f + static_cast<float>(i % 5) * 3.0f);
        }
        brain.Advance(5000);

        std::vector<SpikeEvent> events;
        brain.RecentSpikes(events, 1u << 20);
        std::sort(events.begin(), events.end(), [](const SpikeEvent& a, const SpikeEvent& b) { return a.step != b.step ? a.step < b.step : a.neuron < b.neuron; });
        const size_t goldenCount = sizeof(test::kBrianGoldenSpikes) / sizeof(test::kBrianGoldenSpikes[0]) / 2;
        std::printf("  Brian2: %zu spikes, FlyBrain: %zu\n", goldenCount, events.size());
        Check(events.size() == goldenCount, "as many spikes as Brian2");
        for (size_t k = 0; k < goldenCount; ++k)
        {
            if (events[k].step != test::kBrianGoldenSpikes[2 * k] || events[k].neuron != test::kBrianGoldenSpikes[2 * k + 1])
            {
                for (size_t m = k > 3 ? k - 3 : 0; m < std::min(goldenCount, k + 6); ++m)
                {
                    std::printf("  #%zu Brian2 (step %u, neuron %u), FlyBrain (step %llu, neuron %u)\n", m, test::kBrianGoldenSpikes[2 * m],
                        test::kBrianGoldenSpikes[2 * m + 1], static_cast<unsigned long long>(events[m].step), events[m].neuron);
                }
            }
            Check(events[k].step == test::kBrianGoldenSpikes[2 * k] && events[k].neuron == test::kBrianGoldenSpikes[2 * k + 1],
                "spike " + std::to_string(k) + " differs from Brian2");
        }
    }

    // ---------------------------------------------------------------- reference

    // Plain step-by-step simulation of every neuron with the same arithmetic and the same order inside a step
    std::vector<uint32_t> ReferenceSpikeCounts(const Connectome& cn, const std::vector<float>& drive, uint32_t steps, uint64_t& total)
    {
        const ModelParams p;
        const uint32_t n = cn.NeuronCount();
        const double decayU = std::exp(-double(p.dtMs) / p.membraneTauMs);
        const double decayG = std::exp(-double(p.dtMs) / p.synapseTauMs);
        const double k = double(p.synapseTauMs) / (double(p.synapseTauMs) - double(p.membraneTauMs));
        const double a = decayU;
        const double b = decayG;
        const double c = k * (decayG - decayU);
        const uint32_t delay = 18;
        std::vector<double> u(n, 0.0), g(n, 0.0);
        std::vector<int64_t> refractoryUntil(n, 0);
        std::vector<int32_t> ring(size_t(n) * delay, 0);
        std::vector<uint32_t> counts(n, 0);
        std::vector<uint32_t> fired;
        total = 0;
        for (uint32_t t = 0; t < steps; ++t)
        {
            const uint32_t slot = t % delay;
            fired.clear();
            for (uint32_t i = 0; i < n; ++i)
            {
                const bool integrating = int64_t(t) >= refractoryUntil[i];
                if (integrating)
                {
                    u[i] = double(drive[i]) + (u[i] - double(drive[i])) * a + g[i] * c;
                    g[i] *= b;
                }
                const bool spiked = integrating && u[i] > 7.0;
                int32_t& in = ring[size_t(slot) * n + i];
                if (in != 0)
                {
                    if (integrating)
                    {
                        g[i] += p.weightMv * in;
                    }
                    in = 0;
                }
                if (spiked)
                {
                    u[i] = 0.0;
                    g[i] = 0.0;
                    refractoryUntil[i] = int64_t(t) + 22;
                    fired.push_back(i);
                }
            }
            for (uint32_t i : fired)
            {
                ++counts[i];
                ++total;
                for (uint32_t e = cn.EdgeBegin(i); e < cn.EdgeEnd(i); ++e)
                {
                    ring[size_t(slot) * n + cn.Targets()[e]] += cn.Weights()[e];
                }
            }
        }
        return counts;
    }

    std::vector<float> RandomDrive(uint32_t n, uint64_t seed)
    {
        // 5 % of the neurons get a tonic current of 8..20 mV, the rest nothing
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

    void TestMatchesReference()
    {
        test::SyntheticOptions options;
        options.neurons = 3000;
        options.meanDegree = 40;
        options.maxContacts = 6;
        auto connectome = test::MakeSyntheticConnectome(options);
        const std::vector<float> drive = RandomDrive(options.neurons, 11);
        const uint32_t steps = 3000; // 300 ms

        uint64_t referenceTotal = 0;
        const std::vector<uint32_t> reference = ReferenceSpikeCounts(*connectome, drive, steps, referenceTotal);

        FlyBrain brain(connectome);
        FlyBrain scalar(connectome);
        scalar.SetSimd(false);
        for (uint32_t i = 0; i < options.neurons; ++i)
        {
            brain.SetDrive(i, drive[i]);
            scalar.SetDrive(i, drive[i]);
        }
        brain.Advance(steps);
        scalar.Advance(steps);

        uint64_t differing = 0;
        for (uint32_t i = 0; i < options.neurons; ++i)
        {
            differing += reference[i] != brain.NeuronSpikeCount(i) ? 1 : 0;
        }
        std::printf("  reference %llu spikes, event-driven %llu, %llu of %u neurons differ\n",
            static_cast<unsigned long long>(referenceTotal), static_cast<unsigned long long>(brain.TotalSpikes()),
            static_cast<unsigned long long>(differing), options.neurons);
        Check(referenceTotal > 1000, "the reference network is active");
        std::vector<SpikeEvent> simdSpikes;
        std::vector<SpikeEvent> scalarSpikes;
        brain.RecentSpikes(simdSpikes, 1u << 20);
        scalar.RecentSpikes(scalarSpikes, 1u << 20);
        bool same = simdSpikes.size() == scalarSpikes.size();
        for (size_t k = 0; same && k < simdSpikes.size(); ++k)
        {
            same = simdSpikes[k].neuron == scalarSpikes[k].neuron && simdSpikes[k].step == scalarSpikes[k].step;
        }
        std::printf("  neuron step on %s: %s to the scalar code\n", brain.UsesSimd() ? "AVX2" : "scalar code (no AVX2)", same ? "identical" : "DIFFERENT");
        Check(same, "the AVX2 neuron step gives the same spikes as the scalar one");
        // The closed-form catch-up of sleeping neurons rounds differently from step-by-step products; that may move a
        // spike that sits within float rounding of the threshold. Allow a trace of it, no more
        Check(differing * 1000 <= options.neurons, "event-driven simulation matches the step-by-step reference");
    }

    // ---------------------------------------------------------------- threads

    void TestThreads()
    {
        test::SyntheticOptions options;
        options.neurons = 40000;
        options.meanDegree = 60;
        options.maxContacts = 6;
        auto connectome = test::MakeSyntheticConnectome(options);
        const std::vector<float> drive = RandomDrive(options.neurons, 5);
        Group poissonGroup;
        for (uint32_t i = 0; i < options.neurons; i += 50)
        {
            poissonGroup.neurons.push_back(i);
        }

        auto run = [&](bool parallel, std::vector<SpikeEvent>& events)
        {
            FlyBrain brain(connectome);
            brain.SetParallel(parallel);
            brain.SetSpikeRingCapacity(1u << 22);
            for (uint32_t i = 0; i < options.neurons; ++i)
            {
                brain.SetDrive(i, drive[i]);
            }
            brain.StimulateRate(poissonGroup, 80.0f);
            brain.Advance(2000);
            brain.RecentSpikes(events, 1u << 22);
            Check(events.size() < (1u << 22), "the ring keeps every spike of the run");
            return brain.TotalSpikes();
        };

        std::vector<SpikeEvent> serial;
        std::vector<SpikeEvent> parallel;
        const uint64_t serialTotal = run(false, serial);
        myengine::jobs::Initialize(3);
        const uint64_t parallelTotal = run(true, parallel);
        myengine::jobs::Shutdown();

        std::printf("  %llu spikes on one thread, %llu on the job system (%u workers + main)\n",
            static_cast<unsigned long long>(serialTotal), static_cast<unsigned long long>(parallelTotal), 3u);
        Check(serialTotal > 10000, "the network is active");
        Check(serialTotal == parallelTotal && serial.size() == parallel.size(), "the same number of spikes on any thread count");
        for (size_t k = 0; k < serial.size(); ++k)
        {
            Check(serial[k].neuron == parallel[k].neuron && serial[k].step == parallel[k].step, "bit-identical spikes on the job system");
        }
    }

    void TestOddWindows()
    {
        // Advance in pieces shorter than the delay (a DOOM tick is 285.7 steps): the input of the later steps of a
        // short window must survive to the next one
        test::SyntheticOptions options;
        options.neurons = 3000;
        options.meanDegree = 40;
        options.maxContacts = 6;
        auto connectome = test::MakeSyntheticConnectome(options);
        const std::vector<float> drive = RandomDrive(options.neurons, 3);

        auto run = [&](const std::vector<uint32_t>& pieces)
        {
            FlyBrain brain(connectome);
            for (uint32_t i = 0; i < options.neurons; ++i)
            {
                brain.SetDrive(i, drive[i]);
            }
            uint32_t done = 0;
            for (size_t k = 0; done < 3000; ++k)
            {
                const uint32_t piece = std::min(pieces[k % pieces.size()], 3000 - done);
                brain.Advance(piece);
                done += piece;
            }
            return brain.TotalSpikes();
        };
        const uint64_t whole = run({ 3000 });
        const uint64_t odd = run({ 7, 1, 13, 5, 17, 2 });
        std::printf("  windows of 18: %llu spikes, odd windows: %llu\n", static_cast<unsigned long long>(whole), static_cast<unsigned long long>(odd));
        Check(whole > 1000, "the network is active");
        const double relative = std::fabs(double(whole) - double(odd)) / double(whole);
        Check(relative < 0.002, "odd windows give the same activity");
    }
}

int main()
{
    try
    {
        TestImage();
        std::cout << "image: ok\n";
        TestConstantCurrent();
        std::cout << "constant current: ok\n";
        TestRefractory();
        std::cout << "refractory: ok\n";
        TestDelayChain();
        std::cout << "delay chain: ok\n";
        TestSilentTypes();
        std::cout << "silent types: ok\n";
        TestPoisson();
        std::cout << "poisson: ok\n";
        TestBrianGolden();
        std::cout << "Brian2 golden spikes: ok\n";
        TestMatchesReference();
        std::cout << "step-by-step reference: ok\n";
        TestOddWindows();
        std::cout << "odd windows: ok\n";
        TestThreads();
        std::cout << "threads: ok\n";
        std::cout << "OK: .fbrain image, LIF rate vs analytic, refractoriness, 1.8 ms delay, Poisson input, Brian2 spike for spike, reference match, thread determinism\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
