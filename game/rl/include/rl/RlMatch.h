// RlMatch.h
// A Rocket League match on top of RlSim: teams and slots, the kick-off, the goals, the clock and overtime, the boost pads, the
// respawn of demolished cars, and the bots. It steps at 120 Hz without a renderer. Blue defends the goal at -Z and attacks +Z,
// orange the other way round. The slots are fixed: blue 0..mode-1, orange mode..2*mode-1; the practice mode (mode 0) is one blue
// car and the ball, no clock and no opponents.
//
// All state outside Jolt is in RlWorldState::match, the bot memories and the random generator; SaveState / RestoreState carry
// all of it, so a rollback replays the match exactly.

#pragma once

#include <rl/RlRng.h>
#include <rl/RlSim.h>
#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace rl
{
    struct RlMatchConfig
    {
        int mode = 1;                 // players per team, 1..4; 0 = practice (one car, no opponents, no clock)
        uint64_t seed = 1;            // seed of the random numbers of the match
        uint32_t humanMask = 1;       // bit i set: slot i takes its input from Step(), the others are bots
        int botLevel = 1;             // 0 easy, 1 normal, 2 hard
    };

    // The places of the rules, in metres (RLBot field info, uu / 100).
    struct RlSpot
    {
        float x = 0.0f;
        float z = 0.0f;
        float yaw = 0.0f;  // radians from +Z towards +X, like RlSim::ResetCar
    };

    // Kick-off spot `order` (0..4: right corner, left corner, back right, back left, far back) of a team; blue stands on -Z.
    RlSpot RlKickoffSpot(int team, int order);
    // Where demolished cars of a team come back, `order` 0..3.
    RlSpot RlRespawnSpot(int team, int order);
    // Pad `index` (0..kPadCount-1) in the RLBot order; returns true for a big pad.
    bool RlPadSpot(int index, float& x, float& z);

    class RlMatch
    {
    public:
        RlMatch(const RlTuning& tuning, const RlMatchConfig& config);
        ~RlMatch();
        RlMatch(const RlMatch&) = delete;
        RlMatch& operator=(const RlMatch&) = delete;

        int SlotCount() const;
        int Team(int slot) const;  // 0 blue, 1 orange
        bool IsBot(int slot) const;
        // Bit i set: slot i takes its input from Step(), the others are bots. The mask is not part of SaveState: the netcode changes it at
        // an agreed tick (a player joins or leaves) and sets it again before each replayed tick of a rollback.
        uint32_t HumanMask() const;
        void SetHumanMask(uint32_t mask);
        const RlTuning& Tuning() const;
        void SetTuning(const RlTuning& tuning);  // see RlSim::SetTuning
        const RlSim& Sim() const;
        RlSim& MutableSim();       // the tests set up odd situations through it

        // One 120 Hz tick; `inputs` has SlotCount() entries (the ones of bot slots are ignored).
        void Step(const RlCarInput* inputs);

        const RlWorldState& World() const;
        const RlWorldState& PreviousWorld() const;  // before the last Step, for interpolation
        const RlEvents& LastEvents() const;

        void SaveState(std::vector<uint8_t>& out) const;
        bool RestoreState(const std::vector<uint8_t>& in);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    using RlMatchRing = RlStateRingT<RlMatch>;
}
