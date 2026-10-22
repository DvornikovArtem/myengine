// RlReplay.cpp

#include <rl/RlCinematic.h>
#include <rl/RlReplay.h>

#include "RlV3.h"

#include <algorithm>
#include <cmath>

namespace rl
{
    using namespace detail;

    // ---------------------------------------------------------------- the ring

    RlReplayBuffer::RlReplayBuffer(const std::size_t capacityTicks) : slots_(std::max<std::size_t>(capacityTicks, 2)) {}

    void RlReplayBuffer::Clear()
    {
        count_ = 0;
        newest_ = 0;
    }

    void RlReplayBuffer::Record(const RlWorldState& world)
    {
        const std::size_t capacity = slots_.size();
        if (count_ > 0)
        {
            if (world.tick <= newest_)
            {
                // a tick simulated again (a rollback): what follows it is not true any more
                const uint32_t oldest = OldestTick();
                if (world.tick <= oldest)
                {
                    count_ = 0;
                }
                else
                {
                    count_ = static_cast<std::size_t>(world.tick - oldest);
                    newest_ = world.tick - 1;
                }
            }
            else if (world.tick != newest_ + 1)
            {
                count_ = 0;   // a gap: the ring starts over
            }
        }
        slots_[world.tick % capacity] = world;
        newest_ = world.tick;
        count_ = std::min(count_ + 1, capacity);
    }

    const RlWorldState* RlReplayBuffer::Find(const uint32_t tick) const
    {
        if (count_ == 0 || tick > newest_ || tick < OldestTick())
        {
            return nullptr;
        }
        return &slots_[tick % slots_.size()];
    }

    bool RlReplayBuffer::FindLastGoal(uint32_t& goalTick) const
    {
        if (count_ < 2)
        {
            return false;
        }
        for (uint32_t tick = newest_; tick > OldestTick(); --tick)
        {
            const RlWorldState* now = Find(tick);
            const RlWorldState* before = Find(tick - 1);
            if (now != nullptr && before != nullptr && now->match.phase == RlMatchPhase::GoalScored && before->match.phase != RlMatchPhase::GoalScored)
            {
                goalTick = tick;
                return true;
            }
        }
        return false;
    }

    // ---------------------------------------------------------------- the player

    RlReplayPlayer::RlReplayPlayer(const RlTuning& tuning, const RlReplayConfig& config) : tuning_(tuning), config_(config) {}

    bool RlReplayPlayer::Start(const RlReplayBuffer& buffer, const uint32_t goalTick)
    {
        playing_ = false;
        const RlWorldState* goal = buffer.Find(goalTick);
        if (goal == nullptr || goalTick < buffer.OldestTick() + 240)
        {
            return false;   // less than two seconds before the goal: not worth a replay
        }
        const uint32_t pre = std::min<uint32_t>(static_cast<uint32_t>(config_.preGoalSeconds * kTickRate), goalTick - buffer.OldestTick());
        const uint32_t post = static_cast<uint32_t>(config_.postGoalSeconds * kTickRate);
        const uint32_t first = goalTick - pre;
        const uint32_t last = std::min(goalTick + post, buffer.NewestTick());
        clip_.clear();
        clip_.reserve(last - first + 1);
        for (uint32_t tick = first; tick <= last; ++tick)
        {
            clip_.push_back(*buffer.Find(tick));
        }
        if (clip_.size() < 3)
        {
            return false;
        }
        goalIndex_ = goalTick - first;
        const std::size_t slowTicks = static_cast<std::size_t>(config_.slowSeconds * kTickRate);
        slowIndex_ = goalIndex_ > slowTicks ? goalIndex_ - slowTicks : 0;
        switchIndex_ = static_cast<std::size_t>(static_cast<float>(slowIndex_) * 0.4f);

        info_ = {};
        info_.active = true;
        info_.scorerSlot = goal->match.lastGoalScorer;
        info_.team = goal->match.lastGoalTeam;
        info_.ownGoal = goal->match.lastGoalOwn != 0;
        const RlWorldState& before = clip_[goalIndex_ > 0 ? goalIndex_ - 1 : 0];
        info_.goalSpeedKmh = Len(FromRl(before.ball.linearVelocity)) * 3.6f;
        const int scorer = info_.scorerSlot;
        authorShot_ = scorer >= 0 && scorer < before.carCount && !clip_[switchIndex_].cars[scorer].demolished &&
                      Len(FromRl(clip_[switchIndex_].cars[scorer].position) - FromRl(clip_[switchIndex_].ball.position)) < 45.0f;
        position_ = 0.0;
        index_ = 0;
        alpha_ = 0.0f;
        eyeReady_ = false;
        carCamera_ = RlCameraState{};
        lastShot_ = RlReplayShotKind::Ball;
        crossed_.clear();
        playing_ = true;
        return true;
    }

    void RlReplayPlayer::Skip()
    {
        playing_ = false;
        info_.active = false;
    }

    // The speed of the clip at a position (ticks from its start): 1 up to the slow motion, `slowSpeed` from it, smooth over `rampSeconds`
    float RlReplayPlayer::SpeedAt(const float position) const
    {
        const float half = std::max(config_.rampSeconds * static_cast<float>(kTickRate) * 0.5f, 1.0f);
        const float t = (position - (static_cast<float>(slowIndex_) - half)) / (2.0f * half);
        return 1.0f + (config_.slowSpeed - 1.0f) * Smooth(t);
    }

    float RlReplayPlayer::DurationSeconds() const
    {
        if (clip_.size() < 2)
        {
            return 0.0f;
        }
        double seconds = 0.0;
        for (std::size_t i = 0; i + 1 < clip_.size(); ++i)
        {
            seconds += 1.0 / (static_cast<double>(kTickRate) * static_cast<double>(SpeedAt(static_cast<float>(i) + 0.5f)));
        }
        return static_cast<float>(seconds);
    }

    void RlReplayPlayer::Update(const float dt, const RlSim& sim)
    {
        crossed_.clear();
        if (!playing_ || clip_.size() < 2)
        {
            return;
        }
        const std::size_t before = static_cast<std::size_t>(position_);
        // a few small steps so that the speed changes smoothly inside a long frame
        double remaining = static_cast<double>(std::min(dt, 0.25f));
        while (remaining > 1e-6)
        {
            const double step = std::min(remaining, 1.0 / 240.0);
            position_ += step * static_cast<double>(kTickRate) * static_cast<double>(SpeedAt(static_cast<float>(position_)));
            remaining -= step;
        }
        const double last = static_cast<double>(clip_.size() - 1);
        bool finished = false;
        if (position_ >= last)
        {
            position_ = last;
            finished = true;
        }
        const std::size_t after = static_cast<std::size_t>(position_);
        for (std::size_t i = before + 1; i <= after && i < clip_.size(); ++i)
        {
            crossed_.push_back({&clip_[i - 1], &clip_[i]});
        }
        index_ = std::min<std::size_t>(after, clip_.size() - 2);
        alpha_ = static_cast<float>(position_ - static_cast<double>(index_));
        alpha_ = std::clamp(alpha_, 0.0f, 1.0f);

        info_.speed = SpeedAt(static_cast<float>(position_));
        info_.progress = static_cast<float>(position_ / last);
        UpdateCamera(dt, sim);
        if (finished)
        {
            playing_ = false;
            info_.active = false;
        }
    }

    void RlReplayPlayer::UpdateCamera(const float dt, const RlSim& sim)
    {
        const RlWorldState& a = Previous();
        const RlWorldState& b = Current();
        const V3 ball = Lerp(FromRl(a.ball.position), FromRl(b.ball.position), alpha_);
        const V3 ballVelocity = FromRl(b.ball.linearVelocity);

        RlReplayShotKind shot = RlReplayShotKind::Ball;
        if (index_ >= slowIndex_)
        {
            shot = RlReplayShotKind::Goal;
        }
        else if (authorShot_ && index_ >= switchIndex_)
        {
            shot = RlReplayShotKind::Author;
        }
        if (shot != lastShot_)
        {
            eyeReady_ = false;                 // a cut: the new camera starts where it should be
            carCamera_ = RlCameraState{};
            lastShot_ = shot;
        }
        info_.shot = shot;

        if (shot == RlReplayShotKind::Author)
        {
            const int slot = std::clamp(info_.scorerSlot, 0, kMaxCars - 1);
            const RlCarState& carA = a.cars[slot];
            const RlCarState& carB = b.cars[slot];
            RlCameraInput input;
            input.carPosition = ToRl(Lerp(FromRl(carA.position), FromRl(carB.position), alpha_));
            input.carRotation = Nlerp(carA.rotation, carB.rotation, alpha_);
            input.onSurface = carB.wheelContacts >= 3;
            input.surfaceNormal = carB.surfaceNormal;
            input.ballPosition = ToRl(ball);
            input.ballCam = true;
            camera_.view = RlCameraUpdate(tuning_.camera, carCamera_, sim, input, std::min(dt, 0.1f));
            camera_.fovDegrees = tuning_.camera.fov;
            return;
        }

        if (shot == RlReplayShotKind::Goal)
        {
            // from the net behind the goal line, a little off the middle, looking out at the ball
            const float side = info_.team == 0 ? 1.0f : -1.0f;
            const V3 eye{std::clamp(ball.x * 0.25f, -4.0f, 4.0f), 2.6f, side * (tuning_.arena.halfLength + 3.5f)};
            camera_ = RlLookAt(ToRl(eye), ToRl(ball + V3{0.0f, 0.3f, 0.0f}), 78.0f);
            return;
        }

        // the ball shot: behind the ball along the way it flies, above it, pulled in against the walls and the ceiling
        const float speed = Len(ballVelocity);
        V3 heading = FromRl(shotHeading_);
        if (speed > 6.0f)
        {
            const V3 flat = Norm({ballVelocity.x, 0.0f, ballVelocity.z}, heading);
            heading = eyeReady_ ? Norm(Lerp(heading, flat, 1.0f - std::exp(-dt * 3.0f)), flat) : flat;
        }
        shotHeading_ = ToRl(heading);
        V3 desired = ball - heading * 13.0f + V3{0.0f, 5.5f, 0.0f};
        desired.y = std::clamp(desired.y, 1.0f, tuning_.arena.height - 1.0f);
        V3 eye = eyeReady_ ? Lerp(FromRl(eye_), desired, 1.0f - std::exp(-dt * 6.0f)) : desired;
        {
            const V3 ray = eye - ball;
            const float length = Len(ray);
            const float free = sim.ArenaSegmentFree(ToRl(ball), ToRl(eye));
            if (free < 1.0f && length > 1e-3f)
            {
                eye = ball + ray * (std::max(length * free - tuning_.camera.wallMargin, 2.0f) / length);
            }
        }
        eye_ = ToRl(eye);
        eyeReady_ = true;
        camera_ = RlLookAt(ToRl(eye), ToRl(ball + ballVelocity * 0.1f), 85.0f);
    }
}
