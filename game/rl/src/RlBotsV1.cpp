// RlBotsV1.cpp (the bots of G4b, namespace rl::v1)

#include "RlBotsV1.h"

#include <rl/RlMatch.h>

#include <algorithm>
#include <cmath>

namespace rl::v1
{
    namespace
    {
        constexpr float kPi = 3.14159265f;

        struct V3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        float Len(V3 a) { return std::sqrt(Dot(a, a)); }
        float FlatLen(V3 a) { return std::sqrt(a.x * a.x + a.z * a.z); }
        V3 FromRl(const RlVec3& v) { return {v.x, v.y, v.z}; }
        float Clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
        float WrapPi(float a)
        {
            while (a > kPi) a -= 2.0f * kPi;
            while (a < -kPi) a += 2.0f * kPi;
            return a;
        }

        // the rotation of a quaternion applied to v (the car's axes in the world and back)
        V3 Rotate(const RlQuat& q, V3 v)
        {
            const float tx = 2.0f * (q.y * v.z - q.z * v.y);
            const float ty = 2.0f * (q.z * v.x - q.x * v.z);
            const float tz = 2.0f * (q.x * v.y - q.y * v.x);
            return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx)};
        }
        V3 RotateInverse(const RlQuat& q, V3 v) { return Rotate({-q.x, -q.y, -q.z, q.w}, v); }

        struct Level
        {
            float interval;      // seconds between two decisions
            float aimError;      // radians of random error in the aim
            float predict;       // how far ahead of the ball the attacker looks (share of the full forecast)
            bool boostFreely;
            bool dodges;
            bool airControl;
        };

        Level LevelOf(int level)
        {
            switch (std::clamp(level, 0, 2))
            {
            case 0: return {0.45f, 0.24f, 0.5f, false, false, true};
            case 1: return {0.30f, 0.09f, 0.85f, true, true, true};
            default: return {0.15f, 0.02f, 1.0f, true, true, true};
            }
        }

        int AttackSign(int team) { return team == 0 ? 1 : -1; }  // blue attacks +Z
    }

    RlVec3 RlPredictBall(const RlTuning& tuning, const RlBallState& ball, float seconds)
    {
        V3 p = FromRl(ball.position);
        V3 v = FromRl(ball.linearVelocity);
        const float r = tuning.ball.radius;
        const float hx = tuning.arena.halfWidth - r;
        const float hz = tuning.arena.halfLength - r;
        const float top = tuning.arena.height - r;
        constexpr float kStep = 1.0f / 30.0f;
        float t = 0.0f;
        while (t < seconds)
        {
            const float dt = std::min(kStep, seconds - t);
            t += dt;
            v.y -= tuning.gravity * dt;
            p = p + v * dt;
            if (p.y < r)
            {
                p.y = r;
                v.y = -v.y * tuning.ball.restitution;
                v.x *= 0.97f;
                v.z *= 0.97f;
            }
            if (p.y > top)
            {
                p.y = top;
                v.y = -v.y * 0.6f;
            }
            if (std::fabs(p.x) > hx)
            {
                p.x = std::copysign(hx, p.x);
                v.x = -v.x * 0.7f;
            }
            const bool inGoalMouth = std::fabs(p.x) < tuning.arena.goalHalfWidth && p.y < tuning.arena.goalHeight;
            if (std::fabs(p.z) > hz && !inGoalMouth)
            {
                p.z = std::copysign(hz, p.z);
                v.z = -v.z * 0.7f;
            }
        }
        return {p.x, p.y, p.z};
    }

    namespace
    {
        // where and when a ball that is up in the air comes down to the height of a car (simple flight and bounces, no cars)
        bool LandingSpot(const RlTuning& tuning, const RlBallState& ball, V3& spot, float& seconds)
        {
            constexpr float kStep = 1.0f / 30.0f;
            for (float t = kStep; t <= 3.0f; t += kStep)
            {
                const RlVec3 p = RlPredictBall(tuning, ball, t);
                const RlVec3 q = RlPredictBall(tuning, ball, t - kStep);
                if (p.y < 2.2f && q.y >= p.y)
                {
                    spot = {p.x, p.y, p.z};
                    seconds = t;
                    return true;
                }
            }
            return false;
        }
    }

    void RlBotsAssignRoles(const RlTuning& tuning, const RlWorldState& world, int team, int teamSize, RlBotMemory* memories)
    {
        const int first = team * teamSize;
        const V3 ball = FromRl(world.ball.position);
        const float sign = static_cast<float>(AttackSign(team));
        const float ownZ = -tuning.arena.halfLength * sign;

        int alive[kMaxCars];
        float cost[kMaxCars];
        int count = 0;
        for (int i = 0; i < teamSize; ++i)
        {
            const RlCarState& c = world.cars[first + i];
            if (c.demolished)
            {
                continue;
            }
            const V3 p = FromRl(c.position);
            const bool wrongSide = (p.z - ball.z) * sign > 2.0f;
            cost[count] = FlatLen(ball - p) + (wrongSide ? 10.0f : 0.0f) - c.boost * 0.03f;
            alive[count++] = first + i;
        }
        if (count == 0)
        {
            return;
        }
        // the attacker is the cheapest (ties go to the lower slot), the others by distance to their own goal line
        int attacker = 0;
        for (int k = 1; k < count; ++k)
        {
            if (cost[k] < cost[attacker])
            {
                attacker = k;
            }
        }
        // the car that attacks now keeps the role unless another one is clearly (4 m) closer: no ping-pong between two cars that chase the same ball
        for (int k = 0; k < count; ++k)
        {
            if (k != attacker && memories[alive[k]].activeRole == BotAttacker && memories[alive[k]].role == BotAttacker && cost[k] <= cost[attacker] + 4.0f)
            {
                attacker = k;
                break;
            }
        }
        memories[alive[attacker]].role = BotAttacker;
        int defender = -1;
        float best = 1e9f;
        for (int k = 0; k < count; ++k)
        {
            if (k == attacker)
            {
                continue;
            }
            const float d = std::fabs(world.cars[alive[k]].position.z - ownZ);
            if (d < best)
            {
                best = d;
                defender = k;
            }
        }
        for (int k = 0; k < count; ++k)
        {
            if (k != attacker)
            {
                memories[alive[k]].role = (k == defender) ? BotDefender : BotSupport;
            }
        }
    }

    RlCarInput RlBotThink(const RlBotContext& ctx, RlBotMemory& mem, RlRng& rng)
    {
        const RlTuning& tuning = ctx.tuning;
        const RlWorldState& world = ctx.world;
        const RlCarState& car = world.cars[ctx.slot];
        RlCarInput in;
        const float dt = kTickDt;
        if (car.demolished)
        {
            mem.jumpPrev = 0;
            return in;
        }

        const Level level = LevelOf(ctx.level);
        const float hx = tuning.arena.halfWidth;
        const float hz = tuning.arena.halfLength;
        const float goalW = tuning.arena.goalHalfWidth;
        const float sign = static_cast<float>(AttackSign(ctx.team));
        const float ownZ = -hz * sign;
        const float enemyZ = hz * sign;

        const V3 pos = FromRl(car.position);
        const V3 vel = FromRl(car.linearVelocity);
        const V3 ball = FromRl(world.ball.position);
        const V3 ballVel = FromRl(world.ball.linearVelocity);
        const V3 up = Rotate(car.rotation, {0.0f, 1.0f, 0.0f});
        const V3 fwd = Rotate(car.rotation, {0.0f, 0.0f, 1.0f});
        const float forwardSpeed = Dot(vel, fwd);
        const float speed = Len(vel);
        const float ballDist = FlatLen(ball - pos);
        const bool grounded = car.wheelContacts > 0;

        mem.dodgeCooldown = std::max(mem.dodgeCooldown - dt, 0.0f);
        mem.arriveTimer = std::max(mem.arriveTimer - dt, 0.0f);

        // ---- the decision: role, target, aim error, flip intent
        mem.decisionTimer -= dt;
        if (mem.decisionTimer <= 0.0f)
        {
            mem.decisionTimer = level.interval * (0.8f + 0.4f * rng.Float01());
            mem.aimError = level.aimError * rng.Signed();
            mem.flipIntent = rng.Float01() < 0.12f ? 1 : 0;

            // when the car can be at the ball: its distance over the speed it will have, found with two forecasts
            const float carSpeed = Clamp(speed + 7.0f, 12.0f, tuning.car.maxSpeed);
            float t = Clamp(ballDist / carSpeed, 0.0f, 2.0f);
            RlVec3 first = RlPredictBall(tuning, world.ball, t);
            const float second = FlatLen({first.x - pos.x, 0.0f, first.z - pos.z}) / carSpeed;
            t = Clamp(second, 0.0f, 2.0f) * level.predict;
            RlVec3 predicted = RlPredictBall(tuning, world.ball, t);
            predicted.x = Clamp(predicted.x, -hx + 1.5f, hx - 1.5f);
            predicted.z = Clamp(predicted.z, -hz + 1.0f, hz - 1.0f);

            int role = mem.role;
            const bool danger = (ball.z - ownZ) * sign < 14.0f;
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

            mem.stopAtTarget = 0;
            mem.arriveTimer = 0.0f;
            if (role == BotAttacker)
            {
                float gx = 0.0f - predicted.x;
                float gz = enemyZ - predicted.z;
                if ((predicted.z - ownZ) * sign < 22.0f)
                {
                    // in front of the own goal: clear it to the side, away from the goal
                    gx = predicted.x >= 0.0f ? 14.0f : -14.0f;
                    gz = 22.0f * sign;
                }
                const float gl = std::max(std::sqrt(gx * gx + gz * gz), 0.01f);
                const float toGoalX = gx / gl;
                const float toGoalZ = gz / gl;
                if ((pos.z - predicted.z) * sign > 1.0f)
                {
                    // behind the ball (on the wrong side): get goal-side of it first
                    const float side = pos.x > predicted.x ? 4.0f : -4.0f;
                    mem.targetX = predicted.x + side;
                    mem.targetZ = predicted.z - 6.0f * sign;
                }
                else
                {
                    const float back = ballDist < 8.0f ? 0.0f : 1.0f;
                    mem.targetX = predicted.x - toGoalX * back;
                    mem.targetZ = predicted.z - toGoalZ * back;
                }
                // a ball in the air: wait for it where it comes down and arrive when it does, not before
                mem.arriveTimer = 0.0f;
                V3 landing;
                float landingTime = 0.0f;
                if ((ball.y > 3.0f || ballVel.y > 4.0f) && LandingSpot(tuning, world.ball, landing, landingTime) && landingTime > 0.35f)
                {
                    mem.targetX = Clamp(landing.x - toGoalX * 1.0f, -hx + 1.5f, hx - 1.5f);
                    mem.targetZ = Clamp(landing.z - toGoalZ * 1.0f, -hz + 1.0f, hz - 1.0f);
                    mem.arriveTimer = landingTime;
                }
                // short of boost: a pad that lies on the way is taken
                if (car.boost < 30.0f && !danger && mem.arriveTimer <= 0.0f)
                {
                    const float tx = mem.targetX - pos.x;
                    const float tz = mem.targetZ - pos.z;
                    const float tl = std::max(std::sqrt(tx * tx + tz * tz), 0.01f);
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
                        const float dx = px - pos.x;
                        const float dz = pz - pos.z;
                        const float dl = std::sqrt(dx * dx + dz * dz);
                        if (dl < 2.0f || dl > 15.0f || dl > 0.7f * tl || (dx * tx + dz * tz) / (dl * tl) < 0.6f)
                        {
                            continue;
                        }
                        const float score = dl - (big ? 6.0f : 0.0f);
                        if (score < bestScore)
                        {
                            bestScore = score;
                            mem.targetX = px;
                            mem.targetZ = pz;
                        }
                    }
                }
            }
            else if (role == BotDefender)
            {
                mem.targetX = Clamp(predicted.x * 0.2f + mem.patrolSide * 6.0f, -(goalW - 1.0f), goalW - 1.0f);
                // shadows the ball back to its own goal: the further it is, the further out the defender waits
                const float ballOut = std::max((ball.z - ownZ) * sign - 10.0f, 0.0f);
                mem.targetZ = ownZ + sign * (6.0f + std::min(24.0f, 0.25f * ballOut));
                mem.stopAtTarget = 1;
            }
            else if (role == BotBoost)
            {
                float bestD = 1e9f;
                float bx = 0.0f;
                float bz = ownZ * 0.5f;
                for (int i = 0; i < kPadCount; ++i)
                {
                    float px;
                    float pz;
                    const bool big = RlPadSpot(i, px, pz);
                    if (world.match.pads[i].respawnLeft > 0.0f)
                    {
                        continue;
                    }
                    const float d = std::sqrt((px - pos.x) * (px - pos.x) + (pz - pos.z) * (pz - pos.z)) - (big ? 8.0f : 0.0f);
                    if (d < bestD)
                    {
                        bestD = d;
                        bx = px;
                        bz = pz;
                    }
                }
                mem.targetX = bx;
                mem.targetZ = bz;
            }
            else
            {
                const float lane = ((ctx.slot % 2 != 0) ? 9.0f : -9.0f) + mem.patrolSide * 7.0f;
                mem.targetX = Clamp(predicted.x * 0.5f + lane, -hx + 3.0f, hx - 3.0f);
                mem.targetZ = Clamp(predicted.z - 12.0f * sign, -hz + 4.0f, hz - 4.0f);
                mem.stopAtTarget = 1;
            }
            mem.activeRole = role;
        }

        const bool attackerLike = mem.activeRole == BotAttacker;

        // ---- stuck detection (on a surface, not moving, while it should drive)
        const float flatSpeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        if (grounded && flatSpeed < 1.2f && !(mem.stopAtTarget != 0 && FlatLen({mem.targetX - pos.x, 0.0f, mem.targetZ - pos.z}) < 3.5f))
        {
            mem.stuckTimer += dt;
        }
        else
        {
            mem.stuckTimer = std::max(mem.stuckTimer - 2.0f * dt, 0.0f);
        }
        if (up.y < 0.4f && grounded)
        {
            mem.wallTimer += dt;
        }
        else
        {
            mem.wallTimer = 0.0f;
        }
        if (mem.recoverStage == 0 && (mem.stuckTimer > 4.0f || (mem.stuckTimer > 2.2f && up.y < 0.75f)))
        {
            mem.recoverStage = 3;
            mem.recoverTimer = 0.0f;
            mem.recoverSteer = rng.Float01() < 0.5f ? 1.0f : -1.0f;
            mem.stuckTimer = 1.0f;
        }
        if (mem.recoverStage == 0 && mem.stuckTimer > 1.5f)
        {
            mem.recoverStage = 1;
            mem.recoverTimer = 0.9f;
            mem.recoverSteer = rng.Float01() < 0.5f ? 1.0f : -1.0f;
        }

        // ---- lying on the roof or a side: one press of the jump turns the car back onto its wheels
        const bool lying = car.wheelContacts <= 1 && up.y < 0.5f && car.position.y < 1.0f && speed < 2.5f;
        if (lying)
        {
            mem.flipWait += dt;
            if (mem.flipWait > 0.15f && mem.jumpPrev == 0 && mem.flipWait < 0.25f + 1.2f)
            {
                in.jump = true;
            }
            if (mem.flipWait > 1.4f)
            {
                mem.flipWait = 0.0f;  // it did not work: again
            }
            mem.jumpPrev = in.jump ? 1 : 0;
            return in;
        }
        if (mem.recoverStage == 2)
        {
            mem.recoverStage = 0;
        }

        const bool wantsJumpEdge = mem.jumpPrev == 0;
        float diffToTarget = 0.0f;

        if (!grounded)
        {
            // ---------------- air
            if (mem.recoverStage == 4)
            {
                // the flip after the hop: sideways, then the car levels out by itself
                mem.flipWait += dt;
                if (mem.flipWait > 0.22f && car.canDodge && wantsJumpEdge)
                {
                    in.jump = true;
                    in.steer = mem.recoverSteer;
                    in.pitch = 0.0f;
                    mem.recoverStage = 0;
                    mem.dodgeCooldown = 1.0f;
                }
                else if (mem.jumpHold > 0.0f)
                {
                    mem.jumpHold -= dt;
                    in.jump = true;
                }
                if (mem.flipWait > 1.5f)
                {
                    mem.recoverStage = 0;
                }
                mem.jumpPrev = in.jump ? 1 : 0;
                return in;
            }
            if (mem.recoverStage == 3)
            {
                mem.recoverStage = 0;
            }

            // a dodge into the ball
            const V3 toBallLocal = RotateInverse(car.rotation, ball - pos);
            if (level.dodges && attackerLike && car.canDodge && !car.dodging && wantsJumpEdge && mem.dodgeCooldown <= 0.0f)
            {
                const float distance3 = Len(ball - pos);
                if (distance3 < 5.5f && ball.y > pos.y - 1.0f)
                {
                    const float flat = std::sqrt(toBallLocal.x * toBallLocal.x + toBallLocal.z * toBallLocal.z);
                    in.jump = true;
                    in.pitch = flat > 0.01f ? -Clamp(toBallLocal.z / flat, -1.0f, 1.0f) : -1.0f;
                    in.steer = flat > 0.01f ? Clamp(toBallLocal.x / flat, -1.0f, 1.0f) : 0.0f;
                    mem.dodgeCooldown = 1.0f;
                    mem.jumpPrev = 1;
                    return in;
                }
            }
            if (mem.jumpHold > 0.0f)
            {
                mem.jumpHold -= dt;
                in.jump = true;
            }

            if (level.airControl && !car.dodging)
            {
                // level the car out (the up of the world in the car's frame must come to +Y), turn towards the target
                const V3 u = RotateInverse(car.rotation, {0.0f, 1.0f, 0.0f});
                const V3 w = RotateInverse(car.rotation, FromRl(car.angularVelocity));
                in.roll = Clamp(3.0f * u.x + 0.35f * w.z, -1.0f, 1.0f);
                in.pitch = Clamp(-3.0f * u.z + 0.35f * w.x, -1.0f, 1.0f);
                const V3 toTarget = RotateInverse(car.rotation, {mem.targetX - pos.x, 0.0f, mem.targetZ - pos.z});
                const float yawError = std::atan2(toTarget.x, toTarget.z);
                in.steer = Clamp(1.5f * yawError - 0.25f * w.y, -1.0f, 1.0f);
                if (up.y < 0.0f)
                {
                    in.roll = in.roll >= 0.0f ? 1.0f : -1.0f;  // upside down: the roll error flips sign, pick a side and go
                }
            }
            // boost towards the ball in the air
            if (level.boostFreely && attackerLike && car.boost > 0.0f && ballDist < 14.0f && Dot(fwd, ball - pos) > 0.9f * Len(ball - pos))
            {
                in.boost = true;
            }
            mem.jumpPrev = in.jump ? 1 : 0;
            return in;
        }

        // ---------------- on a surface
        const V3 targetLocal = RotateInverse(car.rotation, {mem.targetX - pos.x, 0.0f, mem.targetZ - pos.z});
        float distance = std::sqrt(targetLocal.x * targetLocal.x + targetLocal.z * targetLocal.z);
        if (up.y < 0.7f)
        {
            // on a wall or the ceiling the "ground" target is far: use the direction in the car's own frame
            const V3 targetWorld = {mem.targetX - pos.x, -pos.y, mem.targetZ - pos.z};
            const V3 local = RotateInverse(car.rotation, targetWorld);
            distance = std::sqrt(local.x * local.x + local.z * local.z);
            diffToTarget = std::atan2(local.x, local.z);
        }
        else
        {
            diffToTarget = std::atan2(targetLocal.x, targetLocal.z);
        }
        if (mem.wallTimer > 1.5f)
        {
            // stuck up on the wall: head for the middle of the floor
            const V3 local = RotateInverse(car.rotation, {-pos.x, -pos.y, -pos.z});
            diffToTarget = std::atan2(local.x, local.z);
            distance = 30.0f;
        }
        const float diff = WrapPi(diffToTarget + mem.aimError);

        in.steer = Clamp(diff / 0.44f, -1.0f, 1.0f);
        in.throttle = 1.0f;
        if (mem.stopAtTarget != 0 && distance < 3.0f)
        {
            mem.standTimer += dt;
            if (mem.standTimer > 1.2f)
            {
                mem.standTimer = 0.0f;
                mem.patrolSide = -mem.patrolSide;
                mem.decisionTimer = 0.0f;
            }
            in.throttle = Clamp(-forwardSpeed * 0.4f, -1.0f, 0.0f);
            const V3 ballLocal = RotateInverse(car.rotation, ball - pos);
            in.steer = Clamp(WrapPi(std::atan2(ballLocal.x, ballLocal.z)) / 0.44f, -1.0f, 1.0f);
        }
        else if (mem.arriveTimer > 0.05f)
        {
            // the ball is still in the air: be at the spot when it lands, with speed to spare for the hit
            mem.standTimer = 0.0f;
            const float wanted = Clamp(distance / mem.arriveTimer + 3.0f, 3.0f, tuning.car.maxSpeed);
            in.throttle = Clamp((wanted - forwardSpeed) * 0.6f, -1.0f, 1.0f);
        }
        else if (mem.stopAtTarget != 0)
        {
            mem.standTimer = 0.0f;
            const float wanted = Clamp((distance - 2.0f) * 1.5f, 0.0f, tuning.car.maxSpeed);
            in.throttle = Clamp((wanted - forwardSpeed) * 0.6f, -1.0f, 1.0f);
        }

        // keep clear of the mates right in front of us (not the attacker)
        if (!attackerLike)
        {
            for (int i = 0; i < ctx.teamSize; ++i)
            {
                const int other = ctx.team * ctx.teamSize + i;
                if (other == ctx.slot || world.cars[other].demolished)
                {
                    continue;
                }
                const V3 rel = RotateInverse(car.rotation, FromRl(world.cars[other].position) - pos);
                const float d = std::sqrt(rel.x * rel.x + rel.z * rel.z);
                if (d < 5.0f && rel.z > 0.7f * d)
                {
                    in.steer = Clamp(in.steer - (rel.x > 0.0f ? 1.0f : -1.0f), -1.0f, 1.0f);
                }
            }
        }

        in.boost = car.boost > 0.0f && std::fabs(diff) < 0.26f &&
                   (distance > 12.0f || (attackerLike && ballDist < 10.0f)) && (level.boostFreely || distance > 25.0f) && mem.arriveTimer <= 0.05f;
        in.handbrake = std::fabs(diff) > 1.3f && distance > 4.0f && speed > 6.0f;

        // ---- jumps off the ground: a high ball in reach, or a flip into a ball that rolls
        if (mem.jumpHold > 0.0f)
        {
            mem.jumpHold -= dt;
            in.jump = true;
        }
        else if (level.dodges && attackerLike && wantsJumpEdge)
        {
            const bool highBall = ballDist < 4.5f && ball.y > 2.6f && ball.y < 6.0f && std::fabs(diff) < 0.52f;
            const bool rollingBall = mem.flipIntent != 0 && ballDist < 7.0f && ball.y < 2.5f && std::fabs(diff) < 0.17f && speed > 15.0f;
            if (highBall || rollingBall)
            {
                in.jump = true;
                mem.jumpHold = 0.15f;
                mem.dodgeCooldown = 0.0f;
            }
        }
        if (mem.wallTimer > 3.0f && wantsJumpEdge)
        {
            in.jump = true;  // let go of the wall
            mem.jumpHold = 0.1f;
            mem.wallTimer = 1.0f;
        }

        // ---- recovery from being stuck
        if (mem.recoverStage == 1)
        {
            mem.recoverTimer -= dt;
            in.throttle = -1.0f;
            in.steer = mem.recoverSteer;
            in.boost = false;
            in.handbrake = false;
            if (mem.recoverTimer <= 0.0f)
            {
                mem.recoverStage = 0;
                // still stuck after the reversing: the next try (the hop or the dodge) comes soon, not after another round of reversing
                mem.stuckTimer = std::min(mem.stuckTimer, 2.4f);
            }
        }
        else if (mem.recoverStage == 3 && car.wheelContacts < 3)
        {
            // wedged on two wheels: that is not the ground for a jump, the press is a dodge (RocketSim): a flip backwards and to the side
            in.jump = wantsJumpEdge;
            in.pitch = 1.0f;
            in.steer = mem.recoverSteer;
            mem.recoverStage = 0;
            mem.dodgeCooldown = 1.0f;
        }
        else if (mem.recoverStage == 3)
        {
            // hop (held for a moment) and then a sideways flip to get free: a car wedged on something
            in.jump = wantsJumpEdge;
            mem.jumpHold = 0.2f;
            mem.flipWait = 0.0f;
            mem.recoverStage = 4;
            mem.dodgeCooldown = 0.0f;
        }
        else if (mem.recoverStage == 4)
        {
            mem.recoverStage = 0;  // it came down again before the flip
        }

        mem.jumpPrev = in.jump ? 1 : 0;
        return in;
    }
}
