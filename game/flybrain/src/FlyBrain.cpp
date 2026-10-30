// FlyBrain.cpp

#include <flybrain/FlyBrain.h>

#include "FlyBrainGpu.h"
#include "FlyBrainPlasticity.h"

#include <myengine/jobs/JobSystem.h>

#include <immintrin.h>
#include <intrin.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>

namespace flybrain
{
    namespace
    {
        // The closed-form catch-up takes the powers from a table of this many steps (0.4 s) and its last entry
        constexpr uint32_t kPowerTableSize = 4096;
        // Below this many synapses a window's spikes are delivered on the calling thread: a dispatch costs more
        constexpr uint64_t kParallelDeliveryEdges = 16384;
        // Neurons of one time-major pass over a window (their state, 20 KB, stays in L1)
        constexpr uint32_t kBlock = 512;

        using Clock = std::chrono::steady_clock;

        double SecondsSince(Clock::time_point start)
        {
            return std::chrono::duration<double>(Clock::now() - start).count();
        }

        // AVX2 and the OS support for the YMM registers. The engine is built for SSE2, so the AVX2 path is chosen at run time
        bool CpuHasAvx2()
        {
            int info[4];
            __cpuid(info, 0);
            if (info[0] < 7)
            {
                return false;
            }
            __cpuid(info, 1);
            const bool osSaves = (info[2] & (1 << 27)) != 0;
            const bool avx = (info[2] & (1 << 28)) != 0;
            __cpuidex(info, 7, 0);
            const bool avx2 = (info[1] & (1 << 5)) != 0;
            return osSaves && avx && avx2 && (_xgetbv(0) & 6) == 6;
        }

        // Counter-based random numbers: the draw of a neuron at a step does not depend on the order or the thread.
        // 32-bit operations only (lowbias32 by C. Wellons), so the compute shader of FlyBrainGpu (SM 5.0, no 64-bit
        // integers) draws the same numbers
        inline uint32_t Mix32(uint32_t x)
        {
            x ^= x >> 16;
            x *= 0x7FEB352Du;
            x ^= x >> 15;
            x *= 0x846CA68Bu;
            x ^= x >> 16;
            return x;
        }

        inline uint32_t PoissonDraw(uint64_t seed, uint32_t neuron, uint64_t step)
        {
            uint32_t h = Mix32(static_cast<uint32_t>(seed) ^ 0x9E3779B9u);
            h = Mix32(h ^ static_cast<uint32_t>(seed >> 32));
            h = Mix32(h ^ static_cast<uint32_t>(step >> 32));
            h = Mix32(h ^ static_cast<uint32_t>(step));
            return Mix32(h ^ neuron);
        }
    }

    struct FlyBrain::Stimulus
    {
        Group group;
        bool isRate = false;
        float value = 0.0f;
        uint64_t endStep = UINT64_MAX;
    };

    struct FlyBrain::Watcher
    {
        uint32_t neuronCount = 0;
        std::vector<uint32_t> perStep; // ring of spike counts per step
        uint64_t total = 0;
    };

    struct FlyBrain::Window
    {
        uint64_t begin = 0;
        uint32_t steps = 0;
    };

    FlyBrain::FlyBrain(std::shared_ptr<const Connectome> connectome, const ModelParams& params, uint32_t replicas)
        : connectome_(std::move(connectome))
        , params_(params)
        , replicas_(std::max(1u, replicas))
    {
        assert(connectome_ != nullptr);
        neuronCount_ = connectome_->NeuronCount() * replicas_;
        silentOutput_.assign(connectome_->NeuronCount(), 0);
        if (!params_.fastModulatorySynapses)
        {
            for (uint32_t i = 0; i < connectome_->NeuronCount(); ++i)
            {
                const Transmitter t = connectome_->NeuronTransmitter(i);
                silentOutput_[i] = t == Transmitter::Dopamine || t == Transmitter::Octopamine || t == Transmitter::Serotonin ? 1 : 0;
            }
        }
        for (const std::string& prefix : params_.silentTypePrefixes)
        {
            for (const uint32_t i : connectome_->FindPrefix(prefix).neurons)
            {
                silentOutput_[i] = 1;
            }
        }
        useAvx2_ = CpuHasAvx2();

        const double dt = params_.dtMs;
        delaySteps_ = static_cast<uint32_t>(std::max(1l, std::lround(params_.delayMs / dt)));
        refractorySteps_ = static_cast<uint32_t>(std::clamp(std::lround(params_.refractoryMs / dt), 0l, 255l));
        thresholdU_ = params_.thresholdMv - params_.restMv;
        resetU_ = params_.resetMv - params_.restMv;
        decayU_ = std::exp(-dt / params_.membraneTauMs);
        decayG_ = std::exp(-dt / params_.synapseTauMs);
        // tau == t_mbr has another closed form; the model never uses it
        assert(params_.synapseTauMs != params_.membraneTauMs);
        couplingK_ = params_.synapseTauMs / (params_.synapseTauMs - params_.membraneTauMs);

        powU_.resize(kPowerTableSize + 1);
        powG_.resize(kPowerTableSize + 1);
        for (uint32_t k = 0; k <= kPowerTableSize; ++k)
        {
            powU_[k] = std::exp(-dt * k / params_.membraneTauMs);
            powG_[k] = std::exp(-dt * k / params_.synapseTauMs);
        }

        const size_t n = neuronCount_;
        u_.assign(n, 0.0);
        g_.assign(n, 0.0);
        lastStep_.assign(n, 0);
        refractoryUntil_.assign(n, 0);
        awake_.assign(n, 0);
        neuronRefractory_.assign(n, static_cast<uint8_t>(refractorySteps_));
        spikeTotals_.assign(n, 0);
        arrivals_.assign(n * delaySteps_, 0);
        pending_ = std::make_unique<std::atomic<uint8_t>[]>(n);
        for (size_t i = 0; i < n; ++i)
        {
            pending_[i].store(0, std::memory_order_relaxed);
        }
        stimulusDrive_.assign(n, 0.0f);
        externalDrive_.assign(n, 0.0f);
        currentStimuli_.assign(n, 0);
        poissonThreshold_.assign(n, 0);
        poissonStimuli_.assign(n, 0);
        stimulusPoisson_.assign(n, 0.0);
        externalPoisson_.assign(n, 0.0);
        trackedIndex_.assign(n, 0);
        watchHead_.assign(n, 0);
        watchMask_.assign(n, 0);

        const size_t chunks = (n + chunkSize_ - 1) / chunkSize_;
        chunkSpikes_.resize(std::max<size_t>(1, chunks));
        chunkUpdates_.resize(std::max<size_t>(1, chunks));
        for (size_t k = 0; k < chunkSpikes_.size(); ++k)
        {
            blocks_.push_back(std::make_unique<Block>());
        }
        SetSpikeRingCapacity(65536);
    }

    FlyBrain::~FlyBrain() = default;

    void FlyBrain::SetSimd(bool enabled)
    {
        useAvx2_ = enabled && CpuHasAvx2();
    }

    // ---------------------------------------------------------------- backend

    bool ParseBackendChoice(std::string_view text, BackendChoice& out)
    {
        if (text == "auto") { out = BackendChoice::Auto; return true; }
        if (text == "cpu") { out = BackendChoice::Cpu; return true; }
        if (text == "gpu") { out = BackendChoice::Gpu; return true; }
        return false;
    }

    const char* ToString(Backend backend)
    {
        return backend == Backend::Gpu ? "gpu" : "cpu";
    }

    namespace
    {
        // A small recurrent network with tonic and Poisson input: the GPU must give the CPU spikes exactly
        bool GpuSelfTest(ID3D12Device* device, std::string* error)
        {
            ConnectomeBuilder builder;
            const uint32_t n = 300;
            for (uint32_t i = 0; i < n; ++i)
            {
                NeuronInfo info;
                info.bodyId = 1 + i;
                info.type = i % 10 == 0 ? "poisson" : (i % 4 == 0 ? "tonic" : "relay");
                builder.AddNeuron(info);
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                for (uint32_t k = 1; k <= 12; ++k)
                {
                    const uint32_t j = (i * 7 + k * 31) % n;
                    if (j != i)
                    {
                        builder.AddEdge(i, j, (i % 3 == 0 ? -1 : 1) * static_cast<int32_t>(1 + (i * k) % 15));
                    }
                }
            }
            auto connectome = Connectome::FromImage(builder.Build("self-test"));
            auto run = [&](FlyBrain& brain)
            {
                brain.StimulateCurrent(connectome->Find("tonic"), 14.0f);
                brain.StimulateRate(connectome->Find("poisson"), 80.0f, 150.0f);
                brain.Advance(1003);
                brain.Advance(997);
                std::vector<SpikeEvent> spikes;
                brain.RecentSpikes(spikes, 1u << 16);
                std::sort(spikes.begin(), spikes.end(), [](const SpikeEvent& a, const SpikeEvent& b) { return a.step != b.step ? a.step < b.step : a.neuron < b.neuron; });
                return spikes;
            };
            FlyBrain cpu(connectome);
            const std::vector<SpikeEvent> expected = run(cpu);
            FlyBrain gpu(connectome);
            GpuOptions options;
            options.device = device;
            options.selfTest = false;
            if (gpu.SetBackend(BackendChoice::Gpu, options, error) != Backend::Gpu)
            {
                return false;
            }
            const std::vector<SpikeEvent> got = run(gpu);
            bool same = got.size() == expected.size() && !expected.empty();
            for (size_t k = 0; same && k < got.size(); ++k)
            {
                same = got[k].neuron == expected[k].neuron && got[k].step == expected[k].step;
            }
            if (!same && error != nullptr)
            {
                *error = "the GPU self-test gave other spikes than the CPU (" + std::to_string(got.size()) + " against " + std::to_string(expected.size()) + ")";
            }
            return same;
        }
    }

    Backend FlyBrain::SetBackend(BackendChoice choice, const GpuOptions& options, std::string* error)
    {
        if (choice == BackendChoice::Cpu)
        {
            if (gpu_ != nullptr)
            {
                gpuStatePulled_ = false;
                PullGpuState();
                gpu_.reset();
            }
            return Backend::Cpu;
        }
        if (gpu_ != nullptr)
        {
            return Backend::Gpu;
        }
        if (step_ + 0x10000000ull > UINT32_MAX)
        {
            if (error != nullptr)
            {
                *error = "the GPU counts steps in 32 bits (119 hours)";
            }
            return Backend::Cpu;
        }

        GpuModelConstants constants;
        constants.decayU = decayU_;
        constants.decayG = decayG_;
        constants.stepCoupling = couplingK_ * (decayG_ - decayU_);
        constants.couplingK = couplingK_;
        constants.weight = params_.weightMv / kFixedScale;
        constants.kick = params_.poissonKickMv;
        constants.threshold = thresholdU_;
        constants.reset = resetU_;
        constants.delaySteps = delaySteps_;
        // PoissonDraw with the high half of the step = 0
        constants.seedHash = Mix32(Mix32(Mix32(static_cast<uint32_t>(params_.seed) ^ 0x9E3779B9u) ^ static_cast<uint32_t>(params_.seed >> 32)) ^ 0u);
        constants.powU = powU_;
        constants.powG = powG_;
        std::unique_ptr<FlyBrainGpu> gpu = FlyBrainGpu::Create(options.device, options.allowWarp, *connectome_, replicas_, constants,
            static_cast<uint32_t>(std::max<size_t>(spikeRing_.size(), 1024)), error);
        if (gpu == nullptr)
        {
            return Backend::Cpu;
        }
        if (plasticity_ != nullptr)
        {
            gpu->SetPlasticity(plasticity_->rowOffset, plasticity_->edgePlastic, plasticity_->weight);
        }
        if (!params_.fastModulatorySynapses || !params_.silentTypePrefixes.empty())
        {
            gpu->SetSilentOutputs(silentOutput_);
        }
        if (options.selfTest && !GpuSelfTest(gpu->Device(), error))
        {
            return Backend::Cpu;
        }

        // The whole state goes to the GPU: the simulation continues there from this step
        GpuStateImage image;
        const size_t n = neuronCount_;
        image.state.resize(n);
        image.params.resize(n);
        image.pending.resize(n);
        image.spikeTotals = spikeTotals_;
        image.arrivals = arrivals_;
        for (size_t i = 0; i < n; ++i)
        {
            image.state[i] = { u_[i], g_[i], static_cast<uint32_t>(lastStep_[i]), static_cast<uint32_t>(std::min<uint64_t>(refractoryUntil_[i], UINT32_MAX)), awake_[i], 0u };
            image.params[i] = { externalDrive_[i], stimulusDrive_[i], poissonThreshold_[i], neuronRefractory_[i], watchMask_[i], { 0u, 0u, 0u } };
            image.pending[i] = pending_[i].load(std::memory_order_relaxed);
        }
        gpu->Upload(image);
        gpu->SetCompletionHandler([this](const GpuCompletedSteps& completed) { OnGpuCompleted(completed); });
        gpu->SetTracked(trackedUnits_);
        trackedTotals_.resize(trackedUnits_.size());
        for (size_t k = 0; k < trackedUnits_.size(); ++k)
        {
            trackedTotals_[k] = spikeTotals_[trackedUnits_[k]];
        }
        gpu_ = std::move(gpu);
        gpuDirty_.assign(n, 0);
        gpuDirtyList_.clear();
        completedStep_ = step_;
        gpuStatePulled_ = false;
        return Backend::Gpu;
    }

    std::string FlyBrain::GpuAdapterName() const
    {
        return gpu_ != nullptr ? gpu_->AdapterName() : std::string();
    }

    uint64_t FlyBrain::GpuMemoryBytes() const
    {
        return gpu_ != nullptr ? gpu_->MemoryBytes() : 0;
    }

    uint64_t FlyBrain::GpuSharedMemoryBytes() const
    {
        return gpu_ != nullptr ? gpu_->SharedMemoryBytes() : 0;
    }

    GpuSpikeRing FlyBrain::GetGpuSpikeRing() const
    {
        GpuSpikeRing ring;
        if (gpu_ != nullptr)
        {
            ring.ring = gpu_->RingBuffer();
            ring.counters = gpu_->CountersBuffer();
            ring.lastSpikeStep = gpu_->LastSpikeBuffer();
            ring.capacity = gpu_->RingCapacity();
            ring.fence = gpu_->Fence();
            ring.fenceValue = gpu_->LastSubmittedFenceValue();
        }
        return ring;
    }

    void FlyBrain::Poll()
    {
        if (gpu_ != nullptr)
        {
            gpu_->Poll(false);
        }
    }

    void FlyBrain::Synchronize()
    {
        if (gpu_ != nullptr)
        {
            FlushGpuUpdates();
            gpu_->Submit();
            gpu_->Poll(true);
        }
    }

    void FlyBrain::MarkGpuDirty(uint32_t neuron, bool wake)
    {
        if (gpuDirty_[neuron] == 0)
        {
            gpuDirtyList_.push_back(neuron);
        }
        gpuDirty_[neuron] = static_cast<uint8_t>(std::max<int>(gpuDirty_[neuron], wake ? 2 : 1));
        gpuStatePulled_ = false;
    }

    void FlyBrain::FlushGpuUpdates()
    {
        // The changes since the last window, with their final values: the GPU settles a woken neuron under its old
        // drive first, as Wake does on the CPU
        for (uint32_t neuron : gpuDirtyList_)
        {
            GpuNeuronUpdate update{};
            update.neuron = neuron;
            update.wake = gpuDirty_[neuron] == 2 ? 1u : 0u;
            update.driveExternal = externalDrive_[neuron];
            update.driveStimulus = stimulusDrive_[neuron];
            update.poissonThreshold = poissonThreshold_[neuron];
            update.refractorySteps = neuronRefractory_[neuron];
            update.watchMask = watchMask_[neuron];
            gpu_->QueueUpdate(update);
            gpuDirty_[neuron] = 0;
        }
        gpuDirtyList_.clear();
    }

    void FlyBrain::OnGpuCompleted(const GpuCompletedSteps& completed)
    {
        const uint32_t stride = FlyBrainGpu::kWatchStride;
        for (uint32_t t = 0; t < completed.steps; ++t)
        {
            const uint64_t step = completed.firstStep + t;
            const uint32_t* row = completed.counts + uint64_t(t) * stride;
            for (WatchId w = 0; w < watchers_.size() && w < FlyBrainGpu::kMaxWatches; ++w)
            {
                Watcher& watcher = *watchers_[w];
                watcher.perStep[step % watcher.perStep.size()] = row[w];
                watcher.total += row[w];
            }
            totalSpikes_ += row[stride - 1];
            stats_.spikes += row[stride - 1];
        }
        completedStep_ = completed.firstStep + completed.steps;
        if (completed.trackedTotals != nullptr)
        {
            std::copy(completed.trackedTotals, completed.trackedTotals + trackedTotals_.size(), trackedTotals_.begin());
        }
        // A window list overflow would lose input: it is sized 4 spikes per neuron, never reached in practice
        assert(completed.lostSpikes == 0);
        if (completed.callEnd)
        {
            LearnFromGpu(completedStep_);
        }
    }

    void FlyBrain::PullGpuState() const
    {
        if (gpu_ == nullptr || gpuStatePulled_)
        {
            return;
        }
        FlyBrain& self = const_cast<FlyBrain&>(*this);
        self.FlushGpuUpdates();
        GpuStateImage image;
        self.gpu_->Download(image);
        for (size_t i = 0; i < neuronCount_; ++i)
        {
            self.u_[i] = image.state[i].u;
            self.g_[i] = image.state[i].g;
            self.lastStep_[i] = image.state[i].lastStep;
            self.refractoryUntil_[i] = image.state[i].refractoryUntil;
            self.awake_[i] = static_cast<uint8_t>(image.state[i].awake);
            self.pending_[i].store(static_cast<uint8_t>(image.pending[i]), std::memory_order_relaxed);
        }
        self.spikeTotals_ = image.spikeTotals;
        self.arrivals_ = image.arrivals;
        gpuStatePulled_ = true;
    }

    uint32_t FlyBrain::NeuronSpikeCount(uint32_t neuron) const
    {
        PullGpuState();
        return spikeTotals_[neuron];
    }

    // ---------------------------------------------------------------- neuron math

    inline void FlyBrain::CatchUp(uint32_t neuron, uint64_t step, double& u, double& g) const
    {
        const uint64_t from = std::max(lastStep_[neuron], refractoryUntil_[neuron]);
        if (step <= from)
        {
            return; // nothing to integrate, or frozen by refractoriness the whole way
        }
        const uint64_t k = step - from;
        // a^k = table[k mod 4096] x table[4096]^(k / 4096): only correctly rounded multiplications, so the GPU
        // (FlyBrainGpu) gets the same bits. After about 40 factors both are 0
        double a = powU_[k % kPowerTableSize];
        double b = powG_[k % kPowerTableSize];
        for (uint64_t q = k / kPowerTableSize; q > 0 && a != 0.0; --q)
        {
            a *= powU_[kPowerTableSize];
            b *= powG_[kPowerTableSize];
        }
        const double drive = Drive(neuron);
        u = drive + (u - drive) * a + g * couplingK_ * (b - a);
        g *= b;
    }

    inline bool FlyBrain::CanFire(uint32_t neuron, double u, double g) const
    {
        // u relaxes towards drive + g(t), and g(t) decays monotonically to 0. When u, drive and drive + g are all at or
        // below the threshold, u stays there until the next input: the bound is exact, not an activity cutoff
        const double drive = Drive(neuron);
        return poissonThreshold_[neuron] != 0 || u > thresholdU_ || drive > thresholdU_ || drive + g > thresholdU_;
    }

    inline double FlyBrain::Drive(uint32_t neuron) const
    {
        return double(externalDrive_[neuron]) + double(stimulusDrive_[neuron]);
    }

    void FlyBrain::Wake(uint32_t neuron)
    {
        if (gpu_ != nullptr)
        {
            MarkGpuDirty(neuron, true);
            return;
        }
        // Settle the state at the current step under the old drive before anything about the neuron changes
        double u = u_[neuron];
        double g = g_[neuron];
        CatchUp(neuron, step_, u, g);
        u_[neuron] = u;
        g_[neuron] = g;
        lastStep_[neuron] = std::max(lastStep_[neuron], step_);
        awake_[neuron] = 1;
    }

    float FlyBrain::VoltageMv(uint32_t neuron) const
    {
        PullGpuState();
        double u = u_[neuron];
        double g = g_[neuron];
        CatchUp(neuron, step_, u, g);
        return static_cast<float>(params_.restMv + u);
    }

    float FlyBrain::SynapticMv(uint32_t neuron) const
    {
        PullGpuState();
        double u = u_[neuron];
        double g = g_[neuron];
        CatchUp(neuron, step_, u, g);
        return static_cast<float>(g);
    }

    uint32_t FlyBrain::AwakeCount() const
    {
        PullGpuState();
        uint32_t count = 0;
        for (uint8_t a : awake_)
        {
            count += a;
        }
        return count;
    }

    // ---------------------------------------------------------------- input

    void FlyBrain::ApplyStimulus(const Stimulus& stimulus, float sign)
    {
        const double probabilityPerStep = std::clamp(double(stimulus.value) * params_.dtMs / 1000.0, 0.0, 1.0);
        for (uint32_t neuron : stimulus.group.neurons)
        {
            Wake(neuron);
            if (stimulus.isRate)
            {
                // Rates of overlapping stimuli add up as probabilities per step
                poissonStimuli_[neuron] = static_cast<uint8_t>(poissonStimuli_[neuron] + (sign > 0 ? 1 : -1));
                stimulusPoisson_[neuron] = poissonStimuli_[neuron] == 0 ? 0.0 : std::clamp(stimulusPoisson_[neuron] + sign * probabilityPerStep, 0.0, 1.0);
                UpdatePoisson(neuron);
            }
            else
            {
                currentStimuli_[neuron] = static_cast<uint8_t>(currentStimuli_[neuron] + (sign > 0 ? 1 : -1));
                stimulusDrive_[neuron] = currentStimuli_[neuron] == 0 ? 0.0f : stimulusDrive_[neuron] + sign * stimulus.value;
            }
        }
    }

    StimulusId FlyBrain::StimulateRate(const Group& group, float rateHz, float durationMs)
    {
        auto stimulus = std::make_unique<Stimulus>();
        stimulus->group = group;
        stimulus->isRate = true;
        stimulus->value = rateHz;
        stimulus->endStep = durationMs < 0.0f ? UINT64_MAX : step_ + static_cast<uint64_t>(std::llround(durationMs / params_.dtMs));
        ApplyStimulus(*stimulus, 1.0f);
        stimuli_.push_back(std::move(stimulus));
        return static_cast<StimulusId>(stimuli_.size() - 1);
    }

    StimulusId FlyBrain::StimulateCurrent(const Group& group, float currentMv, float durationMs)
    {
        auto stimulus = std::make_unique<Stimulus>();
        stimulus->group = group;
        stimulus->isRate = false;
        stimulus->value = currentMv;
        stimulus->endStep = durationMs < 0.0f ? UINT64_MAX : step_ + static_cast<uint64_t>(std::llround(durationMs / params_.dtMs));
        ApplyStimulus(*stimulus, 1.0f);
        stimuli_.push_back(std::move(stimulus));
        return static_cast<StimulusId>(stimuli_.size() - 1);
    }

    void FlyBrain::StopStimulus(StimulusId id)
    {
        if (id < stimuli_.size() && stimuli_[id] != nullptr)
        {
            ApplyStimulus(*stimuli_[id], -1.0f);
            stimuli_[id].reset();
        }
    }

    void FlyBrain::StopAllStimuli()
    {
        for (StimulusId id = 0; id < stimuli_.size(); ++id)
        {
            StopStimulus(id);
        }
    }

    void FlyBrain::ExpireStimuli()
    {
        for (StimulusId id = 0; id < stimuli_.size(); ++id)
        {
            if (stimuli_[id] != nullptr && stimuli_[id]->endStep <= step_)
            {
                StopStimulus(id);
            }
        }
    }

    uint64_t FlyBrain::NextStimulusEnd() const
    {
        uint64_t next = UINT64_MAX;
        for (const auto& stimulus : stimuli_)
        {
            if (stimulus != nullptr)
            {
                next = std::min(next, stimulus->endStep);
            }
        }
        return next;
    }

    void FlyBrain::UpdatePoisson(uint32_t neuron)
    {
        const double probability = std::clamp(stimulusPoisson_[neuron] + externalPoisson_[neuron], 0.0, 1.0);
        poissonThreshold_[neuron] = static_cast<uint32_t>(std::min(probability * 4294967296.0, 4294967295.0));
        // Shiu: a neuron under Poisson input has no refractory period
        const bool noRefractory = params_.poissonWithoutRefractory && (poissonStimuli_[neuron] != 0 || externalPoisson_[neuron] > 0.0);
        neuronRefractory_[neuron] = static_cast<uint8_t>(noRefractory ? 0 : refractorySteps_);
    }

    void FlyBrain::SetPoissonRate(uint32_t neuron, float rateHz)
    {
        const double probability = std::clamp(double(rateHz) * params_.dtMs / 1000.0, 0.0, 1.0);
        if (externalPoisson_[neuron] == probability)
        {
            return;
        }
        Wake(neuron);
        externalPoisson_[neuron] = probability;
        UpdatePoisson(neuron);
    }

    Group FlyBrain::ForReplica(const Group& neurons, uint32_t replica) const
    {
        Group units;
        units.neurons.reserve(neurons.neurons.size());
        for (uint32_t neuron : neurons.neurons)
        {
            units.neurons.push_back(Unit(neuron, replica));
        }
        return units;
    }

    void FlyBrain::Track(const Group& units)
    {
        for (uint32_t unit : units.neurons)
        {
            if (trackedIndex_[unit] == 0)
            {
                trackedUnits_.push_back(unit);
                trackedIndex_[unit] = static_cast<uint32_t>(trackedUnits_.size());
            }
        }
        std::sort(trackedUnits_.begin(), trackedUnits_.end());
        for (size_t k = 0; k < trackedUnits_.size(); ++k)
        {
            trackedIndex_[trackedUnits_[k]] = static_cast<uint32_t>(k + 1);
        }
        if (gpu_ != nullptr)
        {
            Synchronize();
            PullGpuState();
            trackedTotals_.resize(trackedUnits_.size());
            for (size_t k = 0; k < trackedUnits_.size(); ++k)
            {
                trackedTotals_[k] = spikeTotals_[trackedUnits_[k]];
            }
            gpu_->SetTracked(trackedUnits_);
        }
    }

    uint32_t FlyBrain::TrackedSpikes(uint32_t unit) const
    {
        const uint32_t index = trackedIndex_[unit];
        assert(index != 0 && "Track the unit first");
        return gpu_ != nullptr ? trackedTotals_[index - 1] : spikeTotals_[unit];
    }

    void FlyBrain::SetDrive(uint32_t neuron, float currentMv)
    {
        if (externalDrive_[neuron] == currentMv)
        {
            return;
        }
        Wake(neuron);
        externalDrive_[neuron] = currentMv;
    }

    // ---------------------------------------------------------------- simulation

    void FlyBrain::Advance(uint64_t steps)
    {
        if (gpu_ != nullptr && plasticity_ != nullptr && learnWait_)
        {
            // The weights learned from the last call go into this one, as on the CPU: wait for its spikes
            gpu_->Poll(true);
        }
        learnWait_ = false;
        while (steps > 0)
        {
            ExpireStimuli();
            // A window never crosses the end of a stimulus, so a stimulus lasts exactly its steps
            const uint64_t untilStimulusEnd = NextStimulusEnd() - step_;
            const uint64_t window = std::min<uint64_t>({ steps, delaySteps_, untilStimulusEnd });
            if (gpu_ != nullptr)
            {
                FlushGpuUpdates();
                gpu_->RecordWindow(step_, static_cast<uint32_t>(window));
                step_ += window;
                stats_.steps += window;
                ++stats_.windows;
                gpuStatePulled_ = false;
            }
            else
            {
                RunWindow(window);
            }
            steps -= window;
        }
        ExpireStimuli();
        // Learning at the end of this call (PlasticityRule::learnEveryMs): the same calls on the CPU and the GPU
        bool learn = false;
        if (plasticity_ != nullptr)
        {
            const double every = plasticity_->rule.learnEveryMs;
            learn = every <= 0.0 || double(step_ - learnedAtStep_) * params_.dtMs >= every - 1e-9;
            if (learn)
            {
                learnedAtStep_ = step_;
            }
        }
        if (gpu_ != nullptr)
        {
            // Sent, not waited for: the counts of earlier submissions come in as the GPU finishes them
            gpu_->Submit(learn);
            learnWait_ = learn;
            gpu_->Poll(false);
        }
        else if (learn)
        {
            LearnFromCpu();
        }
    }

    void FlyBrain::AdvanceMs(double ms)
    {
        fractionalSteps_ += ms / params_.dtMs;
        // The epsilon keeps 28.57 ms x 7 from losing a step to rounding
        const double whole = std::floor(fractionalSteps_ + 1e-9);
        fractionalSteps_ -= whole;
        Advance(static_cast<uint64_t>(whole));
    }

    // The awake neurons of a block of kBlock neurons, packed: the window runs time-major over them, so a step reads one
    // contiguous stretch of a slot of the ring and the state stays in L1
    struct FlyBrain::Block
    {
        alignas(32) double u[kBlock];
        alignas(32) double g[kBlock];
        alignas(32) double drive[kBlock];
        alignas(32) int64_t refractoryUntil[kBlock];
        alignas(32) uint32_t ids[kBlock];
        uint32_t poissonLanes[kBlock];
        uint32_t count = 0; // lanes in use, the rest up to a multiple of 4 are empty lanes
        uint32_t poissonCount = 0;
    };

    void FlyBrain::IntegrateRange(uint32_t chunk, uint32_t begin, uint32_t end, const Window& window, std::vector<SpikeEvent>& spikes, uint64_t& updated)
    {
        Block& block = *blocks_[chunk];
        for (uint32_t blockBegin = begin; blockBegin < end; blockBegin += kBlock)
        {
            const uint32_t blockEnd = std::min(end, blockBegin + kBlock);
            block.count = 0;
            block.poissonCount = 0;
            for (uint32_t i = blockBegin; i < blockEnd; ++i)
            {
                if (pending_[i].load(std::memory_order_relaxed) == 0 && awake_[i] == 0)
                {
                    continue;
                }
                const uint32_t k = block.count++;
                block.ids[k] = i;
                double u = u_[i];
                double g = g_[i];
                CatchUp(i, window.begin, u, g);
                block.u[k] = u;
                block.g[k] = g;
                block.drive[k] = Drive(i);
                block.refractoryUntil[k] = static_cast<int64_t>(refractoryUntil_[i]);
                if (poissonThreshold_[i] != 0)
                {
                    block.poissonLanes[block.poissonCount++] = k;
                }
            }
            if (block.count == 0)
            {
                continue;
            }
            // Empty lanes up to the vector width: refractory forever, they never change or fire
            for (uint32_t k = block.count; k % 4 != 0; ++k)
            {
                block.ids[k] = blockBegin;
                block.u[k] = 0.0;
                block.g[k] = 0.0;
                block.drive[k] = 0.0;
                block.refractoryUntil[k] = INT64_MAX;
            }

            if (useAvx2_)
            {
                StepBlockAvx2(block, window, spikes);
            }
            else
            {
                StepBlock(block, window, spikes);
            }

            const uint32_t d = delaySteps_;
            const size_t n = neuronCount_;
            const uint32_t firstSlot = static_cast<uint32_t>(window.begin % d);
            const uint64_t windowEnd = window.begin + window.steps;
            for (uint32_t k = 0; k < block.count; ++k)
            {
                const uint32_t i = block.ids[k];
                // A window shorter than the delay leaves the input of its later steps for the next window
                bool inputLeft = false;
                for (uint32_t s = window.steps; s < d && !inputLeft; ++s)
                {
                    inputLeft = arrivals_[((firstSlot + s) % d) * n + i] != 0;
                }
                pending_[i].store(inputLeft ? 1 : 0, std::memory_order_relaxed);

                u_[i] = block.u[k];
                g_[i] = block.g[k];
                lastStep_[i] = windowEnd;
                refractoryUntil_[i] = static_cast<uint64_t>(block.refractoryUntil[k]);
                awake_[i] = CanFire(i, block.u[k], block.g[k]) ? 1 : 0;
            }
            updated += block.count;
        }
    }

    void FlyBrain::FireAndKick(Block& block, uint64_t t, const uint32_t* fired, uint32_t firedCount, std::vector<SpikeEvent>& spikes)
    {
        // Poisson input comes after the threshold check, a spike resets after both (Brian2 order)
        for (uint32_t p = 0; p < block.poissonCount; ++p)
        {
            const uint32_t k = block.poissonLanes[p];
            if (static_cast<int64_t>(t) >= block.refractoryUntil[k] && PoissonDraw(params_.seed, block.ids[k], t) < poissonThreshold_[block.ids[k]])
            {
                block.u[k] += params_.poissonKickMv;
            }
        }
        for (uint32_t f = 0; f < firedCount; ++f)
        {
            const uint32_t k = fired[f];
            const uint32_t i = block.ids[k];
            block.u[k] = resetU_;
            block.g[k] = 0.0;
            block.refractoryUntil[k] = static_cast<int64_t>(t + neuronRefractory_[i]);
            spikes.push_back({ i, 0u, t });
            ++spikeTotals_[i];
        }
    }

    void FlyBrain::StepBlock(Block& block, const Window& window, std::vector<SpikeEvent>& spikes)
    {
        const double a = decayU_;
        const double b = decayG_;
        const double c = couplingK_ * (decayG_ - decayU_);
        const double weight = params_.weightMv / kFixedScale; // the input is summed in 1/256 of a contact
        const double threshold = thresholdU_;
        const uint32_t d = delaySteps_;
        const size_t n = neuronCount_;
        const uint64_t windowEnd = window.begin + window.steps;
        uint32_t fired[kBlock];

        uint32_t slot = static_cast<uint32_t>(window.begin % d);
        for (uint64_t t = window.begin; t < windowEnd; ++t)
        {
            int32_t* in = &arrivals_[slot * n];
            uint32_t firedCount = 0;
            for (uint32_t k = 0; k < block.count; ++k)
            {
                // Brian2 order inside a step: state update, threshold, synaptic input (and Poisson input), reset.
                // Input that lands on a refractory neuron is lost: Brian2 clamps the variables of a refractory neuron
                // ("unless refractory"), checked spike for spike against Brian2 (docs/flybrain/flybrain.md)
                const uint32_t i = block.ids[k];
                const int32_t contacts = in[i];
                if (contacts != 0)
                {
                    in[i] = 0;
                }
                if (static_cast<int64_t>(t) < block.refractoryUntil[k])
                {
                    continue;
                }
                const double next = block.drive[k] + (block.u[k] - block.drive[k]) * a + block.g[k] * c;
                // The input is added after the threshold check, which reads only u
                block.g[k] = block.g[k] * b + weight * contacts;
                block.u[k] = next;
                if (next > threshold)
                {
                    fired[firedCount++] = k;
                }
            }
            if (firedCount != 0 || block.poissonCount != 0)
            {
                FireAndKick(block, t, fired, firedCount, spikes);
            }
            slot = slot + 1 == d ? 0 : slot + 1;
        }
    }

    void FlyBrain::StepBlockAvx2(Block& block, const Window& window, std::vector<SpikeEvent>& spikes)
    {
        // The arithmetic of StepBlock in the same order and without FMA, 4 doubles at a time: bit-identical results
        const __m256d a = _mm256_set1_pd(decayU_);
        const __m256d b = _mm256_set1_pd(decayG_);
        const __m256d c = _mm256_set1_pd(couplingK_ * (decayG_ - decayU_));
        const __m256d weight = _mm256_set1_pd(params_.weightMv / kFixedScale);
        const __m256d threshold = _mm256_set1_pd(thresholdU_);
        const uint32_t d = delaySteps_;
        const size_t n = neuronCount_;
        const uint64_t windowEnd = window.begin + window.steps;
        const uint32_t vectors = (block.count + 3) / 4;
        uint32_t fired[kBlock];

        uint32_t slot = static_cast<uint32_t>(window.begin % d);
        for (uint64_t t = window.begin; t < windowEnd; ++t)
        {
            int32_t* in = &arrivals_[slot * n];
            const __m256i now = _mm256_set1_epi64x(static_cast<int64_t>(t));
            uint32_t firedCount = 0;
            for (uint32_t v = 0; v < vectors; ++v)
            {
                const uint32_t k = v * 4;
                const __m128i index = _mm_load_si128(reinterpret_cast<const __m128i*>(block.ids + k));
                __m128i contacts = _mm_i32gather_epi32(in, index, 4);
                // All bits set where the neuron is refractory: there it keeps u, g and loses the input
                const __m256i refractory = _mm256_cmpgt_epi64(_mm256_load_si256(reinterpret_cast<const __m256i*>(block.refractoryUntil + k)), now);
                const int hasInput = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(contacts, _mm_setzero_si128()))) ^ 0xF;
                if (hasInput != 0)
                {
                    // Consume the input of the real lanes (an empty lane points at a neuron of the block: it must not eat its input)
                    for (uint32_t l = 0; l < 4; ++l)
                    {
                        if (((hasInput >> l) & 1) != 0)
                        {
                            if (k + l < block.count)
                            {
                                in[block.ids[k + l]] = 0;
                            }
                        }
                    }
                    if (k + 4 > block.count)
                    {
                        alignas(16) int32_t values[4];
                        _mm_store_si128(reinterpret_cast<__m128i*>(values), contacts);
                        for (uint32_t l = block.count - k; l < 4; ++l)
                        {
                            values[l] = 0;
                        }
                        contacts = _mm_load_si128(reinterpret_cast<const __m128i*>(values));
                    }
                }
                const __m256d frozen = _mm256_castsi256_pd(refractory);
                const __m256d u = _mm256_load_pd(block.u + k);
                const __m256d g = _mm256_load_pd(block.g + k);
                const __m256d drive = _mm256_load_pd(block.drive + k);
                const __m256d next = _mm256_add_pd(_mm256_add_pd(drive, _mm256_mul_pd(_mm256_sub_pd(u, drive), a)), _mm256_mul_pd(g, c));
                const __m256d gNext = _mm256_add_pd(_mm256_mul_pd(g, b), _mm256_mul_pd(weight, _mm256_cvtepi32_pd(contacts)));
                _mm256_store_pd(block.u + k, _mm256_blendv_pd(next, u, frozen));
                _mm256_store_pd(block.g + k, _mm256_blendv_pd(gNext, g, frozen));
                const int fire = _mm256_movemask_pd(_mm256_andnot_pd(frozen, _mm256_cmp_pd(next, threshold, _CMP_GT_OQ)));
                if (fire != 0)
                {
                    for (uint32_t l = 0; l < 4; ++l)
                    {
                        if (((fire >> l) & 1) != 0)
                        {
                            fired[firedCount++] = k + l;
                        }
                    }
                }
            }
            if (firedCount != 0 || block.poissonCount != 0)
            {
                FireAndKick(block, t, fired, firedCount, spikes);
            }
            slot = slot + 1 == d ? 0 : slot + 1;
        }
    }

    template <bool Atomic>
    void FlyBrain::DeliverRange(const SpikeEvent* begin, const SpikeEvent* end, uint64_t& edges)
    {
        const Connectome& c = *connectome_;
        const uint32_t* rowStart = c.RowStart();
        const uint32_t* targets = c.Targets();
        const int16_t* weights = c.Weights();
        const uint32_t d = delaySteps_;
        const uint32_t replicas = replicas_;
        const Plasticity* plastic = plasticity_.get();
        uint64_t count = 0;
        for (const SpikeEvent* spike = begin; spike != end; ++spike)
        {
            // The spike of step s lands at step s + d: the same slot, already consumed in this window. A spike of a
            // replica reaches the units of that replica only
            int32_t* slot = &arrivals_[(spike->step % d) * size_t(neuronCount_)];
            const uint32_t neuron = spike->neuron / replicas;
            const uint32_t replica = spike->neuron - neuron * replicas;
            if (silentOutput_[neuron] != 0)
            {
                continue; // a modulatory neuron without fast synapses
            }
            const uint32_t e0 = rowStart[neuron];
            const uint32_t e1 = rowStart[neuron + 1];
            // A KC with plastic synapses: their weights are the learned ones of this replica
            const int32_t* rowPlastic = plastic != nullptr && plastic->rowOffset[neuron] != UINT32_MAX ? &plastic->edgePlastic[plastic->rowOffset[neuron]] : nullptr;
            for (uint32_t e = e0; e < e1; ++e)
            {
                const uint32_t post = targets[e] * replicas + replica;
                int32_t weight = int32_t(weights[e]) * kFixedScale;
                if (rowPlastic != nullptr && rowPlastic[e - e0] >= 0)
                {
                    weight = plastic->weight[size_t(rowPlastic[e - e0]) * replicas + replica];
                }
                if constexpr (Atomic)
                {
                    // Integer atomic add (long is 32 bits on Windows); the integration phase reads the ring after jobs::Wait
                    _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(&slot[post]), weight);
                }
                else
                {
                    slot[post] += weight;
                }
                pending_[post].store(1, std::memory_order_relaxed);
            }
            count += e1 - e0;
        }
        edges += count;
    }

    void FlyBrain::RunWindow(uint64_t steps)
    {
        namespace jobs = myengine::jobs;

        Window window;
        window.begin = step_;
        window.steps = static_cast<uint32_t>(steps);

        // 1. Neurons: every chunk independent inside the window
        Clock::time_point start = Clock::now();
        const uint32_t chunkCount = static_cast<uint32_t>(chunkSpikes_.size());
        for (uint32_t k = 0; k < chunkCount; ++k)
        {
            chunkSpikes_[k].clear();
            chunkUpdates_[k] = 0;
        }
        struct IntegrateJob
        {
            FlyBrain* brain;
            const Window* window;
        };
        IntegrateJob integrateJob{ this, &window };
        auto integrateChunk = [](const IntegrateJob& job, uint32_t chunk)
        {
            FlyBrain& brain = *job.brain;
            const uint32_t begin = chunk * brain.chunkSize_;
            const uint32_t end = std::min(brain.neuronCount_, begin + brain.chunkSize_);
            brain.IntegrateRange(chunk, begin, end, *job.window, brain.chunkSpikes_[chunk], brain.chunkUpdates_[chunk]);
        };
        if (parallel_ && chunkCount > 1)
        {
            jobs::Context context;
            jobs::Dispatch(context, chunkCount, 1, [integrateJob, integrateChunk](jobs::JobArgs args) { integrateChunk(integrateJob, args.jobIndex); });
            jobs::Wait(context);
        }
        else
        {
            for (uint32_t k = 0; k < chunkCount; ++k)
            {
                integrateChunk(integrateJob, k);
            }
        }
        stats_.integrateSeconds += SecondsSince(start);

        // 2. Spikes of the window in neuron order (the order does not change the result, only the ring and the stats)
        start = Clock::now();
        windowSpikes_.clear();
        for (uint32_t k = 0; k < chunkCount; ++k)
        {
            windowSpikes_.insert(windowSpikes_.end(), chunkSpikes_[k].begin(), chunkSpikes_[k].end());
            stats_.neuronWindowUpdates += chunkUpdates_[k];
        }
        for (const auto& watcher : watchers_)
        {
            const uint64_t history = watcher->perStep.size();
            for (uint64_t t = window.begin; t < window.begin + window.steps; ++t)
            {
                watcher->perStep[t % history] = 0;
            }
        }
        if (!spikeRing_.empty())
        {
            for (const SpikeEvent& spike : windowSpikes_)
            {
                spikeRing_[spikeRingWritten_++ % spikeRing_.size()] = spike;
            }
        }
        for (const SpikeEvent& spike : windowSpikes_)
        {
            for (uint32_t m = watchHead_[spike.neuron]; m != 0; m = watchNext_[m - 1])
            {
                Watcher& watcher = *watchers_[watchOwner_[m - 1]];
                ++watcher.perStep[spike.step % watcher.perStep.size()];
                ++watcher.total;
            }
        }
        totalSpikes_ += windowSpikes_.size();
        stats_.spikes += windowSpikes_.size();
        stats_.otherSeconds += SecondsSince(start);

        // 3. Delivery, split by synapse count; integer sums make the order irrelevant. The spikes go slot by slot, so the
        // writes of a stretch stay in one slot of the ring (neurons x 4 bytes, 0.7 MB for MaleCNS) instead of all 18
        start = Clock::now();
        const size_t spikeCount = windowSpikes_.size();
        slotStart_.assign(delaySteps_ + 1ull, 0);
        for (const SpikeEvent& spike : windowSpikes_)
        {
            ++slotStart_[spike.step % delaySteps_ + 1];
        }
        for (uint32_t s = 0; s < delaySteps_; ++s)
        {
            slotStart_[s + 1ull] += slotStart_[s];
        }
        deliverySpikes_.resize(spikeCount);
        for (const SpikeEvent& spike : windowSpikes_)
        {
            deliverySpikes_[slotStart_[spike.step % delaySteps_]++] = spike;
        }
        spikeEdgePrefix_.resize(spikeCount + 1);
        spikeEdgePrefix_[0] = 0;
        const uint32_t* rowStart = connectome_->RowStart();
        for (size_t s = 0; s < spikeCount; ++s)
        {
            const uint32_t neuron = deliverySpikes_[s].neuron / replicas_;
            spikeEdgePrefix_[s + 1] = spikeEdgePrefix_[s] + (rowStart[neuron + 1] - rowStart[neuron]);
        }
        const uint64_t totalEdges = spikeEdgePrefix_[spikeCount];
        if (parallel_ && totalEdges >= kParallelDeliveryEdges)
        {
            const uint32_t parts = std::max(2u, (jobs::GetWorkerCount() + 1) * 4);
            std::vector<uint64_t> partEdges(parts, 0);
            struct DeliverJob
            {
                FlyBrain* brain;
                uint64_t total;
                uint32_t parts;
                uint64_t* edges;
            };
            DeliverJob deliverJob{ this, totalEdges, parts, partEdges.data() };
            jobs::Context context;
            jobs::Dispatch(context, parts, 1, [deliverJob](jobs::JobArgs args)
            {
                FlyBrain& brain = *deliverJob.brain;
                const auto& prefix = brain.spikeEdgePrefix_;
                // The part takes the spikes whose first synapse falls into its share of all synapses
                auto spikeAt = [&](uint64_t edge)
                {
                    return static_cast<size_t>(std::lower_bound(prefix.begin(), prefix.end() - 1, edge) - prefix.begin());
                };
                const size_t first = spikeAt(deliverJob.total * args.jobIndex / deliverJob.parts);
                const size_t last = spikeAt(deliverJob.total * (args.jobIndex + 1ull) / deliverJob.parts);
                const SpikeEvent* spikes = brain.deliverySpikes_.data();
                brain.DeliverRange<true>(spikes + first, spikes + last, deliverJob.edges[args.jobIndex]);
            });
            jobs::Wait(context);
            for (uint64_t edges : partEdges)
            {
                stats_.deliveredEdges += edges;
            }
        }
        else if (spikeCount > 0)
        {
            DeliverRange<false>(deliverySpikes_.data(), deliverySpikes_.data() + spikeCount, stats_.deliveredEdges);
        }
        stats_.deliverSeconds += SecondsSince(start);

        step_ += window.steps;
        stats_.steps += window.steps;
        ++stats_.windows;
    }

    // ---------------------------------------------------------------- output

    WatchId FlyBrain::Watch(const Group& group, float historyMs)
    {
        const WatchId id = static_cast<WatchId>(watchers_.size());
        auto watcher = std::make_unique<Watcher>();
        watcher->neuronCount = static_cast<uint32_t>(group.neurons.size());
        const uint64_t history = std::max<uint64_t>(delaySteps_, static_cast<uint64_t>(std::llround(historyMs / params_.dtMs)));
        watcher->perStep.assign(static_cast<size_t>(history), 0);
        for (uint32_t neuron : group.neurons)
        {
            watchNext_.push_back(watchHead_[neuron]);
            watchOwner_.push_back(id);
            watchHead_[neuron] = static_cast<uint32_t>(watchNext_.size());
        }
        watchers_.push_back(std::move(watcher));
        if (id < FlyBrainGpu::kMaxWatches)
        {
            for (uint32_t neuron : group.neurons)
            {
                watchMask_[neuron] |= 1u << id;
                if (gpu_ != nullptr)
                {
                    MarkGpuDirty(neuron, false);
                }
            }
        }
        // The GPU counts the spikes of the first 32 watches only
        assert(gpu_ == nullptr || id < FlyBrainGpu::kMaxWatches);
        return id;
    }

    uint64_t FlyBrain::SpikeCount(WatchId watch, float windowMs) const
    {
        const Watcher& watcher = *watchers_[watch];
        const uint64_t history = watcher.perStep.size();
        const uint64_t end = CompletedStep();
        uint64_t steps = std::min<uint64_t>(history, static_cast<uint64_t>(std::llround(windowMs / params_.dtMs)));
        steps = std::min(steps, end);
        uint64_t count = 0;
        for (uint64_t t = end - steps; t < end; ++t)
        {
            count += watcher.perStep[t % history];
        }
        return count;
    }

    float FlyBrain::RateHz(WatchId watch, float windowMs) const
    {
        const Watcher& watcher = *watchers_[watch];
        const uint64_t history = watcher.perStep.size();
        const uint64_t steps = std::min({ history, static_cast<uint64_t>(std::llround(windowMs / params_.dtMs)), CompletedStep() });
        if (steps == 0 || watcher.neuronCount == 0)
        {
            return 0.0f;
        }
        const double seconds = double(steps) * params_.dtMs / 1000.0;
        return static_cast<float>(double(SpikeCount(watch, windowMs)) / (seconds * watcher.neuronCount));
    }

    uint64_t FlyBrain::TotalSpikes(WatchId watch) const
    {
        return watchers_[watch]->total;
    }

    void FlyBrain::SetSpikeRingCapacity(uint32_t capacity)
    {
        spikeRing_.assign(capacity, SpikeEvent{});
        spikeRingWritten_ = 0;
    }

    void FlyBrain::RecentSpikes(std::vector<SpikeEvent>& out, uint32_t maxCount) const
    {
        out.clear();
        if (gpu_ != nullptr)
        {
            // The ring of the GPU: within a window the order of its spikes is the order the threads wrote them
            const_cast<FlyBrain&>(*this).FlushGpuUpdates();
            std::vector<uint32_t> pairs;
            uint64_t written = 0;
            gpu_->DownloadRing(pairs, written);
            const uint64_t capacity = gpu_->RingCapacity();
            const uint64_t count = std::min<uint64_t>({ written, capacity, maxCount });
            for (uint64_t k = written - count; k < written; ++k)
            {
                const uint64_t index = k & (capacity - 1);
                out.push_back({ pairs[2 * index], 0u, pairs[2 * index + 1] });
            }
            return;
        }
        if (spikeRing_.empty())
        {
            return;
        }
        const uint64_t available = std::min<uint64_t>(spikeRingWritten_, spikeRing_.size());
        const uint64_t count = std::min<uint64_t>(available, maxCount);
        for (uint64_t k = spikeRingWritten_ - count; k < spikeRingWritten_; ++k)
        {
            out.push_back(spikeRing_[k % spikeRing_.size()]);
        }
    }

    size_t FlyBrain::StateBytes() const
    {
        const size_t n = neuronCount_;
        size_t bytes = n * (sizeof(double) * 2 + sizeof(float) * 2 + sizeof(uint64_t) * 2 + sizeof(uint8_t) * 4 + sizeof(uint32_t) * 3 + sizeof(uint8_t) * 2);
        bytes += n * delaySteps_ * sizeof(int32_t);
        bytes += spikeRing_.size() * sizeof(SpikeEvent);
        for (const auto& watcher : watchers_)
        {
            bytes += watcher->perStep.size() * sizeof(uint32_t);
        }
        return bytes;
    }
}
