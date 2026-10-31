// FlyBrainGpu.h
// The GPU backend of FlyBrain: the neuron state lives in DX12 buffers and every window of the simulation is three compute
// dispatches and a compaction (assets/shaders/flybrain.hlsl) on a compute queue of its own. FlyBrain keeps the stimuli and the watches and
// sends what changed; the per-step spike counts of the watches come back asynchronously, a submission later.
// Same model, same order of operations, same double arithmetic: the spikes are the same as on the CPU, bit for bit.
// docs/flybrain/flybrain.md, section "GPU".

#pragma once

#include <flybrain/Connectome.h>

#include <directx-headers/directx/d3dx12.h> // its d3d12.h, newer than the one of the Windows SDK
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flybrain
{
    // The constants of the model as the CPU computed them (bits matter)
    struct GpuModelConstants
    {
        double decayU = 0.0;
        double decayG = 0.0;
        double stepCoupling = 0.0;
        double couplingK = 0.0;
        double weight = 0.0;
        double kick = 0.0;
        double threshold = 0.0;
        double reset = 0.0;
        uint32_t delaySteps = 18;
        uint32_t seedHash = 0;
        std::vector<double> powU; // tableSize + 1 entries
        std::vector<double> powG;
    };

    // Matches NeuronState / NeuronParams / NeuronUpdate of flybrain.hlsl
    struct GpuNeuronState
    {
        double u;
        double g;
        uint32_t lastStep;
        uint32_t refractoryUntil;
        uint32_t awake;
        uint32_t unused;
    };
    static_assert(sizeof(GpuNeuronState) == 32);

    struct GpuNeuronParams
    {
        float driveExternal;
        float driveStimulus;
        uint32_t poissonThreshold;
        uint32_t refractorySteps;
        uint32_t watchMask;
        uint32_t unused[3];
    };
    static_assert(sizeof(GpuNeuronParams) == 32);

    struct GpuNeuronUpdate
    {
        uint32_t neuron;
        uint32_t wake;
        float driveExternal;
        float driveStimulus;
        uint32_t poissonThreshold;
        uint32_t refractorySteps;
        uint32_t watchMask;
        uint32_t unused;
    };
    static_assert(sizeof(GpuNeuronUpdate) == 32);

    // The whole simulation state, for the upload when the backend starts and the download for queries
    struct GpuStateImage
    {
        std::vector<GpuNeuronState> state;
        std::vector<GpuNeuronParams> params;
        std::vector<uint32_t> pending;
        std::vector<uint32_t> spikeTotals;
        std::vector<int32_t> arrivals; // [slot][neuron]
    };

    // The counts of a finished submission: steps x (32 watches + the whole brain)
    struct GpuCompletedSteps
    {
        uint64_t firstStep = 0;
        uint32_t steps = 0;
        const uint32_t* counts = nullptr;
        uint32_t lostSpikes = 0; // spikes beyond the capacity of the window list (not delivered): must stay 0
        const uint32_t* trackedTotals = nullptr; // spike totals of the tracked units at the end of the submission
        bool callEnd = false;
    };

    class FlyBrainGpu
    {
    public:
        static constexpr uint32_t kWatchStride = 33;
        static constexpr uint32_t kMaxWatches = 32;

        // device == nullptr: the backend makes its own device (the fastest hardware adapter, WARP when allowWarp and
        // there is none). nullptr result: error says why (no adapter, no double precision in shaders, no shader)
        static std::unique_ptr<FlyBrainGpu> Create(ID3D12Device* device, bool allowWarp, const Connectome& connectome, uint32_t replicas,
            const GpuModelConstants& constants, uint32_t ringCapacity, std::string* error);
        ~FlyBrainGpu();

        void Upload(const GpuStateImage& image);
        // Waits for the GPU and reads everything back (queries of single neurons, a switch back to the CPU)
        void Download(GpuStateImage& image);
        void DownloadRing(std::vector<uint32_t>& pairs, uint64_t& written);

        // Units whose spike totals every submission copies back (sorted; consecutive units go in one copy)
        void SetTracked(const std::vector<uint32_t>& units);

        // Plastic synapses (N3): per graph neuron the offset of its row into edgePlastic (UINT32_MAX: none), per edge of those
        // rows the synapse or -1, the weights [synapse][replica] in 1/256 of a contact. Once, before the first window
        void SetPlasticity(const std::vector<uint32_t>& rowOffset, const std::vector<int32_t>& edgePlastic, const std::vector<int32_t>& weights);
        // Neurons whose spikes are not delivered (FlyBrain: ModelParams::fastModulatorySynapses = false), per graph neuron
        void SetSilentOutputs(const std::vector<uint8_t>& silent);
        // A learned weight that changed ([synapse][replica] index), applied at the start of the next window
        void QueuePlasticWeight(uint32_t index, int32_t weight) { queuedPlastic_.push_back({ index, static_cast<uint32_t>(weight) }); }

        // Applied at the start of the next recorded window
        void QueueUpdate(const GpuNeuronUpdate& update);
        // Records one window into the open submission; a full submission is sent on its own
        void RecordWindow(uint64_t begin, uint32_t steps);
        // Sends the open submission. Does not wait. callEnd: the last submission of a call of FlyBrain::Advance (learning
        // happens when such a submission is done)
        void Submit(bool callEnd = false);
        // Receives the counts of every finished submission, in order (from Poll, Download and a full slot ring)
        void SetCompletionHandler(std::function<void(const GpuCompletedSteps&)> handler) { onCompleted_ = std::move(handler); }
        // Hands the finished submissions to the handler. wait: until all sent work is done
        void Poll(bool wait);

        const std::string& AdapterName() const { return adapterName_; }
        uint64_t MemoryBytes() const { return memoryBytes_; }
        uint64_t SharedMemoryBytes() const;
        uint32_t RingCapacity() const { return ringCapacity_; }
        ID3D12Device* Device() const { return device_.Get(); }
        ID3D12Resource* RingBuffer() const { return ring_.Get(); }
        ID3D12Resource* CountersBuffer() const { return counters_.Get(); }
        ID3D12Resource* LastSpikeBuffer() const { return lastSpike_.Get(); }
        std::shared_ptr<ID3D12Resource> SnapshotLastSpike();
        ID3D12Fence* Fence() const { return fence_.Get(); }
        uint64_t LastSubmittedFenceValue() const { return fenceValue_; }

    private:
        struct SharedGraph;
        struct Slot;

        FlyBrainGpu() = default;
        bool Initialize(ID3D12Device* device, bool allowWarp, const Connectome& connectome, uint32_t replicas, const GpuModelConstants& constants,
            uint32_t ringCapacity, std::string* error);
        bool BeginSubmission();
        void BindAll();
        Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap, bool unordered, D3D12_RESOURCE_STATES state);
        void UploadTo(ID3D12Resource* destination, const void* data, uint64_t size);
        void ReadBack(ID3D12Resource* source, void* data, uint64_t size);
        void WaitIdle();
        void WaitFence(uint64_t value);
        void Quarantine(IUnknown* extraResource = nullptr, IUnknown* extraAllocator = nullptr, IUnknown* extraSourceOrDestination = nullptr) noexcept;

        Microsoft::WRL::ComPtr<ID3D12Device> device_;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
        HANDLE fenceEvent_ = nullptr;
        uint64_t fenceValue_ = 0;
        bool waitFailed_ = false;
        bool quarantined_ = false;
        std::string adapterName_;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> resetPso_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> updatePso_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> compactPso_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> integratePso_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> deliverPso_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> plasticPso_;

        std::shared_ptr<SharedGraph> graph_;
        Microsoft::WRL::ComPtr<ID3D12Resource> model_; // constant buffer (upload heap)
        Microsoft::WRL::ComPtr<ID3D12Resource> state_;
        Microsoft::WRL::ComPtr<ID3D12Resource> params_;
        Microsoft::WRL::ComPtr<ID3D12Resource> pending_;
        Microsoft::WRL::ComPtr<ID3D12Resource> spikeTotals_;
        Microsoft::WRL::ComPtr<ID3D12Resource> arrivals_;
        Microsoft::WRL::ComPtr<ID3D12Resource> spikeList_;
        Microsoft::WRL::ComPtr<ID3D12Resource> counters_;
        Microsoft::WRL::ComPtr<ID3D12Resource> watchCounts_;
        Microsoft::WRL::ComPtr<ID3D12Resource> ring_;
        Microsoft::WRL::ComPtr<ID3D12Resource> active_;
        Microsoft::WRL::ComPtr<ID3D12Resource> lastSpike_;
        Microsoft::WRL::ComPtr<ID3D12Resource> silentOutput_;
        Microsoft::WRL::ComPtr<ID3D12Resource> plasticRowOffset_;
        Microsoft::WRL::ComPtr<ID3D12Resource> edgePlastic_;
        Microsoft::WRL::ComPtr<ID3D12Resource> plasticWeights_;
        std::vector<std::pair<uint32_t, uint32_t>> queuedPlastic_;
        bool plastic_ = false;
        Microsoft::WRL::ComPtr<ID3D12Resource> powU_;
        Microsoft::WRL::ComPtr<ID3D12Resource> powG_;

        std::vector<std::unique_ptr<Slot>> slots_;
        uint32_t nextSlot_ = 0;
        Slot* open_ = nullptr; // the submission being recorded
        std::deque<Slot*> submitted_;
        std::function<void(const GpuCompletedSteps&)> onCompleted_;
        std::vector<GpuNeuronUpdate> queuedUpdates_;

        uint32_t neuronCount_ = 0; // units
        uint32_t graphNeurons_ = 0;
        std::vector<std::pair<uint32_t, uint32_t>> trackedRuns_; // first unit, count
        uint32_t trackedCount_ = 0;
        uint32_t delaySteps_ = 18;
        uint32_t spikeCapacity_ = 0;
        uint32_t ringCapacity_ = 0;
        uint64_t memoryBytes_ = 0;
    };
}
