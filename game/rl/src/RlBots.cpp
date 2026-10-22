// RlBots.cpp

#include "RlBots.h"

#include "RlBotsBrain.h"

#include <rl/RlMatch.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rl
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

        using Level = brain::Level;
        Level LevelOf(int level) { return brain::LevelOf(level); }

        int AttackSign(int team) { return team == 0 ? 1 : -1; }  // blue attacks +Z
    }

    void RlBotsPlan(const RlTuning& tuning, const RlWorldState& world, const RlForecast& forecast, int team, int teamSize, int level, RlBotTeamPlan& plan,
                    RlBotMemory* memories)
    {
        brain::Plan(tuning, world, forecast, team, teamSize, level, plan, memories);
    }

    // ---- the forecast of the ball ----------------------------------------------------------------------------------------------------

    namespace
    {
        // The free space of the ball's centre: the prism "octagon x [R, H - R]" (the arena pulled in by the ramp radius R, the corners cut) and
        // everything within R - r (r = the radius of the ball) of it. A point farther than that is in the wall and is pushed back along
        // the line to the prism, which is the normal of the wall there (flat wall, quarter-pipe ramp or the chamfer).
        struct ArenaShape
        {
            float hx;
            float hz;
            float height;
            float ramp;
            float ballRadius;
            float verts[8][2];

            explicit ArenaShape(const RlTuning& t)
                : hx(t.arena.halfWidth - t.arena.floorRampRadius)
                , hz(t.arena.halfLength - t.arena.floorRampRadius)
                , height(t.arena.height)
                , ramp(t.arena.floorRampRadius)
                , ballRadius(t.ball.radius)
            {
                const float c = (t.arena.halfWidth + t.arena.halfLength - t.arena.cornerCut) - ramp * 1.41421356f;  // the chamfer line x + z = c
                const float a = c - hx;  // z of the corner on the side wall
                const float b = c - hz;  // x of the corner on the end wall
                const float v[8][2] = {{hx, a}, {b, hz}, {-b, hz}, {-hx, a}, {-hx, -a}, {-b, -hz}, {b, -hz}, {hx, -a}};
                std::memcpy(verts, v, sizeof(verts));
            }

            // the point of the octagon (as a filled polygon) nearest to (x, z)
            void ClosestXz(float x, float z, float& cx, float& cz) const
            {
                bool inside = true;
                for (int i = 0; i < 8 && inside; ++i)
                {
                    const float* p = verts[i];
                    const float* q = verts[(i + 1) & 7];
                    // the vertices run counter clockwise in (x, z) seen from +Y: the inside is on the left of every edge
                    if ((q[0] - p[0]) * (z - p[1]) - (q[1] - p[1]) * (x - p[0]) < 0.0f)
                    {
                        inside = false;
                    }
                }
                if (inside)
                {
                    cx = x;
                    cz = z;
                    return;
                }
                float best = 1e30f;
                for (int i = 0; i < 8; ++i)
                {
                    const float* p = verts[i];
                    const float* q = verts[(i + 1) & 7];
                    const float ex = q[0] - p[0];
                    const float ez = q[1] - p[1];
                    const float u = std::clamp(((x - p[0]) * ex + (z - p[1]) * ez) / (ex * ex + ez * ez), 0.0f, 1.0f);
                    const float px = p[0] + ex * u;
                    const float pz = p[1] + ez * u;
                    const float d = (x - px) * (x - px) + (z - pz) * (z - pz);
                    if (d < best)
                    {
                        best = d;
                        cx = px;
                        cz = pz;
                    }
                }
            }
        };
    }

    void RlForecastBall(const RlTuning& tuning, const RlBallState& ball, uint32_t tick, RlForecast& out)
    {
        const ArenaShape shape(tuning);
        const float limit = shape.ramp - shape.ballRadius;  // how far from the prism the centre may be
        const float gravity = tuning.gravity;
        const float restitution = tuning.ball.restitution;
        const float keep = 1.0f - tuning.ball.linearDamping;
        const float goalWidth = tuning.arena.goalHalfWidth;
        const float goalHeight = tuning.arena.goalHeight;
        const float endLine = tuning.arena.halfLength;

        float px = ball.position.x;
        float py = ball.position.y;
        float pz = ball.position.z;
        float vx = ball.linearVelocity.x;
        float vy = ball.linearVelocity.y;
        float vz = ball.linearVelocity.z;
        float wx = ball.angularVelocity.x;
        float wy = ball.angularVelocity.y;
        float wz = ball.angularVelocity.z;
        const float radius = shape.ballRadius;
        // friction of the contact: Jolt takes the square root of the product of the two surfaces
        const float mu = std::sqrt(tuning.ball.friction * tuning.arena.friction);
        constexpr float kSphereInertia = 0.4f;  // I = 0.4 m r^2
        const float maxSpin = tuning.ball.maxAngularSpeed;
        bool scored = false;

        constexpr int kSubsteps = 3;  // 60 Hz inside a sample of 0.05 s
        const float dt = kForecastDt / static_cast<float>(kSubsteps);
        const float dragStep = std::pow(keep, dt);

        out.valid = 1;
        out.tick = tick;
        for (int i = 0; i < kForecastSamples; ++i)
        {
            out.pos[i][0] = px;
            out.pos[i][1] = py;
            out.pos[i][2] = pz;
            if (scored)
            {
                continue;
            }
            for (int s = 0; s < kSubsteps; ++s)
            {
                vy -= gravity * dt;
                vx *= dragStep;
                vy *= dragStep;
                vz *= dragStep;
                px += vx * dt;
                py += vy * dt;
                pz += vz * dt;

                // through the mouth of a goal the ball is in the net: it stays there
                if (std::fabs(pz) > endLine && std::fabs(px) < goalWidth && py < goalHeight)
                {
                    scored = true;
                    break;
                }

                float cx;
                float cz;
                shape.ClosestXz(px, pz, cx, cz);
                const float cy = std::clamp(py, shape.ramp, shape.height - shape.ramp);
                const float dx = px - cx;
                const float dy = py - cy;
                const float dz = pz - cz;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > limit)
                {
                    // `n` points from the wall into the arena
                    const float nx = -dx / d;
                    const float ny = -dy / d;
                    const float nz = -dz / d;
                    px = cx + dx / d * limit;
                    py = cy + dy / d * limit;
                    pz = cz + dz / d * limit;
                    const float vn = vx * nx + vy * ny + vz * nz;
                    if (vn < 0.0f)
                    {
                        const float bounce = vn > -1.0f ? 0.0f : restitution;  // a ball that only touches rolls on
                        // the normal impulse (per unit of mass) and the friction at the point of the ball that touches the wall: r = -radius * n
                        const float jn = -(1.0f + bounce) * vn;
                        vx += jn * nx;
                        vy += jn * ny;
                        vz += jn * nz;
                        // velocity of the touching point: v + w x r
                        const float rx = -radius * nx;
                        const float ry = -radius * ny;
                        const float rz = -radius * nz;
                        float cx2 = vx + (wy * rz - wz * ry);
                        float cy2 = vy + (wz * rx - wx * rz);
                        float cz2 = vz + (wx * ry - wy * rx);
                        const float cn = cx2 * nx + cy2 * ny + cz2 * nz;
                        cx2 -= cn * nx;
                        cy2 -= cn * ny;
                        cz2 -= cn * nz;
                        const float slip = std::sqrt(cx2 * cx2 + cy2 * cy2 + cz2 * cz2);
                        if (slip > 1e-4f)
                        {
                            // to stop the slip a tangential impulse of slip / (1 + 1 / 0.4) is needed; friction allows mu * jn at most
                            const float jt = std::min(slip / (1.0f + 1.0f / kSphereInertia), mu * jn);
                            const float tx = -cx2 / slip * jt;
                            const float ty = -cy2 / slip * jt;
                            const float tz = -cz2 / slip * jt;
                            vx += tx;
                            vy += ty;
                            vz += tz;
                            const float k = 1.0f / (kSphereInertia * radius * radius);
                            wx += k * (ry * tz - rz * ty);
                            wy += k * (rz * tx - rx * tz);
                            wz += k * (rx * ty - ry * tx);
                            const float spin = std::sqrt(wx * wx + wy * wy + wz * wz);
                            if (spin > maxSpin)
                            {
                                wx *= maxSpin / spin;
                                wy *= maxSpin / spin;
                                wz *= maxSpin / spin;
                            }
                        }
                    }
                }
            }
        }
    }

    RlVec3 RlForecastAt(const RlForecast& forecast, uint32_t now, float seconds)
    {
        const float age = static_cast<float>(static_cast<int32_t>(now - forecast.tick)) * kTickDt;
        const float t = std::clamp(age + seconds, 0.0f, (kForecastSamples - 1) * kForecastDt);
        const float f = t / kForecastDt;
        const int i = std::min(static_cast<int>(f), kForecastSamples - 2);
        const float u = f - static_cast<float>(i);
        return {forecast.pos[i][0] + (forecast.pos[i + 1][0] - forecast.pos[i][0]) * u, forecast.pos[i][1] + (forecast.pos[i + 1][1] - forecast.pos[i][1]) * u,
                forecast.pos[i][2] + (forecast.pos[i + 1][2] - forecast.pos[i][2]) * u};
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

        // ---- the decision: role, target, direction of the hit (RlBotsBrain.cpp)
        mem.decisionTimer -= dt;
        if (mem.decisionTimer <= 0.0f)
        {
            brain::Decide(ctx, mem, rng);
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

            // the aerial: nose at the ball, boost, and a flip into it at the end
            if (mem.aerialMode == 2)
            {
                mem.aerialClock -= dt;
                const float left = std::max(mem.aerialClock, 0.0f);
                const RlVec3 aimAt = RlForecastAt(ctx.forecast, world.tick, std::max(left, 0.12f));
                V3 toAim = V3{aimAt.x, aimAt.y, aimAt.z} - pos;
                const float distanceAim = Len(toAim);
                if (distanceAim > 0.01f)
                {
                    toAim = toAim * (1.0f / distanceAim);
                }
                const V3 local = RotateInverse(car.rotation, toAim);
                const V3 angular = RotateInverse(car.rotation, FromRl(car.angularVelocity));
                const float yawError = std::atan2(local.x, local.z);
                const float pitchError = std::atan2(local.y, std::sqrt(local.x * local.x + local.z * local.z));
                in.pitch = Clamp(3.0f * pitchError + 0.35f * angular.x, -1.0f, 1.0f);
                in.steer = Clamp(2.0f * yawError - 0.3f * angular.y, -1.0f, 1.0f);
                const V3 u = RotateInverse(car.rotation, {0.0f, 1.0f, 0.0f});
                in.roll = Clamp(3.0f * u.x + 0.35f * angular.z, -1.0f, 1.0f);
                if (mem.jumpHold > 0.0f)
                {
                    mem.jumpHold -= dt;
                    in.jump = true;
                }
                const float alignment = fwd.x * toAim.x + fwd.y * toAim.y + fwd.z * toAim.z;
                in.boost = car.boost > 0.0f && alignment > 0.9f;
                const float distanceBall = Len(ball - pos);
                if (distanceBall < 2.7f && car.canDodge && !car.dodging && wantsJumpEdge && mem.jumpHold <= 0.0f && mem.dodgeCooldown <= 0.0f)
                {
                    const V3 toBallLocal2 = RotateInverse(car.rotation, ball - pos);
                    const float flat = std::sqrt(toBallLocal2.x * toBallLocal2.x + toBallLocal2.z * toBallLocal2.z);
                    in.jump = true;
                    in.pitch = flat > 0.01f ? -Clamp(toBallLocal2.z / flat, -1.0f, 1.0f) : -1.0f;
                    in.steer = flat > 0.01f ? Clamp(toBallLocal2.x / flat, -1.0f, 1.0f) : 0.0f;
                    in.roll = 0.0f;
                    mem.dodgeCooldown = 1.0f;
                    mem.aerialMode = 0;
                }
                if (mem.aerialClock < -0.8f || (mem.aerialClock < 0.0f && distanceBall > 9.0f))
                {
                    mem.aerialMode = 0;
                }
                mem.jumpPrev = in.jump ? 1 : 0;
                return in;
            }

            // the flip of the kick-off: straight ahead, a moment after the jump
            if (mem.speedFlip != 0)
            {
                mem.flipWait += dt;
                if (mem.flipWait > 0.12f && car.canDodge && wantsJumpEdge)
                {
                    in.jump = true;
                    in.pitch = -1.0f;
                    mem.speedFlip = 0;
                    mem.dodgeCooldown = 1.0f;
                }
                else if (mem.flipWait > 1.2f)
                {
                    mem.speedFlip = 0;
                }
                mem.jumpPrev = in.jump ? 1 : 0;
                return in;
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
        // a car that is far from facing its target slows down so that it can turn (the radius of a turn grows with the square of the speed)
        float wantedSpeed = tuning.car.maxSpeed;
        {
            const float off = std::fabs(diff);
            if (off > 1.0f)
            {
                wantedSpeed = 5.0f;
            }
            else if (off > 0.6f)
            {
                wantedSpeed = 9.0f;
            }
            else if (off > 0.3f)
            {
                wantedSpeed = 15.0f;
            }
            if (mem.kickoffMode != 0)
            {
                wantedSpeed = tuning.car.maxSpeed;
            }
        }
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
        else if (mem.aerialMode == 1)
        {
            // arrive at the point of the jump when it is time to jump
            mem.standTimer = 0.0f;
            const RlVec3 contact = RlForecastAt(ctx.forecast, world.tick, std::max(mem.aerialClock, 0.0f));
            const float flat = std::sqrt((contact.x - pos.x) * (contact.x - pos.x) + (contact.z - pos.z) * (contact.z - pos.z));
            const float flight = brain::AerialFlightTime(contact.y);
            const float timeToJump = std::max(mem.aerialClock - flight, 0.15f);
            const float jumpPoint = flat - std::max(forwardSpeed, 8.0f) * flight * 0.8f;
            wantedSpeed = Clamp(jumpPoint / timeToJump, 6.0f, tuning.car.maxSpeed);
            in.throttle = Clamp((wantedSpeed - forwardSpeed) * 0.6f, -1.0f, 1.0f);
        }
        else if (wantedSpeed < tuning.car.maxSpeed)
        {
            mem.standTimer = 0.0f;
            in.throttle = Clamp((wantedSpeed - forwardSpeed) * 0.5f, -1.0f, 1.0f);
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
                   (distance > 12.0f || (attackerLike && ballDist < 10.0f)) && (level.boostFreely || distance > 25.0f) && mem.arriveTimer <= 0.05f &&
                   forwardSpeed < wantedSpeed;
        in.handbrake = std::fabs(diff) > 1.3f && distance > 4.0f && speed > 6.0f;

        // ---- the aerial: the car drives under a high ball and jumps when the time is right
        if (mem.aerialMode == 1)
        {
            mem.aerialClock -= dt;
            // where the contact will be and how far the car is from it (flat), against how far it covers until then
            const RlVec3 contact = RlForecastAt(ctx.forecast, world.tick, std::max(mem.aerialClock, 0.0f));
            const float flightTime = brain::AerialFlightTime(contact.y);
            const float flatToContact = std::sqrt((contact.x - pos.x) * (contact.x - pos.x) + (contact.z - pos.z) * (contact.z - pos.z));
            const float covered = std::max(forwardSpeed, 6.0f) * std::max(mem.aerialClock, 0.1f);
            if (mem.aerialClock <= flightTime + 0.05f && std::fabs(diff) < 0.4f && wantsJumpEdge && car.boost >= 15.0f && forwardSpeed > 6.0f &&
                flatToContact <= covered * 1.25f + 3.0f && contact.y > 2.8f)
            {
                in.jump = true;
                mem.jumpHold = 0.2f;
                mem.aerialMode = 2;
                mem.dodgeCooldown = 0.0f;
            }
            else if (mem.aerialClock < -0.2f || (mem.aerialClock <= flightTime + 0.05f && flatToContact < covered * 0.5f - 2.0f))
            {
                mem.aerialMode = 0;  // too late or already past it
            }
        }
        else if (mem.aerialMode == 2)
        {
            mem.aerialClock -= dt;
            if (mem.aerialClock < -0.6f)
            {
                mem.aerialMode = 0;  // the jump did not happen
            }
        }

        // ---- jumps off the ground: a high ball in reach, or a flip into a ball that rolls
        if (mem.jumpHold > 0.0f)
        {
            mem.jumpHold -= dt;
            in.jump = true;
        }
        else if (level.dodges && attackerLike && wantsJumpEdge)
        {
            const bool highBall = ballDist < 4.5f && ball.y > 2.6f && ball.y < 6.0f && std::fabs(diff) < 0.52f;
            // a flip into a ball on the ground: the nose reaches it about 0.2 s after the jump, if the car heads at it along the planned hit
            const float toBall = Len(ball - pos);
            const float toContact = std::max(ballDist - 1.4f, 0.0f) / std::max(forwardSpeed, 4.0f);
            const bool aimedAtBall = toBall > 0.01f && Dot(fwd, ball - pos) > 0.93f * toBall;
            const float alongHit = (mem.hitDirX == 0.0f && mem.hitDirZ == 0.0f) ? 1.0f : (fwd.x * mem.hitDirX + fwd.z * mem.hitDirZ) / std::max(FlatLen(fwd), 0.01f);
            const bool rollingBall = ball.y < 2.0f && toContact > 0.1f && toContact < 0.27f && forwardSpeed > 9.0f && aimedAtBall && alongHit > 0.4f;
            // the kick-off: one flip on the way to the ball for speed (level 2), the flip into the ball comes after it
            const bool speedFlip = level.kickoffFlip && mem.kickoffMode != 0 && mem.speedFlip == 0 && ballDist > 13.0f && ballDist < 24.0f && forwardSpeed > 12.0f &&
                                   mem.dodgeCooldown <= 0.0f && std::fabs(diff) < 0.12f;
            if (speedFlip)
            {
                in.jump = true;
                mem.speedFlip = 1;
                mem.flipWait = 0.0f;
                mem.dodgeCooldown = 1.5f;
            }
            else if (highBall || rollingBall)
            {
                in.jump = true;
                mem.jumpHold = rollingBall ? 0.0f : 0.15f;
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
