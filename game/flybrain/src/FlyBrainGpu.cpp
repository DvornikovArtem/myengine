// FlyBrainGpu.cpp

#include "FlyBrainGpu.h"

#include "ShaderCompiler.h"

#include <myengine/core/AppPaths.h>


#include <dxgi1_6.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>

namespace flybrain
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        constexpr const char* kShaderPath = "assets/shaders/flybrain.hlsl";
        constexpr uint32_t kThreads = 64;           // numthreads of the kernels
        constexpr uint32_t kSlots = 3;              // submissions in flight
        constexpr uint32_t kMaxWindows = 256;       // windows of one submission (a counter each)
        constexpr uint32_t kMaxSteps = 4096;        // steps of one submission (a row of watch counts each)
        constexpr uint32_t kDeliverGroups = 512;
        constexpr uint32_t kMaxTracked = 65536;     // tracked units copied back per submission
        constexpr uint64_t kReadbackTracked = uint64_t(kMaxSteps) * 33 * 4 + 16; // where they go in the readback buffer

        // Root parameters: the model constants, the window constants, 6 SRVs, 11 UAVs (root descriptors: no heaps)
        constexpr UINT kSrvCount = 10;
        constexpr UINT kUavCount = 12;
        enum RootIndex : UINT
        {
            kRootModel = 0,
            kRootWindow = 1,
            kRootFirstSrv = 2,
            kRootFirstUav = kRootFirstSrv + kSrvCount,
            kRootCount = kRootFirstUav + kUavCount
        };

        struct ModelCb
        {
            uint32_t decayU[2];
            uint32_t decayG[2];
            uint32_t stepCoupling[2];
            uint32_t couplingK[2];
            uint32_t weight[2];
            uint32_t kick[2];
            uint32_t threshold[2];
            uint32_t reset[2];
            uint32_t neuronCount;
            uint32_t delaySteps;
            uint32_t tableSize;
            uint32_t spikeCapacity;
            uint32_t ringMask;
            uint32_t seedHash;
            uint32_t watchStride;
            uint32_t maxWindows;
            uint32_t replicas;
            uint32_t plastic;
            uint32_t unused[2];
        };
        static_assert(sizeof(ModelCb) == 112);

        struct WindowConstants
        {
            uint32_t windowBegin;
            uint32_t steps;
            uint32_t firstSlot;
            uint32_t windowIndex;
            uint32_t stepOffset;
            uint32_t updateOffset;
            uint32_t updateCount;
            uint32_t groupCount;
        };

        void PutDouble(uint32_t (&out)[2], double value)
        {
            std::memcpy(out, &value, sizeof(value));
        }

        bool Fail(std::string* error, const std::string& message)
        {
            if (error != nullptr)
            {
                *error = message;
            }
            return false;
        }

        uint32_t NextPowerOfTwo(uint32_t value)
        {
            uint32_t p = 1024;
            while (p < value && p < (1u << 30))
            {
                p <<= 1;
            }
            return p;
        }

        std::filesystem::path FindShader()
        {
            // Like the shaders of the renderer: the working directory, next to the executable, the sources
            std::vector<std::filesystem::path> candidates{ std::filesystem::current_path() / kShaderPath };
            wchar_t module[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, module, MAX_PATH) > 0)
            {
                candidates.push_back(std::filesystem::path(module).parent_path() / kShaderPath);
            }
            const std::filesystem::path sources = myengine::core::apppaths::SourceRoot();
            if (!sources.empty())
            {
                candidates.push_back(sources / kShaderPath);
            }
            for (const auto& candidate : candidates)
            {
                std::error_code ignored;
                if (std::filesystem::exists(candidate, ignored))
                {
                    return candidate;
                }
            }
            return {};
        }

        bool SupportsDoubles(ID3D12Device* device)
        {
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
            return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))
                && options.DoublePrecisionFloatShaderOps;
        }

        std::string Narrow(const wchar_t* text)
        {
            const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            std::string out(size > 0 ? size - 1 : 0, '\0');
            if (size > 1)
            {
                WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
            }
            return out;
        }

        // The fastest hardware adapter with double precision in shaders; WARP when allowed and there is none
        ComPtr<ID3D12Device> CreateDevice(bool allowWarp, std::string& adapterName, std::string* error)
        {
            ComPtr<IDXGIFactory6> factory;
            if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
            {
                Fail(error, "no DXGI 1.6 factory");
                return nullptr;
            }
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT index = 0; factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++index)
            {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                {
                    continue;
                }
                ComPtr<ID3D12Device> device;
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) && SupportsDoubles(device.Get()))
                {
                    adapterName = Narrow(desc.Description);
                    return device;
                }
            }
            if (allowWarp)
            {
                ComPtr<IDXGIAdapter> warp;
                ComPtr<ID3D12Device> device;
                if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
                    && SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) && SupportsDoubles(device.Get()))
                {
                    adapterName = "WARP software rasterizer";
                    return device;
                }
            }
            Fail(error, "no DX12 adapter with double precision in compute shaders");
            return nullptr;
        }
    }

    // CSR of a connectome on a device: one copy for any number of flies
    struct FlyBrainGpu::SharedGraph
    {
        ComPtr<ID3D12Resource> rowStart;
        ComPtr<ID3D12Resource> targets;
        ComPtr<ID3D12Resource> weights;
        uint64_t bytes = 0;
    };

    struct FlyBrainGpu::Slot
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12Resource> updates; // upload heap, mapped
        GpuNeuronUpdate* updatesMapped = nullptr;
        uint32_t updateCapacity = 0;
        uint32_t updateCount = 0;
        ComPtr<ID3D12Resource> readback;
        ComPtr<ID3D12Resource> plasticUpdates; // upload heap, mapped: (index, weight)
        std::pair<uint32_t, uint32_t>* plasticMapped = nullptr;
        uint32_t plasticCapacity = 0;
        uint32_t plasticCount = 0;
        bool callEnd = false;
        uint64_t fenceValue = 0;
        uint64_t firstStep = 0;
        uint32_t steps = 0;
        uint32_t windows = 0;
    };

    namespace
    {
        std::mutex gGraphMutex;
        // weak_ptr<void>: SharedGraph is private to FlyBrainGpu
        std::map<std::pair<ID3D12Device*, const Connectome*>, std::weak_ptr<void>> gGraphs;
    }

    std::unique_ptr<FlyBrainGpu> FlyBrainGpu::Create(ID3D12Device* device, bool allowWarp, const Connectome& connectome, uint32_t replicas,
        const GpuModelConstants& constants, uint32_t ringCapacity, std::string* error)
    {
        std::unique_ptr<FlyBrainGpu> gpu(new FlyBrainGpu());
        if (!gpu->Initialize(device, allowWarp, connectome, replicas, constants, ringCapacity, error))
        {
            return nullptr;
        }
        return gpu;
    }

    FlyBrainGpu::~FlyBrainGpu()
    {
        if (fence_ != nullptr)
        {
            WaitIdle();
        }
        for (auto& slot : slots_)
        {
            if (slot->updates != nullptr)
            {
                slot->updates->Unmap(0, nullptr);
            }
            if (slot->plasticUpdates != nullptr)
            {
                slot->plasticUpdates->Unmap(0, nullptr);
            }
        }
        if (fenceEvent_ != nullptr)
        {
            CloseHandle(fenceEvent_);
        }
    }

    ComPtr<ID3D12Resource> FlyBrainGpu::CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap, bool unordered, D3D12_RESOURCE_STATES state)
    {
        const CD3DX12_HEAP_PROPERTIES properties(heap);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(std::max<uint64_t>(size, 256),
            unordered ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer))))
        {
            return nullptr;
        }
        return buffer;
    }

    bool FlyBrainGpu::Initialize(ID3D12Device* device, bool allowWarp, const Connectome& connectome, uint32_t replicas, const GpuModelConstants& constants,
        uint32_t ringCapacity, std::string* error)
    {
        if (device != nullptr)
        {
            if (!SupportsDoubles(device))
            {
                return Fail(error, "the device has no double precision in compute shaders");
            }
            device_ = device;
            adapterName_ = "the device of the application";
        }
        else
        {
            device_ = CreateDevice(allowWarp, adapterName_, error);
            if (device_ == nullptr)
            {
                return false;
            }
        }
        graphNeurons_ = connectome.NeuronCount();
        neuronCount_ = graphNeurons_ * replicas;
        delaySteps_ = constants.delaySteps;
        spikeCapacity_ = std::max(1024u, neuronCount_ * 4);
        ringCapacity_ = NextPowerOfTwo(std::max(ringCapacity, 1u));

        // ---- shaders: the shader compiler of the renderer (fxc, structured messages), one PSO per kernel
        const std::filesystem::path shaderPath = FindShader();
        std::string source;
        std::vector<myengine::render::ShaderDiagnostic> diagnostics;
        if (shaderPath.empty() || !myengine::render::dx12::ReadShaderSource(shaderPath, source, diagnostics))
        {
            return Fail(error, std::string("cannot find ") + kShaderPath);
        }
        auto compile = [&](const char* entry, ComPtr<ID3DBlob>& blob) -> bool
        {
            myengine::render::dx12::ShaderStageSource request;
            request.sourceName = shaderPath.u8string();
            request.sourcePath = shaderPath;
            request.source = &source;
            request.entry = entry;
            request.profile = "cs_5_0";
            request.stage = myengine::render::ShaderStage::None;
            diagnostics.clear();
            if (myengine::render::dx12::CompileShaderStage(request, blob, diagnostics))
            {
                return true;
            }
            std::string text = std::string("cannot compile ") + entry + ":";
            for (const auto& d : diagnostics)
            {
                text += " " + d.file + "(" + std::to_string(d.line) + "): " + d.message + ";";
            }
            return Fail(error, text);
        };
        ComPtr<ID3DBlob> resetBlob;
        ComPtr<ID3DBlob> updateBlob;
        ComPtr<ID3DBlob> compactBlob;
        ComPtr<ID3DBlob> integrateBlob;
        ComPtr<ID3DBlob> deliverBlob;
        ComPtr<ID3DBlob> plasticBlob;
        if (!compile("CSPlastic", plasticBlob))
        {
            return false;
        }
        if (!compile("CSReset", resetBlob) || !compile("CSUpdate", updateBlob) || !compile("CSCompact", compactBlob) || !compile("CSIntegrate", integrateBlob)
            || !compile("CSDeliver", deliverBlob))
        {
            return false;
        }

        // ---- root signature: written here, the builder of the renderer knows only CBVs and texture tables
        D3D12_ROOT_PARAMETER parameters[kRootCount] = {};
        parameters[kRootModel].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[kRootModel].Descriptor.ShaderRegister = 0;
        parameters[kRootWindow].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[kRootWindow].Constants.ShaderRegister = 1;
        parameters[kRootWindow].Constants.Num32BitValues = sizeof(WindowConstants) / 4;
        for (UINT k = 0; k < kSrvCount; ++k)
        {
            parameters[kRootFirstSrv + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[kRootFirstSrv + k].Descriptor.ShaderRegister = k;
        }
        for (UINT k = 0; k < kUavCount; ++k)
        {
            parameters[kRootFirstUav + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[kRootFirstUav + k].Descriptor.ShaderRegister = k;
        }
        for (auto& parameter : parameters)
        {
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = kRootCount;
        rootDesc.pParameters = parameters;
        ComPtr<ID3DBlob> rootBlob;
        ComPtr<ID3DBlob> rootError;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootError))
            || FAILED(device_->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_))))
        {
            return Fail(error, std::string("root signature: ") + (rootError ? static_cast<const char*>(rootError->GetBufferPointer()) : "creation failed"));
        }
        auto makePso = [&](ID3DBlob* blob, ComPtr<ID3D12PipelineState>& pso)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = rootSignature_.Get();
            desc.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
            return SUCCEEDED(device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
        };
        if (!makePso(resetBlob.Get(), resetPso_) || !makePso(updateBlob.Get(), updatePso_) || !makePso(compactBlob.Get(), compactPso_)
            || !makePso(integrateBlob.Get(), integratePso_) || !makePso(plasticBlob.Get(), plasticPso_)
            || !makePso(deliverBlob.Get(), deliverPso_))
        {
            return Fail(error, "cannot create the compute pipeline states");
        }

        // ---- queue, list, fence
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_))) || FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
        {
            return Fail(error, "cannot create the compute queue");
        }
        queue_->SetName(L"FlyBrain compute");
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const uint32_t updateCapacity = std::max(4096u, neuronCount_);
        for (uint32_t k = 0; k < kSlots; ++k)
        {
            auto slot = std::make_unique<Slot>();
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&slot->allocator))))
            {
                return Fail(error, "cannot create a command allocator");
            }
            slot->updates = CreateBuffer(uint64_t(updateCapacity) * sizeof(GpuNeuronUpdate), D3D12_HEAP_TYPE_UPLOAD, false, D3D12_RESOURCE_STATE_GENERIC_READ);
            slot->readback = CreateBuffer(kReadbackTracked + uint64_t(kMaxTracked) * 4, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST);
            if (slot->updates == nullptr || slot->readback == nullptr)
            {
                return Fail(error, "out of GPU memory (submission buffers)");
            }
            const D3D12_RANGE nothing{ 0, 0 };
            slot->updates->Map(0, &nothing, reinterpret_cast<void**>(&slot->updatesMapped));
            slot->updateCapacity = updateCapacity;
            // Bound even without plasticity; SetPlasticity makes it as large as the learned weights
            slot->plasticUpdates = CreateBuffer(256, D3D12_HEAP_TYPE_UPLOAD, false, D3D12_RESOURCE_STATE_GENERIC_READ);
            slot->plasticUpdates->Map(0, &nothing, reinterpret_cast<void**>(&slot->plasticMapped));
            slot->plasticCapacity = 32;
            slots_.push_back(std::move(slot));
        }
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, slots_[0]->allocator.Get(), nullptr, IID_PPV_ARGS(&list_))))
        {
            return Fail(error, "cannot create a command list");
        }
        list_->Close();

        // ---- buffers
        const uint64_t n = neuronCount_;
        state_ = CreateBuffer(n * sizeof(GpuNeuronState), D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        params_ = CreateBuffer(n * sizeof(GpuNeuronParams), D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        pending_ = CreateBuffer(n * 4, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        spikeTotals_ = CreateBuffer(n * 4, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        arrivals_ = CreateBuffer(n * delaySteps_ * 4, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        spikeList_ = CreateBuffer(uint64_t(spikeCapacity_) * 8, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        counters_ = CreateBuffer(uint64_t(2 + 2 * kMaxWindows) * 4, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        watchCounts_ = CreateBuffer(uint64_t(kMaxSteps) * kWatchStride * 4, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        ring_ = CreateBuffer(uint64_t(ringCapacity_) * 8, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        active_ = CreateBuffer(n * 4, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        lastSpike_ = CreateBuffer(n * 4, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        // Without plasticity the three are bound but not read (kPlastic = 0); the row offsets still say "no plastic synapse"
        // for every neuron, so a stray read stays harmless
        plasticRowOffset_ = CreateBuffer(uint64_t(graphNeurons_) * 4, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
        edgePlastic_ = CreateBuffer(256, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
        plasticWeights_ = CreateBuffer(256, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        model_ = CreateBuffer(256, D3D12_HEAP_TYPE_UPLOAD, false, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!state_ || !params_ || !pending_ || !spikeTotals_ || !arrivals_ || !spikeList_ || !counters_ || !watchCounts_ || !ring_ || !active_ || !lastSpike_ || !model_)
        {
            return Fail(error, "out of GPU memory (neuron state)");
        }
        memoryBytes_ = n * (sizeof(GpuNeuronState) + sizeof(GpuNeuronParams) + 4 + 4 + 4 + 4 + uint64_t(delaySteps_) * 4) + uint64_t(spikeCapacity_) * 8
            + uint64_t(kMaxSteps) * kWatchStride * 4 + uint64_t(ringCapacity_) * 8;

        ModelCb cb{};
        PutDouble(cb.decayU, constants.decayU);
        PutDouble(cb.decayG, constants.decayG);
        PutDouble(cb.stepCoupling, constants.stepCoupling);
        PutDouble(cb.couplingK, constants.couplingK);
        PutDouble(cb.weight, constants.weight);
        PutDouble(cb.kick, constants.kick);
        PutDouble(cb.threshold, constants.threshold);
        PutDouble(cb.reset, constants.reset);
        cb.neuronCount = neuronCount_;
        cb.delaySteps = delaySteps_;
        cb.tableSize = static_cast<uint32_t>(constants.powU.size() - 1);
        cb.spikeCapacity = spikeCapacity_;
        cb.ringMask = ringCapacity_ - 1;
        cb.seedHash = constants.seedHash;
        cb.watchStride = kWatchStride;
        cb.maxWindows = kMaxWindows;
        cb.replicas = replicas;
        void* mapped = nullptr;
        model_->Map(0, nullptr, &mapped);
        std::memcpy(mapped, &cb, sizeof(cb));
        model_->Unmap(0, nullptr);

        // Power tables (per brain: they depend on the parameters)
        powU_ = CreateBuffer(constants.powU.size() * 8, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
        powG_ = CreateBuffer(constants.powG.size() * 8, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
        if (!powU_ || !powG_)
        {
            return Fail(error, "out of GPU memory (tables)");
        }
        UploadTo(powU_.Get(), constants.powU.data(), constants.powU.size() * 8);
        UploadTo(powG_.Get(), constants.powG.data(), constants.powG.size() * 8);

        {
            const std::vector<uint32_t> none(graphNeurons_, UINT32_MAX);
            UploadTo(plasticRowOffset_.Get(), none.data(), none.size() * 4);
            // Every neuron delivers its spikes unless SetSilentOutputs says otherwise
            silentOutput_ = CreateBuffer(uint64_t(graphNeurons_) * 4, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
            const std::vector<uint32_t> zero(graphNeurons_, 0u);
            UploadTo(silentOutput_.Get(), zero.data(), zero.size() * 4);
        }

        // ---- the graph, shared by every brain of this connectome on this device
        {
            std::lock_guard<std::mutex> lock(gGraphMutex);
            const auto key = std::make_pair(device_.Get(), &connectome);
            graph_ = std::static_pointer_cast<SharedGraph>(gGraphs[key].lock());
            if (graph_ == nullptr)
            {
                auto graph = std::make_shared<SharedGraph>();
                const uint64_t edges = connectome.EdgeCount();
                const uint64_t graphNeurons = graphNeurons_;
                graph->rowStart = CreateBuffer((graphNeurons + 1) * 4, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
                graph->targets = CreateBuffer(edges * 4, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
                graph->weights = CreateBuffer((edges * 2 + 3) / 4 * 4, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
                if (!graph->rowStart || !graph->targets || !graph->weights)
                {
                    return Fail(error, "out of GPU memory (connectome)");
                }
                UploadTo(graph->rowStart.Get(), connectome.RowStart(), (graphNeurons + 1) * 4);
                UploadTo(graph->targets.Get(), connectome.Targets(), edges * 4);
                std::vector<int16_t> weights(connectome.Weights(), connectome.Weights() + edges);
                weights.resize((weights.size() + 1) / 2 * 2, 0);
                UploadTo(graph->weights.Get(), weights.data(), weights.size() * 2);
                graph->bytes = (graphNeurons + 1) * 4 + edges * 4 + weights.size() * 2;
                gGraphs[key] = graph;
                graph_ = graph;
            }
        }
        return true;
    }

    uint64_t FlyBrainGpu::SharedMemoryBytes() const
    {
        return graph_ != nullptr ? graph_->bytes : 0;
    }

    void FlyBrainGpu::WaitIdle()
    {
        if (fence_->GetCompletedValue() < fenceValue_)
        {
            fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    void FlyBrainGpu::UploadTo(ID3D12Resource* destination, const void* data, uint64_t size)
    {
        // Once per brain: a temporary upload buffer and a wait
        ComPtr<ID3D12Resource> upload = CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD, false, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        upload->Map(0, nullptr, &mapped);
        std::memcpy(mapped, data, static_cast<size_t>(size));
        upload->Unmap(0, nullptr);
        ComPtr<ID3D12CommandAllocator> allocator;
        device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator));
        list_->Reset(allocator.Get(), nullptr);
        // Buffers in COMMON are promoted to COPY_DEST by the copy and decay back after the submission
        list_->CopyBufferRegion(destination, 0, upload.Get(), 0, size);
        list_->Close();
        ID3D12CommandList* lists[] = { list_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_.Get(), ++fenceValue_);
        WaitIdle();
    }

    void FlyBrainGpu::ReadBack(ID3D12Resource* source, void* data, uint64_t size)
    {
        ComPtr<ID3D12Resource> readback = CreateBuffer(size, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST);
        ComPtr<ID3D12CommandAllocator> allocator;
        device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator));
        list_->Reset(allocator.Get(), nullptr);
        list_->CopyBufferRegion(readback.Get(), 0, source, 0, size);
        list_->Close();
        ID3D12CommandList* lists[] = { list_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_.Get(), ++fenceValue_);
        WaitIdle();
        void* mapped = nullptr;
        const D3D12_RANGE range{ 0, static_cast<SIZE_T>(size) };
        readback->Map(0, &range, &mapped);
        std::memcpy(data, mapped, static_cast<size_t>(size));
        const D3D12_RANGE nothing{ 0, 0 };
        readback->Unmap(0, &nothing);
    }

    void FlyBrainGpu::Upload(const GpuStateImage& image)
    {
        Submit();
        Poll(true);
        const uint64_t n = neuronCount_;
        UploadTo(state_.Get(), image.state.data(), n * sizeof(GpuNeuronState));
        UploadTo(params_.Get(), image.params.data(), n * sizeof(GpuNeuronParams));
        UploadTo(pending_.Get(), image.pending.data(), n * 4);
        UploadTo(spikeTotals_.Get(), image.spikeTotals.data(), n * 4);
        UploadTo(arrivals_.Get(), image.arrivals.data(), n * delaySteps_ * 4);
        // The CPU does not keep the step of the last spike: the GPU starts with "none"
        const std::vector<uint32_t> never(n, 0xFFFFFFFFu);
        UploadTo(lastSpike_.Get(), never.data(), n * 4);
    }

    void FlyBrainGpu::Download(GpuStateImage& image)
    {
        Submit();
        Poll(true);
        const uint64_t n = neuronCount_;
        image.state.resize(n);
        image.params.resize(n);
        image.pending.resize(n);
        image.spikeTotals.resize(n);
        image.arrivals.resize(n * delaySteps_);
        ReadBack(state_.Get(), image.state.data(), n * sizeof(GpuNeuronState));
        ReadBack(params_.Get(), image.params.data(), n * sizeof(GpuNeuronParams));
        ReadBack(pending_.Get(), image.pending.data(), n * 4);
        ReadBack(spikeTotals_.Get(), image.spikeTotals.data(), n * 4);
        ReadBack(arrivals_.Get(), image.arrivals.data(), n * delaySteps_ * 4);
    }

    void FlyBrainGpu::DownloadRing(std::vector<uint32_t>& pairs, uint64_t& written)
    {
        Submit();
        Poll(true);
        pairs.resize(uint64_t(ringCapacity_) * 2);
        ReadBack(ring_.Get(), pairs.data(), uint64_t(ringCapacity_) * 8);
        uint32_t head = 0;
        ReadBack(counters_.Get(), &head, 4);
        written = head;
    }

    void FlyBrainGpu::SetPlasticity(const std::vector<uint32_t>& rowOffset, const std::vector<int32_t>& edgePlastic, const std::vector<int32_t>& weights)
    {
        Submit();
        Poll(true);
        plasticRowOffset_ = CreateBuffer(rowOffset.size() * 4, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
        edgePlastic_ = CreateBuffer(edgePlastic.size() * 4, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
        plasticWeights_ = CreateBuffer(weights.size() * 4, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        UploadTo(plasticRowOffset_.Get(), rowOffset.data(), rowOffset.size() * 4);
        UploadTo(edgePlastic_.Get(), edgePlastic.data(), edgePlastic.size() * 4);
        UploadTo(plasticWeights_.Get(), weights.data(), weights.size() * 4);
        memoryBytes_ += (rowOffset.size() + edgePlastic.size() + weights.size()) * 4;
        // Room for one change of every learned weight per submission
        for (auto& slot : slots_)
        {
            if (slot->plasticUpdates != nullptr)
            {
                slot->plasticUpdates->Unmap(0, nullptr);
            }
            slot->plasticCapacity = static_cast<uint32_t>(std::max<size_t>(weights.size(), 256));
            slot->plasticUpdates = CreateBuffer(uint64_t(slot->plasticCapacity) * 8, D3D12_HEAP_TYPE_UPLOAD, false, D3D12_RESOURCE_STATE_GENERIC_READ);
            const D3D12_RANGE nothing{ 0, 0 };
            slot->plasticUpdates->Map(0, &nothing, reinterpret_cast<void**>(&slot->plasticMapped));
        }
        ModelCb* cb = nullptr;
        model_->Map(0, nullptr, reinterpret_cast<void**>(&cb));
        cb->plastic = 1;
        model_->Unmap(0, nullptr);
        plastic_ = true;
    }

    void FlyBrainGpu::SetSilentOutputs(const std::vector<uint8_t>& silent)
    {
        Submit();
        Poll(true);
        const std::vector<uint32_t> words(silent.begin(), silent.end());
        UploadTo(silentOutput_.Get(), words.data(), words.size() * 4);
    }

    void FlyBrainGpu::SetTracked(const std::vector<uint32_t>& units)
    {
        // Consecutive units (the flies of one neuron) are one copy
        trackedRuns_.clear();
        trackedCount_ = static_cast<uint32_t>(std::min<size_t>(units.size(), kMaxTracked));
        for (uint32_t k = 0; k < trackedCount_; ++k)
        {
            if (!trackedRuns_.empty() && trackedRuns_.back().first + trackedRuns_.back().second == units[k])
            {
                ++trackedRuns_.back().second;
            }
            else
            {
                trackedRuns_.push_back({ units[k], 1u });
            }
        }
    }

    void FlyBrainGpu::QueueUpdate(const GpuNeuronUpdate& update)
    {
        queuedUpdates_.push_back(update);
    }

    void FlyBrainGpu::BindAll()
    {
        list_->SetComputeRootSignature(rootSignature_.Get());
        list_->SetComputeRootConstantBufferView(kRootModel, model_->GetGPUVirtualAddress());
        ID3D12Resource* srvs[] = { graph_->rowStart.Get(), graph_->targets.Get(), graph_->weights.Get(), powU_.Get(), powG_.Get(), open_->updates.Get(),
            plasticRowOffset_.Get(), edgePlastic_.Get(), open_->plasticUpdates.Get(), silentOutput_.Get() };
        for (UINT k = 0; k < kSrvCount; ++k)
        {
            list_->SetComputeRootShaderResourceView(kRootFirstSrv + k, srvs[k]->GetGPUVirtualAddress());
        }
        ID3D12Resource* uavs[] = { state_.Get(), params_.Get(), pending_.Get(), spikeTotals_.Get(), arrivals_.Get(), spikeList_.Get(),
            counters_.Get(), watchCounts_.Get(), ring_.Get(), active_.Get(), lastSpike_.Get(), plasticWeights_.Get() };
        for (UINT k = 0; k < kUavCount; ++k)
        {
            list_->SetComputeRootUnorderedAccessView(kRootFirstUav + k, uavs[k]->GetGPUVirtualAddress());
        }
    }

    bool FlyBrainGpu::BeginSubmission()
    {
        Slot& slot = *slots_[nextSlot_];
        if (slot.fenceValue != 0)
        {
            // The slot is still in flight or not read yet: hand its results over first
            Poll(true);
        }
        nextSlot_ = (nextSlot_ + 1) % kSlots;
        open_ = &slot;
        slot.allocator->Reset();
        list_->Reset(slot.allocator.Get(), resetPso_.Get());
        slot.updateCount = 0;
        slot.plasticCount = 0;
        slot.callEnd = false;
        slot.steps = 0;
        slot.windows = 0;
        slot.firstStep = 0;
        BindAll();

        // Clear the watch counts and the window counters of this submission (the ring head keeps counting)
        WindowConstants reset{};
        reset.groupCount = kMaxSteps * kWatchStride;
        reset.updateCount = 2 + 2 * kMaxWindows; // Counters[1..]
        list_->SetComputeRoot32BitConstants(kRootWindow, sizeof(reset) / 4, &reset, 0);
        list_->Dispatch((reset.groupCount + kThreads - 1) / kThreads, 1, 1);
        const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        list_->ResourceBarrier(1, &barrier);
        return true;
    }

    void FlyBrainGpu::RecordWindow(uint64_t begin, uint32_t steps)
    {
        if (open_ != nullptr && (open_->updateCount + queuedUpdates_.size() > open_->updateCapacity
            || open_->plasticCount + queuedPlastic_.size() > open_->plasticCapacity))
        {
            Submit();
        }
        if (open_ == nullptr)
        {
            BeginSubmission();
            open_->firstStep = begin;
        }
        Slot& slot = *open_;
        const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);

        WindowConstants window{};
        window.windowBegin = static_cast<uint32_t>(begin);
        window.steps = steps;
        window.firstSlot = static_cast<uint32_t>(begin % delaySteps_);
        window.windowIndex = slot.windows;
        window.stepOffset = slot.steps;

        if (!queuedUpdates_.empty())
        {
            // FlyBrain sends one update per neuron per window, and a slot holds one per neuron: it always fits
            const uint32_t count = static_cast<uint32_t>(queuedUpdates_.size());
            std::memcpy(slot.updatesMapped + slot.updateCount, queuedUpdates_.data(), count * sizeof(GpuNeuronUpdate));
            window.updateOffset = slot.updateCount;
            window.updateCount = count;
            list_->SetPipelineState(updatePso_.Get());
            list_->SetComputeRoot32BitConstants(kRootWindow, sizeof(window) / 4, &window, 0);
            list_->Dispatch((count + kThreads - 1) / kThreads, 1, 1);
            list_->ResourceBarrier(1, &barrier);
            slot.updateCount += count;
            queuedUpdates_.clear();
        }

        // Learned weights: every change of the learning steps since the last window (at most one per synapse and replica)
        if (!queuedPlastic_.empty())
        {
            const uint32_t count = static_cast<uint32_t>(queuedPlastic_.size());
            std::memcpy(slot.plasticMapped + slot.plasticCount, queuedPlastic_.data(), count * sizeof(queuedPlastic_[0]));
            window.updateOffset = slot.plasticCount;
            window.updateCount = count;
            list_->SetPipelineState(plasticPso_.Get());
            list_->SetComputeRoot32BitConstants(kRootWindow, sizeof(window) / 4, &window, 0);
            list_->Dispatch((count + kThreads - 1) / kThreads, 1, 1);
            list_->ResourceBarrier(1, &barrier);
            slot.plasticCount += count;
            queuedPlastic_.clear();
        }

        Slot& current = slot;
        window.updateOffset = 0;
        window.updateCount = 0;
        list_->SetPipelineState(compactPso_.Get());
        list_->SetComputeRoot32BitConstants(kRootWindow, sizeof(window) / 4, &window, 0);
        list_->Dispatch((neuronCount_ + kThreads - 1) / kThreads, 1, 1);
        list_->ResourceBarrier(1, &barrier);
        // As many threads as neurons; the ones beyond the active count of the window return at once
        list_->SetPipelineState(integratePso_.Get());
        list_->Dispatch((neuronCount_ + kThreads - 1) / kThreads, 1, 1);
        list_->ResourceBarrier(1, &barrier);

        window.groupCount = kDeliverGroups;
        list_->SetPipelineState(deliverPso_.Get());
        list_->SetComputeRoot32BitConstants(kRootWindow, sizeof(window) / 4, &window, 0);
        list_->Dispatch(kDeliverGroups, 1, 1);
        list_->ResourceBarrier(1, &barrier);

        current.windows += 1;
        current.steps += steps;
        if (current.windows == kMaxWindows || current.steps + delaySteps_ > kMaxSteps)
        {
            Submit();
        }
    }

    void FlyBrainGpu::Submit(bool callEnd)
    {
        if (open_ == nullptr)
        {
            return;
        }
        Slot& slot = *open_;
        slot.callEnd = callEnd;
        // The watch counts of the steps and the lost-spike counter go to the readback buffer of the slot
        const D3D12_RESOURCE_BARRIER toCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(watchCounts_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(spikeTotals_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        list_->ResourceBarrier(3, toCopy);
        uint64_t trackedOffset = kReadbackTracked;
        for (const auto& run : trackedRuns_)
        {
            list_->CopyBufferRegion(slot.readback.Get(), trackedOffset, spikeTotals_.Get(), uint64_t(run.first) * 4, uint64_t(run.second) * 4);
            trackedOffset += uint64_t(run.second) * 4;
        }
        const uint64_t countBytes = uint64_t(std::max(slot.steps, 1u)) * kWatchStride * 4;
        list_->CopyBufferRegion(slot.readback.Get(), 0, watchCounts_.Get(), 0, countBytes);
        list_->CopyBufferRegion(slot.readback.Get(), uint64_t(kMaxSteps) * kWatchStride * 4, counters_.Get(), 4, 4);
        list_->Close();
        ID3D12CommandList* lists[] = { list_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_.Get(), ++fenceValue_);
        slot.fenceValue = fenceValue_;
        submitted_.push_back(&slot);
        open_ = nullptr;
    }

    void FlyBrainGpu::Poll(bool wait)
    {
        while (!submitted_.empty())
        {
            Slot& slot = *submitted_.front();
            if (fence_->GetCompletedValue() < slot.fenceValue)
            {
                if (!wait)
                {
                    return;
                }
                fence_->SetEventOnCompletion(slot.fenceValue, fenceEvent_);
                WaitForSingleObject(fenceEvent_, INFINITE);
            }
            const uint64_t bytes = kReadbackTracked + uint64_t(trackedCount_) * 4;
            const D3D12_RANGE range{ 0, static_cast<SIZE_T>(bytes) };
            uint8_t* mapped = nullptr;
            slot.readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
            GpuCompletedSteps completed;
            completed.firstStep = slot.firstStep;
            completed.steps = slot.steps;
            completed.counts = reinterpret_cast<const uint32_t*>(mapped);
            std::memcpy(&completed.lostSpikes, mapped + uint64_t(kMaxSteps) * kWatchStride * 4, 4);
            completed.trackedTotals = trackedCount_ > 0 ? reinterpret_cast<const uint32_t*>(mapped + kReadbackTracked) : nullptr;
            completed.callEnd = slot.callEnd;
            if (onCompleted_ && slot.steps > 0)
            {
                onCompleted_(completed);
            }
            const D3D12_RANGE nothing{ 0, 0 };
            slot.readback->Unmap(0, &nothing);
            slot.fenceValue = 0;
            submitted_.pop_front();
        }
    }
}
