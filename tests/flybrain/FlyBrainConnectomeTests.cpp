// FlyBrainConnectomeTests.cpp
// N0 (FlyDoom): the model on the real connectome MaleCNS v1.0 (assets/flybrain/malecns_v1.fbrain). Exit code 77 (skipped)
// when the file is not downloaded (tools/flybrain/fetch_malecns.ps1).
//   - the converter kept the proofread (status Traced) neurons: 164,789 neurons, 25,563,197 edges, 124,025,046 contacts
//   - cell counts of the types FlyDoom uses (R1-R6, DNa02, MDN, Giant Fiber, LPLC2, ...) among the traced neurons
//   - known body ids (PPL101, MBON11 of DOOMFLY)
//   - silence: with no input nothing fires
//   - Shiu et al. 2024: taste neurons at 150 Hz make the proboscis motor neuron MN9 fire (proboscis extension): LB3c (the
//     sugar input of DOOMFLY) does, and some gustatory types do not
//
//   myengine_flybrain_connectome_tests [path.fbrain]

#include <flybrain/Connectome.h>
#include <flybrain/FlyBrain.h>

#include <myengine/jobs/JobSystem.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

    void CheckCount(const Connectome& cn, const char* type, Side side, size_t expected)
    {
        const size_t count = cn.Find(type, side).Size();
        Check(count == expected, std::string(type) + " (" + ToString(side) + "): " + std::to_string(count) + " neurons, expected " + std::to_string(expected));
    }

    void TestGraph(const Connectome& cn)
    {
        Check(cn.NeuronCount() == 164789, "164,789 traced neurons, got " + std::to_string(cn.NeuronCount()));
        Check(cn.EdgeCount() == 25563197, "25,563,197 edges between them, got " + std::to_string(cn.EdgeCount()));
        uint64_t contacts = 0;
        for (uint64_t e = 0; e < cn.EdgeCount(); ++e)
        {
            contacts += static_cast<uint64_t>(std::abs(int(cn.Weights()[e])));
        }
        std::printf("  %u neurons, %llu edges, %llu contacts, %u types\n", cn.NeuronCount(), static_cast<unsigned long long>(cn.EdgeCount()),
            static_cast<unsigned long long>(contacts), cn.TypeCount());
        Check(contacts == 124025046, "124,025,046 synaptic contacts (no weight clamped to int16)");

        // Traced neurons of MaleCNS v1.0 (the Cell Type Explorer counts all R1-R6, 3377, most of them not traced)
        CheckCount(cn, "R1-R6", Side::Left, 501);
        CheckCount(cn, "R1-R6", Side::Right, 893);
        CheckCount(cn, "DNa01", Side::Left, 1);
        CheckCount(cn, "DNa01", Side::Right, 1);
        CheckCount(cn, "DNa02", Side::Left, 1);
        CheckCount(cn, "DNa02", Side::Right, 1);
        CheckCount(cn, "DNp09", Side::Any, 2);
        CheckCount(cn, "MDN", Side::Any, 4);
        CheckCount(cn, "DNp01", Side::Any, 2);
        CheckCount(cn, "LPLC2", Side::Any, 185);
        CheckCount(cn, "LC4", Side::Any, 126);
        CheckCount(cn, "PPL101", Side::Any, 2);
        CheckCount(cn, "MN9", Side::Any, 2);
        CheckCount(cn, "L1", Side::Any, 1776);

        for (const uint64_t bodyId : { 11327ull, 11900ull })
        {
            const uint32_t neuron = cn.FindBodyId(bodyId);
            Check(neuron != UINT32_MAX && cn.Type(neuron) == "PPL101", "body " + std::to_string(bodyId) + " is PPL101");
            Check(cn.NeuronTransmitter(neuron) == Transmitter::Dopamine, "PPL101 is dopaminergic");
        }
        for (const uint64_t bodyId : { 10704ull, 11402ull })
        {
            const uint32_t neuron = cn.FindBodyId(bodyId);
            Check(neuron != UINT32_MAX && cn.Type(neuron) == "MBON11", "body " + std::to_string(bodyId) + " is MBON11");
        }
        // Photoreceptors sit in optic lobe columns; the converter keeps assignedOlHex1/2 for the eye of N1
        const Group l1 = cn.Find("L1");
        size_t withHex = 0;
        for (uint32_t neuron : l1.neurons)
        {
            withHex += cn.Hex1(neuron) != kNoHex ? 1 : 0;
        }
        Check(withHex * 10 >= l1.Size() * 9, "L1 neurons have optic lobe columns");
    }

    void TestSilence(const std::shared_ptr<const Connectome>& connectome)
    {
        FlyBrain brain(connectome);
        brain.SetParallel(true);
        brain.AdvanceMs(200.0);
        Check(brain.TotalSpikes() == 0, "with no input the LIF network stays at rest");
    }

    void TestFeeding(const std::shared_ptr<const Connectome>& connectome)
    {
        const Connectome& cn = *connectome;
        const Group mn9 = cn.Find("MN9");
        Check(mn9.Size() == 2, "MN9 is in the connectome");

        // Every gustatory type of the labellum, the taste pegs and the pharynx, one at a time at 150 Hz for 1 s
        std::vector<std::string> types;
        for (const char* prefix : { "LB", "PhG", "claw_tpGRN", "dorsal_tpGRN" })
        {
            const Group group = cn.FindPrefix(prefix);
            for (uint32_t neuron : group.neurons)
            {
                const std::string type(cn.Type(neuron));
                if (cn.CellClass(neuron) == "gustatory" && std::find(types.begin(), types.end(), type) == types.end())
                {
                    types.push_back(type);
                }
            }
        }
        std::sort(types.begin(), types.end());
        Check(types.size() >= 30, "the gustatory types of the labellum, the taste pegs and the pharynx are annotated");
        float lb3c = 0.0f;
        size_t silentTypes = 0;
        for (const std::string& type : types)
        {
            const Group input = cn.Find(type);
            FlyBrain brain(connectome);
            brain.SetParallel(true);
            const WatchId watch = brain.Watch(mn9, 1000.0f);
            brain.StimulateRate(input, 150.0f, 1000.0f);
            brain.AdvanceMs(1000.0);
            const float rate = brain.RateHz(watch, 1000.0f);
            std::printf("  %-14s x%-3zu -> MN9 %6.1f Hz, %8llu spikes in the brain\n", type.c_str(), input.Size(), rate,
                static_cast<unsigned long long>(brain.TotalSpikes()));
            lb3c = type == "LB3c" ? rate : lb3c;
            silentTypes += rate < 1.0f ? 1 : 0;
        }
        // MaleCNS does not say which receptor neurons sense sugar. LB3c is the sugar input of DOOMFLY; the response must also be
        // selective, not every taste neuron moving the proboscis
        Check(lb3c > 20.0f, "LB3c (the sugar input of DOOMFLY) drives MN9");
        Check(silentTypes >= 5, "some gustatory types leave MN9 silent");
    }
}

int main(int argc, char** argv)
{
    const std::string path = argc > 1 ? argv[1] : std::string(MYENGINE_SOURCE_DIR) + "/assets/flybrain/malecns_v1.fbrain";
    if (!std::filesystem::exists(std::filesystem::u8path(path)))
    {
        std::cout << "SKIPPED: " << path << " is not downloaded (tools/flybrain/fetch_malecns.ps1)\n";
        return 77;
    }
    try
    {
        std::string error;
        const auto connectome = Connectome::Load(path, &error);
        Check(connectome != nullptr, "load " + path + ": " + error);
        std::cout << connectome->Attribution() << "\n";
        TestGraph(*connectome);
        std::cout << "graph: ok\n";

        myengine::jobs::Initialize();
        TestSilence(connectome);
        std::cout << "silence: ok\n";
        TestFeeding(connectome);
        std::cout << "feeding: ok\n";
        myengine::jobs::Shutdown();
        std::cout << "OK: MaleCNS v1.0 traced graph, cell counts, known ids, silence at rest, LB3c -> MN9 and selective\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        myengine::jobs::Shutdown();
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
