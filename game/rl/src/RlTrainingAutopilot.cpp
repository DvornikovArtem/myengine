// RlTrainingAutopilot.cpp

#include <rl/RlTrainingAutopilot.h>

#include "RlBots.h"
#include "RlV3.h"

#include <rl/RlBotForecast.h>
#include <rl/RlRng.h>
#include <rl/RlTraining.h>

#include <algorithm>
#include <cmath>

namespace rl
{
    namespace
    {
        using detail::Cross;
        using detail::Dot;
        using detail::FromRl;
        using detail::Len;
        using detail::Norm;
        using detail::V3;

        constexpr float kRoleInterval = 0.2f;  // as in RlMatch for the bots of the second kind

        V3 Rotate(const RlQuat& q, const V3 v)
        {
            const V3 u{q.x, q.y, q.z};
            const V3 t = Cross(u, v) * 2.0f;
            return v + t * q.w + Cross(u, t);
        }

        float Clamp1(const float value) { return std::clamp(value, -1.0f, 1.0f); }

        // the flight of an aerial from the jump to the contact at height y (the same rule of thumb as the bots')
        float FlightTime(const float y) { return 0.35f + 0.2f * std::max(y, 0.0f); }
    }

    struct RlTrainingAutopilot::Impl
    {
        RlTuning tuning;
        int level = 2;
        RlAutopilotStyle style = RlAutopilotStyle::Bot;
        float minHeight = 4.0f;
        RlRng rng;
        RlBotMemory memories[kMaxCars];
        RlBotTeamPlan plan;
        RlForecast forecast;
        float roleTimer = 0.0f;
        uint64_t seed = 1;

        // the aerial: where and when the ball is met (decided again every tick from the forecast), how long the first jump has been held
        float jumpHeld = 0.0f;
        bool inFlight = false;
        bool planned = false;       // a contact is chosen and kept until the ball has passed it (a new choice every tick would never arrive)
        uint32_t planTick = 0;      // the tick of the contact

        Impl(const RlTuning& t, const int l, const uint64_t s)
            : tuning(t)
            , level(std::clamp(l, 0, 2))
            , rng(s)
            , seed(s)
        {
        }

        // The first moment of the forecast at which the ball is high enough and the car can be under it in time
        bool PickContact(const RlCarState& car, const RlWorldState& world, const float lowest, V3& point, float& time) const
        {
            const V3 pos = FromRl(car.position);
            const float wanted = lowest + 0.6f;
            bool found = false;
            for (int i = 8; i < kForecastSamples; ++i)
            {
                const float t = static_cast<float>(i) * kForecastDt;
                const V3 p{forecast.pos[i][0], forecast.pos[i][1], forecast.pos[i][2]};
                if (p.y < wanted || p.y > std::min(wanted + 3.5f, tuning.arena.height - 3.5f))
                {
                    continue;
                }
                const float dx = p.x - pos.x;
                const float dz = p.z - pos.z;
                const float distance = std::sqrt(dx * dx + dz * dz);
                const bool standing = car.onGround || (car.position.y < 0.6f && std::fabs(car.linearVelocity.y) < 1.5f);
                const float flat = std::sqrt(car.linearVelocity.x * car.linearVelocity.x + car.linearVelocity.z * car.linearVelocity.z);
                const float coverable = std::min(23.0f * t, flat * t + 4.0f * t * t);
                if (t >= FlightTime(p.y) + (standing ? 0.3f : 0.0f) && (!standing || distance - 1.5f <= coverable))
                {
                    point = p;
                    time = t;
                    return true;
                }
                found = true;
                point = p;
                time = t;
            }
            (void)world;
            return found;
        }

        // A save: a ball that comes in low is met on the floor where the car can be first, a ball that comes in high is flown to
        RlCarInput Goalie(const RlWorldState& world, const int slot)
        {
            const RlCarState& car = world.cars[slot];
            const float line = tuning.arena.halfLength;
            int cross = kForecastSamples - 1;
            for (int i = 1; i < kForecastSamples; ++i)
            {
                if (forecast.pos[i][2] < -line)
                {
                    cross = i;
                    break;
                }
            }
            if (forecast.pos[cross][1] > 2.6f || inFlight)
            {
                return Aerial(world, slot, 1.6f);
            }
            RlCarInput in;
            const V3 pos = FromRl(car.position);
            const V3 vel = FromRl(car.linearVelocity);
            const V3 forward = Rotate(car.rotation, {0.0f, 0.0f, 1.0f});
            const float speed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
            // the first moment at which the car can be where the low ball is
            int pick = cross;
            for (int i = 2; i <= cross; ++i)
            {
                const float t = static_cast<float>(i) * kForecastDt;
                const V3 p{forecast.pos[i][0], forecast.pos[i][1], forecast.pos[i][2]};
                if (p.y > 2.4f)
                {
                    continue;
                }
                const float dx = p.x - pos.x;
                const float dz = p.z - pos.z;
                const float distance = std::sqrt(dx * dx + dz * dz);
                if (distance - 1.0f <= std::min(23.0f * t, speed * t + 5.0f * t * t))
                {
                    pick = i;
                    break;
                }
            }
            const float t = std::max(static_cast<float>(pick) * kForecastDt, 0.1f);
            const V3 target{forecast.pos[pick][0], forecast.pos[pick][1], forecast.pos[pick][2]};
            const float dx = target.x - pos.x;
            const float dz = target.z - pos.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            float error = std::atan2(dx, dz) - std::atan2(forward.x, forward.z);
            while (error > 3.14159265f)
            {
                error -= 6.2831853f;
            }
            while (error < -3.14159265f)
            {
                error += 6.2831853f;
            }
            const float wantedSpeed = distance / t;
            if (std::fabs(error) > 2.2f)
            {
                in.throttle = -1.0f;                       // the ball is behind: reverse and turn
                in.steer = Clamp1(-error * 2.0f);
                return in;
            }
            in.steer = Clamp1(error * 2.5f);
            in.throttle = speed < wantedSpeed + 2.0f ? 1.0f : 0.0f;
            in.boost = std::fabs(error) < 0.25f && speed < wantedSpeed - 1.0f && car.boost > 0.0f;
            return in;
        }

        RlCarInput Aerial(const RlWorldState& world, const int slot, const float lowest)
        {
            const RlCarState& car = world.cars[slot];
            RlCarInput in;
            if (!planned || world.tick >= planTick)
            {
                V3 picked;
                float when = 0.0f;
                planned = PickContact(car, world, lowest, picked, when);
                if (!planned)
                {
                    // nothing to reach: wait under the ball
                    const V3 ball = FromRl(world.ball.position);
                    in.throttle = std::clamp((ball.z - car.position.z) * 0.3f, -1.0f, 1.0f);
                    return in;
                }
                planTick = world.tick + static_cast<uint32_t>(std::lround(when / kTickDt));
            }
            const float t = static_cast<float>(planTick - world.tick) * kTickDt;
            const int sample = std::clamp(static_cast<int>(std::lround(t / kForecastDt)), 0, kForecastSamples - 1);
            const V3 target{forecast.pos[sample][0], forecast.pos[sample][1], forecast.pos[sample][2]};
            const V3 pos = FromRl(car.position);
            const V3 vel = FromRl(car.linearVelocity);
            const float flight = FlightTime(target.y);
            const V3 forward = Rotate(car.rotation, {0.0f, 0.0f, 1.0f});
            // a car that has just been put on the floor has not touched it yet: it still counts as standing
            const bool grounded = car.onGround || (car.position.y < 0.6f && std::fabs(car.linearVelocity.y) < 1.5f);

            if (grounded && !inFlight)
            {
                const float dx = target.x - pos.x;
                const float dz = target.z - pos.z;
                const float distance = std::sqrt(dx * dx + dz * dz);
                const float want = std::atan2(dx, dz);
                const float heading = std::atan2(forward.x, forward.z);
                float error = want - heading;
                while (error > 3.14159265f)
                {
                    error -= 6.2831853f;
                }
                while (error < -3.14159265f)
                {
                    error += 6.2831853f;
                }
                in.steer = Clamp1(error * 2.5f);
                const float speed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
                // the horizontal speed holds in the air: the car needs the distance over the time to the contact
                const float wantedSpeed = distance / std::max(t, 0.2f);
                in.throttle = speed < wantedSpeed + 1.0f ? 1.0f : (speed > wantedSpeed + 3.0f ? -0.5f : 0.0f);
                in.boost = std::fabs(error) < 0.25f && speed < wantedSpeed - 2.0f && car.boost > 0.0f;
                const float timeToJump = t - flight;
                if (timeToJump <= 0.05f && distance > speed * flight * 1.15f + 2.0f)
                {
                    planned = false;   // the car is too far to cover the rest in the flight: the ball is met later
                }
                else if (timeToJump <= 0.05f && std::fabs(error) < 0.6f)
                {
                    in.jump = true;
                    inFlight = true;
                    jumpHeld = 0.0f;
                }
                return in;
            }

            if (grounded && jumpHeld > 0.3f)
            {
                inFlight = false;  // landed: a new try
                planned = false;
                return in;
            }

            // in the air: the acceleration that brings the car to the ball in the time left, the nose along it, the boost while it is needed
            jumpHeld += kTickDt;
            in.jump = jumpHeld < 0.2f;  // the first jump is held for the push, the button is let go after (no double jump)
            const float remaining = std::max(t - 0.05f, 0.3f);
            // the acceleration that brings the car to the ball in the time left (the nose goes along it, the boost burns while it is needed):
            // an aerial climbs with the nose high first and flattens at the end
            const V3 gravity{0.0f, -tuning.gravity, 0.0f};
            const V3 wanted = ((target - pos) - vel * remaining) * (2.0f / (remaining * remaining)) - gravity;
            const float boostNeeded = Len(wanted);
            const V3 aim = Norm(wanted, forward);
            const V3 right = Rotate(car.rotation, {1.0f, 0.0f, 0.0f});
            const V3 up = Rotate(car.rotation, {0.0f, 1.0f, 0.0f});
            const V3 omega = FromRl(car.angularVelocity);
            const float yawError = std::atan2(Dot(aim, right), Dot(aim, forward));
            const float pitchError = std::atan2(Dot(aim, up), Dot(aim, forward));   // > 0: the nose has to go up
            const float yawRate = Dot(omega, up);                                     // > 0: to the right
            const float noseUpRate = -Dot(omega, right);                              // the rotation about +X pitches the nose down
            // an aim that points straight up has no direction to the side: the yaw is then left to the damping
            const float sideways = std::clamp(std::sqrt(std::max(0.0f, 1.0f - aim.y * aim.y)) * 3.0f, 0.0f, 1.0f);
            in.steer = Clamp1(yawError * 3.5f * sideways - yawRate * 0.8f);
            in.pitch = Clamp1(pitchError * 3.5f - noseUpRate * 0.8f);
            const float misaligned = std::max(std::fabs(yawError), std::fabs(pitchError));
            in.boost = car.boost > 0.0f && misaligned < 0.5f && boostNeeded > 3.0f;
            return in;
        }
    };

    RlTrainingAutopilot::RlTrainingAutopilot(const RlTuning& tuning, const int level, const uint64_t seed)
        : impl_(std::make_unique<Impl>(tuning, level, seed))
    {
    }

    RlTrainingAutopilot::~RlTrainingAutopilot() = default;

    void RlTrainingAutopilot::SetStyle(const RlAutopilotStyle style, const float minHeight)
    {
        impl_->style = style;
        impl_->minHeight = minHeight;
    }

    void RlTrainingAutopilot::SetStyleFor(const RlTrainingExercise& exercise)
    {
        switch (exercise.condition)
        {
        case RlTrainingCondition::Saved: SetStyle(RlAutopilotStyle::Goalie); break;
        case RlTrainingCondition::TouchAbove: SetStyle(RlAutopilotStyle::Aerial, exercise.height); break;
        default: SetStyle(RlAutopilotStyle::Bot); break;
        }
    }

    void RlTrainingAutopilot::Reset()
    {
        Impl& m = *impl_;
        m.rng = RlRng(m.seed);
        for (RlBotMemory& memory : m.memories)
        {
            memory = RlBotMemory{};
        }
        m.plan = RlBotTeamPlan{};
        m.forecast = RlForecast{};
        m.roleTimer = 0.0f;
        m.jumpHeld = 0.0f;
        m.inFlight = false;
        m.planned = false;
    }

    RlCarInput RlTrainingAutopilot::Think(const RlWorldState& world, const int slot)
    {
        Impl& m = *impl_;
        RlForecastBall(m.tuning, world.ball, world.tick, m.forecast);
        if (m.style == RlAutopilotStyle::Aerial)
        {
            return m.Aerial(world, slot, m.minHeight);
        }
        if (m.style == RlAutopilotStyle::Goalie)
        {
            return m.Goalie(world, slot);
        }
        m.roleTimer -= kTickDt;
        if (m.roleTimer <= 0.0f)
        {
            m.roleTimer = kRoleInterval;
            RlBotsPlan(m.tuning, world, m.forecast, 0, 1, m.level, m.plan, m.memories);
        }
        const RlBotContext context{m.tuning, world, m.forecast, m.plan, slot, 0, 1, m.level};
        return RlBotThink(context, m.memories[slot], m.rng);
    }
}
