// FlyBrainKcVisionTests.cpp
// N3b (FlyDoom): the fly learns what it sees. On the connectome (assets/flybrain/malecns_v1.fbrain; exit code 77, skipped,
// without it), with dopamine as a modulator and the local neurons of the antennal lobe silent (ModelParams):
//   - KcVision finds the visual inputs of the Kenyon cells and a receptive field of optic lobe columns for almost all
//   - a picture X (a part of the left eye) makes a sparse part of the visual KCs (KCg-d, KCab-p) fire, and the brain
//     comes back to its background after it: no self-sustained activity
//   - "view X + damage": X paired with PPL1 (punishment) -> the MBONs of the PPL1 compartments answer X less; a picture Y
//     of the other eye (other KCs, not paired) keeps its answer; with frozen learning the answer to X stays
// docs/flybrain/flybrain.md, section "Муха учится в игре".

#include <flybrain/Connectome.h>
#include <flybrain/FlyBrain.h>
#include <flybrain/KcVision.h>

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

    ModelParams GameParams()
    {
        ModelParams params;
        params.fastModulatorySynapses = false;
        params.silentTypePrefixes = { "lLN" };
        return params;
    }

    uint64_t Spikes(const FlyBrain& brain, const std::vector<uint32_t>& neurons)
    {
        uint64_t total = 0;
        for (uint32_t n : neurons)
        {
            total += brain.NeuronSpikeCount(n);
        }
        return total;
    }

    struct Scene
    {
        std::shared_ptr<const Connectome> cn;
        KcVision vision;
        std::vector<float> pictureX;
        std::vector<float> pictureY;
        std::vector<float> dark;
        Group lamina;
        std::vector<uint32_t> visualKcs; // KCg-d, KCab-p
        std::vector<uint32_t> mbons;     // the MBONs of the PPL1 compartments
        std::vector<uint32_t> readersX;  // of them, the ones the KCs of X reach (Readers)
        std::vector<uint32_t> readersY;
        Group punishment;
    };

    // The fly sees `picture` for `ms`; returns the spikes of the MBONs in that time
    uint64_t Show(FlyBrain& brain, const Scene& scene, const std::vector<float>& picture, double ms)
    {
        const uint64_t before = Spikes(brain, scene.mbons);
        scene.vision.Apply(brain, picture);
        for (double t = 0.0; t + 0.5 < ms; t += kTick)
        {
            brain.AdvanceMs(kTick);
        }
        scene.vision.Apply(brain, scene.dark);
        return Spikes(brain, scene.mbons) - before;
    }

    void Rest(FlyBrain& brain, double ms)
    {
        for (double t = 0.0; t + 0.5 < ms; t += kTick)
        {
            brain.AdvanceMs(kTick);
        }
    }

    void TestVision(Scene& scene)
    {
        const Connectome& cn = *scene.cn;
        std::string error;
        Check(scene.vision.Build(cn, KcVisionConfig{}, &error), "KcVision: " + error);
        const auto& inputs = scene.vision.Inputs();
        const size_t withField = std::count_if(inputs.begin(), inputs.end(), [](const KcVisionInput& in) { return in.field.size() >= 3; });
        std::printf("  KcVision: %zu visual inputs of KCs, %zu with a field of >= 3 columns, %zu columns\n", inputs.size(), withField,
            scene.vision.Columns().size());
        Check(inputs.size() >= 100, "at least 100 visual neurons make 10 or more contacts onto KCs");
        Check(withField * 10 >= inputs.size() * 9, "at least 90 % of them have a receptive field");

        // X: the half of the left eye with the lower hex1; Y: the same half of the right eye
        std::vector<int> hex1[2];
        for (const KcVisionColumn& c : scene.vision.Columns())
        {
            if (c.side == Side::Left || c.side == Side::Right)
            {
                hex1[c.side == Side::Left ? 0 : 1].push_back(c.hex1);
            }
        }
        int median[2] = { 0, 0 };
        for (int eye = 0; eye < 2; ++eye)
        {
            Check(!hex1[eye].empty(), "columns on both sides");
            std::nth_element(hex1[eye].begin(), hex1[eye].begin() + hex1[eye].size() / 2, hex1[eye].end());
            median[eye] = hex1[eye][hex1[eye].size() / 2];
        }
        const size_t columns = scene.vision.Columns().size();
        scene.pictureX.assign(columns, 0.0f);
        scene.pictureY.assign(columns, 0.0f);
        scene.dark.assign(columns, 0.0f);
        for (size_t i = 0; i < columns; ++i)
        {
            const KcVisionColumn& c = scene.vision.Columns()[i];
            if (c.side == Side::Left && c.hex1 <= median[0])
            {
                scene.pictureX[i] = 1.0f;
            }
            if (c.side == Side::Right && c.hex1 <= median[1])
            {
                scene.pictureY[i] = 1.0f;
            }
        }
        for (const char* type : { "L1", "L2", "L3", "L5" })
        {
            const Group g = cn.Find(type);
            scene.lamina.neurons.insert(scene.lamina.neurons.end(), g.neurons.begin(), g.neurons.end());
        }
        for (uint32_t n = 0; n < cn.NeuronCount(); ++n)
        {
            if (cn.Type(n) == "KCg-d" || cn.Type(n) == "KCab-p")
            {
                scene.visualKcs.push_back(n);
            }
        }

        // Sparse KCs and no runaway: 1 s of the background, the picture for 1 s, 1 s of the background again; the same
        // brain without the picture is the control (the lamina gets its constant drive as in the game, FlyEyes)
        std::vector<uint32_t> kcs;
        for (uint32_t n = 0; n < cn.NeuronCount(); ++n)
        {
            if (cn.Type(n).substr(0, 2) == "KC")
            {
                kcs.push_back(n);
            }
        }
        struct Run
        {
            uint32_t fired = 0;
            uint64_t during = 0;
            uint64_t after = 0;   // brain spikes in the last 0.5 s
            uint64_t kcAfter = 0; // KC spikes in the last 0.5 s
        };
        auto run = [&](bool picture)
        {
            FlyBrain brain(scene.cn, GameParams());
            brain.SetParallel(true);
            brain.StimulateCurrent(scene.lamina, 12.0f);
            Rest(brain, 1000.0);
            std::vector<uint32_t> kcBefore;
            for (uint32_t kc : scene.visualKcs)
            {
                kcBefore.push_back(brain.NeuronSpikeCount(kc));
            }
            Run result;
            const uint64_t duringBefore = brain.TotalSpikes();
            if (picture)
            {
                Show(brain, scene, scene.pictureX, 1000.0);
            }
            else
            {
                Rest(brain, 1000.0);
            }
            result.during = brain.TotalSpikes() - duringBefore;
            for (size_t k = 0; k < scene.visualKcs.size(); ++k)
            {
                result.fired += brain.NeuronSpikeCount(scene.visualKcs[k]) > kcBefore[k] ? 1 : 0;
            }
            Rest(brain, 500.0);
            const uint64_t afterBefore = brain.TotalSpikes();
            const uint64_t kcAfterBefore = Spikes(brain, kcs);
            Rest(brain, 500.0);
            result.after = brain.TotalSpikes() - afterBefore;
            result.kcAfter = Spikes(brain, kcs) - kcAfterBefore;
            return result;
        };
        const Run seen = run(true);
        const Run control = run(false);
        const double share = double(seen.fired) / double(scene.visualKcs.size());
        std::printf("  picture X: %u of %zu visual KCs fired (%.1f %%, without the picture %u); brain spikes / s: with X %llu (without %llu), "
                    "0.5 s after X %llu (without %llu); KC spikes in that 0.5 s %llu (without %llu)\n",
            seen.fired, scene.visualKcs.size(), 100.0 * share, control.fired, static_cast<unsigned long long>(seen.during),
            static_cast<unsigned long long>(control.during), static_cast<unsigned long long>(seen.after * 2), static_cast<unsigned long long>(control.after * 2),
            static_cast<unsigned long long>(seen.kcAfter), static_cast<unsigned long long>(control.kcAfter));
        Check(share >= 0.02 && share <= 0.30, "a sparse part of the visual KCs answers the picture (2 .. 30 %)");
        // After the picture the ring attractor of the central complex (PEN, EPG, FR1, Delta7) may stay in another state
        // (some 80 000 spikes / s more, the probe shows it with --diff): that is its persistence, not a runaway. The KCs
        // must be quiet again and the brain far from the self-sustained activity (millions of spikes / s, 76 % of KCs)
        Check(seen.after <= control.after + control.after / 2 && seen.kcAfter * 2 <= scene.visualKcs.size(),
            "after the picture the KCs are quiet again and the brain does not run away");
    }

    struct Learned
    {
        uint64_t xBefore = 0;    // spikes of the MBONs that read X, to X
        uint64_t yBefore = 0;    // spikes of the MBONs that read Y, to Y
        uint64_t xAfter = 0;
        uint64_t yAfter = 0;
        double xScale = 1.0;     // mean scale of the synapses of the KCs that answer X onto the MBONs of the PPL1 compartments
        double yScale = 1.0;     // the same for Y
        double quietScale = 1.0; // the same for the KCs that answer neither
    };

    // The MBONs of the PPL1 compartments that a picture reaches: its KCs make at least 2 % of their excitatory contacts.
    // Others answer a picture too, but through the central complex (MBON30 gets FR1, CRE067, LAL155), whose state drifts
    std::vector<uint32_t> Readers(const Connectome& cn, const std::vector<uint32_t>& mbons, const std::set<uint32_t>& kcs)
    {
        const std::set<uint32_t> targets(mbons.begin(), mbons.end());
        std::map<uint32_t, std::pair<uint64_t, uint64_t>> contacts; // from the KCs of the picture, all excitatory
        for (uint32_t pre = 0; pre < cn.NeuronCount(); ++pre)
        {
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                const uint32_t post = cn.Targets()[e];
                if (targets.count(post) && cn.Weights()[e] > 0)
                {
                    contacts[post].second += uint64_t(cn.Weights()[e]);
                    contacts[post].first += kcs.count(pre) ? uint64_t(cn.Weights()[e]) : 0;
                }
            }
        }
        std::vector<uint32_t> readers;
        for (const auto& [mbon, c] : contacts)
        {
            if (c.first * 50 >= c.second)
            {
                readers.push_back(mbon);
            }
        }
        return readers;
    }

    // The MBON answers are measured with learning frozen, so that only the pairings can change a weight (the DANs fire
    // a little at any picture, and learning that is always on would learn from the measurements too)
    Learned Condition(Scene& scene, bool learning)
    {
        FlyBrain brain(scene.cn, GameParams());
        brain.SetParallel(true);
        std::string error;
        Check(brain.EnablePlasticity(PlasticityRule{}, &error), "plasticity: " + error);
        brain.SetLearning(false);
        const Connectome& cn = *scene.cn;
        if (scene.mbons.empty())
        {
            // The MBONs of the PPL1 compartments: a PPL1 makes at least compartmentContacts contacts onto them
            std::set<uint32_t> mbons;
            for (uint32_t dan : brain.PunishmentNeurons().neurons)
            {
                for (uint32_t e = cn.EdgeBegin(dan); e < cn.EdgeEnd(dan); ++e)
                {
                    if (cn.Type(cn.Targets()[e]).substr(0, 4) == "MBON" && std::abs(int(cn.Weights()[e])) >= int(PlasticityRule{}.compartmentContacts))
                    {
                        mbons.insert(cn.Targets()[e]);
                    }
                }
            }
            scene.mbons.assign(mbons.begin(), mbons.end());
            scene.punishment = brain.PunishmentNeurons();
        }
        brain.StimulateCurrent(scene.lamina, 12.0f);
        Rest(brain, 500.0);

        std::set<uint32_t> plasticKcs;
        for (uint32_t k = 0; k < brain.PlasticEdgeCount(); ++k)
        {
            plasticKcs.insert(brain.PlasticPre(k));
        }
        // Shows a picture for 2 s: the KCs that fire, and the spikes of every MBON of the PPL1 compartments
        auto look = [&](const std::vector<float>& picture, std::vector<uint32_t>& mbonSpikes)
        {
            std::vector<uint32_t> kcBefore;
            for (uint32_t kc : plasticKcs)
            {
                kcBefore.push_back(brain.NeuronSpikeCount(kc));
            }
            std::vector<uint32_t> mbonBefore;
            for (uint32_t m : scene.mbons)
            {
                mbonBefore.push_back(brain.NeuronSpikeCount(m));
            }
            Show(brain, scene, picture, 2000.0);
            mbonSpikes.clear();
            for (size_t i = 0; i < scene.mbons.size(); ++i)
            {
                mbonSpikes.push_back(brain.NeuronSpikeCount(scene.mbons[i]) - mbonBefore[i]);
            }
            std::set<uint32_t> fired;
            size_t index = 0;
            for (uint32_t kc : plasticKcs)
            {
                if (brain.NeuronSpikeCount(kc) > kcBefore[index++])
                {
                    fired.insert(kc);
                }
            }
            return fired;
        };
        auto sumOver = [&](const std::vector<uint32_t>& spikes, const std::vector<uint32_t>& readers)
        {
            uint64_t total = 0;
            for (size_t i = 0; i < scene.mbons.size(); ++i)
            {
                total += std::count(readers.begin(), readers.end(), scene.mbons[i]) ? spikes[i] : 0;
            }
            return total;
        };

        Learned result;
        std::vector<uint32_t> spikesX;
        std::vector<uint32_t> spikesY;
        const std::set<uint32_t> xKcs = look(scene.pictureX, spikesX);
        Rest(brain, 500.0);
        const std::set<uint32_t> yKcs = look(scene.pictureY, spikesY);
        Rest(brain, 500.0);
        if (scene.readersX.empty())
        {
            scene.readersX = Readers(cn, scene.mbons, xKcs);
            scene.readersY = Readers(cn, scene.mbons, yKcs);
        }
        result.xBefore = sumOver(spikesX, scene.readersX);
        result.yBefore = sumOver(spikesY, scene.readersY);
        {
            uint32_t both = 0;
            uint32_t xLeft = 0;
            uint32_t yLeft = 0;
            for (uint32_t kc : xKcs)
            {
                both += yKcs.count(kc) ? 1 : 0;
                xLeft += cn.NeuronSide(kc) == Side::Left ? 1 : 0;
            }
            for (uint32_t kc : yKcs)
            {
                yLeft += cn.NeuronSide(kc) == Side::Left ? 1 : 0;
            }
            if (learning)
            {
                std::printf("  KCs that answer X (left eye): %zu, %u of them on the left; Y (right eye): %zu, %u on the left; both: %u\n", xKcs.size(), xLeft,
                    yKcs.size(), yLeft, both);
            }
        }

        // "view X + damage": X for 0.6 s with the punishment of FlyPlayer (15 mV into PPL1, above their threshold)
        brain.SetLearning(learning);
        for (int trial = 0; trial < 12; ++trial)
        {
            const StimulusId d = brain.StimulateCurrent(scene.punishment, 15.0f);
            Show(brain, scene, scene.pictureX, 600.0);
            brain.StopStimulus(d);
            Rest(brain, 400.0);
        }
        brain.SetLearning(false);
        Rest(brain, 500.0);
        look(scene.pictureX, spikesX);
        Rest(brain, 500.0);
        look(scene.pictureY, spikesY);
        result.xAfter = sumOver(spikesX, scene.readersX);
        result.yAfter = sumOver(spikesY, scene.readersY);

        const std::set<uint32_t> mbons(scene.mbons.begin(), scene.mbons.end());
        const std::vector<double> scales = brain.GetPlasticScales();
        double sum[3] = { 0.0, 0.0, 0.0 };
        uint32_t count[3] = { 0, 0, 0 };
        for (uint32_t k = 0; k < brain.PlasticEdgeCount(); ++k)
        {
            if (!mbons.count(brain.PlasticPost(k)))
            {
                continue;
            }
            const uint32_t kc = brain.PlasticPre(k);
            const int kind = xKcs.count(kc) ? 0 : yKcs.count(kc) ? 1 : 2;
            sum[kind] += scales[k];
            ++count[kind];
        }
        result.xScale = count[0] ? sum[0] / count[0] : 1.0;
        result.yScale = count[1] ? sum[1] / count[1] : 1.0;
        result.quietScale = count[2] ? sum[2] / count[2] : 1.0;
        std::printf("  learning %s: the %zu MBONs that read X answer X %llu -> %llu spikes; the %zu that read Y answer Y %llu -> %llu; mean scale of "
                    "the synapses of the KCs of X %.3f (%u), of Y %.3f (%u), of the other KCs %.4f (%u)\n",
            learning ? "on " : "off", scene.readersX.size(), static_cast<unsigned long long>(result.xBefore), static_cast<unsigned long long>(result.xAfter),
            scene.readersY.size(), static_cast<unsigned long long>(result.yBefore), static_cast<unsigned long long>(result.yAfter), result.xScale, count[0],
            result.yScale, count[1], result.quietScale, count[2]);
        return result;
    }

    void TestLearning(Scene& scene)
    {
        const Learned on = Condition(scene, true);
        const Learned off = Condition(scene, false);
        std::printf("  MBONs that read X:");
        for (uint32_t m : scene.readersX)
        {
            std::printf(" %s", std::string(scene.cn->Type(m)).c_str());
        }
        std::printf("; read Y:");
        for (uint32_t m : scene.readersY)
        {
            std::printf(" %s", std::string(scene.cn->Type(m)).c_str());
        }
        std::printf("\n");
        Check(on.xBefore >= 20 && on.yBefore >= 20, "the MBONs that read the pictures answer them");
        Check(on.xBefore == off.xBefore && on.yBefore == off.yBefore, "before the pairings both arms are the same brain");
        Check(off.xScale == 1.0 && off.yScale == 1.0 && off.quietScale == 1.0, "frozen learning changes no weight");
        Check(on.xScale < 0.9, "the synapses of the KCs that answer X onto the PPL1 compartments weaken");
        Check(on.quietScale > 0.995, "the synapses of the KCs that answer neither picture stay");
        // Some KCs of the right mushroom body answer X too (visual inputs from the left eye reach both calyces): over the
        // pairings they are punished as well, so Y loses a part of its answer. X loses more
        Check(on.xScale < on.yScale, "the synapses of the KCs of X weaken more than those of the KCs of Y");
        // The brain drifts between the measurements (the central complex), the same in both arms: learning is the difference
        Check(on.xAfter * 10 <= off.xAfter * 8, "after X + damage the MBONs answer X at least 20 % less than without learning");
        Check(on.xAfter * off.yAfter < on.yAfter * off.xAfter, "the answer to Y (not paired) falls less than the answer to X");
    }
}

int main(int argc, char** argv)
{
    const std::string path = argc > 1 ? argv[1] : std::string(MYENGINE_SOURCE_DIR) + "/assets/flybrain/malecns_v1.fbrain";
    if (!fs::exists(fs::u8path(path)))
    {
        std::printf("SKIPPED: no connectome at %s (tools/flybrain/fetch_malecns.ps1)\n", path.c_str());
        return 77;
    }
    std::string error;
    Scene scene;
    scene.cn = Connectome::Load(path, &error);
    if (!scene.cn)
    {
        std::cerr << "FAILED: " << error << "\n";
        return 1;
    }
    myengine::jobs::Initialize();
    int result = 0;
    try
    {
        TestVision(scene);
        TestLearning(scene);
        std::printf("flybrain kc vision tests: OK\n");
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAILED: " << e.what() << "\n";
        result = 1;
    }
    myengine::jobs::Shutdown();
    return result;
}
