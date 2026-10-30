// KcVision.h
// What the fly sees, into its mushroom body (N3b). In MaleCNS 290 visual neurons of the optic lobes synapse onto Kenyon
// cells (aMe12, aMe26, MeVP41, LoVP42, ...: 1.2 % of the input of the KCs, almost all into KCg-d and KCab-p). Their own
// input is the colour pathway R7/R8 through histamine and glutamate, which a point LIF model turns into inhibition only:
// from the photoreceptors nothing reaches the KCs. So the picture goes straight into these neurons as Poisson input, and
// each one sees the optic lobe columns of its presynaptic neurons (1 and 2 synapses up) - its receptive field from the
// connectome. That bridge in place of the optic lobe is ours. docs/flybrain/flybrain.md, section "Муха учится в игре".

#pragma once

#include <flybrain/Connectome.h>

#include <cstdint>
#include <string>
#include <vector>

namespace flybrain
{
    class FlyBrain;

    struct KcVisionConfig
    {
        uint32_t minContactsOntoKcs = 10;  // a visual neuron counts with at least so many contacts onto KCs
        uint32_t minSynapseContacts = 5;   // the synapses that make a receptive field (both hops)
        float maxRateHz = 120.0f;          // the rate of an input whose whole field sees the feature
        float gain = 1.0f;                 // rate = maxRateHz * clamp(gain * feature averaged over the field)
    };

    // One optic lobe column of one side, as the connectome numbers it (assignedOlHex1/2)
    struct KcVisionColumn
    {
        Side side = Side::Unknown;
        int16_t hex1 = kNoHex;
        int16_t hex2 = kNoHex;
    };

    struct KcVisionInput
    {
        uint32_t neuron = 0;
        uint32_t contactsOntoKcs = 0;
        // Columns (indices into Columns()) with weights that add up to 1; empty when no column reaches it
        std::vector<std::pair<uint32_t, float>> field;
    };

    class KcVision
    {
    public:
        bool Build(const Connectome& connectome, const KcVisionConfig& config = {}, std::string* error = nullptr);

        const KcVisionConfig& Config() const { return config_; }
        const std::vector<KcVisionColumn>& Columns() const { return columns_; }
        const std::vector<KcVisionInput>& Inputs() const { return inputs_; }
        // The inputs as a group (for stimuli, watches)
        Group Neurons() const;
        // Index of a column in Columns(), or UINT32_MAX
        uint32_t FindColumn(Side side, int16_t hex1, int16_t hex2) const;

        // The rate of every input from a feature per column (Columns().size() values, 0 nothing .. 1 the feature)
        float RateHz(const KcVisionInput& input, const std::vector<float>& columnFeature) const;
        // Sets the Poisson rates of the inputs in one replica of the brain
        void Apply(FlyBrain& brain, const std::vector<float>& columnFeature, uint32_t replica = 0) const;

    private:
        KcVisionConfig config_;
        std::vector<KcVisionColumn> columns_;
        std::vector<KcVisionInput> inputs_;
    };
}
