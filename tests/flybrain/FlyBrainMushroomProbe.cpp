// FlyBrainMushroomProbe.cpp
// N3b (FlyDoom): what the connectome says about learning in the game (docs/flybrain/flybrain.md, "Муха учится в игре").
//   inputs       the presynaptic types of the Kenyon cells: contacts, how many KCs each reaches, visual or not
//   mbon         how the output neurons of the mushroom body reach the descending neurons of the controls (1 and 2 hops)
//   drive        Poisson input into the visual inputs of the KCs: what share of the KCs fires, and whether the brain runs away
//   --who A,B    the inputs of these types: presynaptic types by signed contacts
//   fields       the receptive fields of the visual inputs of KCs: optic lobe columns 1 and 2 hops up
//   ppl1         the PPL1 rates under a constant current (--rates are the currents, mV)
//   mbresponse   the MBON rates with and without a picture (--share of the visual inputs of KCs at the first of --rates)
//   determinism  the same game-like run (drive every tic, damage, learning) twice, without learning, on one thread: the same spikes?
//
//   myengine_flybrain_mb_probe [inputs] [mbon] [drive] [determinism] [--fbrain <path>] [--rates 10,20,40] [--types A,B] [--seconds 10]
//                              [--no-kc-kc]   (drops the KC -> KC synapses first) [--no-axo] (and the sensory -> sensory ones)
//                              [--drop lLN,B] (drops the outputs of the types with these prefixes)
//                              [--lamina 12] (drive: a constant current into L1/L2/L3/L5 as FlyEyes gives)
//                              [--share 0.2] (drive: a fixed part of the inputs; --types visual: all visual inputs of KCs)
//                              [--on 1000] (drive: the input stops after so many ms) [--diff] (the timeline above a run without input)
//                              --seconds: of drive (1 by default) and of the determinism run (10) [--top 8] (drive: every 50 ms the types with most spikes)

#include <flybrain/Connectome.h>
#include <flybrain/FlyBrain.h>
#include <flybrain/KcVision.h>

#include <myengine/jobs/JobSystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
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

    bool IsKc(std::string_view type)
    {
        return type.substr(0, 2) == "KC";
    }

    bool IsVisual(std::string_view superclass)
    {
        return superclass == "visual_projection" || superclass == "visual_projection_tbc" || superclass == "ol_intrinsic" || superclass == "ol_sensory"
            || superclass == "visual_centrifugal";
    }

    struct InputType
    {
        std::string type;
        std::string superclass;
        uint64_t contacts = 0;
        uint32_t neurons = 0;
        std::set<uint32_t> kcs;
    };

    std::vector<InputType> KcInputs(const Connectome& cn)
    {
        std::map<std::string, InputType> byType;
        std::set<uint32_t> seenPre;
        for (uint32_t pre = 0; pre < cn.NeuronCount(); ++pre)
        {
            if (IsKc(cn.Type(pre)))
            {
                continue; // KC -> KC is not an input from outside
            }
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                const uint32_t post = cn.Targets()[e];
                if (!IsKc(cn.Type(post)))
                {
                    continue;
                }
                const std::string type = cn.Type(pre).empty() ? "(untyped)" : std::string(cn.Type(pre));
                InputType& in = byType[type];
                in.type = type;
                in.superclass = std::string(cn.Superclass(pre));
                in.contacts += static_cast<uint64_t>(std::abs(int(cn.Weights()[e])));
                in.kcs.insert(post);
                if (seenPre.insert(pre).second)
                {
                    ++in.neurons;
                }
            }
        }
        std::vector<InputType> out;
        for (auto& [type, in] : byType)
        {
            out.push_back(std::move(in));
        }
        std::sort(out.begin(), out.end(), [](const InputType& a, const InputType& b) { return a.contacts > b.contacts; });
        return out;
    }

    void ReportInputs(const Connectome& cn)
    {
        std::map<std::string, uint32_t> kcTypes;
        uint32_t kcs = 0;
        for (uint32_t i = 0; i < cn.NeuronCount(); ++i)
        {
            if (IsKc(cn.Type(i)))
            {
                ++kcTypes[std::string(cn.Type(i))];
                ++kcs;
            }
        }
        std::printf("KCs: %u in %zu types:", kcs, kcTypes.size());
        for (const auto& [type, count] : kcTypes)
        {
            std::printf(" %s %u;", type.c_str(), count);
        }
        uint64_t kcToKc = 0;
        uint64_t kcOut = 0;
        for (uint32_t pre = 0; pre < cn.NeuronCount(); ++pre)
        {
            if (!IsKc(cn.Type(pre)))
            {
                continue;
            }
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                const uint64_t contacts = static_cast<uint64_t>(std::abs(int(cn.Weights()[e])));
                kcOut += contacts;
                kcToKc += IsKc(cn.Type(cn.Targets()[e])) ? contacts : 0;
            }
        }
        std::printf("\nKC outputs: %llu contacts, of them onto other KCs: %llu (%.1f %%)\n\n", static_cast<unsigned long long>(kcOut),
            static_cast<unsigned long long>(kcToKc), kcOut ? 100.0 * kcToKc / kcOut : 0.0);

        const std::vector<InputType> inputs = KcInputs(cn);
        uint64_t total = 0;
        uint64_t visual = 0;
        for (const InputType& in : inputs)
        {
            total += in.contacts;
            visual += IsVisual(in.superclass) ? in.contacts : 0;
        }
        std::printf("inputs of the KCs from outside: %zu types, %llu contacts, of them visual (optic lobe / visual projection): %llu (%.1f %%)\n\n",
            inputs.size(), static_cast<unsigned long long>(total), static_cast<unsigned long long>(visual), total ? 100.0 * visual / total : 0.0);
        std::printf("| type | superclass | neurons | contacts onto KCs | KCs reached | KC types reached |\n| --- | --- | --- | --- | --- | --- |\n");
        int shown = 0;
        for (const InputType& in : inputs)
        {
            if (shown >= 30 && !IsVisual(in.superclass))
            {
                continue;
            }
            std::map<std::string, uint32_t> reached;
            for (uint32_t kc : in.kcs)
            {
                ++reached[std::string(cn.Type(kc))];
            }
            std::string kinds;
            for (const auto& [type, count] : reached)
            {
                kinds += type + " " + std::to_string(count) + " ";
            }
            std::printf("| %s%s | %s | %u | %llu | %zu | %s|\n", in.type.c_str(), IsVisual(in.superclass) ? " (visual)" : "", in.superclass.c_str(), in.neurons,
                static_cast<unsigned long long>(in.contacts), in.kcs.size(), kinds.c_str());
            if (++shown >= 60)
            {
                break;
            }
        }
        std::printf("\n");
    }

    // The inputs of one type: presynaptic types by signed contacts (the untyped ones as "(untyped) superclass")
    void ReportWho(const Connectome& cn, const std::string& type)
    {
        const Group group = cn.Find(type);
        std::set<uint32_t> members(group.neurons.begin(), group.neurons.end());
        if (members.empty())
        {
            std::printf("who %s: no such type\n", type.c_str());
            return;
        }
        std::map<std::string, std::pair<int64_t, uint32_t>> byType;
        int64_t excitatory = 0;
        int64_t inhibitory = 0;
        for (uint32_t pre = 0; pre < cn.NeuronCount(); ++pre)
        {
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                if (!members.count(cn.Targets()[e]))
                {
                    continue;
                }
                const std::string name = cn.Type(pre).empty() ? "(untyped) " + std::string(cn.Superclass(pre)) : std::string(cn.Type(pre));
                auto& entry = byType[name + " " + ToString(cn.NeuronTransmitter(pre))];
                entry.first += cn.Weights()[e];
                ++entry.second;
                (cn.Weights()[e] > 0 ? excitatory : inhibitory) += cn.Weights()[e];
            }
        }
        std::vector<std::pair<std::string, std::pair<int64_t, uint32_t>>> sorted(byType.begin(), byType.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return std::llabs(a.second.first) > std::llabs(b.second.first); });
        std::printf("who %s: %zu neurons, transmitter %s, superclass %s; input +%lld / %lld contacts per neuron +%.0f / %.0f\n", type.c_str(), members.size(),
            ToString(cn.NeuronTransmitter(group.neurons[0])), std::string(cn.Superclass(group.neurons[0])).c_str(), static_cast<long long>(excitatory),
            static_cast<long long>(inhibitory), double(excitatory) / members.size(), double(inhibitory) / members.size());
        for (size_t k = 0; k < sorted.size() && k < 15; ++k)
        {
            std::printf("    %-40s %+8lld contacts (%u synapses)\n", sorted[k].first.c_str(), static_cast<long long>(sorted[k].second.first), sorted[k].second.second);
        }
        std::printf("\n");
    }

    void ReportMbons(const Connectome& cn)
    {
        const std::vector<std::string> dnTypes{ "DNa01", "DNa02", "DNp09", "MDN", "DNp01", "DNp20", "DNpe017" };
        std::set<uint32_t> dns;
        for (const std::string& t : dnTypes)
        {
            for (uint32_t n : cn.Find(t).neurons)
            {
                dns.insert(n);
            }
        }
        // Share of the input of a neuron that a synapse carries
        std::vector<uint64_t> inTotal(cn.NeuronCount(), 0);
        for (uint32_t pre = 0; pre < cn.NeuronCount(); ++pre)
        {
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                inTotal[cn.Targets()[e]] += static_cast<uint64_t>(std::abs(int(cn.Weights()[e])));
            }
        }
        std::map<std::string, std::pair<double, double>> byMbon; // direct, two-hop share of the DN input (signed)
        for (uint32_t m = 0; m < cn.NeuronCount(); ++m)
        {
            if (cn.Type(m).substr(0, 4) != "MBON")
            {
                continue;
            }
            auto& entry = byMbon[std::string(cn.Type(m))];
            for (uint32_t e = cn.EdgeBegin(m); e < cn.EdgeEnd(m); ++e)
            {
                const uint32_t x = cn.Targets()[e];
                const double share = double(cn.Weights()[e]) / double(std::max<uint64_t>(1, inTotal[x]));
                if (dns.count(x))
                {
                    entry.first += share;
                }
                for (uint32_t f = cn.EdgeBegin(x); f < cn.EdgeEnd(x); ++f)
                {
                    const uint32_t dn = cn.Targets()[f];
                    if (dns.count(dn))
                    {
                        entry.second += share * double(cn.Weights()[f]) / double(std::max<uint64_t>(1, inTotal[dn]));
                    }
                }
            }
        }
        std::printf("MBON -> descending neurons of the controls (%zu DNs: DNa01, DNa02, DNp09, MDN, DNp01, DNp20, DNpe017): signed share of the DN input\n", dns.size());
        std::printf("| MBON | direct | 2 hops |\n| --- | --- | --- |\n");
        std::vector<std::pair<std::string, std::pair<double, double>>> sorted(byMbon.begin(), byMbon.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return std::fabs(a.second.second) + std::fabs(a.second.first) > std::fabs(b.second.second) + std::fabs(b.second.first); });
        for (size_t k = 0; k < sorted.size() && k < 25; ++k)
        {
            std::printf("| %s | %+.5f | %+.6f |\n", sorted[k].first.c_str(), sorted[k].second.first, sorted[k].second.second);
        }
        std::printf("\n");
    }

    bool IsSensory(std::string_view superclass)
    {
        return superclass.size() >= 8 && superclass.substr(superclass.size() - 8) == "_sensory";
    }

    // The connectome without the synapses of KCs onto KCs (axo-axonal, in the lobes) and, with sensory, of sensory
    // neurons onto sensory neurons (axon terminals onto axon terminals): a hypothesis test only
    std::shared_ptr<const Connectome> WithoutKcToKc(const Connectome& cn, bool kcToKc, bool sensory, const std::vector<std::string>& dropped)
    {
        ConnectomeBuilder builder;
        for (uint32_t n = 0; n < cn.NeuronCount(); ++n)
        {
            builder.AddNeuron(cn.Info(n));
        }
        for (uint32_t pre = 0; pre < cn.NeuronCount(); ++pre)
        {
            const bool kc = kcToKc && IsKc(cn.Type(pre));
            const bool sense = sensory && IsSensory(cn.Superclass(pre));
            const std::string_view type = cn.Type(pre);
            if (std::any_of(dropped.begin(), dropped.end(), [&](const std::string& prefix) { return type.substr(0, prefix.size()) == prefix; }))
            {
                continue; // the outputs of this type are dropped
            }
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                const uint32_t post = cn.Targets()[e];
                if (!(kc && IsKc(cn.Type(post))) && !(sense && IsSensory(cn.Superclass(post))))
                {
                    builder.AddEdge(pre, cn.Targets()[e], cn.Weights()[e]);
                }
            }
        }
        std::string error;
        auto out = Connectome::FromImage(builder.Build(cn.Attribution()), &error);
        if (!out)
        {
            std::fprintf(stderr, "%s\n", error.c_str());
        }
        return out;
    }

    void Drive(const std::shared_ptr<const Connectome>& cn, const std::vector<std::string>& types, const std::vector<std::string>& rates, int topTypes, float laminaMv,
        double share, double ms, double onMs, bool diff)
    {
        // the visual neurons that synapse onto KCs
        std::vector<uint32_t> visualInputs;
        for (uint32_t pre = 0; pre < cn->NeuronCount(); ++pre)
        {
            if (!IsVisual(cn->Superclass(pre)))
            {
                continue;
            }
            for (uint32_t e = cn->EdgeBegin(pre); e < cn->EdgeEnd(pre); ++e)
            {
                if (IsKc(cn->Type(cn->Targets()[e])))
                {
                    visualInputs.push_back(pre);
                    break;
                }
            }
        }
        Group lamina;
        for (const char* type : { "L1", "L2", "L3", "L5" })
        {
            const Group g = cn->Find(type);
            lamina.neurons.insert(lamina.neurons.end(), g.neurons.begin(), g.neurons.end());
        }
        Group input;
        for (const std::string& t : types)
        {
            // "visual": all the visual neurons with synapses onto KCs
            const Group g = t == "visual" ? Group{ visualInputs } : cn->Find(t);
            input.neurons.insert(input.neurons.end(), g.neurons.begin(), g.neurons.end());
        }
        if (share < 1.0)
        {
            // a fixed pseudo-random part of the inputs: a picture that excites some of them
            Group part;
            for (uint32_t n : input.neurons)
            {
                uint32_t h = n * 2654435761u;
                h = (h ^ (h >> 16)) * 2246822519u;
                if (double(h % 10000) < share * 10000.0)
                {
                    part.neurons.push_back(n);
                }
            }
            input = part;
        }
        std::vector<uint32_t> kcs;
        for (uint32_t i = 0; i < cn->NeuronCount(); ++i)
        {
            if (IsKc(cn->Type(i)))
            {
                kcs.push_back(i);
            }
        }
        std::printf("drive: %zu neurons of", input.Size());
        for (const std::string& t : types)
        {
            std::printf(" %s", t.c_str());
        }
        std::printf(", %.0f ms of Poisson input, dopamine as a modulator", ms);
        if (laminaMv != 0.0f)
        {
            std::printf(", %.1f mV into %zu L1/L2/L3/L5 as in the game", laminaMv, lamina.Size());
        }
        std::printf("\n| rate, Hz | visual inputs of KCs that fired (of %zu) | KCs that fired | share of KCs | of them KCg-d / KCab-p | KC spikes / s | brain spikes / s |\n| --- | --- | --- | --- | --- | --- | --- |\n",
            visualInputs.size());
        // --diff: a run without the input first; the timeline then shows the spikes above that run
        std::vector<std::string> runs = rates;
        if (diff)
        {
            runs.insert(runs.begin(), "control");
        }
        std::vector<std::map<std::string, int64_t>> controlParts;
        for (const std::string& r : runs)
        {
            const bool control = r == "control";
            ModelParams params;
            params.fastModulatorySynapses = false;
            FlyBrain brain(cn, params);
            brain.SetParallel(true);
            if (!control)
            {
                brain.StimulateRate(input, std::stof(r), onMs > 0.0 ? static_cast<float>(onMs) : kForever);
            }
            if (laminaMv != 0.0f)
            {
                brain.StimulateCurrent(lamina, laminaMv);
            }
            std::vector<uint32_t> last(cn->NeuronCount(), 0);
            std::string timeline;
            const int parts = std::max(1, static_cast<int>(ms / 50.0));
            for (int part = 0; part < parts; ++part)
            {
                brain.AdvanceMs(50.0);
                if (topTypes <= 0)
                {
                    continue;
                }
                // where the spikes of these 50 ms are: the types with most spikes (above the control run with --diff)
                std::map<std::string, std::pair<int64_t, uint32_t>> byType;
                int64_t total = 0;
                for (uint32_t n = 0; n < cn->NeuronCount(); ++n)
                {
                    const uint32_t count = brain.NeuronSpikeCount(n);
                    auto& entry = byType[std::string(cn->Type(n)) + " / " + std::string(cn->Superclass(n))];
                    entry.first += int64_t(count - last[n]);
                    ++entry.second;
                    total += int64_t(count - last[n]);
                    last[n] = count;
                }
                if (control)
                {
                    std::map<std::string, int64_t> counts;
                    for (const auto& [type, entry] : byType)
                    {
                        counts[type] = entry.first;
                    }
                    counts[""] = total;
                    controlParts.push_back(std::move(counts));
                    continue;
                }
                if (diff)
                {
                    for (auto& [type, entry] : byType)
                    {
                        entry.first -= controlParts[part][type];
                    }
                    total -= controlParts[part][""];
                }
                std::vector<std::pair<std::string, std::pair<int64_t, uint32_t>>> sorted(byType.begin(), byType.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
                char line[256];
                std::snprintf(line, sizeof(line), "    %4d ms: %lld spikes%s:", (part + 1) * 50, static_cast<long long>(total), diff ? " above the control" : "");
                timeline += line;
                for (int k = 0; k < topTypes && k < int(sorted.size()); ++k)
                {
                    std::snprintf(line, sizeof(line), " %s (%u) %llu;", sorted[k].first.c_str(), sorted[k].second.second,
                        static_cast<unsigned long long>(sorted[k].second.first));
                    timeline += line;
                }
                timeline += "\n";
            }
            uint32_t fired = 0;
            uint32_t gd = 0;
            uint32_t abp = 0;
            uint64_t kcSpikes = 0;
            for (uint32_t kc : kcs)
            {
                const bool spiked = brain.NeuronSpikeCount(kc) > 0;
                fired += spiked ? 1 : 0;
                gd += spiked && cn->Type(kc) == "KCg-d" ? 1 : 0;
                abp += spiked && cn->Type(kc) == "KCab-p" ? 1 : 0;
                kcSpikes += brain.NeuronSpikeCount(kc);
            }
            uint32_t visualFired = 0;
            for (uint32_t n : visualInputs)
            {
                visualFired += brain.NeuronSpikeCount(n) > 0 ? 1 : 0;
            }
            const double perSecond = 1000.0 / (parts * 50.0);
            std::printf("| %s | %u | %u | %.1f %% | %u / %u | %.0f | %.0f |\n", r.c_str(), visualFired, fired, 100.0 * fired / kcs.size(), gd, abp,
                double(kcSpikes) * perSecond, double(brain.TotalSpikes()) * perSecond);
            std::printf("%s", timeline.c_str());
        }
        std::printf("\n");
    }

    // The receptive fields of the visual inputs of KCs: the optic lobe columns of their presynaptic neurons (1 and 2 hops)
    void ReportFields(const Connectome& cn)
    {
        std::vector<std::vector<std::pair<uint32_t, int>>> inputsOf(cn.NeuronCount());
        for (uint32_t pre = 0; pre < cn.NeuronCount(); ++pre)
        {
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                inputsOf[cn.Targets()[e]].push_back({ pre, cn.Weights()[e] });
            }
        }
        uint32_t total = 0;
        uint32_t direct = 0;
        uint32_t twoHops = 0;
        std::map<std::string, std::pair<uint32_t, uint32_t>> byType; // inputs, with a field
        for (uint32_t v = 0; v < cn.NeuronCount(); ++v)
        {
            if (!IsVisual(cn.Superclass(v)))
            {
                continue;
            }
            uint64_t ontoKc = 0;
            for (uint32_t e = cn.EdgeBegin(v); e < cn.EdgeEnd(v); ++e)
            {
                ontoKc += IsKc(cn.Type(cn.Targets()[e])) ? std::abs(int(cn.Weights()[e])) : 0;
            }
            if (ontoKc == 0)
            {
                continue;
            }
            ++total;
            std::set<int> columns1;
            std::set<int> columns2;
            for (const auto& [pre, w] : inputsOf[v])
            {
                if (cn.Hex1(pre) != kNoHex)
                {
                    columns1.insert(cn.Hex1(pre) * 1000 + cn.Hex2(pre));
                }
                for (const auto& [pre2, w2] : inputsOf[pre])
                {
                    if (cn.Hex1(pre2) != kNoHex && std::abs(w) >= 5 && std::abs(w2) >= 5)
                    {
                        columns2.insert(cn.Hex1(pre2) * 1000 + cn.Hex2(pre2));
                    }
                }
            }
            direct += columns1.size() >= 3 ? 1 : 0;
            twoHops += columns2.size() >= 3 ? 1 : 0;
            auto& entry = byType[std::string(cn.Type(v))];
            ++entry.first;
            entry.second += columns1.size() >= 3 || columns2.size() >= 3 ? 1 : 0;
        }
        std::printf("visual inputs of KCs: %u; with a field of >= 3 columns from 1 hop: %u, from 2 hops (synapses >= 5 contacts): %u\n", total, direct, twoHops);
        std::printf("types without a field:");
        for (const auto& [type, entry] : byType)
        {
            if (entry.second < entry.first)
            {
                std::printf(" %s %u/%u;", type.c_str(), entry.first - entry.second, entry.first);
            }
        }
        std::printf("\n\n");
    }

    // N4: which eye the visual KCs see, and which eye the MBONs of the controls hear through them. The eye of a visual input is the side of the
    // columns of its field (KcVision); a KC gets the contacts of the inputs; an MBON the KC -> MBON contacts times the eye share of the KC.
    void ReportLaterality(const Connectome& cn)
    {
        KcVision vision;
        std::string error;
        if (!vision.Build(cn, {}, &error))
        {
            std::printf("laterality: %s\n", error.c_str());
            return;
        }
        const std::vector<KcVisionColumn>& columns = vision.Columns();
        // the visual drive of every KC by eye: contacts of an input x the share of its field on that side
        std::vector<double> kcLeft(cn.NeuronCount(), 0.0), kcRight(cn.NeuronCount(), 0.0);
        double ipsi = 0.0, contra = 0.0, inputsLeft = 0.0, inputsRight = 0.0;
        uint32_t inputsBoth = 0;
        for (const KcVisionInput& input : vision.Inputs())
        {
            double left = 0.0, right = 0.0;
            for (const auto& [c, w] : input.field)
            {
                (columns[c].side == Side::Left ? left : right) += w;
            }
            if (input.field.empty())
            {
                continue;
            }
            inputsLeft += left;
            inputsRight += right;
            inputsBoth += left > 0.2 && right > 0.2 ? 1 : 0;
            for (uint32_t e = cn.EdgeBegin(input.neuron); e < cn.EdgeEnd(input.neuron); ++e)
            {
                const uint32_t kc = cn.Targets()[e];
                if (!IsKc(cn.Type(kc)) || cn.Weights()[e] <= 0)
                {
                    continue;
                }
                const double contacts = cn.Weights()[e];
                kcLeft[kc] += contacts * left;
                kcRight[kc] += contacts * right;
                const Side side = cn.NeuronSide(kc);
                if (side == Side::Left || side == Side::Right)
                {
                    ipsi += contacts * (side == Side::Left ? left : right);
                    contra += contacts * (side == Side::Left ? right : left);
                }
            }
        }
        std::printf("visual inputs of KCs: fields %.0f %% left eye, %.0f %% right; %u inputs see both eyes (> 20 %% each)\n", 100.0 * inputsLeft / (inputsLeft + inputsRight),
            100.0 * inputsRight / (inputsLeft + inputsRight), inputsBoth);
        std::printf("visual contacts onto KCs: from the eye of the KC's side %.1f %%, from the other eye %.1f %%\n", 100.0 * ipsi / (ipsi + contra), 100.0 * contra / (ipsi + contra));
        // the MBONs: the eye they hear through the KCs (contacts KC -> MBON x the eye shares of the KC)
        for (const char* type : { "MBON32", "MBON20", "MBON30", "MBON21" })
        {
            for (const Side side : { Side::Left, Side::Right })
            {
                const Group mbons = cn.Find(type, side);
                double left = 0.0, right = 0.0;
                uint64_t kcContacts = 0;
                for (uint32_t kc = 0; kc < cn.NeuronCount(); ++kc)
                {
                    if (kcLeft[kc] + kcRight[kc] <= 0.0)
                    {
                        continue;
                    }
                    const double eyeLeft = kcLeft[kc] / (kcLeft[kc] + kcRight[kc]);
                    for (uint32_t e = cn.EdgeBegin(kc); e < cn.EdgeEnd(kc); ++e)
                    {
                        if (cn.Weights()[e] > 0 && std::binary_search(mbons.neurons.begin(), mbons.neurons.end(), cn.Targets()[e]))
                        {
                            left += cn.Weights()[e] * eyeLeft;
                            right += cn.Weights()[e] * (1.0 - eyeLeft);
                            kcContacts += cn.Weights()[e];
                        }
                    }
                }
                std::printf("  %s %s (%zu): contacts from visual KCs %llu, they see the left eye %.0f %%, the right %.0f %%\n", type, side == Side::Left ? "L" : "R",
                    mbons.neurons.size(), static_cast<unsigned long long>(kcContacts), kcContacts ? 100.0 * left / (left + right) : 0.0, kcContacts ? 100.0 * right / (left + right) : 0.0);
            }
        }
        std::printf("\n");
    }

    // PPL1 (punishment) under a constant current: the rates of every PPL1 type. The damage of FlyPlayer is a current
    void ReportPunishment(const std::shared_ptr<const Connectome>& cn, const std::vector<std::string>& currents)
    {
        std::printf("PPL1 under a constant current for 500 ms (dopamine as a modulator), mean rate per type, Hz\n");
        std::map<std::string, Group> types;
        Group all;
        for (uint32_t n = 0; n < cn->NeuronCount(); ++n)
        {
            if (cn->Type(n).substr(0, 4) == "PPL1")
            {
                types[std::string(cn->Type(n))].neurons.push_back(n);
                all.neurons.push_back(n);
            }
        }
        std::printf("| current, mV |");
        for (const auto& [type, group] : types)
        {
            std::printf(" %s (%zu) |", type.c_str(), group.Size());
        }
        std::printf(" brain spikes / s |\n|---|");
        for (size_t k = 0; k < types.size(); ++k)
        {
            std::printf("---|");
        }
        std::printf("---|\n");
        for (const std::string& c : currents)
        {
            ModelParams params;
            params.fastModulatorySynapses = false;
            FlyBrain brain(cn, params);
            brain.SetParallel(true);
            brain.StimulateCurrent(all, std::stof(c));
            brain.AdvanceMs(500.0);
            std::printf("| %s |", c.c_str());
            for (const auto& [type, group] : types)
            {
                uint64_t spikes = 0;
                for (uint32_t n : group.neurons)
                {
                    spikes += brain.NeuronSpikeCount(n);
                }
                std::printf(" %.0f |", double(spikes) / group.Size() / 0.5);
            }
            std::printf(" %.0f |\n", double(brain.TotalSpikes()) / 0.5);
        }
        std::printf("\n");
    }

    // The MBONs that answer a visual picture: a fixed part of the visual inputs of KCs at rateHz for 1 s
    void ReportMbonResponse(const std::shared_ptr<const Connectome>& cn, double share, float rateHz)
    {
        Group input;
        for (uint32_t pre = 0; pre < cn->NeuronCount(); ++pre)
        {
            if (!IsVisual(cn->Superclass(pre)))
            {
                continue;
            }
            bool ontoKc = false;
            for (uint32_t e = cn->EdgeBegin(pre); e < cn->EdgeEnd(pre) && !ontoKc; ++e)
            {
                ontoKc = IsKc(cn->Type(cn->Targets()[e]));
            }
            uint32_t h = pre * 2654435761u;
            h = (h ^ (h >> 16)) * 2246822519u;
            if (ontoKc && double(h % 10000) < share * 10000.0)
            {
                input.neurons.push_back(pre);
            }
        }
        std::map<std::string, Group> mbons;
        for (uint32_t n = 0; n < cn->NeuronCount(); ++n)
        {
            if (cn->Type(n).substr(0, 4) == "MBON")
            {
                mbons[std::string(cn->Type(n))].neurons.push_back(n);
            }
        }
        std::map<std::string, double> rates[2];
        for (int withPicture = 0; withPicture < 2; ++withPicture)
        {
            ModelParams params;
            params.fastModulatorySynapses = false;
            FlyBrain brain(cn, params);
            brain.SetParallel(true);
            if (withPicture)
            {
                brain.StimulateRate(input, rateHz);
            }
            brain.AdvanceMs(1000.0);
            for (const auto& [type, group] : mbons)
            {
                uint64_t spikes = 0;
                for (uint32_t n : group.neurons)
                {
                    spikes += brain.NeuronSpikeCount(n);
                }
                rates[withPicture][type] = double(spikes) / group.Size();
            }
        }
        std::printf("MBONs with a picture (%zu visual inputs of KCs at %.0f Hz) and without, mean rate per type, Hz\n| MBON | neurons | without | with |\n| --- | --- | --- | --- |\n",
            input.Size(), rateHz);
        for (const auto& [type, group] : mbons)
        {
            if (rates[0][type] > 0.0 || rates[1][type] > 0.0)
            {
                std::printf("| %s | %zu | %.1f | %.1f |\n", type.c_str(), group.Size(), rates[0][type], rates[1][type]);
            }
        }
        std::printf("\n");
    }

    // A game-like run: per-neuron drive that changes every DOOM tic, damage pulses into PPL1, KC -> MBON learning.
    // Returns the spike count of every neuron
    std::vector<uint32_t> GameLikeRun(const std::shared_ptr<const Connectome>& cn, const Group& eyes, bool parallel, bool simd, bool learning, double seconds)
    {
        ModelParams params;
        params.seed = 1;
        params.fastModulatorySynapses = false;
        FlyBrain brain(cn, params);
        brain.SetParallel(parallel);
        brain.SetSimd(simd);
        brain.EnablePlasticity();
        brain.SetLearning(learning);
        const double ticMs = 1000.0 / 35.0;
        const int tics = static_cast<int>(seconds * 35.0);
        for (int tic = 0; tic < tics; ++tic)
        {
            for (size_t i = 0; i < eyes.Size(); ++i)
            {
                uint32_t h = static_cast<uint32_t>(i * 2654435761u) ^ static_cast<uint32_t>((tic / 4) * 40503u);
                h = (h ^ (h >> 15)) * 2246822519u;
                brain.SetDrive(eyes.neurons[i], 4.0f + float(h % 1000) * 0.006f);
            }
            if (tic % 70 == 35)
            {
                brain.StimulateCurrent(brain.PunishmentNeurons(), 15.0f, 200.0f);
            }
            brain.AdvanceMs(ticMs);
        }
        std::vector<uint32_t> counts(cn->NeuronCount());
        for (uint32_t n = 0; n < cn->NeuronCount(); ++n)
        {
            counts[n] = brain.NeuronSpikeCount(n);
        }
        return counts;
    }

    void Determinism(const std::shared_ptr<const Connectome>& cn, const std::vector<std::string>& types, double seconds)
    {
        Group eyes;
        for (const std::string& t : types)
        {
            const Group g = cn->Find(t);
            eyes.neurons.insert(eyes.neurons.end(), g.neurons.begin(), g.neurons.end());
        }
        struct Run
        {
            const char* name;
            bool parallel;
            bool simd;
            bool learning;
        };
        const Run runs[] = { { "parallel, AVX2, learning on", true, true, true }, { "parallel, AVX2, learning on (again)", true, true, true },
            { "parallel, AVX2, learning off", true, true, false }, { "one thread, scalar, learning on", false, false, true } };
        std::printf("determinism: %zu driven neurons, %.0f s of 35 tics a second, damage into PPL1 every 2 s\n", eyes.Size(), seconds);
        std::vector<uint32_t> first;
        for (const Run& run : runs)
        {
            const std::vector<uint32_t> counts = GameLikeRun(cn, eyes, run.parallel, run.simd, run.learning, seconds);
            uint64_t total = 0;
            for (uint32_t c : counts)
            {
                total += c;
            }
            if (first.empty())
            {
                first = counts;
            }
            uint32_t differ = 0;
            for (size_t n = 0; n < counts.size(); ++n)
            {
                differ += counts[n] != first[n] ? 1 : 0;
            }
            std::printf("  %-40s spikes %llu, neurons that differ from the first run: %u\n", run.name, static_cast<unsigned long long>(total), differ);
        }
        std::printf("\n");
    }
}

int main(int argc, char** argv)
{
    std::string path = std::string(MYENGINE_SOURCE_DIR) + "/assets/flybrain/malecns_v1.fbrain";
    std::set<std::string> modes;
    std::vector<std::string> rates{ "10", "20", "40", "80" };
    std::vector<std::string> types;
    double seconds = 0.0;
    bool noKcToKc = false;
    bool noSensoryToSensory = false;
    int topTypes = 0;
    std::vector<std::string> who;
    std::vector<std::string> dropped;
    float laminaMv = 0.0f;
    double share = 1.0;
    double onMs = 0.0;
    bool diff = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--fbrain" && i + 1 < argc) path = argv[++i];
        else if (arg == "--rates" && i + 1 < argc) rates = Split(argv[++i]);
        else if (arg == "--types" && i + 1 < argc) types = Split(argv[++i]);
        else if (arg == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (arg == "--no-kc-kc") noKcToKc = true;
        else if (arg == "--no-axo") noKcToKc = noSensoryToSensory = true;
        else if (arg == "--top" && i + 1 < argc) topTypes = std::atoi(argv[++i]);
        else if (arg == "--who" && i + 1 < argc) who = Split(argv[++i]);
        else if (arg == "--drop" && i + 1 < argc) dropped = Split(argv[++i]);
        else if (arg == "--lamina" && i + 1 < argc) laminaMv = std::stof(argv[++i]);
        else if (arg == "--share" && i + 1 < argc) share = std::atof(argv[++i]);
        else if (arg == "--on" && i + 1 < argc) onMs = std::atof(argv[++i]);
        else if (arg == "--diff") diff = true;
        else modes.insert(arg);
    }
    if (modes.empty() && who.empty())
    {
        modes = { "inputs", "mbon" };
    }
    std::string error;
    auto cn = Connectome::Load(path, &error);
    if (!cn)
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 77;
    }
    if (noKcToKc || noSensoryToSensory || !dropped.empty())
    {
        cn = WithoutKcToKc(*cn, noKcToKc, noSensoryToSensory, dropped);
        if (!cn)
        {
            return 1;
        }
        std::printf("without%s%s", noKcToKc ? " KC -> KC synapses," : "", noSensoryToSensory ? " sensory -> sensory synapses," : "");
        for (const std::string& prefix : dropped)
        {
            std::printf(" outputs of %s*,", prefix.c_str());
        }
        std::printf("\n");
    }
    if (modes.count("inputs")) ReportInputs(*cn);
    if (modes.count("mbon")) ReportMbons(*cn);
    if (modes.count("fields")) ReportFields(*cn);
    if (modes.count("laterality")) ReportLaterality(*cn);
    for (const std::string& type : who)
    {
        ReportWho(*cn, type);
    }
    if (modes.count("drive") || modes.count("determinism") || modes.count("ppl1") || modes.count("mbresponse"))
    {
        if (types.empty() && (modes.count("drive") || modes.count("determinism")))
        {
            std::fprintf(stderr, "drive and determinism need --types\n");
            return 2;
        }
        myengine::jobs::Initialize();
        if (modes.count("drive")) Drive(cn, types, rates, topTypes, laminaMv, share, (seconds > 0.0 ? seconds : 1.0) * 1000.0, onMs, diff);
        if (modes.count("ppl1")) ReportPunishment(cn, rates);
        if (modes.count("mbresponse")) ReportMbonResponse(cn, share, std::stof(rates.front()));
        if (modes.count("determinism")) Determinism(cn, types, seconds > 0.0 ? seconds : 10.0);
        myengine::jobs::Shutdown();
    }
    return 0;
}
