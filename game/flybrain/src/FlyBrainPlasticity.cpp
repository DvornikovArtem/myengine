// FlyBrainPlasticity.cpp
// Learning of the fly brain (N3): the KC -> MBON synapses, their compartments, the learning step and the memory file.
// FlyBrain.h has the rule; docs/flybrain/flybrain.md, section "Обучение (N3)".

#include <flybrain/FlyBrain.h>

#include "FlyBrainGpu.h"
#include "FlyBrainPlasticity.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace flybrain
{
    namespace
    {
        constexpr char kMemoryMagic[8] = { 'F', 'L', 'Y', 'M', 'E', 'M', '0', '1' };

        bool StartsWithAny(std::string_view text, const std::vector<std::string>& prefixes)
        {
            for (const std::string& prefix : prefixes)
            {
                if (!prefix.empty() && text.substr(0, prefix.size()) == prefix)
                {
                    return true;
                }
            }
            return false;
        }

        bool Fail(std::string* error, const std::string& message)
        {
            if (error != nullptr)
            {
                *error = message;
            }
            return false;
        }

        uint64_t Fnv(uint64_t hash, uint64_t value)
        {
            for (int k = 0; k < 8; ++k)
            {
                hash = (hash ^ ((value >> (8 * k)) & 0xFF)) * 1099511628211ull;
            }
            return hash;
        }
    }

    bool FlyBrain::EnablePlasticity(const PlasticityRule& rule, std::string* error)
    {
        if (gpu_ != nullptr)
        {
            return Fail(error, "enable the plasticity before the GPU backend");
        }
        const Connectome& cn = *connectome_;
        const uint32_t n = cn.NeuronCount();
        auto p = std::make_unique<Plasticity>();
        p->rule = rule;

        // Which neurons are KCs, MBONs, dopamine neurons of punishment and reward
        std::vector<int32_t> kcIndex(n, -1);
        std::vector<int32_t> mbonIndex(n, -1);
        std::vector<int32_t> danIndex(n, -1);
        for (uint32_t i = 0; i < n; ++i)
        {
            const std::string_view type = cn.Type(i);
            if (StartsWithAny(type, rule.preTypePrefixes))
            {
                kcIndex[i] = static_cast<int32_t>(p->kcs.size());
                p->kcs.push_back(i);
            }
            else if (StartsWithAny(type, rule.postTypePrefixes))
            {
                mbonIndex[i] = static_cast<int32_t>(p->mbons.size());
                p->mbons.push_back(i);
            }
            const bool punishment = StartsWithAny(type, rule.punishmentTypePrefixes);
            const bool reward = StartsWithAny(type, rule.rewardTypePrefixes);
            if (punishment || reward)
            {
                danIndex[i] = static_cast<int32_t>(p->dans.size());
                p->dans.push_back(i);
                (punishment ? p->punishment : p->reward).neurons.push_back(i);
            }
        }

        // The plastic synapses: KC -> MBON, in CSR order; per plastic row an index of its edges
        p->rowOffset.assign(n, UINT32_MAX);
        for (uint32_t kc : p->kcs)
        {
            bool any = false;
            for (uint32_t e = cn.EdgeBegin(kc); e < cn.EdgeEnd(kc) && !any; ++e)
            {
                any = mbonIndex[cn.Targets()[e]] >= 0;
            }
            if (!any)
            {
                continue;
            }
            p->rowOffset[kc] = static_cast<uint32_t>(p->edgePlastic.size());
            for (uint32_t e = cn.EdgeBegin(kc); e < cn.EdgeEnd(kc); ++e)
            {
                const uint32_t post = cn.Targets()[e];
                if (mbonIndex[post] >= 0)
                {
                    p->edgePlastic.push_back(static_cast<int32_t>(p->pre.size()));
                    p->pre.push_back(kc);
                    p->post.push_back(post);
                    p->edgeKc.push_back(static_cast<uint32_t>(kcIndex[kc]));
                    p->edgeMbon.push_back(static_cast<uint32_t>(mbonIndex[post]));
                    p->base.push_back(cn.Weights()[e]);
                }
                else
                {
                    p->edgePlastic.push_back(-1);
                }
            }
        }
        if (p->pre.empty())
        {
            return Fail(error, "the connectome has no KC -> MBON synapses");
        }

        // The compartment of an MBON: the dopamine neurons that synapse onto it, by their share of those contacts
        p->mbonDans.assign(p->mbons.size(), {});
        for (uint32_t d = 0; d < p->dans.size(); ++d)
        {
            const uint32_t dan = p->dans[d];
            for (uint32_t e = cn.EdgeBegin(dan); e < cn.EdgeEnd(dan); ++e)
            {
                const int32_t m = mbonIndex[cn.Targets()[e]];
                if (m >= 0 && uint32_t(std::abs(int(cn.Weights()[e]))) >= rule.compartmentContacts)
                {
                    p->mbonDans[m].push_back({ d, double(std::abs(int(cn.Weights()[e]))) });
                }
            }
        }
        for (auto& dans : p->mbonDans)
        {
            double total = 0.0;
            for (const auto& [d, contacts] : dans)
            {
                total += contacts;
            }
            for (auto& entry : dans)
            {
                entry.second /= total;
            }
        }

        const size_t r = replicas_;
        p->scale.assign(p->pre.size() * r, 1.0);
        p->weight.resize(p->pre.size() * r);
        for (size_t k = 0; k < p->pre.size(); ++k)
        {
            for (size_t f = 0; f < r; ++f)
            {
                p->weight[k * r + f] = p->base[k] * kFixedScale;
            }
        }
        p->trace.assign(p->kcs.size() * r, 0.0);
        p->lastKc.assign(p->kcs.size() * r, 0);
        p->lastDan.assign(p->dans.size() * r, 0);
        p->lastStep = step_;
        learnedAtStep_ = step_;
        p->info.edges = static_cast<uint32_t>(p->pre.size());
        for (size_t k = 0; k < p->pre.size(); ++k)
        {
            p->info.edgesWithDopamine += p->mbonDans[p->edgeMbon[k]].empty() ? 0 : 1;
        }
        std::vector<uint8_t> isPre(n, 0);
        std::vector<uint8_t> isPost(n, 0);
        for (size_t k = 0; k < p->pre.size(); ++k)
        {
            isPre[p->pre[k]] = 1;
            isPost[p->post[k]] = 1;
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            p->info.preNeurons += isPre[i];
            p->info.postNeurons += isPost[i];
        }
        p->info.punishmentNeurons = static_cast<uint32_t>(p->punishment.Size());
        p->info.rewardNeurons = static_cast<uint32_t>(p->reward.Size());

        // The spike totals of the KCs and the DANs, cheap to read on the GPU too
        for (uint32_t f = 0; f < replicas_; ++f)
        {
            Group units;
            for (uint32_t kc : p->kcs)
            {
                units.neurons.push_back(Unit(kc, f));
            }
            for (uint32_t dan : p->dans)
            {
                units.neurons.push_back(Unit(dan, f));
            }
            Track(units);
        }
        for (size_t k = 0; k < p->kcs.size(); ++k)
        {
            for (uint32_t f = 0; f < replicas_; ++f)
            {
                p->lastKc[k * r + f] = spikeTotals_[Unit(p->kcs[k], f)];
            }
        }
        for (size_t d = 0; d < p->dans.size(); ++d)
        {
            for (uint32_t f = 0; f < replicas_; ++f)
            {
                p->lastDan[d * r + f] = spikeTotals_[Unit(p->dans[d], f)];
            }
        }
        plasticity_ = std::move(p);
        return true;
    }

    const PlasticityInfo& FlyBrain::GetPlasticityInfo() const
    {
        static const PlasticityInfo none;
        return plasticity_ != nullptr ? plasticity_->info : none;
    }

    void FlyBrain::SetLearning(bool learning)
    {
        if (plasticity_ != nullptr)
        {
            plasticity_->learning = learning;
        }
    }

    bool FlyBrain::IsLearning() const
    {
        return plasticity_ != nullptr && plasticity_->learning;
    }

    uint32_t FlyBrain::PlasticEdgeCount() const
    {
        return plasticity_ != nullptr ? static_cast<uint32_t>(plasticity_->pre.size()) : 0;
    }

    uint32_t FlyBrain::PlasticPre(uint32_t edge) const
    {
        return plasticity_->pre[edge];
    }

    uint32_t FlyBrain::PlasticPost(uint32_t edge) const
    {
        return plasticity_->post[edge];
    }

    double FlyBrain::PlasticScale(uint32_t edge, uint32_t replica) const
    {
        return plasticity_->scale[size_t(edge) * replicas_ + replica];
    }

    std::vector<double> FlyBrain::GetPlasticScales() const
    {
        return plasticity_ != nullptr ? plasticity_->scale : std::vector<double>{};
    }

    void FlyBrain::SetPlasticScales(const std::vector<double>& scales)
    {
        Plasticity& p = *plasticity_;
        assert(scales.size() == p.scale.size());
        p.scale = scales;
        for (size_t k = 0; k < p.scale.size(); ++k)
        {
            const int32_t weight = static_cast<int32_t>(std::lround(double(p.base[k / replicas_]) * kFixedScale * p.scale[k]));
            if (weight != p.weight[k])
            {
                p.weight[k] = weight;
                if (gpu_ != nullptr)
                {
                    gpu_->QueuePlasticWeight(static_cast<uint32_t>(k), weight);
                }
            }
        }
    }

    void FlyBrain::ResetPlasticity()
    {
        if (plasticity_ == nullptr)
        {
            return;
        }
        SetPlasticScales(std::vector<double>(plasticity_->scale.size(), 1.0));
        std::fill(plasticity_->trace.begin(), plasticity_->trace.end(), 0.0);
    }

    const Group& FlyBrain::PunishmentNeurons() const
    {
        static const Group none;
        return plasticity_ != nullptr ? plasticity_->punishment : none;
    }

    const Group& FlyBrain::RewardNeurons() const
    {
        static const Group none;
        return plasticity_ != nullptr ? plasticity_->reward : none;
    }

    int32_t FlyBrain::PlasticWeight(int32_t plastic, uint32_t replica) const
    {
        return plasticity_->weight[size_t(plastic) * replicas_ + replica];
    }

    template <typename Totals>
    void FlyBrain::LearningStep(uint64_t step, const Totals& totals)
    {
        Plasticity& p = *plasticity_;
        if (step <= p.lastStep)
        {
            return;
        }
        const PlasticityRule& rule = p.rule;
        const double elapsedMs = double(step - p.lastStep) * params_.dtMs;
        const double traceDecay = std::exp(-elapsedMs / rule.traceTauMs);
        const double forget = rule.forgetTauMs > 0.0 ? std::exp(-elapsedMs / rule.forgetTauMs) : 1.0;
        const size_t r = replicas_;

        // Eligibility of every KC: its spikes of this call on top of the decayed trace
        for (size_t k = 0; k < p.kcs.size(); ++k)
        {
            for (size_t f = 0; f < r; ++f)
            {
                const uint32_t total = totals(Unit(p.kcs[k], static_cast<uint32_t>(f)));
                const size_t i = k * r + f;
                p.trace[i] = p.trace[i] * traceDecay + double(total - p.lastKc[i]);
                p.lastKc[i] = total;
            }
        }
        std::vector<double> danSpikes(p.dans.size() * r);
        for (size_t d = 0; d < p.dans.size(); ++d)
        {
            for (size_t f = 0; f < r; ++f)
            {
                const uint32_t total = totals(Unit(p.dans[d], static_cast<uint32_t>(f)));
                const size_t i = d * r + f;
                danSpikes[i] = double(total - p.lastDan[i]);
                p.lastDan[i] = total;
            }
        }
        p.lastStep = step;
        if (!p.learning)
        {
            return;
        }

        // Dopamine of every compartment: the spikes of its DANs by their share of the contacts onto the MBON
        std::vector<double> dopamine(p.mbons.size() * r, 0.0);
        for (size_t m = 0; m < p.mbons.size(); ++m)
        {
            for (const auto& [d, share] : p.mbonDans[m])
            {
                for (size_t f = 0; f < r; ++f)
                {
                    dopamine[m * r + f] += share * danSpikes[d * r + f];
                }
            }
        }
        for (size_t k = 0; k < p.pre.size(); ++k)
        {
            for (size_t f = 0; f < r; ++f)
            {
                const size_t i = k * r + f;
                double scale = 1.0 + (p.scale[i] - 1.0) * forget;
                scale -= rule.learningRate * p.trace[size_t(p.edgeKc[k]) * r + f] * dopamine[size_t(p.edgeMbon[k]) * r + f];
                scale = std::clamp(scale, rule.minScale, rule.maxScale);
                p.scale[i] = scale;
                const int32_t weight = static_cast<int32_t>(std::lround(double(p.base[k]) * kFixedScale * scale));
                if (weight != p.weight[i])
                {
                    p.weight[i] = weight;
                    if (gpu_ != nullptr)
                    {
                        gpu_->QueuePlasticWeight(static_cast<uint32_t>(i), weight);
                    }
                }
            }
        }
    }

    // The two backends that call it
    void FlyBrain::LearnFromCpu()
    {
        if (plasticity_ != nullptr)
        {
            LearningStep(step_, [this](uint32_t unit) { return spikeTotals_[unit]; });
        }
    }

    void FlyBrain::LearnFromGpu(uint64_t completedStep)
    {
        if (plasticity_ != nullptr)
        {
            LearningStep(completedStep, [this](uint32_t unit) { return TrackedSpikes(unit); });
        }
    }

    // ---------------------------------------------------------------- memory

    std::string FlyBrain::DefaultMemoryPath()
    {
        // Wide: the user folder may have letters outside the ANSI code page
        wchar_t* local = nullptr;
        size_t length = 0;
        std::filesystem::path base = std::filesystem::temp_directory_path();
        if (_wdupenv_s(&local, &length, L"LOCALAPPDATA") == 0 && local != nullptr)
        {
            base = std::filesystem::path(local);
        }
        std::free(local);
        return (base / "FlyDoom" / "fly_memory.bin").u8string();
    }

    bool FlyBrain::SaveMemory(const std::string& path, std::string* error) const
    {
        if (plasticity_ == nullptr)
        {
            return Fail(error, "no plasticity");
        }
        const Plasticity& p = *plasticity_;
        uint64_t fingerprint = 14695981039346656037ull;
        for (size_t k = 0; k < p.pre.size(); ++k)
        {
            fingerprint = Fnv(Fnv(fingerprint, connectome_->BodyId(p.pre[k])), connectome_->BodyId(p.post[k]));
        }
        std::error_code ignored;
        std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ignored);
        std::ofstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
        const uint64_t edges = p.pre.size();
        const uint64_t replicas = replicas_;
        file.write(kMemoryMagic, sizeof(kMemoryMagic));
        file.write(reinterpret_cast<const char*>(&fingerprint), 8);
        file.write(reinterpret_cast<const char*>(&edges), 8);
        file.write(reinterpret_cast<const char*>(&replicas), 8);
        file.write(reinterpret_cast<const char*>(p.scale.data()), static_cast<std::streamsize>(p.scale.size() * sizeof(double)));
        if (!file)
        {
            return Fail(error, "cannot write " + path);
        }
        return true;
    }

    bool FlyBrain::LoadMemory(const std::string& path, std::string* error)
    {
        if (plasticity_ == nullptr)
        {
            return Fail(error, "no plasticity");
        }
        const Plasticity& p = *plasticity_;
        std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
        char magic[8] = {};
        uint64_t fingerprint = 0;
        uint64_t edges = 0;
        uint64_t replicas = 0;
        file.read(magic, 8);
        file.read(reinterpret_cast<char*>(&fingerprint), 8);
        file.read(reinterpret_cast<char*>(&edges), 8);
        file.read(reinterpret_cast<char*>(&replicas), 8);
        if (!file || std::memcmp(magic, kMemoryMagic, 8) != 0)
        {
            return Fail(error, "not a fly memory: " + path);
        }
        uint64_t expected = 14695981039346656037ull;
        for (size_t k = 0; k < p.pre.size(); ++k)
        {
            expected = Fnv(Fnv(expected, connectome_->BodyId(p.pre[k])), connectome_->BodyId(p.post[k]));
        }
        if (fingerprint != expected || edges != p.pre.size() || replicas != replicas_)
        {
            return Fail(error, "the memory is of another connectome or another number of flies");
        }
        std::vector<double> scales(p.scale.size());
        file.read(reinterpret_cast<char*>(scales.data()), static_cast<std::streamsize>(scales.size() * sizeof(double)));
        if (!file)
        {
            return Fail(error, "the memory file is cut: " + path);
        }
        SetPlasticScales(scales);
        return true;
    }
}
