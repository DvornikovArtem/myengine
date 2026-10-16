// Dx12GpuProfiler.cpp

// The vendored d3dx12.h (and with it the vendored d3d12.h) comes before anything that includes the system header
#include <directx-headers/directx/d3dx12.h>

#include "Dx12GpuProfiler.h"

#include <algorithm>

namespace myengine::render::dx12
{
    namespace
    {
        // The metadata value of ID3D12GraphicsCommandList::BeginEvent for an ANSI string (PIX_EVENT_ANSI_VERSION)
        constexpr UINT kAnsiEventVersion = 1;
    }

    void BeginGpuEvent(ID3D12GraphicsCommandList* commandList, const std::string& name)
    {
        if (commandList != nullptr)
        {
            commandList->BeginEvent(kAnsiEventVersion, name.c_str(), static_cast<UINT>(name.size() + 1));
        }
    }

    void EndGpuEvent(ID3D12GraphicsCommandList* commandList)
    {
        if (commandList != nullptr)
        {
            commandList->EndEvent();
        }
    }

    bool Dx12GpuProfiler::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, const UINT slotCount)
    {
        slotCount_ = std::min(slotCount, kMaxSlots);
        timingAvailable_ = false;
        if (device == nullptr || queue == nullptr || slotCount_ == 0)
        {
            return false;
        }

        UINT64 frequency = 0;
        if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0)
        {
            return false;
        }
        ticksToMs_ = 1000.0 / static_cast<double>(frequency);

        D3D12_QUERY_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heapDesc.Count = kMaxQueriesPerFrame * slotCount_;
        if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(queryHeap_.GetAddressOf()))))
        {
            return false;
        }

        const UINT64 bytes = static_cast<UINT64>(heapDesc.Count) * sizeof(std::uint64_t);
        const CD3DX12_HEAP_PROPERTIES readbackHeap(D3D12_HEAP_TYPE_READBACK);
        const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        if (FAILED(device->CreateCommittedResource(
                &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(readback_.GetAddressOf()))))
        {
            queryHeap_.Reset();
            return false;
        }

        void* mapped = nullptr;
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(bytes)};
        if (FAILED(readback_->Map(0, &readRange, &mapped)))
        {
            readback_.Reset();
            queryHeap_.Reset();
            return false;
        }
        mapped_ = static_cast<const std::uint64_t*>(mapped);
        timingAvailable_ = true;
        return true;
    }

    void Dx12GpuProfiler::Shutdown()
    {
        if (readback_ != nullptr)
        {
            readback_->Unmap(0, nullptr);
        }
        mapped_ = nullptr;
        readback_.Reset();
        queryHeap_.Reset();
        timingAvailable_ = false;
        recording_ = false;
        stack_.clear();
    }

    Dx12GpuProfiler::PassRecord& Dx12GpuProfiler::Current()
    {
        SlotRecord& slot = slots_[currentSlot_];
        return slot.passes[stack_.empty() ? 0 : stack_.back()];
    }

    void Dx12GpuProfiler::CollectSlot(const UINT slotIndex)
    {
        if (slotIndex >= kMaxSlots)
        {
            return;
        }
        SlotRecord& slot = slots_[slotIndex];
        if (!slot.pending)
        {
            return;
        }
        slot.pending = false;

        GpuFrameStats stats;
        stats.frame = slot.frame;
        for (const PassRecord& record : slot.passes)
        {
            GpuPassStats pass = record.stats;
            if (timingAvailable_ && mapped_ != nullptr && record.beginQuery >= 0 && record.endQuery >= 0)
            {
                const std::uint64_t* base = mapped_ + static_cast<std::size_t>(slotIndex) * kMaxQueriesPerFrame;
                const std::uint64_t begin = base[record.beginQuery];
                const std::uint64_t end = base[record.endQuery];
                if (end >= begin)
                {
                    pass.gpuMs = static_cast<double>(end - begin) * ticksToMs_;
                }
            }

            stats.draws += pass.draws;
            stats.triangles += pass.triangles;
            stats.psoSwitches += pass.psoSwitches;
            stats.textureSwitches += pass.textureSwitches;
            stats.barriers += pass.barriers;
            if (pass.renderTarget)
            {
                ++stats.renderTargetPasses;
            }
            if (pass.depth == 0)
            {
                stats.gpuMs = pass.gpuMs;
            }
            stats.passes.push_back(std::move(pass));
        }

        if (!hasCompleted_ || stats.frame >= lastCompleted_.frame)
        {
            lastCompleted_ = std::move(stats);
            hasCompleted_ = true;
        }
    }

    void Dx12GpuProfiler::BeginFrame(ID3D12GraphicsCommandList* commandList, const UINT slot, const std::uint64_t frame)
    {
        currentSlot_ = std::min(slot, kMaxSlots - 1);
        SlotRecord& record = slots_[currentSlot_];
        record = SlotRecord{};
        record.frame = frame;
        stack_.clear();
        lastPipeline_ = nullptr;
        lastTexture_ = 0xFFFFFFFFu;
        recording_ = true;

        PassRecord root;
        root.stats.name = "Frame";
        root.stats.depth = 0;
        record.passes.push_back(std::move(root));
        stack_.push_back(0);

        BeginGpuEvent(commandList, "Frame");
        if (timingAvailable_ && commandList != nullptr)
        {
            const UINT index = currentSlot_ * kMaxQueriesPerFrame + record.queriesUsed;
            commandList->EndQuery(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
            record.passes[0].beginQuery = static_cast<int>(record.queriesUsed);
            ++record.queriesUsed;
        }
    }

    void Dx12GpuProfiler::BeginPass(
        ID3D12GraphicsCommandList* commandList,
        const std::string& name,
        const bool renderTarget,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!recording_)
        {
            return;
        }

        SlotRecord& slot = slots_[currentSlot_];
        PassRecord pass;
        pass.stats.name = name;
        pass.stats.depth = static_cast<int>(stack_.size());
        pass.stats.renderTarget = renderTarget;
        pass.stats.width = width;
        pass.stats.height = height;
        slot.passes.push_back(std::move(pass));
        stack_.push_back(slot.passes.size() - 1);

        BeginGpuEvent(commandList, name);
        PassRecord& added = slot.passes.back();
        if (timingAvailable_ && commandList != nullptr && slot.queriesUsed + 2 <= kMaxQueriesPerFrame)
        {
            commandList->EndQuery(
                queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, currentSlot_ * kMaxQueriesPerFrame + slot.queriesUsed);
            added.beginQuery = static_cast<int>(slot.queriesUsed);
            ++slot.queriesUsed;
        }
    }

    void Dx12GpuProfiler::EndPass(ID3D12GraphicsCommandList* commandList)
    {
        if (!recording_ || stack_.size() <= 1)
        {
            return; // the frame itself is closed by EndFrame
        }

        SlotRecord& slot = slots_[currentSlot_];
        PassRecord& pass = slot.passes[stack_.back()];
        if (timingAvailable_ && commandList != nullptr && pass.beginQuery >= 0 && slot.queriesUsed < kMaxQueriesPerFrame)
        {
            commandList->EndQuery(
                queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, currentSlot_ * kMaxQueriesPerFrame + slot.queriesUsed);
            pass.endQuery = static_cast<int>(slot.queriesUsed);
            ++slot.queriesUsed;
        }
        stack_.pop_back();
        EndGpuEvent(commandList);
    }

    void Dx12GpuProfiler::EndFrame(ID3D12GraphicsCommandList* commandList)
    {
        if (!recording_)
        {
            return;
        }

        while (stack_.size() > 1)
        {
            EndPass(commandList);
        }

        SlotRecord& slot = slots_[currentSlot_];
        PassRecord& root = slot.passes[0];
        if (timingAvailable_ && commandList != nullptr && root.beginQuery >= 0 && slot.queriesUsed < kMaxQueriesPerFrame)
        {
            commandList->EndQuery(
                queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, currentSlot_ * kMaxQueriesPerFrame + slot.queriesUsed);
            root.endQuery = static_cast<int>(slot.queriesUsed);
            ++slot.queriesUsed;
        }
        EndGpuEvent(commandList);

        if (timingAvailable_ && commandList != nullptr && slot.queriesUsed > 0)
        {
            commandList->ResolveQueryData(
                queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, currentSlot_ * kMaxQueriesPerFrame, slot.queriesUsed,
                readback_.Get(), static_cast<UINT64>(currentSlot_) * kMaxQueriesPerFrame * sizeof(std::uint64_t));
        }

        slot.pending = true;
        stack_.clear();
        recording_ = false;
    }

    void Dx12GpuProfiler::NotePipeline(const void* pipelineState)
    {
        if (!recording_ || pipelineState == lastPipeline_)
        {
            return;
        }
        lastPipeline_ = pipelineState;
        ++Current().stats.psoSwitches;
    }

    void Dx12GpuProfiler::NoteTexture(const UINT descriptorIndex)
    {
        if (!recording_ || descriptorIndex == lastTexture_)
        {
            return;
        }
        lastTexture_ = descriptorIndex;
        ++Current().stats.textureSwitches;
    }

    void Dx12GpuProfiler::NoteDraw(const std::uint64_t triangles, const std::uint64_t lines)
    {
        if (!recording_)
        {
            return;
        }
        PassRecord& pass = Current();
        ++pass.stats.draws;
        pass.stats.triangles += triangles;
        pass.stats.lines += lines;
    }

    void Dx12GpuProfiler::NoteBarriers(const std::uint32_t count)
    {
        if (recording_)
        {
            Current().stats.barriers += count;
        }
    }

    bool Dx12GpuProfiler::GetLastCompleted(GpuFrameStats& out) const
    {
        if (!hasCompleted_)
        {
            return false;
        }
        out = lastCompleted_;
        return true;
    }
}
