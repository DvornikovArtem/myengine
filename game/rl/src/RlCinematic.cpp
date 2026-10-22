// RlCinematic.cpp

#include <rl/RlCinematic.h>

#include "RlV3.h"

#include <algorithm>
#include <cmath>

namespace rl
{
    using namespace detail;

    RlCinematicView RlLookAt(const RlVec3& position, const RlVec3& target, const float fovDegrees)
    {
        const V3 eye = FromRl(position);
        const V3 forward = Norm(FromRl(target) - eye, {0.0f, 0.0f, 1.0f});
        V3 right = Cross({0.0f, 1.0f, 0.0f}, forward);
        right = Norm(right, {1.0f, 0.0f, 0.0f});
        const V3 up = Cross(forward, right);
        RlCinematicView result;
        result.view = {position, ToRl(forward), ToRl(up), ToRl(right)};
        result.fovDegrees = fovDegrees;
        return result;
    }

    RlCinematicView RlBlendViews(const RlCinematicView& a, const RlCinematicView& b, const float t)
    {
        const float k = std::clamp(t, 0.0f, 1.0f);
        const V3 position = Lerp(FromRl(a.view.position), FromRl(b.view.position), k);
        const V3 forward = Norm(Lerp(FromRl(a.view.forward), FromRl(b.view.forward), k), FromRl(b.view.forward));
        V3 up = Norm(Lerp(FromRl(a.view.up), FromRl(b.view.up), k), {0.0f, 1.0f, 0.0f});
        const V3 right = Norm(Cross(up, forward), FromRl(b.view.right));
        up = Cross(forward, right);
        RlCinematicView result;
        result.view = {ToRl(position), ToRl(forward), ToRl(up), ToRl(right)};
        result.fovDegrees = a.fovDegrees + (b.fovDegrees - a.fovDegrees) * k;
        return result;
    }

    RlCinematicView RlFlyoverView(const RlTuning& tuning, const float t)
    {
        const float k = Smooth(t);
        const float rx = tuning.arena.halfWidth * 0.7f;
        const float rz = tuning.arena.halfLength * 0.75f;
        // round the arena from behind the orange goal (z > 0) through the right side to behind the blue kick-off spots
        const float angle = 1.5707963f - k * 3.1415927f;
        const V3 eye{rx * std::cos(angle), 17.0f + (6.5f - 17.0f) * k, rz * std::sin(angle)};
        const V3 centre{0.0f, 2.0f, 0.0f};
        const V3 kickoffLook{0.0f, 3.0f, eye.z + 30.0f};
        const V3 target = Lerp(centre, kickoffLook, Smooth(k * k));
        return RlLookAt(ToRl(eye), ToRl(target), 80.0f + (tuning.camera.fov - 80.0f) * k);
    }

    RlCinematicView RlGoalCelebrationView(const RlTuning& tuning, const int side, const float x, const float t, const float duration)
    {
        const float s = side >= 0 ? 1.0f : -1.0f;
        const float k = std::clamp(t / std::max(duration, 0.1f), 0.0f, 1.0f);
        const V3 goal{std::clamp(x, -tuning.arena.goalHalfWidth, tuning.arena.goalHalfWidth), 2.0f, s * tuning.arena.halfLength};
        // in front of the goal, coming closer and rising a little, drifting sideways
        const float back = 22.0f + (15.0f - 22.0f) * k;
        const V3 eye{goal.x * 0.3f + std::sin(t * 0.6f) * 3.0f, 4.0f + 3.5f * k, goal.z - s * back};
        return RlLookAt(ToRl(eye), ToRl(goal + V3{0.0f, 1.5f, 0.0f}), 70.0f - 5.0f * k);
    }

    RlCinematicView RlResultsView(const RlTuning& tuning, const RlWorldState& world, const int winnerTeam, const float t)
    {
        V3 sum{};
        int count = 0;
        const int mode = world.match.mode;
        for (int i = 0; i < std::min(world.carCount, kMaxCars); ++i)
        {
            const int team = mode <= 0 ? 0 : (i < mode ? 0 : 1);
            if (team == winnerTeam)
            {
                sum = sum + FromRl(world.cars[i].position);
                ++count;
            }
        }
        const V3 centre = count > 0 ? sum * (1.0f / static_cast<float>(count)) : V3{0.0f, 0.5f, 0.0f};
        const float angle = 0.7f + t * 0.35f;
        V3 eye = centre + V3{std::cos(angle) * 9.0f, 3.2f, std::sin(angle) * 9.0f};
        eye.x = std::clamp(eye.x, -tuning.arena.halfWidth + 1.0f, tuning.arena.halfWidth - 1.0f);
        eye.y = std::clamp(eye.y, 1.0f, tuning.arena.height - 1.0f);
        eye.z = std::clamp(eye.z, -tuning.arena.halfLength + 1.0f, tuning.arena.halfLength + 4.0f);
        return RlLookAt(ToRl(eye), ToRl(centre + V3{0.0f, 0.7f, 0.0f}), 55.0f);
    }

    // ---------------------------------------------------------------- the director

    RlDirector::RlDirector(const RlTuning& tuning, const RlReplayConfig& config)
        : tuning_(tuning),
          config_(config),
          buffer_(static_cast<std::size_t>(std::max(config.bufferSeconds, 2.0f) * static_cast<float>(kTickRate))),
          player_(tuning, config)
    {
    }

    void RlDirector::SetTuning(const RlTuning& tuning)
    {
        tuning_ = tuning;
        player_.SetTuning(tuning);
    }

    void RlDirector::SetConfig(const RlReplayConfig& config)
    {
        config_ = config;
        player_.SetConfig(config);
    }

    void RlDirector::Reset()
    {
        buffer_.Clear();
        player_.Skip();
        shot_ = config_.flyoverEnabled ? RlShot::Flyover : RlShot::Gameplay;
        shotTime_ = 0.0f;
        pendingGoal_ = false;
        finished_ = false;
        blendLeft_ = 0.0f;
        hasHandledGoal_ = false;
    }

    void RlDirector::OnTick(const RlWorldState& world)
    {
        const RlWorldState* before = buffer_.Find(world.tick - 1);
        buffer_.Record(world);
        if (before != nullptr && before->match.phase != RlMatchPhase::GoalScored && world.match.phase == RlMatchPhase::GoalScored)
        {
            // a goal; the same goal found again after a rollback (a few ticks off) is not another one
            const bool same = hasHandledGoal_ && (world.tick + 240 > handledGoalTick_ && world.tick < handledGoalTick_ + 240);
            if (!same)
            {
                pendingGoal_ = true;
                goalTick_ = world.tick;
                goalTeam_ = world.match.lastGoalTeam;
                goalX_ = world.ball.position.x;
                handledGoalTick_ = world.tick;
                hasHandledGoal_ = true;
            }
        }
        if (world.match.phase == RlMatchPhase::Finished)
        {
            finished_ = true;
            winner_ = world.match.winner < 0 ? 0 : world.match.winner;
        }
    }

    void RlDirector::EnterGameplay(RlDirectorFrame& out)
    {
        (void)out;
        shot_ = RlShot::Gameplay;
        shotTime_ = 0.0f;
        blendLeft_ = config_.blendSeconds;
        blendFrom_ = lastCamera_;
    }

    RlDirectorFrame RlDirector::Frame(const float dt, const bool skip, const RlSim& sim, const RlWorldState& livePrevious, const RlWorldState& liveCurrent,
                                      const float liveAlpha, const RlCameraView* gameplayCamera)
    {
        RlDirectorFrame out;
        out.previous = &livePrevious;
        out.current = &liveCurrent;
        out.alpha = liveAlpha;
        shotTime_ += dt;

        // a goal that the ticks have found: the camera goes to its explosion (not while a replay is on: that match is frozen anyway)
        if (pendingGoal_)
        {
            pendingGoal_ = false;
            if (shot_ != RlShot::Replay)
            {
                shot_ = RlShot::Celebration;
                shotTime_ = 0.0f;
            }
        }

        switch (shot_)
        {
        case RlShot::Flyover:
            if (skip || shotTime_ >= config_.flyoverSeconds || liveCurrent.match.phase != RlMatchPhase::Countdown)
            {
                EnterGameplay(out);
                break;
            }
            out.cinematicCamera = true;
            out.camera = RlFlyoverView(tuning_, shotTime_ / std::max(config_.flyoverSeconds, 0.1f));
            break;

        case RlShot::Celebration:
            if (skip)
            {
                EnterGameplay(out);
                break;
            }
            if (shotTime_ >= config_.celebrationSeconds)
            {
                if (config_.replayEnabled && player_.Start(buffer_, goalTick_))
                {
                    shot_ = RlShot::Replay;
                    shotTime_ = 0.0f;
                    out.replayStarted = true;
                }
                else
                {
                    EnterGameplay(out);
                }
                if (shot_ != RlShot::Replay)
                {
                    break;
                }
                // the replay starts in this very frame (falls through to its code below)
            }
            else
            {
                out.cinematicCamera = true;
                out.camera = RlGoalCelebrationView(tuning_, goalTeam_ == 0 ? 1 : -1, goalX_, shotTime_, config_.celebrationSeconds);
                break;
            }
            [[fallthrough]];

        case RlShot::Replay:
            if (skip)
            {
                player_.Skip();
            }
            else if (!canFreeze_ && liveCurrent.match.phase != RlMatchPhase::GoalScored)
            {
                player_.Skip();   // online: the kick-off began, the replay is over
            }
            if (player_.Playing())
            {
                player_.Update(dt, sim);
            }
            out.replay = player_.Info();
            out.replayTicks = &player_.CrossedTicks();
            out.previous = &player_.Previous();
            out.current = &player_.Current();
            out.alpha = player_.Alpha();
            out.freezeLive = canFreeze_;
            out.cinematicCamera = true;
            out.camera = player_.Camera();
            if (!player_.Playing())
            {
                out.replayEnded = true;
                out.replay.active = false;
                out.freezeLive = false;
                out.previous = &livePrevious;
                out.current = &liveCurrent;
                out.alpha = liveAlpha;
                lastCamera_ = out.camera;
                EnterGameplay(out);
                out.cinematicCamera = false;
            }
            break;

        case RlShot::Results:
            out.cinematicCamera = true;
            out.camera = RlResultsView(tuning_, liveCurrent, winner_, shotTime_);
            break;

        case RlShot::Gameplay:
            break;
        }

        // the match is over: after the celebration and the replay of the last goal the camera of the results
        if (shot_ == RlShot::Gameplay && liveCurrent.match.phase == RlMatchPhase::Finished && finished_)
        {
            shot_ = RlShot::Results;
            shotTime_ = 0.0f;
            out.cinematicCamera = true;
            out.camera = RlResultsView(tuning_, liveCurrent, winner_, 0.0f);
        }

        // a cinematic camera melts into the camera of the game after it
        if (shot_ == RlShot::Gameplay && blendLeft_ > 0.0f && gameplayCamera != nullptr)
        {
            blendLeft_ -= dt;
            RlCinematicView target;
            target.view = *gameplayCamera;
            target.fovDegrees = tuning_.camera.fov;
            const float t = 1.0f - std::max(blendLeft_, 0.0f) / std::max(config_.blendSeconds, 0.05f);
            out.cinematicCamera = true;
            out.camera = RlBlendViews(blendFrom_, target, Smooth(t));
        }
        else if (shot_ == RlShot::Gameplay)
        {
            blendLeft_ = 0.0f;
        }

        out.shot = shot_;
        if (out.cinematicCamera)
        {
            lastCamera_ = out.camera;
        }
        return out;
    }
}
