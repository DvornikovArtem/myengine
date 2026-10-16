// Dx12GpuProfiler.h: marker groups (the tree of a RenderDoc / PIX capture), per-pass counters and GPU timestamps.
// Internal to the DX12 adapter; the results are published as render::GpuFrameStats.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include <myengine/render/GpuStats.h>

namespace myengine::render::dx12
{
    class Dx12GpuProfiler
    {
    public:
        static constexpr UINT kMaxSlots = 4;
        static constexpr UINT kMaxQueriesPerFrame = 256; // two per pass: 128 passes a frame are timed, the rest only counted

        bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, UINT slotCount);
        void Shutdown();

        // The GPU work of `slot` has finished (the caller waited for its fence): its timings become the last frame
        void CollectSlot(UINT slot);

        // Opens the frame group "Frame" of `slot`; everything recorded until EndFrame is counted in `frame`
        void BeginFrame(ID3D12GraphicsCommandList* commandList, UINT slot, std::uint64_t frame);
        // Closes the open passes and the frame, resolves the timestamps of the slot
        void EndFrame(ID3D12GraphicsCommandList* commandList);

        // A group of commands: a marker for the capture tools and a timed pass of the statistics. Nests.
        void BeginPass(ID3D12GraphicsCommandList* commandList, const std::string& name, bool renderTarget = false,
            std::uint32_t width = 0, std::uint32_t height = 0);
        void EndPass(ID3D12GraphicsCommandList* commandList);
        std::size_t OpenPassDepth() const { return stack_.size(); }

        // Counters of what the renderer records; they go to the innermost open pass
        void NotePipeline(const void* pipelineState);
        void NoteTexture(UINT descriptorIndex);
        void NoteDraw(std::uint64_t triangles, std::uint64_t lines = 0);
        void NoteBarriers(std::uint32_t count);

        bool GetLastCompleted(GpuFrameStats& out) const;
        bool IsInFrame() const { return recording_; }

    private:
        struct PassRecord
        {
            GpuPassStats stats;
            int beginQuery = -1;
            int endQuery = -1;
        };

        struct SlotRecord
        {
            std::uint64_t frame = 0;
            std::vector<PassRecord> passes;
            UINT queriesUsed = 0;
            bool pending = false; // recorded and resolved, waiting for the GPU
        };

        PassRecord& Current();

        Microsoft::WRL::ComPtr<ID3D12QueryHeap> queryHeap_;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
        const std::uint64_t* mapped_ = nullptr;
        double ticksToMs_ = 0.0;
        UINT slotCount_ = 0;
        bool timingAvailable_ = false;

        std::array<SlotRecord, kMaxSlots> slots_{};
        UINT currentSlot_ = 0;
        bool recording_ = false;
        std::vector<std::size_t> stack_; // indices into the passes of the current slot

        const void* lastPipeline_ = nullptr;
        UINT lastTexture_ = 0xFFFFFFFFu;

        GpuFrameStats lastCompleted_;
        bool hasCompleted_ = false;
    };

    // A marker group in the command list (PIX / RenderDoc read the ANSI event format)
    void BeginGpuEvent(ID3D12GraphicsCommandList* commandList, const std::string& name);
    void EndGpuEvent(ID3D12GraphicsCommandList* commandList);
}
