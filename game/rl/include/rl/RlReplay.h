// RlReplay.h
// The replay of a goal: a ring of the last seconds of the match (the visual state of every tick, no Jolt), the player of a clip taken out of it
// (slow motion before the goal, three shots) and the cameras of the shots. Plain data, no renderer: the game draws the world states the player
// hands out as it draws the live ones, and puts the camera of the shot where its own camera would be. docs/game/rl-replay.md.
//
// What is recorded: the whole RlWorldState of every tick (cars, ball, the pads and the score; about 1.4 KB), so the replay can show exactly what
// happened and RlFeedback can find the same events in it (hits, jumps, the demolitions) as in a live match. 1200 ticks = 10 s = about 2.1 MB (under 2.25 MB); a
// record is one copy of the structure. A rollback of the netcode simulates a tick again: recording the same tick number drops the ticks after it
// and writes the new one, so the ring always holds the timeline the match has now.

#pragma once

#include <rl/RlCamera.h>
#include <rl/RlSim.h>
#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace rl
{
    // The numbers of the replay and of the cinematic cameras
    struct RlReplayConfig
    {
        float bufferSeconds = 10.0f;       // the ring
        float preGoalSeconds = 5.0f;       // the clip starts this long before the goal
        float postGoalSeconds = 0.9f;      // and ends this long after it (the ball in the net)
        float slowSeconds = 1.25f;         // the last seconds before the goal run in slow motion ...
        float slowSpeed = 0.5f;            // ... at this speed
        float rampSeconds = 0.25f;         // the change of the speed is smooth over this time
        float celebrationSeconds = 1.8f;   // the camera on the explosion of the goal, before the replay
        float flyoverSeconds = 2.4f;       // the flight over the stadium before the first kick-off (shorter than the countdown)
        float blendSeconds = 0.4f;         // a cinematic camera melts into the camera of the game over this time
        bool replayEnabled = true;
        bool flyoverEnabled = true;
    };

    // ---------------------------------------------------------------------------------------------------------------------------------

    class RlReplayBuffer
    {
    public:
        explicit RlReplayBuffer(std::size_t capacityTicks = 1200);

        // After every simulated tick. The same tick again (a rollback replays it) or an older one: the ticks from it on are dropped and it is
        // written; a tick that does not follow the last one (a gap) starts the ring over.
        void Record(const RlWorldState& world);
        void Clear();

        std::size_t Capacity() const { return slots_.size(); }
        std::size_t Size() const { return count_; }
        bool Empty() const { return count_ == 0; }
        uint32_t OldestTick() const { return newest_ + 1 - static_cast<uint32_t>(count_); }
        uint32_t NewestTick() const { return newest_; }
        // The world of a tick, or nullptr when the ring does not hold it
        const RlWorldState* Find(uint32_t tick) const;
        // The tick at which the phase turned to GoalScored last (searching back from the newest); false when the ring holds no goal
        bool FindLastGoal(uint32_t& goalTick) const;
        // Bytes of the ring
        std::size_t Bytes() const { return slots_.size() * sizeof(RlWorldState); }

    private:
        std::vector<RlWorldState> slots_;
        std::size_t count_ = 0;
        uint32_t newest_ = 0;
    };

    // ---------------------------------------------------------------------------------------------------------------------------------

    // A camera of the replay or of a cinematic shot: the view, and the horizontal field of view in degrees (like RlCameraTuning::fov)
    struct RlCinematicView
    {
        RlCameraView view;
        float fovDegrees = 90.0f;
    };

    enum class RlReplayShotKind : int
    {
        Ball = 0,     // behind the ball, a little above, looking at it
        Author = 1,   // the camera of the car that scored (the ball cam)
        Goal = 2,     // from the net, looking out at the ball coming
    };

    // What the player tells the HUD
    struct RlReplayInfo
    {
        bool active = false;
        int scorerSlot = -1;        // -1: nobody touched the ball
        int team = -1;              // the team the goal counts for
        bool ownGoal = false;
        float goalSpeedKmh = 0.0f;  // the speed of the ball at the goal
        float progress = 0.0f;      // 0..1 of the clip
        float speed = 1.0f;         // the playback speed now (1, 0.5)
        RlReplayShotKind shot = RlReplayShotKind::Ball;
    };

    // Plays a clip: a copy of the ticks before and after a goal, taken out of the buffer when it starts (the ring may be overwritten while it plays).
    class RlReplayPlayer
    {
    public:
        explicit RlReplayPlayer(const RlTuning& tuning, const RlReplayConfig& config = {});
        void SetTuning(const RlTuning& tuning) { tuning_ = tuning; }
        void SetConfig(const RlReplayConfig& config) { config_ = config; }

        // Takes the clip around `goalTick` out of the buffer. False when the buffer holds less than two seconds before the goal.
        bool Start(const RlReplayBuffer& buffer, uint32_t goalTick);
        void Skip();
        bool Playing() const { return playing_; }

        // Advances by `dt` real seconds (the clip runs at its own speed) and updates the camera. `sim` is used for the ray against the arena only.
        void Update(float dt, const RlSim& sim);

        // The two ticks round the playback position and the share between them: draw them as `before`, `now`, `alpha`
        const RlWorldState& Previous() const { return clip_[index_]; }
        const RlWorldState& Current() const { return clip_[std::min(index_ + 1, clip_.size() - 1)]; }
        float Alpha() const { return alpha_; }
        // The ticks the last Update has passed (indices of `Current()` for each, in order): RlFeedback reads the events of each
        const std::vector<std::pair<const RlWorldState*, const RlWorldState*>>& CrossedTicks() const { return crossed_; }

        const RlCinematicView& Camera() const { return camera_; }
        const RlReplayInfo& Info() const { return info_; }
        std::size_t ClipTicks() const { return clip_.size(); }
        // The length of the clip in real seconds at the speeds of the profile (the tests compare it with the time the playback takes)
        float DurationSeconds() const;

    private:
        float SpeedAt(float position) const;
        void UpdateCamera(float dt, const RlSim& sim);

        RlTuning tuning_;
        RlReplayConfig config_;
        std::vector<RlWorldState> clip_;
        std::size_t index_ = 0;
        float alpha_ = 0.0f;
        double position_ = 0.0;       // in ticks from the start of the clip
        std::size_t goalIndex_ = 0;   // the index of the goal tick in the clip
        std::size_t slowIndex_ = 0;   // where the slow motion begins
        std::size_t switchIndex_ = 0; // the cut from the first shot to the second
        bool playing_ = false;
        bool authorShot_ = false;
        RlCinematicView camera_;
        RlReplayInfo info_;
        std::vector<std::pair<const RlWorldState*, const RlWorldState*>> crossed_;

        // the cameras
        RlVec3 eye_{};
        RlVec3 shotHeading_{0.0f, 0.0f, 1.0f};
        bool eyeReady_ = false;
        RlReplayShotKind lastShot_ = RlReplayShotKind::Ball;
        RlCameraState carCamera_;
    };
}
