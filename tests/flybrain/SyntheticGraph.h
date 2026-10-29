// SyntheticGraph.h
// A random connectome for the tests and the benchmark when the real one is not downloaded: every neuron gets a random
// number of targets, contacts 1..maxContacts with a geometric tail, and the sign of its transmitter (about 62 %
// acetylcholine as in MaleCNS v1.0, the rest GABA or glutamate).

#pragma once

#include <flybrain/Connectome.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace flybrain::test
{
    struct SyntheticOptions
    {
        uint32_t neurons = 1000;
        uint32_t meanDegree = 50;
        uint32_t maxContacts = 12;
        float excitatoryShare = 0.62f;
        uint64_t seed = 7;
    };

    inline uint64_t SplitMix(uint64_t& state)
    {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    inline double Uniform(uint64_t& state)
    {
        return double(SplitMix(state) >> 11) * (1.0 / 9007199254740992.0);
    }

    inline std::shared_ptr<const Connectome> MakeSyntheticConnectome(const SyntheticOptions& options)
    {
        ConnectomeBuilder builder;
        uint64_t rng = options.seed;
        // The sign of a neuron's synapses comes from its transmitter, like in the converter
        std::vector<int32_t> sign(options.neurons);
        for (uint32_t i = 0; i < options.neurons; ++i)
        {
            NeuronInfo info;
            info.bodyId = 1000 + i;
            info.type = "syn" + std::to_string(i % 100);
            info.superclass = "synthetic";
            info.side = (i & 1) ? Side::Right : Side::Left;
            const bool excitatory = Uniform(rng) < options.excitatoryShare;
            info.transmitter = excitatory ? Transmitter::Acetylcholine : (Uniform(rng) < 0.5 ? Transmitter::Gaba : Transmitter::Glutamate);
            sign[i] = excitatory ? 1 : -1;
            builder.AddNeuron(info);
        }
        for (uint32_t i = 0; i < options.neurons; ++i)
        {
            // Degree: uniform 0..2 x mean
            const uint32_t degree = static_cast<uint32_t>(Uniform(rng) * 2.0 * options.meanDegree);
            for (uint32_t k = 0; k < degree; ++k)
            {
                const uint32_t post = static_cast<uint32_t>(SplitMix(rng) % options.neurons);
                uint32_t contacts = 1;
                while (contacts < options.maxContacts && Uniform(rng) < 0.45)
                {
                    ++contacts;
                }
                builder.AddEdge(i, post, sign[i] * static_cast<int32_t>(contacts));
            }
        }
        return Connectome::FromImage(builder.Build("synthetic graph of the flybrain tests"));
    }
}
