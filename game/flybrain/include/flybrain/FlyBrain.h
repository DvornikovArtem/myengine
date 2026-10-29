// FlyBrain.h
// Leaky integrate-and-fire simulation of a whole fly connectome with the model and constants of Shiu et al. 2024
// (Nature 634, "A Drosophila computational brain model reveals sensorimotor processing", code MIT). It is not a fly:
// it is a LIF network constrained by a wiring diagram. docs/flybrain/flybrain.md has the equations and the measurements.
//
// One neuron (u = v - v_0, mV):
//   du/dt = (I - u + g) / t_mbr      g: synaptic input, I: external drive (StimulateCurrent / SetDrive)
//   dg/dt = -g / tau
//   u > v_th - v_0  ->  spike, u = v_rst - v_0, g = 0, both frozen for t_rfc (input that lands meanwhile is lost)
//   a spike of neuron j adds w_syn * weight(j -> i) to g of every target i after t_dly
// integrated exactly on a fixed step dt (the linear system has a closed form), in the order of Brian2:
// state update, threshold, synaptic input, reset. Spike for spike the same as Brian2 with Shiu's equations.
//
// Parallel scheme. A spike reaches its targets t_dly = 18 steps later, so within a window of up to 18 steps every neuron
// evolves independently of the others: the neurons of a window run in parallel jobs, the spikes are delivered once per
// window. Input is summed in integer contacts (the weights are integer), with integer atomics when the delivery runs on
// several threads, and multiplied by w_syn when it is consumed. Integer sums do not depend on the order, so the result is
// bit-identical for any thread count. A neuron that cannot reach the threshold before its next input sleeps and is
// brought forward in closed form when the input comes. The others go through a window in blocks of 512, time-major,
// 4 neurons per AVX2 instruction when the CPU has it.

#pragma once

#include <flybrain/Connectome.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12Resource;

namespace flybrain
{
    class FlyBrainGpu;
    struct GpuCompletedSteps;

    // Where the simulation runs. The CPU is the reference; the GPU gives the same spikes (FlyBrainGpu, compute shaders)
    enum class Backend : uint8_t
    {
        Cpu,
        Gpu
    };

    enum class BackendChoice : uint8_t
    {
        Auto, // the GPU when there is a DX12 device with double precision and the self-test passes, else the CPU
        Cpu,
        Gpu
    };

    // "auto", "cpu", "gpu" (the --flybrain flag); false for anything else
    bool ParseBackendChoice(std::string_view text, BackendChoice& out);
    const char* ToString(Backend backend);

    struct GpuOptions
    {
        ID3D12Device* device = nullptr; // the device of the renderer (BrainView reads the spike ring there); null: an own one
        bool allowWarp = false;         // an own device may be WARP when there is no hardware adapter (tests)
        bool selfTest = true;           // compare a small network with the CPU before trusting the GPU
    };

    // The ring of the last spikes on the GPU, for a view that draws them without a copy to the CPU. ring: uint2 (neuron,
    // step) x capacity; counters[0]: spikes written so far (the next index & (capacity - 1)). Both are in COMMON state
    // between submissions; a graphics queue waits for fence >= fenceValue before reading
    struct GpuSpikeRing
    {
        ID3D12Resource* ring = nullptr;
        ID3D12Resource* counters = nullptr;
        uint32_t capacity = 0;
        ID3D12Fence* fence = nullptr;
        uint64_t fenceValue = 0;
    };

    // Shiu et al. 2024, model.py default_params. dt is the Brian2 default
    struct ModelParams
    {
        double restMv = -52.0; // v_0
        double resetMv = -52.0; // v_rst
        double thresholdMv = -45.0; // v_th
        double membraneTauMs = 20.0; // t_mbr
        double synapseTauMs = 5.0; // tau
        double refractoryMs = 2.2; // t_rfc
        double delayMs = 1.8; // t_dly; also the length of a parallel window
        double weightMv = 0.275; // w_syn: mV per synaptic contact
        double poissonKickMv = 0.275 * 250.0; // w_syn * f_poi: one Poisson input event added to v
        double dtMs = 0.1;
        // Shiu sets t_rfc = 0 for the neurons that get Poisson input, so their rate follows the input
        bool poissonWithoutRefractory = true;
        uint64_t seed = 1;
    };

    struct SpikeEvent
    {
        uint32_t neuron;
        uint32_t reserved;
        uint64_t step;
    };

    using StimulusId = uint32_t;
    using WatchId = uint32_t;
    constexpr float kForever = -1.0f;

    // Time spent in the last Advance calls (since ResetStats)
    struct BrainStats
    {
        uint64_t steps = 0;
        uint64_t windows = 0;
        uint64_t spikes = 0;
        uint64_t deliveredEdges = 0;
        uint64_t neuronWindowUpdates = 0; // neurons stepped through a window (awake or with input)
        double integrateSeconds = 0.0;
        double deliverSeconds = 0.0;
        double otherSeconds = 0.0;
    };

    class FlyBrain
    {
    public:
        explicit FlyBrain(std::shared_ptr<const Connectome> connectome, const ModelParams& params = {});
        ~FlyBrain();
        FlyBrain(const FlyBrain&) = delete;
        FlyBrain& operator=(const FlyBrain&) = delete;

        const Connectome& GetConnectome() const { return *connectome_; }
        const ModelParams& Params() const { return params_; }

        // true: windows run as jobs of the engine job system (jobs::Initialize must have been called).
        // false: everything on the calling thread. The result is the same either way
        void SetParallel(bool parallel) { parallel_ = parallel; }
        bool IsParallel() const { return parallel_; }
        // The neuron step runs on AVX2 when the CPU has it (the same results bit for bit); false forces the scalar code
        void SetSimd(bool enabled);
        bool UsesSimd() const { return useAvx2_; }

        // Moves the simulation to the CPU or the GPU, with its whole state, at any step. Returns the backend in use;
        // error says why the GPU was not taken. On the GPU at most 32 watches count spikes
        Backend SetBackend(BackendChoice choice, const GpuOptions& options = {}, std::string* error = nullptr);
        Backend GetBackend() const { return gpu_ != nullptr ? Backend::Gpu : Backend::Cpu; }
        std::string GpuAdapterName() const;
        // GPU memory of this brain and of the connectome it shares with other brains on the device
        uint64_t GpuMemoryBytes() const;
        uint64_t GpuSharedMemoryBytes() const;
        GpuSpikeRing GetGpuSpikeRing() const;

        // ---- input
        // Poisson input at rateHz into every neuron of the group: each event adds poissonKickMv to v (Shiu's stimulation)
        StimulusId StimulateRate(const Group& group, float rateHz, float durationMs = kForever);
        // Constant drive I (mV) into every neuron of the group
        StimulusId StimulateCurrent(const Group& group, float currentMv, float durationMs = kForever);
        void StopStimulus(StimulusId id);
        void StopAllStimuli();
        // Per-neuron drive on top of the stimuli (for inputs that change every frame, such as photoreceptors)
        void SetDrive(uint32_t neuron, float currentMv);
        double Drive(uint32_t neuron) const;

        // ---- simulation
        void Advance(uint64_t steps);
        // Advances by ms, carrying the fraction of a step to the next call (a DOOM tick of 28.57 ms is 285 or 286 steps)
        void AdvanceMs(double ms);
        uint64_t Step() const { return step_; }
        // On the GPU Advance only records and sends the windows: the spike counts of the watches arrive a submission
        // later. CompletedStep is the end of what is known (Step() on the CPU); Synchronize waits for everything
        uint64_t CompletedStep() const { return gpu_ != nullptr ? completedStep_ : step_; }
        // Takes the counts of the submissions the GPU has finished, without waiting (Advance does it too)
        void Poll();
        void Synchronize();
        double TimeMs() const { return static_cast<double>(step_) * params_.dtMs; }
        uint32_t StepsPerWindow() const { return delaySteps_; }

        // ---- output
        // A watched group counts its spikes per step for the last historyMs
        WatchId Watch(const Group& group, float historyMs = 2000.0f);
        // Spikes of the group in the last windowMs before CompletedStep() (at most historyMs)
        uint64_t SpikeCount(WatchId watch, float windowMs) const;
        // Mean rate of one neuron of the group in the last windowMs, Hz
        float RateHz(WatchId watch, float windowMs) const;
        uint64_t TotalSpikes(WatchId watch) const;
        // Spikes of one neuron since the start (on the GPU: waits for the GPU and reads the state back)
        uint32_t NeuronSpikeCount(uint32_t neuron) const;
        // Spikes of the whole brain until CompletedStep()
        uint64_t TotalSpikes() const { return totalSpikes_; }

        // The last spikes of the whole brain, oldest first (for a raster or a 3D view). capacity is the ring size
        void SetSpikeRingCapacity(uint32_t capacity);
        void RecentSpikes(std::vector<SpikeEvent>& out, uint32_t maxCount) const;

        // Membrane potential (mV) and synaptic input (mV) of a neuron at the current step (GPU: a synchronous read back)
        float VoltageMv(uint32_t neuron) const;
        float SynapticMv(uint32_t neuron) const;
        // Neurons that are not asleep (stepped through every window)
        uint32_t AwakeCount() const;

        const BrainStats& Stats() const { return stats_; }
        void ResetStats() { stats_ = {}; }
        // Bytes of the simulation state, without the connectome
        size_t StateBytes() const;

    private:
        struct Stimulus;
        struct Watcher;
        struct Window;

        void RunWindow(uint64_t steps);
        struct Block;
        void IntegrateRange(uint32_t chunk, uint32_t begin, uint32_t end, const Window& window, std::vector<SpikeEvent>& spikes, uint64_t& updated);
        void StepBlock(Block& block, const Window& window, std::vector<SpikeEvent>& spikes);
        void StepBlockAvx2(Block& block, const Window& window, std::vector<SpikeEvent>& spikes);
        void FireAndKick(Block& block, uint64_t t, const uint32_t* fired, uint32_t firedCount, std::vector<SpikeEvent>& spikes);
        template <bool Atomic>
        void DeliverRange(const SpikeEvent* begin, const SpikeEvent* end, uint64_t& edges);
        // Brings a sleeping neuron from lastStep_ to step in closed form (no threshold can be crossed on the way)
        void CatchUp(uint32_t neuron, uint64_t step, double& u, double& g) const;
        void Wake(uint32_t neuron);
        void ApplyStimulus(const Stimulus& stimulus, float sign);
        void ExpireStimuli();
        uint64_t NextStimulusEnd() const;
        bool CanFire(uint32_t neuron, double u, double g) const;
        void MarkGpuDirty(uint32_t neuron, bool wake);
        void FlushGpuUpdates();
        void OnGpuCompleted(const GpuCompletedSteps& completed);
        // GPU: reads the neuron state back into the CPU arrays (they are not kept up to date while the GPU runs)
        void PullGpuState() const;

        std::shared_ptr<const Connectome> connectome_;
        ModelParams params_;
        bool parallel_ = false;
        bool useAvx2_ = false;
        uint32_t neuronCount_ = 0;
        uint32_t delaySteps_ = 18;
        uint32_t refractorySteps_ = 22;
        double thresholdU_ = 7.0; // v_th - v_0
        double resetU_ = 0.0; // v_rst - v_0
        double decayU_ = 0.0; // exp(-dt / t_mbr)
        double decayG_ = 0.0; // exp(-dt / tau)
        double couplingK_ = 0.0; // tau / (tau - t_mbr)
        std::vector<double> powU_; // decayU_^k for the closed-form catch-up
        std::vector<double> powG_;

        uint64_t step_ = 0;
        double fractionalSteps_ = 0.0;

        // Neuron state (structure of arrays, double like Brian2: a float32 state moves a spike near the threshold by a step). u, g are valid at lastStep_; refractoryUntil_ is the first step that integrates again
        std::vector<double> u_;
        std::vector<double> g_;
        std::vector<uint64_t> lastStep_;
        std::vector<uint64_t> refractoryUntil_;
        std::vector<uint8_t> awake_;
        std::vector<uint8_t> neuronRefractory_; // per neuron: 0 for Poisson-driven neurons (poissonWithoutRefractory)
        std::vector<uint32_t> spikeTotals_;

        // Input. arrivals_[(step % delaySteps_) * neuronCount_ + neuron]: contacts that land on the neuron at that step
        // Written with integer atomics by the parallel delivery, read plainly by the integration (a jobs::Wait between)
        std::vector<int32_t> arrivals_;
        std::unique_ptr<std::atomic<uint8_t>[]> pending_;
        std::vector<float> stimulusDrive_;
        std::vector<float> externalDrive_;
        std::vector<uint8_t> currentStimuli_; // active current stimuli per neuron, to reset the sum to exact 0
        std::vector<uint32_t> poissonThreshold_; // P(event per step) * 2^32
        std::vector<uint8_t> poissonStimuli_;

        std::vector<std::unique_ptr<Stimulus>> stimuli_;
        std::vector<std::unique_ptr<Watcher>> watchers_;
        std::vector<uint32_t> watchHead_; // per neuron: first membership + 1, 0 when not watched
        std::vector<uint32_t> watchNext_; // memberships: next + 1
        std::vector<WatchId> watchOwner_;

        // Work of a window
        uint32_t chunkSize_ = 4096;
        std::vector<std::vector<SpikeEvent>> chunkSpikes_;
        std::vector<uint64_t> chunkUpdates_;
        std::vector<std::unique_ptr<Block>> blocks_; // one per chunk
        std::vector<SpikeEvent> windowSpikes_;
        std::vector<SpikeEvent> deliverySpikes_; // the same, ordered by slot
        std::vector<size_t> slotStart_;
        std::vector<uint64_t> spikeEdgePrefix_;

        std::vector<SpikeEvent> spikeRing_;
        uint64_t spikeRingWritten_ = 0;
        uint64_t totalSpikes_ = 0;
        BrainStats stats_;

        std::vector<uint32_t> watchMask_; // per neuron: bit w for watch w < 32 (the GPU counts these)
        std::unique_ptr<FlyBrainGpu> gpu_;
        std::vector<uint8_t> gpuDirty_; // per neuron: 1 changed, 2 changed and woken
        std::vector<uint32_t> gpuDirtyList_;
        uint64_t completedStep_ = 0;
        mutable bool gpuStatePulled_ = false;
    };
}
