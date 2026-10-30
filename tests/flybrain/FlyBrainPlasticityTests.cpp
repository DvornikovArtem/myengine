// FlyBrainPlasticityTests.cpp
// N3 (FlyDoom): the fly learns. Always, on a small mushroom body built in code:
//   - the plastic synapses are KC -> MBON, the compartment of an MBON is the DANs that synapse onto it
//   - classical conditioning: odour A (a group of KCs) paired with punishment (PPL1) -> the response of the MBON of that
//     compartment to A falls; to B (other KCs, not paired) it stays; A onto the MBON of another compartment (PAM) stays
//   - frozen learning changes nothing; the memory file restores the scales; reset brings them back to 1
//   - the same learning on the GPU: every scale and every spike the same as on the CPU
// With the connectome (assets/flybrain/malecns_v1.fbrain): the plastic synapses and compartments of MaleCNS, and after
// pairings of a group of KCs with PPL1 the synapses of those KCs onto the MBONs of the PPL1 compartments weaken, the
// synapses of the other KCs do not.

#include <flybrain/Connectome.h>
#include <flybrain/FlyBrain.h>

#include <myengine/jobs/JobSystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
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

    constexpr double kTick = 1000.0 / 35.0;

    NeuronInfo Neuron(uint64_t id, const char* type, Transmitter nt = Transmitter::Acetylcholine)
    {
        NeuronInfo info;
        info.bodyId = id;
        info.type = type;
        info.transmitter = nt;
        return info;
    }

    // 20 KCs of odour A, 20 of odour B, MBON01 in the compartment of PPL101, MBON02 in the compartment of PAM01
    std::shared_ptr<const Connectome> MushroomBody()
    {
        ConnectomeBuilder b;
        std::vector<uint32_t> a, bKcs;
        for (uint32_t i = 0; i < 20; ++i)
        {
            a.push_back(b.AddNeuron(Neuron(100 + i, "KC_A")));
            bKcs.push_back(b.AddNeuron(Neuron(200 + i, "KC_B")));
        }
        const uint32_t mbon1 = b.AddNeuron(Neuron(300, "MBON01", Transmitter::Glutamate));
        const uint32_t mbon2 = b.AddNeuron(Neuron(301, "MBON02", Transmitter::Glutamate));
        const uint32_t ppl1 = b.AddNeuron(Neuron(400, "PPL101", Transmitter::Dopamine));
        const uint32_t ppl2 = b.AddNeuron(Neuron(401, "PPL101", Transmitter::Dopamine));
        const uint32_t pam = b.AddNeuron(Neuron(500, "PAM01", Transmitter::Dopamine));
        for (uint32_t kc : a)
        {
            b.AddEdge(kc, mbon1, 10);
            b.AddEdge(kc, mbon2, 10);
        }
        for (uint32_t kc : bKcs)
        {
            b.AddEdge(kc, mbon1, 10);
        }
        b.AddEdge(ppl1, mbon1, 12);
        b.AddEdge(ppl2, mbon1, 12);
        b.AddEdge(pam, mbon2, 12);
        return Connectome::FromImage(b.Build("synthetic mushroom body"));
    }

    // Spikes of the MBON while one odour is on for 1 s (in ticks of DOOM, as the game would run it)
    uint32_t Response(FlyBrain& brain, const Group& odour, const Group& mbon)
    {
        brain.Synchronize();
        const uint32_t before = brain.NeuronSpikeCount(mbon.neurons[0]);
        const StimulusId id = brain.StimulateRate(odour, 40.0f);
        for (int t = 0; t < 35; ++t)
        {
            brain.AdvanceMs(kTick);
        }
        brain.StopStimulus(id);
        brain.Synchronize();
        const uint32_t spikes = brain.NeuronSpikeCount(mbon.neurons[0]) - before;
        // Quiet for 15 s: the eligibility traces (2 s) are gone before anything else happens
        brain.AdvanceMs(15000.0);
        return spikes;
    }

    void Pair(FlyBrain& brain, const Group& odour, const Group& dans, int trials)
    {
        for (int k = 0; k < trials; ++k)
        {
            const StimulusId o = brain.StimulateRate(odour, 40.0f);
            const StimulusId d = brain.StimulateCurrent(dans, 15.0f);
            for (int t = 0; t < 7; ++t)
            {
                brain.AdvanceMs(kTick);
            }
            brain.StopStimulus(o);
            brain.StopStimulus(d);
            for (int t = 0; t < 14; ++t)
            {
                brain.AdvanceMs(kTick);
            }
        }
        brain.AdvanceMs(15000.0);
    }

    struct Conditioning
    {
        uint32_t aBefore = 0, aAfter = 0, bBefore = 0, bAfter = 0, a2Before = 0, a2After = 0;
        std::vector<double> scales;
        std::vector<SpikeEvent> spikes;
    };

    // learnEveryMs (N4): the weights change once in this many ms, not at every call of Advance
    Conditioning RunConditioning(const std::shared_ptr<const Connectome>& cn, bool gpu, bool learning, double learnEveryMs = 0.0)
    {
        FlyBrain brain(cn);
        brain.SetSpikeRingCapacity(1u << 20);
        std::string error;
        PlasticityRule rule;
        rule.learnEveryMs = learnEveryMs;
        Check(brain.EnablePlasticity(rule, &error), "plasticity: " + error);
        if (gpu)
        {
            GpuOptions options;
            options.allowWarp = true;
            options.selfTest = false;
            Check(brain.SetBackend(BackendChoice::Gpu, options, &error) == Backend::Gpu, "GPU: " + error);
        }
        brain.SetLearning(learning);
        const Group a = cn->Find("KC_A");
        const Group b = cn->Find("KC_B");
        const Group mbon1 = cn->Find("MBON01");
        const Group mbon2 = cn->Find("MBON02");
        Conditioning c;
        c.aBefore = Response(brain, a, mbon1);
        c.bBefore = Response(brain, b, mbon1);
        c.a2Before = Response(brain, a, mbon2);
        Pair(brain, a, brain.PunishmentNeurons(), 10);
        c.aAfter = Response(brain, a, mbon1);
        c.bAfter = Response(brain, b, mbon1);
        c.a2After = Response(brain, a, mbon2);
        brain.Synchronize();
        c.scales = brain.GetPlasticScales();
        brain.RecentSpikes(c.spikes, 1u << 20);
        std::sort(c.spikes.begin(), c.spikes.end(), [](const SpikeEvent& x, const SpikeEvent& y) { return x.step != y.step ? x.step < y.step : x.neuron < y.neuron; });
        return c;
    }

    bool gGpu = false;

    void TestSynthetic()
    {
        const auto cn = MushroomBody();
        {
            FlyBrain brain(cn);
            Check(brain.EnablePlasticity(), "plasticity on the mushroom body");
            const PlasticityInfo& info = brain.GetPlasticityInfo();
            Check(info.edges == 60 && info.preNeurons == 40 && info.postNeurons == 2 && info.punishmentNeurons == 2 && info.rewardNeurons == 1,
                "60 KC -> MBON synapses, 40 KCs, 2 MBONs, 2 PPL1 and 1 PAM");
            Check(info.edgesWithDopamine == 60, "every MBON has a compartment");
        }

        const Conditioning learned = RunConditioning(cn, false, true);
        std::printf("  MBON01 to A: %u -> %u spikes, to B: %u -> %u; MBON02 to A: %u -> %u\n", learned.aBefore, learned.aAfter, learned.bBefore,
            learned.bAfter, learned.a2Before, learned.a2After);
        Check(learned.aBefore > 20, "MBON01 answers odour A");
        Check(learned.aAfter * 10 < learned.aBefore * 7, "after pairing A with punishment MBON01 answers A at least 30 % less");
        Check(learned.bAfter * 10 >= learned.bBefore * 9, "the answer to B (not paired) stays");
        Check(learned.a2After * 10 >= learned.a2Before * 9, "the answer of MBON02 (another compartment) to A stays");

        FlyBrain probe(cn);
        probe.EnablePlasticity();
        double aScale = 0.0;
        double bScale = 0.0;
        double a2Scale = 0.0;
        for (uint32_t k = 0; k < probe.PlasticEdgeCount(); ++k)
        {
            const std::string_view pre = cn->Type(probe.PlasticPre(k));
            const std::string_view post = cn->Type(probe.PlasticPost(k));
            const double s = learned.scales[k];
            if (pre == "KC_A" && post == "MBON01") aScale = s;
            if (pre == "KC_B" && post == "MBON01") bScale = s;
            if (pre == "KC_A" && post == "MBON02") a2Scale = s;
        }
        std::printf("  scales: A->MBON01 %.3f, B->MBON01 %.3f, A->MBON02 %.3f\n", aScale, bScale, a2Scale);
        Check(aScale < 0.8 && bScale > 0.98 && a2Scale > 0.98, "only the paired synapses of the punished compartment weaken");

        const Conditioning frozen = RunConditioning(cn, false, false);
        // Without learning the same pairing leaves every weight; the answer to A changes only by the noise of the Poisson input
        std::printf("  learning off: MBON01 to A: %u -> %u spikes\n", frozen.aBefore, frozen.aAfter);
        Check(std::all_of(frozen.scales.begin(), frozen.scales.end(), [](double s) { return s == 1.0; }), "frozen learning changes no weight");
        Check(frozen.aAfter * 10 >= frozen.aBefore * 8 && frozen.aAfter > learned.aAfter + 10, "without learning the answer to A stays");

        // The memory
        const fs::path path = fs::temp_directory_path() / "myengine_fly_memory_test.bin";
        {
            FlyBrain brain(cn);
            brain.EnablePlasticity();
            brain.SetPlasticScales(learned.scales);
            std::string error;
            Check(brain.SaveMemory(path.u8string(), &error), "save: " + error);
            FlyBrain other(cn);
            other.EnablePlasticity();
            Check(other.LoadMemory(path.u8string(), &error), "load: " + error);
            Check(other.GetPlasticScales() == learned.scales, "the memory file restores every scale");
            other.ResetPlasticity();
            const std::vector<double> afterReset = other.GetPlasticScales();
            Check(std::all_of(afterReset.begin(), afterReset.end(), [](double s) { return s == 1.0; }), "reset");
            FlyBrain foreign(MushroomBody());
            foreign.EnablePlasticity();
            ConnectomeBuilder smaller;
            smaller.AddNeuron(Neuron(1, "KC_A"));
            smaller.AddNeuron(Neuron(2, "MBON01"));
            smaller.AddEdge(0, 1, 3);
            FlyBrain wrong(Connectome::FromImage(smaller.Build("x")));
            wrong.EnablePlasticity();
            Check(!wrong.LoadMemory(path.u8string(), &error), "a memory of another connectome is refused");
            fs::remove(path);
        }
        Check(FlyBrain::DefaultMemoryPath().find("FlyDoom") != std::string::npos, "the default memory is in %LOCALAPPDATA%\\FlyDoom");

        if (gGpu)
        {
            const Conditioning onGpu = RunConditioning(cn, true, true);
            Check(onGpu.scales == learned.scales, "the GPU learns the same scales, bit for bit");
            Check(onGpu.spikes.size() == learned.spikes.size(), "the same number of spikes on the GPU");
            for (size_t k = 0; k < onGpu.spikes.size(); ++k)
            {
                Check(onGpu.spikes[k].neuron == learned.spikes[k].neuron && onGpu.spikes[k].step == learned.spikes[k].step, "spike " + std::to_string(k) + " on the GPU");
            }
            std::printf("  GPU: the same %zu spikes and %zu scales\n", onGpu.spikes.size(), onGpu.scales.size());
        }

        // N4: learning once in 3 calls of Advance learns as well, and the GPU (that then runs ahead between the changes) learns the same
        const Conditioning sparse = RunConditioning(cn, false, true, 3.0 * kTick - 0.5);
        std::printf("  learning every 3 calls: MBON01 to A: %u -> %u spikes\n", sparse.aBefore, sparse.aAfter);
        Check(sparse.aAfter * 10 < sparse.aBefore * 7, "learning every 3 calls: MBON01 answers A at least 30 % less");
        Check(sparse.scales != learned.scales, "learning every 3 calls changes the weights at other moments");
        if (gGpu)
        {
            const Conditioning sparseGpu = RunConditioning(cn, true, true, 3.0 * kTick - 0.5);
            Check(sparseGpu.scales == sparse.scales, "learning every 3 calls: the GPU learns the same scales, bit for bit");
            Check(sparseGpu.spikes.size() == sparse.spikes.size(), "learning every 3 calls: the same number of spikes on the GPU");
            for (size_t k = 0; k < sparseGpu.spikes.size() && k < sparse.spikes.size(); ++k)
            {
                Check(sparseGpu.spikes[k].neuron == sparse.spikes[k].neuron && sparseGpu.spikes[k].step == sparse.spikes[k].step, "learning every 3 calls: spike " + std::to_string(k) + " on the GPU");
            }
        }
    }

    void TestConnectome(const std::shared_ptr<const Connectome>& cn, bool gpu)
    {
        // Dopamine as a modulator: with fast excitatory dopamine synapses PPL101 alone starts the self-sustained
        // activity of the central brain, and every KC and every compartment learns (the doc)
        ModelParams params;
        params.fastModulatorySynapses = false;
        FlyBrain brain(cn, params);
        brain.SetParallel(true);
        std::string error;
        Check(brain.EnablePlasticity(PlasticityRule{}, &error), "plasticity on MaleCNS: " + error);
        const PlasticityInfo& info = brain.GetPlasticityInfo();
        std::printf("  MaleCNS: %u KC -> MBON synapses (%u onto an MBON with DANs), %u KCs, %u MBONs, %u PPL1, %u PAM\n", info.edges,
            info.edgesWithDopamine, info.preNeurons, info.postNeurons, info.punishmentNeurons, info.rewardNeurons);
        Check(info.edges > 1000 && info.punishmentNeurons > 0 && info.rewardNeurons > 0, "KC -> MBON synapses, PPL1 and PAM in MaleCNS");
        if (gpu)
        {
            GpuOptions options;
            options.selfTest = false;
            Check(brain.SetBackend(BackendChoice::Gpu, options, &error) == Backend::Gpu, "GPU: " + error);
        }

        // Odour: every 40th KC that has plastic synapses (about 100 KCs, a sparse odour code as in the fly)
        std::set<uint32_t> kcs;
        for (uint32_t k = 0; k < brain.PlasticEdgeCount(); ++k)
        {
            kcs.insert(brain.PlasticPre(k));
        }
        Group odour;
        size_t index = 0;
        for (uint32_t kc : kcs)
        {
            if (index++ % 40 == 0)
            {
                odour.neurons.push_back(kc);
            }
        }
        const std::set<uint32_t> odourSet(odour.neurons.begin(), odour.neurons.end());
        Group ppl101 = cn->Find("PPL101");
        std::vector<uint32_t> kcBefore;
        for (uint32_t kc : kcs)
        {
            kcBefore.push_back(brain.NeuronSpikeCount(kc));
        }
        const uint64_t spikesBefore = brain.TotalSpikes();
        for (int trial = 0; trial < 5; ++trial)
        {
            const StimulusId o = brain.StimulateRate(odour, 20.0f);
            const StimulusId d = brain.StimulateCurrent(ppl101, 15.0f);
            for (int t = 0; t < 7; ++t)
            {
                brain.AdvanceMs(kTick);
            }
            brain.StopStimulus(o);
            brain.StopStimulus(d);
            for (int t = 0; t < 14; ++t)
            {
                brain.AdvanceMs(kTick);
            }
        }
        brain.Synchronize();
        uint32_t firedOutside = 0;
        size_t kcIndex = 0;
        for (uint32_t kc : kcs)
        {
            firedOutside += odourSet.count(kc) == 0 && brain.NeuronSpikeCount(kc) > kcBefore[kcIndex] ? 1 : 0;
            ++kcIndex;
        }
        std::printf("  %s: %llu spikes in the brain during the pairings (5 x 0.6 s); %u KCs outside the odour fired\n", gpu ? "GPU" : "CPU",
            static_cast<unsigned long long>(brain.TotalSpikes() - spikesBefore), firedOutside);

        // The MBONs of the PPL101 compartments: PPL101 makes at least compartmentContacts contacts onto them
        std::set<uint32_t> punishedMbons;
        for (uint32_t dan : ppl101.neurons)
        {
            for (uint32_t e = cn->EdgeBegin(dan); e < cn->EdgeEnd(dan); ++e)
            {
                if (cn->Type(cn->Targets()[e]).substr(0, 4) == "MBON" && std::abs(int(cn->Weights()[e])) >= int(PlasticityRule{}.compartmentContacts))
                {
                    punishedMbons.insert(cn->Targets()[e]);
                }
            }
        }
        uint32_t weakenedOdour = 0;
        uint32_t odourInCompartment = 0;
        uint32_t weakenedOthers = 0;
        uint32_t others = 0;
        double minScale = 1.0;
        std::map<std::string, std::pair<uint32_t, uint32_t>> byMbon; // weakened, all (odour KCs)
        for (uint32_t k = 0; k < brain.PlasticEdgeCount(); ++k)
        {
            const double s = brain.PlasticScale(k);
            minScale = std::min(minScale, s);
            const bool fromOdour = odourSet.count(brain.PlasticPre(k)) != 0;
            if (fromOdour && punishedMbons.count(brain.PlasticPost(k)) != 0)
            {
                ++odourInCompartment;
                weakenedOdour += s < 0.999 ? 1 : 0;
                auto& entry = byMbon[std::string(cn->Type(brain.PlasticPost(k)))];
                entry.first += s < 0.999 ? 1 : 0;
                ++entry.second;
            }
            if (!fromOdour)
            {
                ++others;
                weakenedOthers += s < 0.999 ? 1 : 0;
            }
        }
        std::printf("  %s: PPL101 reaches %zu MBONs; odour (%zu KCs) synapses onto them weakened: %u of %u (lowest scale %.3f); synapses of other KCs weakened: %u of %u\n",
            gpu ? "GPU" : "CPU", punishedMbons.size(), odour.Size(), weakenedOdour, odourInCompartment, minScale, weakenedOthers, others);
        for (const auto& [type, entry] : byMbon)
        {
            std::printf("    %s: %u of %u weakened\n", type.c_str(), entry.first, entry.second);
        }
        Check(!punishedMbons.empty() && weakenedOdour * 2 > odourInCompartment, "most odour synapses onto the PPL101 compartments weaken");
        Check(weakenedOthers * 50 < others, "the synapses of the KCs outside the odour stay (fewer than 2 % change)");
    }
}

int main()
{
    myengine::jobs::Initialize();
    int result = 0;
    try
    {
        {
            ConnectomeBuilder probe;
            probe.AddNeuron(Neuron(1, "x"));
            FlyBrain brain(Connectome::FromImage(probe.Build("x")));
            GpuOptions options;
            options.allowWarp = true;
            options.selfTest = false;
            gGpu = brain.SetBackend(BackendChoice::Gpu, options) == Backend::Gpu;
        }
        TestSynthetic();
        std::cout << "conditioning on a synthetic mushroom body: ok\n";
        const std::string path = std::string(MYENGINE_SOURCE_DIR) + "/assets/flybrain/malecns_v1.fbrain";
        if (fs::exists(fs::u8path(path)))
        {
            std::string error;
            const auto cn = Connectome::Load(path, &error);
            Check(cn != nullptr, error);
            TestConnectome(cn, false);
            if (gGpu)
            {
                TestConnectome(cn, true);
            }
            std::cout << "MaleCNS: ok\n";
        }
        else
        {
            std::cout << "the connectome is not downloaded: its part is skipped\n";
        }
        std::cout << "OK: KC -> MBON learning by the dopamine of the compartment, memory, CPU = GPU\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        result = 1;
    }
    myengine::jobs::Shutdown();
    return result;
}
