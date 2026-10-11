// RenderTargetPool.h

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <myengine/render/IRenderAdapter.h>

namespace myengine::render
{
    // Offscreen targets that are reused: thumbnails and viewers lease a target of a size, render into it and
    // return it, so the adapter does not create and destroy targets while the editor runs.
    // Main thread only. The adapter must outlive the pool; targets are created and destroyed between frames.
    class RenderTargetPool
    {
    public:
        RenderTargetPool(IRenderAdapter& adapter, std::size_t maxTargets);
        ~RenderTargetPool();

        RenderTargetPool(const RenderTargetPool&) = delete;
        RenderTargetPool& operator=(const RenderTargetPool&) = delete;

        // A free target of exactly this size, or a new one while the pool is under its limit. Invalid when the
        // limit is reached or the adapter cannot create the target. A new target is created on the spot, so lease
        // the sizes you need before a frame where you can.
        RenderTargetHandle Acquire(std::uint32_t width, std::uint32_t height);
        // Returns a leased target (its content is not cleared). Unknown handles are ignored.
        void Release(RenderTargetHandle target);
        // Destroys the free targets (the leased ones stay)
        void Trim();

        std::size_t Capacity() const { return maxTargets_; }
        std::size_t LeasedCount() const { return leasedCount_; }
        std::size_t FreeCount() const;
        std::size_t TotalCount() const { return targets_.size(); }

    private:
        struct Slot
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            bool leased = false;
        };

        IRenderAdapter& adapter_;
        std::size_t maxTargets_ = 0;
        std::size_t leasedCount_ = 0;
        std::unordered_map<std::uint32_t, Slot> targets_; // by handle value
    };
}
