// KcVision.cpp
// The visual inputs of the Kenyon cells and their receptive fields from the connectome (N3b).

#include <flybrain/KcVision.h>

#include <flybrain/FlyBrain.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace flybrain
{
    namespace
    {
        bool IsKc(std::string_view type)
        {
            return type.substr(0, 2) == "KC";
        }

        bool IsVisual(std::string_view superclass)
        {
            return superclass == "visual_projection" || superclass == "visual_projection_tbc" || superclass == "ol_intrinsic" ||
                   superclass == "ol_sensory" || superclass == "visual_centrifugal";
        }

        int64_t ColumnKey(Side side, int16_t hex1, int16_t hex2)
        {
            return (int64_t(side) << 32) | (int64_t(uint16_t(hex1)) << 16) | int64_t(uint16_t(hex2));
        }
    }

    bool KcVision::Build(const Connectome& cn, const KcVisionConfig& config, std::string* error)
    {
        config_ = config;
        columns_.clear();
        inputs_.clear();
        const uint32_t count = cn.NeuronCount();

        // The inputs of every neuron (the connectome stores the outputs) and their totals in contacts
        std::vector<uint32_t> inStart(count + 1, 0);
        for (uint32_t e = 0; e < cn.EdgeCount(); ++e)
        {
            ++inStart[cn.Targets()[e] + 1];
        }
        for (uint32_t n = 0; n < count; ++n)
        {
            inStart[n + 1] += inStart[n];
        }
        std::vector<std::pair<uint32_t, uint32_t>> inEdges(cn.EdgeCount()); // pre, contacts
        std::vector<uint32_t> fill(inStart.begin(), inStart.end() - 1);
        std::vector<uint64_t> inTotal(count, 0);
        for (uint32_t pre = 0; pre < count; ++pre)
        {
            for (uint32_t e = cn.EdgeBegin(pre); e < cn.EdgeEnd(pre); ++e)
            {
                const uint32_t post = cn.Targets()[e];
                const uint32_t contacts = static_cast<uint32_t>(std::abs(int(cn.Weights()[e])));
                inEdges[fill[post]++] = { pre, contacts };
                inTotal[post] += contacts;
            }
        }

        std::map<int64_t, uint32_t> columnIndex;
        auto columnOf = [&](uint32_t neuron) -> uint32_t
        {
            const int64_t key = ColumnKey(cn.NeuronSide(neuron), cn.Hex1(neuron), cn.Hex2(neuron));
            const auto found = columnIndex.find(key);
            if (found != columnIndex.end())
            {
                return found->second;
            }
            const uint32_t index = static_cast<uint32_t>(columns_.size());
            columns_.push_back({ cn.NeuronSide(neuron), cn.Hex1(neuron), cn.Hex2(neuron) });
            columnIndex.emplace(key, index);
            return index;
        };

        for (uint32_t v = 0; v < count; ++v)
        {
            if (!IsVisual(cn.Superclass(v)))
            {
                continue;
            }
            uint32_t ontoKcs = 0;
            for (uint32_t e = cn.EdgeBegin(v); e < cn.EdgeEnd(v); ++e)
            {
                ontoKcs += IsKc(cn.Type(cn.Targets()[e])) ? static_cast<uint32_t>(std::abs(int(cn.Weights()[e]))) : 0;
            }
            if (ontoKcs < config_.minContactsOntoKcs)
            {
                continue;
            }

            // The field: a columnar presynaptic neuron gives its column the contacts of its synapse; one more hop up,
            // a columnar neuron gives the contacts of the first synapse times its share of the input of the middle one
            std::map<uint32_t, double> field;
            for (uint32_t i = inStart[v]; i < inStart[v + 1]; ++i)
            {
                const auto [pre, contacts] = inEdges[i];
                if (contacts < config_.minSynapseContacts)
                {
                    continue;
                }
                if (cn.Hex1(pre) != kNoHex)
                {
                    field[columnOf(pre)] += contacts;
                    continue;
                }
                for (uint32_t j = inStart[pre]; j < inStart[pre + 1]; ++j)
                {
                    const auto [pre2, contacts2] = inEdges[j];
                    if (contacts2 >= config_.minSynapseContacts && cn.Hex1(pre2) != kNoHex)
                    {
                        field[columnOf(pre2)] += double(contacts) * double(contacts2) / double(std::max<uint64_t>(1, inTotal[pre]));
                    }
                }
            }
            KcVisionInput input;
            input.neuron = v;
            input.contactsOntoKcs = ontoKcs;
            double sum = 0.0;
            for (const auto& [column, weight] : field)
            {
                sum += weight;
            }
            for (const auto& [column, weight] : field)
            {
                input.field.push_back({ column, static_cast<float>(weight / sum) });
            }
            inputs_.push_back(std::move(input));
        }
        if (inputs_.empty())
        {
            if (error != nullptr)
            {
                *error = "the connectome has no visual neurons with synapses onto KCs";
            }
            return false;
        }
        return true;
    }

    Group KcVision::Neurons() const
    {
        Group group;
        for (const KcVisionInput& input : inputs_)
        {
            group.neurons.push_back(input.neuron);
        }
        return group;
    }

    uint32_t KcVision::FindColumn(Side side, int16_t hex1, int16_t hex2) const
    {
        for (uint32_t i = 0; i < columns_.size(); ++i)
        {
            if (columns_[i].side == side && columns_[i].hex1 == hex1 && columns_[i].hex2 == hex2)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

    float KcVision::RateHz(const KcVisionInput& input, const std::vector<float>& columnFeature) const
    {
        float seen = 0.0f;
        for (const auto& [column, weight] : input.field)
        {
            seen += weight * (column < columnFeature.size() ? columnFeature[column] : 0.0f);
        }
        return config_.maxRateHz * std::clamp(config_.gain * seen, 0.0f, 1.0f);
    }

    void KcVision::Apply(FlyBrain& brain, const std::vector<float>& columnFeature, uint32_t replica) const
    {
        for (const KcVisionInput& input : inputs_)
        {
            brain.SetPoissonRate(brain.Unit(input.neuron, replica), RateHz(input, columnFeature));
        }
    }
}
