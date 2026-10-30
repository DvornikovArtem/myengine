// FlyBrainPlasticity.h
// The state of learning of a FlyBrain (internal): the plastic synapses, the compartments, the traces, the scales.

#pragma once

#include <flybrain/FlyBrain.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace flybrain
{
    // Synaptic input is summed in 1/256 of a contact: a plastic synapse can be a fraction of its contacts and the sums
    // stay integer (the same on the CPU and the GPU). Exact: w_syn / 256 x 256 c rounds like w_syn x c
    constexpr int32_t kFixedScale = 256;

    struct FlyBrain::Plasticity
    {
        PlasticityRule rule;
        PlasticityInfo info;
        bool learning = true;
        // plastic synapses, in CSR order
        std::vector<uint32_t> pre;
        std::vector<uint32_t> post;
        std::vector<uint32_t> edgeKc;   // index into kcs
        std::vector<uint32_t> edgeMbon; // index into mbons
        std::vector<int32_t> base;      // contacts x sign
        // per neuron: offset of its row into edgePlastic, UINT32_MAX for a row without plastic synapses
        std::vector<uint32_t> rowOffset;
        std::vector<int32_t> edgePlastic; // per edge of those rows: the plastic synapse or -1
        std::vector<uint32_t> kcs;
        std::vector<uint32_t> mbons;
        std::vector<uint32_t> dans;
        Group punishment;
        Group reward;
        std::vector<std::vector<std::pair<uint32_t, double>>> mbonDans; // per MBON: (dan, share of its DAN contacts)
        // per replica: [synapse][replica], [kc][replica], [dan][replica]
        std::vector<double> scale;
        std::vector<int32_t> weight;    // base x 256 x scale, rounded: what the delivery adds
        std::vector<double> trace;
        std::vector<uint32_t> lastKc;
        std::vector<uint32_t> lastDan;
        uint64_t lastStep = 0;
    };
}
