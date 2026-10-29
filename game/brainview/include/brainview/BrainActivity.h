// BrainActivity.h
// The numbers the 3D view and its HUD are drawn from (docs/flybrain/brain-view.md): when each neuron flashed last (one float per neuron, the GPU turns it into a flash)
// and how active each named group is (a rate for the bar, a coarse raster of the last two seconds). Both take time as an argument and know nothing of the GPU or
// of the simulation, so they are tested without either.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace brainview
{
    // The time of the last spike of every neuron on the clock of the view (seconds). float on the GPU: the numbers are kept relative to a base that moves forward
    // now and then, so that a view that runs for hours does not lose the milliseconds.
    class SpikeFlashes
    {
    public:
        static constexpr float kNever = -1.0e6f;

        void Reset(std::size_t neuronCount);
        // The neuron spiked at `time` (the clock of the view)
        void Mark(uint32_t neuron, double time);
        // Called every frame with the clock: moves the base when the numbers grow large
        void SetNow(double now);

        std::size_t Count() const { return times_.size(); }
        const float* Times() const { return times_.data(); }
        // `now` as the GPU gets it, relative to the base
        float Now() const { return static_cast<float>(now_ - base_); }
        double Base() const { return base_; }

    private:
        std::vector<float> times_;
        double base_ = 0.0;
        double now_ = 0.0;
    };

    // The activity of the groups: spike counts in bins of 10 ms for the last 2 s, and per group a grid (rows x bins) for the raster. A group of up to `kMaxRows`
    // neurons has a row of its own for each; a bigger one folds its neurons into kMaxRows rows (a heat map instead of dots).
    class GroupActivity
    {
    public:
        static constexpr int kBinCount = 200;          // 2 s
        static constexpr double kBinSeconds = 0.01;
        static constexpr uint32_t kMaxRows = 48;

        void Configure(const std::vector<uint32_t>& groupSizes);
        // A spike of the neuron `index` (its position in the group) of `group` at `time`
        void Record(std::size_t group, uint32_t index, double time);
        // Moves the window to `now`: the bins that fall out are cleared
        void Advance(double now);

        std::size_t GroupCount() const { return groups_.size(); }
        uint32_t GroupSize(std::size_t group) const { return groups_[group].size; }
        // Spikes per second per neuron over the last `windowSeconds` (at most 2)
        float RateHz(std::size_t group, double windowSeconds) const;
        uint32_t Rows(std::size_t group) const { return groups_[group].rows; }
        // The raster: rows x kBinCount counts, the newest bin last (oldest first); fills `out` with `Rows * kBinCount` values
        void Raster(std::size_t group, std::vector<uint16_t>& out) const;
        uint64_t TotalSpikes(std::size_t group) const { return groups_[group].total; }

    private:
        struct Group
        {
            uint32_t size = 0;
            uint32_t rows = 0;
            std::vector<uint16_t> counts;   // kBinCount
            std::vector<uint16_t> cells;    // rows * kBinCount, ring by bin
            uint64_t total = 0;
        };
        std::int64_t BinOf(double time) const;

        std::vector<Group> groups_;
        std::int64_t currentBin_ = 0;
        bool started_ = false;
    };
}
