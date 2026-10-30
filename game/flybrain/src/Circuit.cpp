// Circuit.cpp

#include <flybrain/Circuit.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>

namespace flybrain
{
    namespace
    {
        // Hops from the seeds along strong synapses. upstream: from a target to the sources that give it at least share
        // of its input; downstream: from a source to the targets that get at least share of its output
        std::vector<uint8_t> Closure(const Connectome& cn, const std::vector<uint32_t>& seeds, uint32_t hops, double share, bool upstream,
            const std::vector<uint32_t>& inStart, const std::vector<uint32_t>& inSource, const std::vector<int32_t>& inWeight,
            const std::vector<uint64_t>& inTotal, const std::vector<uint64_t>& outTotal)
        {
            std::vector<uint8_t> reached(cn.NeuronCount(), 0);
            std::vector<uint32_t> frontier = seeds;
            for (uint32_t s : seeds)
            {
                reached[s] = 1;
            }
            for (uint32_t hop = 0; hop < hops && !frontier.empty(); ++hop)
            {
                std::vector<uint32_t> next;
                for (uint32_t n : frontier)
                {
                    if (upstream)
                    {
                        const double minimum = share * double(inTotal[n]);
                        for (uint32_t k = inStart[n]; k < inStart[n + 1]; ++k)
                        {
                            const uint32_t pre = inSource[k];
                            if (!reached[pre] && double(std::abs(inWeight[k])) >= minimum)
                            {
                                reached[pre] = 1;
                                next.push_back(pre);
                            }
                        }
                    }
                    else
                    {
                        const double minimum = share * double(outTotal[n]);
                        for (uint32_t e = cn.EdgeBegin(n); e < cn.EdgeEnd(n); ++e)
                        {
                            const uint32_t post = cn.Targets()[e];
                            if (!reached[post] && double(std::abs(int(cn.Weights()[e]))) >= minimum)
                            {
                                reached[post] = 1;
                                next.push_back(post);
                            }
                        }
                    }
                }
                frontier.swap(next);
            }
            return reached;
        }

        bool Fail(std::string* error, const std::string& message)
        {
            if (error != nullptr)
            {
                *error = message;
            }
            return false;
        }
    }

    std::string CircuitReport::Text(size_t topTypes) const
    {
        std::ostringstream out;
        out << neurons << " neurons, " << edges << " synapses, " << contacts << " contacts; " << inputs << " input and " << outputs
            << " output neurons; " << types.size() << " types:";
        for (size_t k = 0; k < types.size() && k < topTypes; ++k)
        {
            out << ' ' << (types[k].first.empty() ? std::string("(untyped)") : types[k].first) << ' ' << types[k].second;
        }
        if (types.size() > topTypes)
        {
            out << " ...";
        }
        return out.str();
    }

    std::vector<std::byte> ExtractCircuitImage(const Connectome& cn, const CircuitSpec& spec, CircuitReport* report, std::string* error)
    {
        const uint32_t n = cn.NeuronCount();
        std::vector<uint32_t> inputs;
        std::vector<uint32_t> outputs;
        for (const std::string& type : spec.inputTypes)
        {
            const Group g = cn.Find(type);
            if (g.Empty())
            {
                Fail(error, "no neurons of the input type " + type);
                return {};
            }
            inputs.insert(inputs.end(), g.neurons.begin(), g.neurons.end());
        }
        for (const std::string& type : spec.outputTypes)
        {
            const Group g = cn.Find(type);
            if (g.Empty())
            {
                Fail(error, "no neurons of the output type " + type);
                return {};
            }
            outputs.insert(outputs.end(), g.neurons.begin(), g.neurons.end());
        }

        // Incoming synapses (the transpose of the CSR) and the totals of contacts in and out of every neuron
        std::vector<uint32_t> inStart(n + 1ull, 0);
        std::vector<uint64_t> inTotal(n, 0);
        std::vector<uint64_t> outTotal(n, 0);
        for (uint32_t pre = 0; pre < n; ++pre)
        {
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                const uint32_t post = cn.Targets()[e];
                const uint64_t contacts = static_cast<uint64_t>(std::abs(int(cn.Weights()[e])));
                ++inStart[post + 1ull];
                inTotal[post] += contacts;
                outTotal[pre] += contacts;
            }
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            inStart[i + 1ull] += inStart[i];
        }
        std::vector<uint32_t> inSource(cn.EdgeCount());
        std::vector<int32_t> inWeight(cn.EdgeCount());
        std::vector<uint32_t> cursor(inStart.begin(), inStart.end() - 1);
        for (uint32_t pre = 0; pre < n; ++pre)
        {
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                const uint32_t k = cursor[cn.Targets()[e]]++;
                inSource[k] = pre;
                inWeight[k] = cn.Weights()[e];
            }
        }

        const std::vector<uint8_t> up = Closure(cn, outputs, spec.upstreamHops, spec.upstreamShare, true, inStart, inSource, inWeight, inTotal, outTotal);
        const std::vector<uint8_t> down = Closure(cn, inputs, spec.downstreamHops, spec.downstreamShare, false, inStart, inSource, inWeight, inTotal, outTotal);
        std::vector<uint8_t> keep(n, 0);
        for (uint32_t i = 0; i < n; ++i)
        {
            keep[i] = up[i] && down[i];
        }
        for (uint32_t i : inputs)
        {
            keep[i] = 1;
        }
        for (uint32_t o : outputs)
        {
            keep[o] = 1;
            if (spec.keepOutputInputs)
            {
                const double minimum = spec.upstreamShare * double(inTotal[o]);
                for (uint32_t k = inStart[o]; k < inStart[o + 1ull]; ++k)
                {
                    if (double(std::abs(inWeight[k])) >= minimum)
                    {
                        keep[inSource[k]] = 1;
                    }
                }
            }
        }

        ConnectomeBuilder builder;
        std::vector<uint32_t> newIndex(n, UINT32_MAX);
        std::map<std::string, uint32_t> typeCounts;
        CircuitReport local;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (keep[i])
            {
                newIndex[i] = builder.AddNeuron(cn.Info(i));
                ++typeCounts[std::string(cn.Type(i))];
                ++local.neurons;
            }
        }
        for (uint32_t pre = 0; pre < n; ++pre)
        {
            if (!keep[pre])
            {
                continue;
            }
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                const uint32_t post = cn.Targets()[e];
                if (keep[post])
                {
                    builder.AddEdge(newIndex[pre], newIndex[post], cn.Weights()[e]);
                    ++local.edges;
                    local.contacts += static_cast<uint64_t>(std::abs(int(cn.Weights()[e])));
                }
            }
        }
        local.inputs = static_cast<uint32_t>(inputs.size());
        local.outputs = static_cast<uint32_t>(outputs.size());
        local.types.assign(typeCounts.begin(), typeCounts.end());
        std::sort(local.types.begin(), local.types.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });

        std::ostringstream attribution;
        attribution << cn.Attribution() << "\nCircuit cut out by myengine (flybrain::ExtractCircuit): inputs";
        for (const auto& t : spec.inputTypes) attribution << ' ' << t;
        attribution << ", outputs";
        for (const auto& t : spec.outputTypes) attribution << ' ' << t;
        attribution << "; " << spec.upstreamHops << " hops upstream at " << spec.upstreamShare * 100.0 << " % of the input, " << spec.downstreamHops
                    << " hops downstream at " << spec.downstreamShare * 100.0 << " % of the output; " << local.neurons << " neurons, " << local.edges << " synapses.";
        if (report != nullptr)
        {
            *report = local;
        }
        return builder.Build(attribution.str());
    }

    std::shared_ptr<const Connectome> ExtractCircuit(const Connectome& source, const CircuitSpec& spec, CircuitReport* report, std::string* error)
    {
        std::vector<std::byte> image = ExtractCircuitImage(source, spec, report, error);
        if (image.empty())
        {
            return nullptr;
        }
        return Connectome::FromImage(std::move(image), error);
    }
}
