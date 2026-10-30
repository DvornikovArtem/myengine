// Circuit.h
// A small circuit cut out of a connectome (N2: the brains of the swarm of enemy flies). From the input types (looming
// detectors LPLC2, LC4) downstream and from the output types (Giant Fiber DNp01, the steering DNa01/DNa02, walking DNp09,
// backing MDN) upstream, a synapse counts when it carries at least a share of the input of its target (upstream) or of the
// output of its source (downstream). The circuit is the neurons on such paths within the hops, the inputs and the outputs,
// with every synapse between them as in the full connectome. docs/flybrain/flybrain.md, section "Swarm".

#pragma once

#include <flybrain/Connectome.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace flybrain
{
    struct CircuitSpec
    {
        std::vector<std::string> inputTypes{ "LPLC2", "LC4" };
        std::vector<std::string> outputTypes{ "DNp01", "DNa01", "DNa02", "DNp09", "MDN" };
        uint32_t upstreamHops = 3;
        double upstreamShare = 0.0125; // of all contacts onto the target
        uint32_t downstreamHops = 3;
        double downstreamShare = 0.0125; // of all contacts from the source
        // Also every strong direct input of an output, even when no input type reaches it (the inhibition around it)
        bool keepOutputInputs = true;
    };

    struct CircuitReport
    {
        uint32_t neurons = 0;
        uint64_t edges = 0;
        uint64_t contacts = 0;
        uint32_t inputs = 0;
        uint32_t outputs = 0;
        std::vector<std::pair<std::string, uint32_t>> types; // by count, most first
        std::string Text(size_t topTypes = 25) const;
    };

    // The circuit as a connectome of its own (the same .fbrain format; Save it with the builder or keep it in memory).
    // nullptr when an input or output type is missing
    std::shared_ptr<const Connectome> ExtractCircuit(const Connectome& source, const CircuitSpec& spec, CircuitReport* report = nullptr,
        std::string* error = nullptr);
    // The image of the same, for saving as a file
    std::vector<std::byte> ExtractCircuitImage(const Connectome& source, const CircuitSpec& spec, CircuitReport* report = nullptr,
        std::string* error = nullptr);
}
