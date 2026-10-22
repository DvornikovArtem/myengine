// RlBotsBrain.cpp

#include "RlBotsBrain.h"

#include <rl/RlMatch.h>

#include <algorithm>
#include <cmath>

namespace rl::brain
{
    namespace
    {
        constexpr float kPi = 3.14159265f;
        constexpr float kContactDistance = 1.45f;  // from the centre of the car to the centre of the ball when the nose touches it

        struct V3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float FlatLen(float x, float z) { return std::sqrt(x * x + z * z); }
        V3 FromRl(const RlVec3& v) { return {v.x, v.y, v.z}; }
        float Clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
        float WrapPi(float a)
        {
            while (a > kPi)
            {
                a -= 2.0f * kPi;
            }
            while (a < -kPi)
            {
                a += 2.0f * kPi;
            }
            return a;
        }

        V3 Rotate(const RlQuat& q, V3 v)
        {
            const float tx = 2.0f * (q.y * v.z - q.z * v.y);
            const float ty = 2.0f * (q.z * v.x - q.x * v.z);
            const float tz = 2.0f * (q.x * v.y - q.y * v.x);
            return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx)};
        }

        int AttackSign(int team) { return team == 0 ? 1 : -1; }  // blue attacks +Z

        V3 ForecastPos(const RlForecast& f, int i) { return {f.pos[i][0], f.pos[i][1], f.pos[i][2]}; }

        // the share of the throttle force that is left at forward speed v
        float DriveFactor(const RlTuning& t, float v)
        {
            const float* s = t.car.driveSpeed;
            const float* f = t.car.driveFactor;
            if (v <= s[0])
            {
                return f[0];
            }
            for (int i = 1; i < 3; ++i)
            {
                if (v <= s[i])
                {
                    const float u = (v - s[i - 1]) / std::max(s[i] - s[i - 1], 1e-4f);
                    return f[i - 1] + (f[i] - f[i - 1]) * u;
                }
            }
            return f[2];
        }

        // the distance a car covers in a straight line in 0..3 s (table at the steps of the forecast) with the throttle and the boost it has
        void BuildReach(const RlTuning& t, float v0, float boost, float* table)
        {
            float v = std::max(v0, 0.0f);
            float d = 0.0f;
            float boostLeft = boost;
            constexpr int kSub = 5;
            const float dt = kForecastDt / static_cast<float>(kSub);
            table[0] = 0.0f;
            for (int i = 1; i < kForecastSamples; ++i)
            {
                for (int s = 0; s < kSub; ++s)
                {
                    float a = t.car.throttleAccel * DriveFactor(t, v);
                    float top = t.car.driveSpeed[2];
                    if (boostLeft > 0.0f)
                    {
                        a += t.car.boostAccelGround;
                        boostLeft -= t.car.boostUsePerSecond * dt;
                        top = t.car.maxSpeed;
                    }
                    v = std::min(v + a * dt, std::max(top, v));
                    d += v * dt;
                }
                table[i] = d;
            }
        }

        float ReachAt(const float* table, float seconds)
        {
            if (seconds <= 0.0f)
            {
                return 0.0f;
            }
            const float f = seconds / kForecastDt;
            const int i = std::min(static_cast<int>(f), kForecastSamples - 2);
            return table[i] + (table[i + 1] - table[i]) * (f - static_cast<float>(i));
        }

        // The ball is at the kick-off spot and nobody has touched it
        bool AtKickoff(const RlWorldState& world)
        {
            const RlBallState& b = world.ball;
            return FlatLen(b.position.x, b.position.z) < 0.6f && std::fabs(b.linearVelocity.x) + std::fabs(b.linearVelocity.z) < 0.5f;
        }

        // nearest ready pad to (x, z) within `maxDistance`, big ones count as nearer
        bool NearestPad(const RlWorldState& world, float x, float z, float maxDistance, float& padX, float& padZ)
        {
            float best = 1e9f;
            bool found = false;
            for (int i = 0; i < kPadCount; ++i)
            {
                float px;
                float pz;
                const bool big = RlPadSpot(i, px, pz);
                if (world.match.pads[i].respawnLeft > 0.0f)
                {
                    continue;
                }
                const float d = FlatLen(px - x, pz - z);
                if (d > maxDistance)
                {
                    continue;
                }
                const float score = d - (big ? 8.0f : 0.0f);
                if (score < best)
                {
                    best = score;
                    padX = px;
                    padZ = pz;
                    found = true;
                }
            }
            return found;
        }
    }

    Level LevelOf(int level)
    {
        switch (std::clamp(level, 0, 2))
        {
        case 0: return {0.45f, 0.24f, 1.2f, 2.2f, false, false, true, false, false};
        case 1: return {0.25f, 0.06f, 2.6f, 2.8f, true, true, true, true, false};
        default: return {0.12f, 0.015f, 3.0f, 2.9f, true, true, true, true, true};
        }
    }

    Intercept FindIntercept(const RlTuning& tuning, const RlForecast& forecast, const RlCarState& car, const Level& level)
    {
        Intercept result;
        const V3 pos = FromRl(car.position);
        const V3 vel = FromRl(car.linearVelocity);
        const V3 fwd = Rotate(car.rotation, {0.0f, 0.0f, 1.0f});
        const float heading = std::atan2(fwd.x, fwd.z);
        const float flatForward = FlatLen(fwd.x, fwd.z);
        const float speedAlong = flatForward > 0.01f ? (vel.x * fwd.x + vel.z * fwd.z) / flatForward : 0.0f;
        float table[kForecastSamples];
        BuildReach(tuning, speedAlong, car.boost, table);

        const float hz = tuning.arena.halfLength;
        for (int i = 1; i < kForecastSamples; ++i)
        {
            const float t = static_cast<float>(i) * kForecastDt;
            if (t > level.horizon)
            {
                break;
            }
            const V3 p = ForecastPos(forecast, i);
            if (p.y > level.heightMax || std::fabs(p.z) > hz - 0.3f)
            {
                continue;
            }
            const float dx = p.x - pos.x;
            const float dz = p.z - pos.z;
            const float need = std::max(FlatLen(dx, dz) - kContactDistance, 0.0f);
            const float angle = std::fabs(WrapPi(std::atan2(dx, dz) - heading));
            const float penalty = angle > 0.3f ? (angle - 0.3f) * 0.55f : 0.0f;
            if (ReachAt(table, t - penalty) >= need)
            {
                result.ok = true;
                result.time = t;
                result.x = p.x;
                result.y = p.y;
                result.z = p.z;
                return result;
            }
        }
        return result;
    }

    float AerialFlightTime(float y) { return 0.25f + 0.16f * std::max(y, 0.0f); }

    Intercept FindAerial(const RlTuning& tuning, const RlForecast& forecast, const RlCarState& car, const Level& level)
    {
        Intercept result;
        const V3 pos = FromRl(car.position);
        const V3 vel = FromRl(car.linearVelocity);
        const V3 fwd = Rotate(car.rotation, {0.0f, 0.0f, 1.0f});
        const float heading = std::atan2(fwd.x, fwd.z);
        const float flatForward = FlatLen(fwd.x, fwd.z);
        const float speedAlong = flatForward > 0.01f ? (vel.x * fwd.x + vel.z * fwd.z) / flatForward : 0.0f;
        float table[kForecastSamples];
        BuildReach(tuning, speedAlong, car.boost, table);
        const float hz = tuning.arena.halfLength;
        for (int i = 16; i < kForecastSamples; ++i)
        {
            const float t = static_cast<float>(i) * kForecastDt;
            if (t > level.horizon)
            {
                break;
            }
            const V3 p = ForecastPos(forecast, i);
            if (p.y < 3.0f || p.y > 7.5f || std::fabs(p.z) > hz - 2.0f || std::fabs(p.x) > tuning.arena.halfWidth - 3.0f)
            {
                continue;
            }
            const float dx = p.x - pos.x;
            const float dz = p.z - pos.z;
            const float need = std::max(FlatLen(dx, dz) - 6.0f, 0.0f);  // the last metres are flown
            const float angle = std::fabs(WrapPi(std::atan2(dx, dz) - heading));
            const float penalty = angle > 0.3f ? (angle - 0.3f) * 0.55f : 0.0f;
            if (t > AerialFlightTime(p.y) + 0.3f && ReachAt(table, t - AerialFlightTime(p.y) - penalty) >= need)
            {
                result.ok = true;
                result.time = t;
                result.x = p.x;
                result.y = p.y;
                result.z = p.z;
                return result;
            }
        }
        return result;
    }

    void Plan(const RlTuning& tuning, const RlWorldState& world, const RlForecast& forecast, int team, int teamSize, int level, RlBotTeamPlan& plan,
              RlBotMemory* memories)
    {
        const Level L = LevelOf(level);
        const int first = team * teamSize;
        const float sign = static_cast<float>(AttackSign(team));
        const V3 ball = FromRl(world.ball.position);

        int alive[kMaxCars];
        float reach[kMaxCars];
        int count = 0;
        for (int i = 0; i < teamSize; ++i)
        {
            const RlCarState& c = world.cars[first + i];
            if (c.demolished)
            {
                continue;
            }
            const Intercept in = FindIntercept(tuning, forecast, c, L);
            const float flat = FlatLen(ball.x - c.position.x, ball.z - c.position.z);
            alive[count] = first + i;
            reach[count] = in.ok ? in.time : 3.0f + flat / 18.0f;  // out of reach for now: the farther the later
            ++count;
        }
        if (count == 0)
        {
            return;
        }

        // the kick-off: the car nearest to the ball goes, the others fetch boost
        const bool kickoff = AtKickoff(world);
        if (kickoff && plan.kickoffId != static_cast<int32_t>(world.match.kickoffs))
        {
            plan.kickoffId = static_cast<int32_t>(world.match.kickoffs);
            int best = 0;
            float bestDistance = 1e9f;
            for (int k = 0; k < count; ++k)
            {
                const RlCarState& c = world.cars[alive[k]];
                const float d = FlatLen(ball.x - c.position.x, ball.z - c.position.z);
                if (d < bestDistance - 0.05f)
                {
                    bestDistance = d;
                    best = k;
                }
            }
            plan.kickoffTaker = alive[best];
        }

        // the attacker: the earliest at the ball, and the one that attacks now keeps it unless another is clearly (0.4 s) earlier. A ball in the
        // own third is cleared by a car that is on the goal side of it (a car in front of it can only push it towards the own goal).
        const float ownZ = -tuning.arena.halfLength * sign;
        const bool ownThird = (ball.z - ownZ) * sign < 26.0f;
        bool eligible[kMaxCars];
        bool anyEligible = false;
        for (int k = 0; k < count; ++k)
        {
            eligible[k] = !ownThird || (world.cars[alive[k]].position.z - ball.z) * sign < 0.5f;
            anyEligible = anyEligible || eligible[k];
        }
        if (!anyEligible)
        {
            std::fill(eligible, eligible + count, true);
        }
        int attacker = -1;
        for (int k = 0; k < count; ++k)
        {
            if (eligible[k] && (attacker < 0 || reach[k] < reach[attacker]))
            {
                attacker = k;
            }
        }
        for (int k = 0; k < count; ++k)
        {
            if (k != attacker && eligible[k] && plan.role[alive[k]] == BotAttacker && reach[k] <= reach[attacker] + 0.4f)
            {
                attacker = k;
                break;
            }
        }
        if (kickoff)
        {
            for (int k = 0; k < count; ++k)
            {
                if (alive[k] == plan.kickoffTaker)
                {
                    attacker = k;
                }
            }
        }

        // one car per team: it plays the ball unless the opponent is clearly earlier at it and the ball is in the own half
        bool lastManDefends = false;
        if (teamSize == 1 && !kickoff)
        {
            float opponent = 1e9f;
            const int enemyFirst = (1 - team) * teamSize;
            for (int i = 0; i < teamSize; ++i)
            {
                const RlCarState& c = world.cars[enemyFirst + i];
                if (!c.demolished)
                {
                    const Intercept in = FindIntercept(tuning, forecast, c, LevelOf(2));
                    opponent = std::min(opponent, in.ok ? in.time : 6.0f);
                }
            }
            lastManDefends = opponent + 0.5f < reach[0] && ball.z * sign < -4.0f;
        }

        // the defender is the car farthest back, the supporter the earliest of the rest
        int defender = -1;
        float farthestBack = 1e9f;
        for (int k = 0; k < count; ++k)
        {
            if (k == attacker)
            {
                continue;
            }
            const float advance = world.cars[alive[k]].position.z * sign;
            if (advance < farthestBack)
            {
                farthestBack = advance;
                defender = k;
            }
        }
        int supporter = -1;
        for (int k = 0; k < count; ++k)
        {
            if (k == attacker || k == defender)
            {
                continue;
            }
            if (supporter < 0 || reach[k] < reach[supporter])
            {
                supporter = k;
            }
        }

        for (int k = 0; k < count; ++k)
        {
            int role = BotSupport;
            if (k == attacker)
            {
                role = lastManDefends ? BotDefender : BotAttacker;
            }
            else if (k == defender)
            {
                role = BotDefender;
            }
            else if (k == supporter)
            {
                role = BotSupport;
            }
            else if (world.cars[alive[k]].boost < 50.0f)
            {
                role = BotBoost;
            }
            plan.role[alive[k]] = role;
            memories[alive[k]].role = role;
        }
    }

    void Decide(const RlBotContext& ctx, RlBotMemory& mem, RlRng& rng)
    {
        const RlTuning& tuning = ctx.tuning;
        const RlWorldState& world = ctx.world;
        const RlCarState& car = world.cars[ctx.slot];
        const Level level = LevelOf(ctx.level);
        const float hx = tuning.arena.halfWidth;
        const float hz = tuning.arena.halfLength;
        const float goalW = tuning.arena.goalHalfWidth;
        const float sign = static_cast<float>(AttackSign(ctx.team));
        const float ownZ = -hz * sign;
        const float enemyZ = hz * sign;

        const V3 pos = FromRl(car.position);
        const V3 ball = FromRl(world.ball.position);
        const float ballDist = FlatLen(ball.x - pos.x, ball.z - pos.z);

        mem.decisionTimer = level.interval * (0.8f + 0.4f * rng.Float01());
        mem.aimError = level.aimError * rng.Signed();
        mem.flipIntent = 0;
        mem.stopAtTarget = 0;
        mem.arriveTimer = 0.0f;
        mem.contactTime = -1.0f;
        mem.hitDirX = 0.0f;
        mem.hitDirZ = 0.0f;
        mem.kickoffMode = 0;
        if (mem.aerialMode == 2)
        {
            return;  // in the air after the ball: the flight is steered every tick, not decided again
        }

        int role = mem.role;
        const bool kickoff = AtKickoff(world);
        const bool danger = (ball.z - ownZ) * sign < 20.0f;
        if (kickoff)
        {
            role = ctx.plan.kickoffTaker == ctx.slot ? BotAttacker : BotBoost;
        }
        else
        {
            if (role == BotDefender && danger)
            {
                role = BotAttacker;
            }
            if (role == BotSupport && car.boost < 35.0f)
            {
                role = BotBoost;
            }
            if (car.boost < 12.0f && role == BotDefender && !danger)
            {
                role = BotBoost;
            }
            if (role == BotAttacker && car.boost < 25.0f && ballDist > 38.0f && !danger)
            {
                role = BotBoost;
            }
        }

        if (role == BotAttacker)
        {
            if (kickoff)
            {
                // straight at the ball, as fast as the car goes
                mem.kickoffMode = 1;
                mem.targetX = ball.x;
                mem.targetZ = ball.z;
                mem.hitDirX = 0.0f;
                mem.hitDirZ = sign;
                mem.contactTime = 3.0f;
            }
            else
            {
                Intercept in = FindIntercept(tuning, ctx.forecast, car, level);
                bool aerial = false;
                if (level.aerial && car.boost >= 25.0f && car.wheelContacts >= 3 && ballDist < 40.0f)
                {
                    const Intercept high = FindAerial(tuning, ctx.forecast, car, level);
                    if (high.ok && (!in.ok || high.time + 0.1f < in.time))
                    {
                        in = high;
                        aerial = true;
                    }
                }
                V3 p;
                float t;
                if (in.ok)
                {
                    p = {in.x, in.y, in.z};
                    t = in.time;
                }
                else
                {
                    // out of reach: drive to where the ball will be when the car could come
                    const float speed = std::sqrt(car.linearVelocity.x * car.linearVelocity.x + car.linearVelocity.z * car.linearVelocity.z);
                    t = Clamp(ballDist / Clamp(speed + 7.0f, 12.0f, tuning.car.maxSpeed), 0.3f, 2.5f);
                    const RlVec3 q = RlForecastAt(ctx.forecast, world.tick, t);
                    p = {q.x, q.y, q.z};
                }
                p.x = Clamp(p.x, -hx + 1.5f, hx - 1.5f);
                p.z = Clamp(p.z, -hz + 1.0f, hz - 1.0f);

                // where the ball should go: at the other goal, or, deep in the own half, away from the own goal to the side
                float gx = Clamp(-p.x * 0.3f, -4.0f, 4.0f);
                float gz = enemyZ;
                if ((p.z - ownZ) * sign < 26.0f)
                {
                    const float side = p.x >= 0.0f ? 1.0f : -1.0f;
                    gx = side * (hx - 8.0f);
                    gz = p.z + sign * 30.0f;
                }
                float dx = gx - p.x;
                float dz = gz - p.z;
                if ((p.z - ownZ) * sign < 14.0f)
                {
                    // right at the own goal line: the ball goes sideways and out, never along the line to the net
                    const float side = p.x >= 0.0f ? 1.0f : -1.0f;
                    dx = side * 0.9f;
                    dz = sign * 0.45f;
                }
                const float dl = std::max(FlatLen(dx, dz), 0.01f);
                dx /= dl;
                dz /= dl;
                mem.hitDirX = dx;
                mem.hitDirZ = dz;
                mem.contactTime = t;

                // how the car comes: behind the ball on the line of the hit
                const float cx = pos.x - p.x;
                const float cz = pos.z - p.z;
                const float behind = -(cx * dx + cz * dz);      // metres the car is behind the ball along the hit
                const float lateral = cx * (-dz) + cz * dx;      // and to the side of the line
                const float lateralSide = lateral >= 0.0f ? 1.0f : -1.0f;
                if (behind < 0.0f)
                {
                    // in front of the ball: go round it, not through it
                    mem.targetX = p.x - dx * 5.0f - dz * lateralSide * 5.5f;
                    mem.targetZ = p.z - dz * 5.0f + dx * lateralSide * 5.5f;
                }
                else
                {
                    // behind it: aim at a point on the line of the hit that is the farther back the farther the car is off the line, so that the
                    // path curves into the line; on the line it is the ball itself
                    const float s = Clamp(0.55f * std::fabs(lateral) - 0.2f * Clamp(behind - 3.0f, 0.0f, 20.0f) * 0.0f, 0.3f, 10.0f);
                    mem.targetX = p.x - dx * s;
                    mem.targetZ = p.z - dz * s;
                }
                mem.targetX = Clamp(mem.targetX, -hx + 1.5f, hx - 1.5f);
                mem.targetZ = Clamp(mem.targetZ, -hz + 1.0f, hz - 1.0f);

                // a ball that is still high: be there when it comes down, not before
                if (in.ok && !aerial && p.y > 2.2f && t > 0.35f && behind >= 2.0f)
                {
                    mem.arriveTimer = t;
                }
                if (aerial)
                {
                    mem.aerialMode = 1;
                    mem.aerialClock = t;
                    mem.aerialY = p.y;
                    // under the ball and moving: the target is the ground below the contact
                    mem.targetX = Clamp(p.x - dx * 1.0f, -hx + 1.5f, hx - 1.5f);
                    mem.targetZ = Clamp(p.z - dz * 1.0f, -hz + 1.0f, hz - 1.0f);
                }
                else
                {
                    mem.aerialMode = 0;
                }
                // short of boost: a pad that lies on the way is taken
                if (car.boost < 30.0f && !danger && mem.arriveTimer <= 0.0f && level.boostFreely)
                {
                    const float tx = mem.targetX - pos.x;
                    const float tz = mem.targetZ - pos.z;
                    const float tl = std::max(FlatLen(tx, tz), 0.01f);
                    float bestScore = 1e9f;
                    for (int i = 0; i < kPadCount; ++i)
                    {
                        float px;
                        float pz;
                        const bool big = RlPadSpot(i, px, pz);
                        if (world.match.pads[i].respawnLeft > 0.0f)
                        {
                            continue;
                        }
                        const float ddx = px - pos.x;
                        const float ddz = pz - pos.z;
                        const float d = FlatLen(ddx, ddz);
                        if (d < 2.0f || d > 15.0f || d > 0.7f * tl || (ddx * tx + ddz * tz) / (d * tl) < 0.6f)
                        {
                            continue;
                        }
                        const float score = d - (big ? 6.0f : 0.0f);
                        if (score < bestScore)
                        {
                            bestScore = score;
                            mem.targetX = px;
                            mem.targetZ = pz;
                        }
                    }
                }
            }
        }
        else if (role == BotDefender)
        {
            const RlVec3 q = RlForecastAt(ctx.forecast, world.tick, 0.7f);
            mem.targetX = Clamp(q.x * 0.25f + mem.patrolSide * 5.0f, -(goalW - 1.0f), goalW - 1.0f);
            // shadows the ball back to its own goal: the further it is, the further out the defender waits
            const float ballOut = std::max((ball.z - ownZ) * sign - 10.0f, 0.0f);
            mem.targetZ = ownZ + sign * (6.0f + std::min(24.0f, 0.25f * ballOut));
            mem.stopAtTarget = 1;
        }
        else if (role == BotBoost)
        {
            float bx = 0.0f;
            float bz = ownZ * 0.5f;
            if (!NearestPad(world, pos.x, pos.z, kickoff ? 30.0f : 1000.0f, bx, bz))
            {
                bx = pos.x * 0.5f;
                bz = ownZ * 0.4f + pos.z * 0.2f;
                mem.stopAtTarget = 1;
            }
            mem.targetX = bx;
            mem.targetZ = bz;
        }
        else
        {
            const RlVec3 q = RlForecastAt(ctx.forecast, world.tick, 1.0f);
            const float lane = ((ctx.slot % 2 != 0) ? 9.0f : -9.0f) + mem.patrolSide * 7.0f;
            mem.targetX = Clamp(q.x * 0.5f + lane, -hx + 3.0f, hx - 3.0f);
            mem.targetZ = Clamp(q.z - 12.0f * sign, -hz + 4.0f, hz - 4.0f);
            mem.stopAtTarget = 1;
        }
        if (role != BotAttacker)
        {
            mem.aerialMode = 0;
        }
        mem.activeRole = role;
    }
}
