// RlCinematic.h
// The cameras that are not the camera of the car (the flight over the stadium before the first kick-off, the camera on the explosion of a goal,
// the camera of the results) and the director that decides every frame what the player sees: the match as it is, a cinematic shot, or the
// replay of the goal (RlReplay.h). Plain data, no renderer, no input device: the game gives it the world, the skip button and the camera of the
// game, and gets back what to draw and where to put the camera. docs/game/rl-replay.md has the exact way to hook it into RlGameSystem.
//
//     RlDirector director(tuning);                         // one for the game
//     director.Reset();                                    // a new match
//     ... after every simulated tick:   director.OnTick(match->World());
//     ... every frame:                  const RlDirectorFrame shot = director.Frame(dt, skip, match->Sim(), match->PreviousWorld(), match->World(), alpha, &gameCamera);
//         shot.cinematicCamera ? use shot.camera : use the camera of the game;  draw shot.previous / shot.current / shot.alpha;  if (shot.freezeLive) do not step the match

#pragma once

#include <rl/RlReplay.h>

namespace rl
{
    // A camera that looks at `target` from `position` (the world up is Y), with a horizontal field of view in degrees
    RlCinematicView RlLookAt(const RlVec3& position, const RlVec3& target, float fovDegrees);
    // Mixes two cameras (the position and the axes are interpolated, the axes stay orthonormal): t = 0 is `a`, t = 1 is `b`
    RlCinematicView RlBlendViews(const RlCinematicView& a, const RlCinematicView& b, float t);

    // The flight over the stadium before the first kick-off: t is 0..1. It starts high behind the orange goal, swings round the arena and ends
    // low behind the blue kick-off spots, looking up the field (where the camera of the game starts).
    RlCinematicView RlFlyoverView(const RlTuning& tuning, float t);

    // The camera on the explosion of a goal: in front of the goal that was hit, looking at it, slowly rising and coming closer. `side` is +1 for the
    // goal at +Z (a goal of the blue team), -1 for the other; `x` the place along the goal line; t is the time since the goal in seconds.
    RlCinematicView RlGoalCelebrationView(const RlTuning& tuning, int side, float x, float t, float duration);

    // The camera of the results: orbits the cars of the winning team (t in seconds)
    RlCinematicView RlResultsView(const RlTuning& tuning, const RlWorldState& world, int winnerTeam, float t);

    // ---------------------------------------------------------------------------------------------------------------------------------

    enum class RlShot : int
    {
        Gameplay = 0,    // the camera and the world of the game
        Flyover,         // before the first kick-off
        Celebration,     // the explosion of a goal
        Replay,          // the replay of the goal
        Results,         // the match is over
    };

    struct RlDirectorFrame
    {
        RlShot shot = RlShot::Gameplay;
        bool cinematicCamera = false;       // true: put the camera at `camera`; false: the camera of the game
        RlCinematicView camera;
        // What to draw: the live worlds while the match is shown, the worlds of the clip in the replay (valid until the next call)
        const RlWorldState* previous = nullptr;
        const RlWorldState* current = nullptr;
        float alpha = 0.0f;
        // Offline: while the replay plays the match must not be stepped (it goes on after the replay, from where it was). Online it is never set:
        // the replay is cut when the kick-off starts.
        bool freezeLive = false;
        bool replayStarted = false;         // this frame: RlFeedback::BeginReplay
        bool replayEnded = false;           // this frame: RlFeedback::EndReplay
        RlReplayInfo replay;                // for the REPLAY label of the HUD
        // The ticks the replay passed in this frame: RlFeedback::OnTick for each (the events of the replay)
        const std::vector<std::pair<const RlWorldState*, const RlWorldState*>>* replayTicks = nullptr;
    };

    class RlDirector
    {
    public:
        explicit RlDirector(const RlTuning& tuning, const RlReplayConfig& config = {});
        void SetTuning(const RlTuning& tuning);
        void SetConfig(const RlReplayConfig& config);
        // The match can be frozen (a game against bots on this machine): the replay then plays in full and the match waits. Default true.
        void SetCanFreeze(bool canFreeze) { canFreeze_ = canFreeze; }

        // A new match: the ring is cleared, the flight over the stadium is ready (and the first countdown starts under it)
        void Reset();

        // After EVERY simulated tick (also a tick that a rollback simulates again): records it and notices the goal and the end of the match
        void OnTick(const RlWorldState& world);

        // Once per frame. `skip` is the press of the skip button of this frame. `gameplayCamera` is the camera of the game this frame (the cinematic cameras melt
        // into it at their end); nullptr when the game has none.
        RlDirectorFrame Frame(float dt, bool skip, const RlSim& sim, const RlWorldState& livePrevious, const RlWorldState& liveCurrent, float liveAlpha,
                              const RlCameraView* gameplayCamera);

        RlShot Shot() const { return shot_; }
        const RlReplayBuffer& Buffer() const { return buffer_; }
        const RlReplayPlayer& Player() const { return player_; }

    private:
        void EnterGameplay(RlDirectorFrame& out);

        RlTuning tuning_;
        RlReplayConfig config_;
        RlReplayBuffer buffer_;
        RlReplayPlayer player_;
        bool canFreeze_ = true;
        RlShot shot_ = RlShot::Gameplay;
        float shotTime_ = 0.0f;
        bool pendingGoal_ = false;
        uint32_t goalTick_ = 0;
        int goalTeam_ = 0;
        float goalX_ = 0.0f;
        bool finished_ = false;
        int winner_ = 0;
        RlCinematicView lastCamera_;
        float blendLeft_ = 0.0f;
        RlCinematicView blendFrom_;
        uint32_t handledGoalTick_ = 0;   // the goal that was shown last: a rollback finds it again a few ticks off
        bool hasHandledGoal_ = false;
    };
}
