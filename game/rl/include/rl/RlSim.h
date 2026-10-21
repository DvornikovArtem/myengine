// RlSim.h
// The Rocket League world: a Jolt physics system with the arena, one ball and up to eight cars, stepped at 120 Hz.
// All bodies live for the whole match; a demolished car is a state (a layer that collides with nothing, parked outside the
// arena), so SaveState / RestoreState can roll the world back. Single threaded: call it from one thread.

#pragma once

#include <rl/RlArena.h>
#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace rl
{
    class RlSim
    {
    public:
        RlSim(const RlTuning& tuning, int carCount);
        ~RlSim();
        RlSim(const RlSim&) = delete;
        RlSim& operator=(const RlSim&) = delete;

        int CarCount() const;
        const RlTuning& Tuning() const;
        const RlArena& Arena() const;

        // Puts a car upright on the floor at (x, z) looking `yaw` radians from +Z towards +X, at rest, with the starting boost.
        // This is also the point it comes back to after a demolition.
        void ResetCar(int index, float x, float z, float yaw);
        void ResetBall(RlVec3 position, RlVec3 velocity = {}, RlVec3 angularVelocity = {});
        void SetCarBoost(int index, float boost);
        // Where a demolished car comes back (ResetCar sets it to the spot the car was reset to).
        void SetRespawnPoint(int index, float x, float z, float yaw);
        // Puts a car at any pose (tests, kick-off spots, flip resets); the car keeps its boost and its jump state.
        void SetCarPose(int index, RlVec3 position, RlQuat rotation, RlVec3 velocity = {});

        // One 120 Hz tick; `inputs` has CarCount() entries.
        void Step(const RlCarInput* inputs);

        const RlWorldState& State() const;
        const RlWorldState& PreviousState() const;  // the state before the last Step, for interpolation
        const RlEvents& LastEvents() const;         // what the last Step did (ball hits, demolitions)

        // Snapshot of the whole simulation (Jolt and the car state around it) into a flat buffer; the vector keeps its capacity.
        void SaveState(std::vector<uint8_t>& out) const;
        bool RestoreState(const std::vector<uint8_t>& in);
        static uint64_t HashState(const std::vector<uint8_t>& snapshot);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // A ring of snapshots by tick for the rollback of G5; works for anything with SaveState(vector&) and RestoreState(vector).
    template <typename Simulation>
    class RlStateRingT
    {
    public:
        explicit RlStateRingT(size_t capacity = 128)
            : slots_(capacity)
        {
        }
        void Push(uint32_t tick, const Simulation& sim)
        {
            if (slots_.empty())
            {
                return;
            }
            Slot& slot = slots_[next_];
            sim.SaveState(slot.bytes);
            slot.tick = tick;
            slot.used = true;
            next_ = (next_ + 1) % slots_.size();
        }
        bool Restore(uint32_t tick, Simulation& sim) const
        {
            for (const Slot& slot : slots_)
            {
                if (slot.used && slot.tick == tick)
                {
                    return sim.RestoreState(slot.bytes);
                }
            }
            return false;
        }

    private:
        struct Slot
        {
            uint32_t tick = 0;
            bool used = false;
            std::vector<uint8_t> bytes;
        };
        std::vector<Slot> slots_;
        size_t next_ = 0;
    };

    using RlStateRing = RlStateRingT<RlSim>;
}
