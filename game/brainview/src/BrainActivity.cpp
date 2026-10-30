#include <brainview/BrainActivity.h>

#include <algorithm>
#include <cmath>

namespace brainview
{
    void SpikeFlashes::Reset(const std::size_t neuronCount)
    {
        times_.assign(neuronCount, kNever);
        base_ = 0.0;
        now_ = 0.0;
    }

    void SpikeFlashes::Mark(const uint32_t neuron, const double time)
    {
        if (neuron < times_.size())
        {
            times_[neuron] = static_cast<float>(time - base_);
        }
    }

    void SpikeFlashes::SetNow(const double now)
    {
        now_ = now;
        // after 3000 s the numbers move forward by 2400 s (a float keeps about 7 digits: 3000 s are still a tenth of a millisecond)
        if (now_ - base_ > 3000.0)
        {
            const double shift = 2400.0;
            for (float& value : times_)
            {
                if (value > kNever * 0.5f)
                {
                    value -= static_cast<float>(shift);
                    if (value < kNever * 0.5f)
                    {
                        value = kNever;
                    }
                }
            }
            base_ += shift;
        }
    }

    void GroupActivity::Configure(const std::vector<uint32_t>& groupSizes)
    {
        groups_.assign(groupSizes.size(), Group{});
        for (std::size_t g = 0; g < groupSizes.size(); ++g)
        {
            Group& group = groups_[g];
            group.size = groupSizes[g];
            group.rows = std::max<uint32_t>(1, std::min(groupSizes[g], kMaxRows));
            group.counts.assign(kBinCount, 0);
            group.cells.assign(static_cast<std::size_t>(group.rows) * kBinCount, 0);
        }
        currentBin_ = 0;
        started_ = false;
    }

    std::int64_t GroupActivity::BinOf(const double time) const
    {
        return static_cast<std::int64_t>(std::floor(time / kBinSeconds));
    }

    void GroupActivity::Record(const std::size_t group, const uint32_t index, const double time)
    {
        if (group >= groups_.size())
        {
            return;
        }
        const std::int64_t bin = BinOf(time);
        if (!started_ || bin > currentBin_ || bin <= currentBin_ - kBinCount)
        {
            return;   // a spike of the future or older than the window: the window moves with Advance
        }
        Group& g = groups_[group];
        const std::size_t slot = static_cast<std::size_t>(((bin % kBinCount) + kBinCount) % kBinCount);
        const uint32_t row = g.size <= kMaxRows ? index : static_cast<uint32_t>(static_cast<uint64_t>(index) * g.rows / g.size);
        if (g.counts[slot] < 65535)
        {
            ++g.counts[slot];
        }
        uint16_t& cell = g.cells[static_cast<std::size_t>(std::min(row, g.rows - 1)) * kBinCount + slot];
        if (cell < 65535)
        {
            ++cell;
        }
        ++g.total;
    }

    void GroupActivity::RecordCount(const std::size_t group, const double time, const uint32_t count)
    {
        if (group >= groups_.size() || count == 0)
        {
            return;
        }
        const std::int64_t bin = BinOf(time);
        if (!started_ || bin > currentBin_ || bin <= currentBin_ - kBinCount)
        {
            return;
        }
        Group& g = groups_[group];
        const std::size_t slot = static_cast<std::size_t>(((bin % kBinCount) + kBinCount) % kBinCount);
        const auto add = [](uint16_t& cell, const uint32_t amount)
        {
            cell = static_cast<uint16_t>(std::min<uint32_t>(65535u, static_cast<uint32_t>(cell) + amount));
        };
        add(g.counts[slot], count);
        const uint32_t each = count / g.rows;
        const uint32_t rest = count % g.rows;
        for (uint32_t row = 0; row < g.rows; ++row)
        {
            add(g.cells[static_cast<std::size_t>(row) * kBinCount + slot], each + (row < rest ? 1u : 0u));
        }
        g.total += count;
    }

    void GroupActivity::Advance(const double now)
    {
        const std::int64_t bin = BinOf(now);
        if (!started_)
        {
            started_ = true;
            currentBin_ = bin;
            return;
        }
        if (bin <= currentBin_)
        {
            return;
        }
        const std::int64_t steps = std::min<std::int64_t>(bin - currentBin_, kBinCount);
        for (std::int64_t k = 1; k <= steps; ++k)
        {
            const std::size_t slot = static_cast<std::size_t>((((currentBin_ + k) % kBinCount) + kBinCount) % kBinCount);
            for (Group& g : groups_)
            {
                g.counts[slot] = 0;
                for (uint32_t row = 0; row < g.rows; ++row)
                {
                    g.cells[static_cast<std::size_t>(row) * kBinCount + slot] = 0;
                }
            }
        }
        currentBin_ = bin;
    }

    float GroupActivity::RateHz(const std::size_t group, const double windowSeconds) const
    {
        if (group >= groups_.size() || groups_[group].size == 0 || !started_)
        {
            return 0.0f;
        }
        const Group& g = groups_[group];
        const int bins = std::clamp(static_cast<int>(std::lround(windowSeconds / kBinSeconds)), 1, kBinCount - 1);
        // the current bin is not finished: it is left out, so the rate does not dip at the start of a bin
        uint64_t sum = 0;
        for (int k = 1; k <= bins; ++k)
        {
            const std::size_t slot = static_cast<std::size_t>((((currentBin_ - k) % kBinCount) + kBinCount) % kBinCount);
            sum += g.counts[slot];
        }
        return static_cast<float>(static_cast<double>(sum) / (static_cast<double>(bins) * kBinSeconds * static_cast<double>(g.size)));
    }

    void GroupActivity::Raster(const std::size_t group, std::vector<uint16_t>& out) const
    {
        out.clear();
        if (group >= groups_.size() || !started_)
        {
            return;
        }
        const Group& g = groups_[group];
        out.assign(static_cast<std::size_t>(g.rows) * kBinCount, 0);
        for (uint32_t row = 0; row < g.rows; ++row)
        {
            for (int k = 0; k < kBinCount; ++k)
            {
                // k = 0 is the oldest bin that is still in the window, the last one the current bin
                const std::int64_t bin = currentBin_ - (kBinCount - 1) + k;
                const std::size_t slot = static_cast<std::size_t>(((bin % kBinCount) + kBinCount) % kBinCount);
                out[static_cast<std::size_t>(row) * kBinCount + static_cast<std::size_t>(k)] = g.cells[static_cast<std::size_t>(row) * kBinCount + slot];
            }
        }
    }
}
