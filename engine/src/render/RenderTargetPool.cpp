// RenderTargetPool.cpp

#include <myengine/render/RenderTargetPool.h>

namespace myengine::render
{
    RenderTargetPool::RenderTargetPool(IRenderAdapter& adapter, const std::size_t maxTargets)
        : adapter_(adapter), maxTargets_(maxTargets)
    {
    }

    RenderTargetPool::~RenderTargetPool()
    {
        for (const auto& [handleValue, slot] : targets_)
        {
            (void)slot;
            adapter_.DestroyRenderTarget(RenderTargetHandle{handleValue});
        }
    }

    RenderTargetHandle RenderTargetPool::Acquire(const std::uint32_t width, const std::uint32_t height)
    {
        for (auto& [handleValue, slot] : targets_)
        {
            if (!slot.leased && slot.width == width && slot.height == height)
            {
                slot.leased = true;
                ++leasedCount_;
                return RenderTargetHandle{handleValue};
            }
        }

        if (targets_.size() >= maxTargets_)
        {
            // A free target of another size makes room for this one
            for (auto it = targets_.begin(); it != targets_.end(); ++it)
            {
                if (!it->second.leased)
                {
                    adapter_.DestroyRenderTarget(RenderTargetHandle{it->first});
                    targets_.erase(it);
                    break;
                }
            }
            if (targets_.size() >= maxTargets_)
            {
                return {};
            }
        }

        const RenderTargetHandle created = adapter_.CreateRenderTarget(width, height);
        if (!created.IsValid())
        {
            return {};
        }
        targets_[created.value] = Slot{width, height, true};
        ++leasedCount_;
        return created;
    }

    void RenderTargetPool::Release(const RenderTargetHandle target)
    {
        const auto it = targets_.find(target.value);
        if (it == targets_.end() || !it->second.leased)
        {
            return;
        }
        it->second.leased = false;
        --leasedCount_;
    }

    void RenderTargetPool::Trim()
    {
        for (auto it = targets_.begin(); it != targets_.end();)
        {
            if (!it->second.leased)
            {
                adapter_.DestroyRenderTarget(RenderTargetHandle{it->first});
                it = targets_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::size_t RenderTargetPool::FreeCount() const
    {
        return targets_.size() - leasedCount_;
    }
}
