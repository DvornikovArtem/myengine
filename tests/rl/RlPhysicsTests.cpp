// RlPhysicsTests.cpp
// Headless tests of the Rocket League physics (game/rl): driving, jumps, walls, ball, demolition, determinism, rollback.
// Arguments: --write-tuning <path> writes the default tuning as json and exits; --verbose prints every measurement.

#include <rl/RlArena.h>
#include <rl/RlSim.h>
#include <rl/RlTuning.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace rl;

    int g_failures = 0;
    int g_checks = 0;
    bool g_verbose = false;
    bool g_trace = false;
    int g_traceDirection = 0;

    void Check(bool ok, const char* what, const char* file, int line)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("  FAIL %s (%s:%d)\n", what, file, line);
        }
    }
#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)

    // printf with doubles only (every argument is converted to double)
    template <typename... Args>
    void Note(const char* format, Args... args)
    {
        std::printf("  ");
        std::printf(format, static_cast<double>(args)...);
        std::printf("\n");
    }

    struct Vec
    {
        float x = 0, y = 0, z = 0;
    };
    Vec V(const RlVec3& v) { return {v.x, v.y, v.z}; }
    float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    float Len(Vec a) { return std::sqrt(Dot(a, a)); }

    // columns of the rotation matrix of a quaternion: the car's right, up and forward in the world
    Vec Rotate(const RlQuat& q, Vec v)
    {
        const float tx = 2.0f * (q.y * v.z - q.z * v.y);
        const float ty = 2.0f * (q.z * v.x - q.x * v.z);
        const float tz = 2.0f * (q.x * v.y - q.y * v.x);
        return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx)};
    }
    Vec Forward(const RlCarState& c) { return Rotate(c.rotation, {0, 0, 1}); }
    Vec Up(const RlCarState& c) { return Rotate(c.rotation, {0, 1, 0}); }
    Vec Right(const RlCarState& c) { return Rotate(c.rotation, {1, 0, 0}); }

    RlTuning g_tuning;

    using Inputs = std::vector<RlCarInput>;
    using Script = std::function<void(int tick, const RlWorldState& state, Inputs& inputs)>;

    // Runs `ticks` ticks; the script sets the inputs of every tick (inputs start neutral).
    void Run(RlSim& sim, int ticks, const Script& script)
    {
        Inputs inputs(static_cast<size_t>(sim.CarCount()));
        for (int t = 0; t < ticks; ++t)
        {
            for (RlCarInput& in : inputs)
            {
                in = RlCarInput{};
            }
            if (script)
            {
                script(t, sim.State(), inputs);
            }
            sim.Step(inputs.data());
        }
    }

    int Ticks(float seconds) { return static_cast<int>(std::lround(seconds * static_cast<float>(kTickRate))); }

    // The ball starts above the middle of the field; the tests of the car alone move it into a corner first.
    void QuietBall(RlSim& sim) { sim.ResetBall({-32.0f, 1.0f, 40.0f}); }

    // a car that has settled on its springs
    void Settle(RlSim& sim)
    {
        if (sim.CarCount() > 0 && std::fabs(sim.State().ball.position.y - 3.0f) < 0.01f)
        {
            QuietBall(sim);
        }
        Run(sim, Ticks(1.0f), nullptr);
    }

    // ---------------------------------------------------------------- arena

    void TestArenaShell()
    {
        std::printf("arena shell\n");
        RlArena arena(g_tuning.arena);
        CHECK(arena.TriangleCount() > 400);
        const RlVec3 lo = arena.BoundsMin();
        const RlVec3 hi = arena.BoundsMax();
        Note("bounds x %.2f..%.2f  y %.2f..%.2f", lo.x, hi.x, lo.y, hi.y);
        CHECK(std::fabs(hi.x - 40.96f) < 0.01f && std::fabs(lo.x + 40.96f) < 0.01f);
        CHECK(std::fabs(hi.y - 20.44f) < 0.01f && std::fabs(lo.y) < 0.01f);
        CHECK(std::fabs(hi.z - (51.2f + 8.8f)) < 0.01f && std::fabs(lo.z + (51.2f + 8.8f)) < 0.01f);

        // a closed shell: every edge is used by exactly two triangles, once in each direction
        std::map<std::pair<uint32_t, uint32_t>, int> edges;
        const std::vector<uint32_t>& idx = arena.Indices();
        for (size_t i = 0; i + 2 < idx.size(); i += 3)
        {
            for (size_t k = 0; k < 3; ++k)
            {
                ++edges[{idx[i + k], idx[i + (k + 1) % 3]}];
            }
        }
        int open = 0;
        int doubled = 0;
        for (const auto& [edge, count] : edges)
        {
            if (count > 1)
            {
                ++doubled;
            }
            if (edges.find({edge.second, edge.first}) == edges.end())
            {
                ++open;
                if (g_verbose)
                {
                    const RlVec3 a = arena.Vertices()[edge.first];
                    const RlVec3 b = arena.Vertices()[edge.second];
                    std::printf("    open edge (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)\n", a.x, a.y, a.z, b.x, b.y, b.z);
                }
            }
        }
        Note("edges %.0f, open (no twin) %.0f, same direction twice %.0f", static_cast<double>(edges.size()), open, doubled);
        CHECK(open == 0);
        CHECK(doubled == 0);

        // the normals look into the arena: the signed volume of the shell is negative-free (positive for outward normals, so negative here)
        double volume = 0.0;
        const std::vector<RlVec3>& v = arena.Vertices();
        for (size_t i = 0; i + 2 < idx.size(); i += 3)
        {
            const RlVec3 a = v[idx[i]];
            const RlVec3 b = v[idx[i + 1]];
            const RlVec3 c = v[idx[i + 2]];
            volume += (a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x)) / 6.0;
        }
        Note("signed volume %.0f", volume);
        CHECK(volume < -20000.0);  // about 82 x 102 x 20 m, minus the corners and ramps, plus the goals
        const std::string obj = arena.ToObj();
        CHECK(obj.find("\nf ") != std::string::npos);
    }

    // ---------------------------------------------------------------- driving

    // drives a car along z at the given x and prints where the speed or the height jumps
    void Probe(float x)
    {
        RlSim sim(g_tuning, 1);
        sim.ResetCar(0, x, -49.0f, 0.0f);
        sim.SetCarBoost(0, 100.0f);
        Settle(sim);
        float prevV = 0;
        float prevY = 0;
        Run(sim, Ticks(4.0f), [&](int t, const RlWorldState& st, Inputs& in) {
            const RlCarState& k = st.cars[0];
            in[0].throttle = 1.0f;
            in[0].boost = t < Ticks(2.0f);
            const float v = k.linearVelocity.z;
            if (t > 5 && (std::fabs(v - prevV) > 0.5f || std::fabs(k.position.y - prevY) > 0.02f))
            {
                std::printf("    t=%d z=%.2f v %.2f -> %.2f y %.3f -> %.3f contacts %d%c", t, k.position.z, prevV, v, prevY, k.position.y, k.wheelContacts, 10);
            }
            prevV = v;
            prevY = k.position.y;
        });
    }


    void TestSettle()
    {
        std::printf("car settles on the floor\n");
        RlSim sim(g_tuning, 1);
        if (g_trace)
        {
            Run(sim, Ticks(2.0f), [&](int t, const RlWorldState& st, Inputs&) {
                const RlCarState& k = st.cars[0];
                if ((t % 6 == 0 && t < 70) || (t >= 76 && t <= 92))
                    std::printf("    c=%d comp=(%.3f %.3f %.3f %.3f) t=%.3f y=%.4f v=(%.4f %.4f %.4f) w=(%.4f %.4f %.4f) up.y=%.5f\n", k.wheelContacts, k.wheelCompression[0], k.wheelCompression[1], k.wheelCompression[2], k.wheelCompression[3], static_cast<double>(t) / kTickRate, k.position.y,
                                k.linearVelocity.x, k.linearVelocity.y, k.linearVelocity.z, k.angularVelocity.x, k.angularVelocity.y, k.angularVelocity.z, Up(k).y);
            });
        }
        else
        {
            Settle(sim);
        }
        const RlCarState& c = sim.State().cars[0];
        Note("height %.3f, contacts %.0f, speed %.4f", c.position.y, c.wheelContacts, Len(V(c.linearVelocity)));
        CHECK(c.wheelContacts == 4);
        CHECK(c.position.y > 0.2f && c.position.y < 0.36f);
        CHECK(Len(V(c.linearVelocity)) < 0.05f);
        CHECK(Up(c).y > 0.999f);
    }

    void TestAcceleration()
    {
        std::printf("acceleration, top speed, brake\n");
        RlSim sim(g_tuning, 1);
        sim.ResetCar(0, 0.0f, -49.0f, 0.0f);
        Settle(sim);
        float v1 = 0, v3 = 0;
        Run(sim, Ticks(3.0f), [&](int t, const RlWorldState& s, Inputs& in) {
            in[0].throttle = 1.0f;
            if (t == Ticks(1.0f) - 1) v1 = Len(V(s.cars[0].linearVelocity));
            if (t == Ticks(3.0f) - 1) v3 = Len(V(s.cars[0].linearVelocity));
        });
        Note("throttle only: 1 s %.2f m/s, 3 s %.2f m/s (RL: ~9, 14.1)", v1, v3);
        CHECK(v1 > 8.0f && v1 < 10.5f);
        CHECK(v3 > 13.0f && v3 < 14.3f);

        // top speed with the boost (a fresh car at the other end so the run does not hit the wall)
        RlSim fast(g_tuning, 1);
        fast.ResetCar(0, 0.0f, -49.0f, 0.0f);
        fast.SetCarBoost(0, 100.0f);
        Settle(fast);
        float maxSpeed = 0;
        bool supersonic = false;
        Run(fast, Ticks(2.2f), [&](int, const RlWorldState& s, Inputs& in) {
            in[0].throttle = 1.0f;
            in[0].boost = true;
            maxSpeed = std::max(maxSpeed, Len(V(s.cars[0].linearVelocity)));
            supersonic = supersonic || s.cars[0].supersonic;
        });
        Note("boost: top speed %.2f m/s (RL 23), supersonic flag %.0f, boost left %.1f", maxSpeed, supersonic ? 1.0 : 0.0, fast.State().cars[0].boost);
        CHECK(maxSpeed > 22.4f && maxSpeed <= 23.05f);
        CHECK(supersonic);

        // brake from about 23 m/s: throttle reversed
        const float before = Len(V(fast.State().cars[0].linearVelocity));
        Run(fast, Ticks(0.2f), [&](int, const RlWorldState&, Inputs& in) { in[0].throttle = -1.0f; });
        const float afterBrake = Len(V(fast.State().cars[0].linearVelocity));
        Note("0.2 s of brake from %.1f: %.2f m/s (35 m/s^2 -> -7)", before, afterBrake);
        CHECK(afterBrake > before - 8.0f && afterBrake < before - 6.0f);
        Run(fast, Ticks(1.0f), [&](int t, const RlWorldState& st, Inputs&) {
            const RlCarState& k = st.cars[0];
            if (g_trace && g_traceDirection == 8 && t >= 72 && t <= 84)
                std::printf("    t=%.2f z=%.2f v=%.2f fwd.y=%.3f y=%.3f contacts=%d ball=(%.2f %.2f %.2f)\n", static_cast<double>(t) / kTickRate, k.position.z, k.linearVelocity.z, Forward(k).y, k.position.y, k.wheelContacts,
                            st.ball.position.x, st.ball.position.y, st.ball.position.z);
        });
        const float coasted = Len(V(fast.State().cars[0].linearVelocity));
        Note("1 s of coasting: %.2f m/s (5.25 m/s^2 -> -5.25)", coasted);
        CHECK(coasted < afterBrake - 4.0f && coasted > afterBrake - 6.5f);
    }

    void TestSteering()
    {
        std::printf("steering and drift\n");
        RlSim sim(g_tuning, 1);
        sim.ResetCar(0, -30.0f, -30.0f, 0.0f);
        Settle(sim);
        // hold about 10 m/s, then turn
        const auto cruise = [](const RlCarState& c) { return std::clamp((10.0f - Len(V(c.linearVelocity))) * 2.0f, -1.0f, 1.0f); };
        Run(sim, Ticks(2.0f), [&](int, const RlWorldState& s, Inputs& in) { in[0].throttle = cruise(s.cars[0]); });
        float yawRate = 0, side = 0, speed = 0;
        int n = 0;
        Run(sim, Ticks(1.0f), [&](int, const RlWorldState& s, Inputs& in) {
            in[0].throttle = cruise(s.cars[0]);
            in[0].steer = 1.0f;
            const RlCarState& c = s.cars[0];
            yawRate += Dot(V(c.angularVelocity), {0, 1, 0});
            side += std::fabs(Dot(V(c.linearVelocity), Right(c)));
            speed += Len(V(c.linearVelocity));
            ++n;
        });
        yawRate /= static_cast<float>(n);
        side /= static_cast<float>(n);
        speed /= static_cast<float>(n);
        Note("full steer at %.1f m/s: yaw rate %.2f rad/s (curvature table %.2f), sideways speed %.2f m/s", speed, yawRate, 0.235f * speed, side);
        CHECK(yawRate > 1.6f && yawRate < 3.2f);
        CHECK(side < 1.5f);

        // straight driving stays straight
        RlSim straight(g_tuning, 1);
        straight.ResetCar(0, 0.0f, -40.0f, 0.0f);
        Settle(straight);
        Run(straight, Ticks(3.0f), [&](int, const RlWorldState&, Inputs& in) { in[0].throttle = 1.0f; });
        const RlCarState& c = straight.State().cars[0];
        Note("3 s straight: x %.3f, yaw rate %.4f", c.position.x, c.angularVelocity.y);
        CHECK(std::fabs(c.position.x) < 0.1f);

        // the handbrake lets the tail slide: sideways speed stays much longer
        float slide[2] = {0, 0};
        for (int brake = 0; brake < 2; ++brake)
        {
            RlSim s(g_tuning, 1);
            s.ResetCar(0, -30.0f, -30.0f, 0.0f);
            Settle(s);
            Run(s, Ticks(2.0f), [&](int, const RlWorldState& st, Inputs& in) { in[0].throttle = cruise(st.cars[0]); });
            Run(s, Ticks(0.8f), [&](int, const RlWorldState& st, Inputs& in) {
                in[0].throttle = cruise(st.cars[0]);
                in[0].steer = 1.0f;
                in[0].handbrake = brake == 1;
            });
            Run(s, Ticks(0.15f), [&](int, const RlWorldState&, Inputs& in) {
                in[0].handbrake = brake == 1;
                in[0].steer = 0.0f;
            });
            const RlCarState& cs = s.State().cars[0];
            slide[brake] = std::fabs(Dot(V(cs.linearVelocity), Right(cs)));
        }
        Note("sideways speed after a turn: normal %.2f, handbrake %.2f", slide[0], slide[1]);
        CHECK(slide[1] > slide[0] + 0.8f);
    }

    // ---------------------------------------------------------------- jumps

    struct DodgeLanding
    {
        bool landed = false;
        float touchTime = 0;
        float upAtTouch = 0;
        float upLater = 0;
        int wheelsLater = 0;
    };

    // jump (held 0.25 s), dodge in `direction` (0 front, 1 back, 2 right, 3 left) at 0.4 s, no input afterwards
    DodgeLanding DodgeAndLand(const RlTuning& tuning, int direction, bool trace)
    {
        RlSim sim(tuning, 1);
        sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
        Settle(sim);
        DodgeLanding result;
        int touchTick = -1;
        const int press = Ticks(0.4f);
        Run(sim, Ticks(4.0f), [&](int t, const RlWorldState& s, Inputs& in) {
            const RlCarState& c = s.cars[0];
            if (trace && t % 6 == 0)
            {
                std::printf("    t=%.2f y=%.2f vy=%.2f up.y=%.2f fwd.y=%.2f w=(%.1f %.1f %.1f) contacts=%d canDodge=%d\n", static_cast<double>(t) / kTickRate,
                            c.position.y, c.linearVelocity.y, Up(c).y, Forward(c).y, c.angularVelocity.x, c.angularVelocity.y, c.angularVelocity.z,
                            c.wheelContacts, c.canDodge ? 1 : 0);
            }
            if (t > Ticks(0.7f) && c.wheelContacts > 0 && touchTick < 0)
            {
                touchTick = t;
                result.landed = true;
                result.upAtTouch = Up(c).y;
                result.touchTime = static_cast<float>(t) / kTickRate;
            }
            if (touchTick >= 0 && t == touchTick + Ticks(1.5f))
            {
                result.upLater = Up(c).y;
                result.wheelsLater = c.wheelContacts;
            }
            in[0].jump = (t >= 2 && t < Ticks(0.25f)) || (t >= press && t < press + 3);
            if (t >= press && t < press + 3)
            {
                in[0].pitch = direction == 0 ? -1.0f : direction == 1 ? 1.0f : 0.0f;
                in[0].steer = direction == 2 ? 1.0f : direction == 3 ? -1.0f : 0.0f;
            }
        });
        return result;
    }

    void SweepDodge()
    {
        std::printf("dodge sweep: spin time -> up.y at the first touch (front, back, right, left)\n");
        for (const float spinTime : {0.55f, 0.65f, 0.75f, 0.85f, 0.95f})
        {
            RlTuning tuning = g_tuning;
            tuning.car.dodgeSpinTime = spinTime;
            tuning.car.dodgeLockTime = spinTime;
            std::printf("  %.2f:", spinTime);
            for (int direction = 0; direction < 4; ++direction)
            {
                const DodgeLanding d = DodgeAndLand(tuning, direction, false);
                std::printf("  %5.2f (%.2f s, later %.2f)", d.upAtTouch, d.touchTime, d.upLater);
            }
            std::printf("\n");
        }
    }


    struct JumpResult
    {
        float rise = 0;
        float forwardGain = 0;
        float upGain = 0;
    };

    float ApexAfter(RlSim& sim, int ticks, const Script& script, float baseY)
    {
        float apex = baseY;
        Run(sim, ticks, [&](int t, const RlWorldState& s, Inputs& in) {
            if (script) script(t, s, in);
            apex = std::max(apex, s.cars[0].position.y);
        });
        return apex - baseY;
    }

    void TestJumps()
    {
        std::printf("jump, double jump, dodge\n");
        float tap = 0, held = 0, dbl = 0;
        {
            RlSim sim(g_tuning, 1);
            Settle(sim);
            const float base = sim.State().cars[0].position.y;
            tap = ApexAfter(sim, Ticks(2.5f), [&](int t, const RlWorldState&, Inputs& in) { in[0].jump = t == 2; }, base);
        }
        {
            RlSim sim(g_tuning, 1);
            Settle(sim);
            const float base = sim.State().cars[0].position.y;
            held = ApexAfter(sim, Ticks(2.5f), [&](int t, const RlWorldState&, Inputs& in) { in[0].jump = t >= 2 && t < Ticks(0.5f); }, base);
        }
        {
            RlSim sim(g_tuning, 1);
            Settle(sim);
            const float base = sim.State().cars[0].position.y;
            dbl = ApexAfter(sim, Ticks(3.0f), [&](int t, const RlWorldState&, Inputs& in) {
                // the first jump held for 0.2 s, released, a second press 0.35 s after the start, no direction
                in[0].jump = (t >= 2 && t < Ticks(0.25f)) || (t >= Ticks(0.35f) && t < Ticks(0.35f) + 3);
            }, base);
        }
        Note("rise: tap %.2f m, held %.2f m, held + double %.2f m", tap, held, dbl);
        CHECK(tap > 0.5f && tap < 0.9f);
        CHECK(held > 1.6f && held < 2.8f);
        CHECK(dbl > held + 1.5f && dbl < held + 3.0f);

        // the jump is not available twice in the air
        {
            RlSim sim(g_tuning, 1);
            Settle(sim);
            int jumpsSeen = 0;
            float prevVy = 0;
            Run(sim, Ticks(2.0f), [&](int t, const RlWorldState& s, Inputs& in) {
                in[0].jump = (t % 20) < 3;  // pressed every 1/6 s
                const float vy = s.cars[0].linearVelocity.y;
                if (t < Ticks(0.95f) && vy > prevVy + 2.2f && vy > 1.0f) ++jumpsSeen;  // an impulse of 2.92 m/s, not a landing bounce
                prevVy = vy;
            });
            Note("jump presses that gave a push in a row: %.0f (jump + double jump = 2)", jumpsSeen);
            CHECK(jumpsSeen == 2);
        }

        // dodge forward at about 9 m/s: speed gain along the car, lower than the double jump, a front flip rotation
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
            Settle(sim);
            float forwardBefore = 0;
            float forwardAfter = 0;
            float apex = 0;
            float spin = 0;
            const int start = Ticks(1.0f);
            Run(sim, Ticks(2.2f), [&](int t, const RlWorldState& s, Inputs& in) {
                const RlCarState& c = s.cars[0];
                if (t >= start) apex = std::max(apex, c.position.y);
                if (t >= start) spin = std::max(spin, Dot(V(c.angularVelocity), Right(c)));
                if (t < start) in[0].throttle = 1.0f;
                const int press = start + Ticks(0.3f);
                in[0].jump = (t >= start && t < start + Ticks(0.2f)) || (t >= press && t < press + 3);
                in[0].pitch = (t >= press && t < press + 3) ? -1.0f : 0.0f;
                if (t == press - 1) forwardBefore = Dot(V(c.linearVelocity), Forward(c));
                if (t == press + Ticks(0.1f)) forwardAfter = Dot(V(c.linearVelocity), {0, 0, 1});
            });
            Note("front flip: speed along z before %.2f, after %.2f, apex %.2f m, peak spin %.2f rad/s", forwardBefore, forwardAfter, apex, spin);
            CHECK(spin > 4.5f && spin <= 5.6f);
            CHECK(forwardAfter - forwardBefore > 3.5f && forwardAfter - forwardBefore < 8.0f);
            CHECK(apex < 2.5f);
        }

        // after a dodge the car comes down on its wheels (the flip is about one turn) with no input at all
        for (int direction = 0; direction < 4; ++direction)
        {
            const DodgeLanding d = DodgeAndLand(g_tuning, direction, g_trace && direction == g_traceDirection);
            Note("dodge %.0f (0 front, 1 back, 2 right, 3 left): first touch at %.2f s with up.y %.2f, 1.5 s later up.y %.2f on %.0f wheels", direction,
                 d.touchTime, d.upAtTouch, d.upLater, d.wheelsLater);
            CHECK(d.landed);
            CHECK(d.upAtTouch > 0.2f);
            CHECK(d.upLater > 0.95f && d.wheelsLater == 4);
        }
    }


    // a car on its roof flips back with a jump and a direction
    void TestFlipFromRoof()
    {
        std::printf("flip from the roof\n");
        for (int direction = 0; direction < 2; ++direction)
        {
            RlSim sim(g_tuning, 1);
            QuietBall(sim);
            sim.SetCarPose(0, {0.0f, 0.5f, -30.0f}, {0.0f, 0.0f, 1.0f, 0.0f});  // a half turn about Z: upside down
            Run(sim, Ticks(1.0f), nullptr);
            const RlCarState& rest = sim.State().cars[0];
            const bool onRoof = Up(rest).y < -0.95f && rest.wheelContacts == 0;
            bool upright = false;
            float uprightAt = 0;
            Run(sim, Ticks(3.0f), [&](int t, const RlWorldState& s, Inputs& in) {
                // a hop off the roof, then the flip
                const int flip = Ticks(0.25f);
                in[0].jump = (t >= 2 && t < Ticks(0.12f)) || (t >= flip && t < flip + 3);
                in[0].pitch = (t >= flip && t < flip + 3) ? (direction == 0 ? -1.0f : 1.0f) : 0.0f;
                const RlCarState& c = s.cars[0];
                if (!upright && t > Ticks(0.5f) && c.wheelContacts == 4 && Up(c).y > 0.95f)
                {
                    upright = true;
                    uprightAt = static_cast<float>(t) / kTickRate;
                }
            });
            Note("direction %.0f: rested on the roof %.0f, back on four wheels at %.2f s", direction, onRoof ? 1.0 : 0.0, uprightAt);
            CHECK(onRoof);
            CHECK(upright);
        }
    }

    // ---------------------------------------------------------------- walls and ceiling

    void TestWalls()
    {
        std::printf("ramp, wall, ceiling\n");
        RlSim sim(g_tuning, 1);
        sim.ResetCar(0, 25.0f, 0.0f, 1.5707963f);  // looks along +X at the wall
        sim.SetCarBoost(0, 100.0f);
        Settle(sim);
        float maxY = 0;
        int wallContactTicks = 0;
        int ceilingTicks = 0;
        float minUp = 1;
        Run(sim, Ticks(5.0f), [&](int t, const RlWorldState& s, Inputs& in) {
            const RlCarState& c = s.cars[0];
            in[0].throttle = 1.0f;
            in[0].boost = t < Ticks(4.0f);
            maxY = std::max(maxY, c.position.y);
            if (c.wheelContacts >= 3 && c.position.y > 4.0f && std::fabs(Up(c).y) < 0.5f) ++wallContactTicks;
            if (c.wheelContacts >= 3 && c.position.y > 19.0f && Up(c).y < -0.8f) ++ceilingTicks;
            minUp = std::min(minUp, Up(c).y);
            (void)t;
        });
        Note("max height %.2f m, ticks on the wall %.0f, ticks on the ceiling %.0f, min up.y %.2f", maxY, wallContactTicks, ceilingTicks, minUp);
        CHECK(maxY > 8.0f);
        CHECK(wallContactTicks > 60);

        // adhesion: let go of everything on the wall, the car stays on it
        RlSim stay(g_tuning, 1);
        stay.ResetCar(0, 25.0f, 0.0f, 1.5707963f);
        stay.SetCarBoost(0, 100.0f);
        Settle(stay);
        int releaseTick = -1;
        int heldTicks = 0;
        int totalAfter = 0;
        float minDistance = 99.0f;
        Run(stay, Ticks(5.0f), [&](int t, const RlWorldState& s, Inputs& in) {
            const RlCarState& c = s.cars[0];
            if (releaseTick < 0 && c.position.y > 7.0f && std::fabs(Up(c).y) < 0.3f && c.wheelContacts >= 3)
            {
                releaseTick = t;
            }
            if (releaseTick >= 0)
            {
                ++totalAfter;
                if (c.wheelContacts >= 3 && std::fabs(Up(c).y) < 0.4f) ++heldTicks;
                minDistance = std::min(minDistance, 40.96f - c.position.x);
                if (t - releaseTick > Ticks(1.0f)) return;
                return;
            }
            in[0].throttle = 1.0f;
            in[0].boost = true;
        });
        Note("after letting go at tick %.0f: %.0f of %.0f ticks on the wall with 3+ wheels (1 s of them matters)", releaseTick, heldTicks, totalAfter);
        CHECK(releaseTick > 0);
        CHECK(heldTicks > Ticks(0.8f));
    }

    // ---------------------------------------------------------------- ball

    void TestBallBounce()
    {
        std::printf("ball bounce\n");
        RlSim sim(g_tuning, 0);
        sim.ResetBall({0.0f, 12.0f, 0.0f});
        float before = 0, after = 0;
        float prevVy = 0;
        bool bounced = false;
        Run(sim, Ticks(4.0f), [&](int, const RlWorldState& s, Inputs&) {
            const float vy = s.ball.linearVelocity.y;
            if (!bounced && prevVy < -2.0f && vy > 0.0f)
            {
                bounced = true;
                before = -prevVy;
                after = vy;
            }
            prevVy = vy;
        });
        Note("first bounce: %.2f m/s down, %.2f m/s up, ratio %.3f (RL 0.6)", before, after, after / std::max(before, 0.01f));
        CHECK(bounced);
        CHECK(after / before > 0.54f && after / before < 0.66f);

        // spin: a ball rolling on the floor gets angular velocity, its speed stays
        RlSim roll(g_tuning, 0);
        roll.ResetBall({0.0f, 0.93f, 0.0f}, {8.0f, 0.0f, 0.0f});
        Run(roll, Ticks(1.0f), nullptr);
        const RlBallState& b = roll.State().ball;
        Note("rolling ball: speed %.2f, spin %.2f rad/s (rolls at v/r = %.2f)", Len(V(b.linearVelocity)), Len(V(b.angularVelocity)), 8.0 / 0.9275);
        CHECK(Len(V(b.angularVelocity)) > 3.0f);
        CHECK(Len(V(b.linearVelocity)) > 4.0f);

        // the speed limit
        RlSim fast(g_tuning, 0);
        fast.ResetBall({0.0f, 10.0f, 0.0f}, {0.0f, 0.0f, 150.0f});
        float maxSpeed = 0;
        Run(fast, Ticks(1.0f), [&](int, const RlWorldState& s, Inputs&) { maxSpeed = std::max(maxSpeed, Len(V(s.ball.linearVelocity))); });
        Note("ball launched at 150 m/s: top speed %.2f", maxSpeed);
        CHECK(maxSpeed <= 60.5f);
    }

    void TestBallStaysInside()
    {
        std::printf("the ball stays in the arena (walls, corners, ceiling, goals)\n");
        const RlVec3 hi = RlArena(g_tuning.arena).BoundsMax();
        const RlVec3 lo = RlArena(g_tuning.arena).BoundsMin();
        struct Shot
        {
            RlVec3 position;
            RlVec3 velocity;
        };
        const Shot shots[] = {
            {{0, 3, 0}, {55, 10, 0}},        // into the side wall
            {{0, 3, 0}, {-50, 30, 20}},      // into the wall and the ceiling
            {{0, 3, 0}, {30, 20, 45}},       // into a corner
            {{0, 3, 0}, {-35, 25, -40}},     // the other corner
            {{3, 3, 0}, {0, 25, 55}},        // at the goal mouth
            {{-5, 3, 0}, {5, 5, -58}},       // into the other goal
            {{0, 15, 0}, {0, 58, 0}},        // into the ceiling
        };
        int escaped = 0;
        for (const Shot& shot : shots)
        {
            RlSim sim(g_tuning, 0);
            sim.ResetBall(shot.position, shot.velocity);
            bool out = false;
            Run(sim, Ticks(8.0f), [&](int, const RlWorldState& s, Inputs&) {
                const RlVec3 p = s.ball.position;
                if (p.x < lo.x || p.x > hi.x || p.y < lo.y || p.y > hi.y || p.z < lo.z || p.z > hi.z) out = true;
            });
            if (out) ++escaped;
        }
        Note("shots that left the shell: %.0f of 7", escaped);
        CHECK(escaped == 0);
    }

    void TestBallHit()
    {
        std::printf("car hits the ball\n");
        RlSim sim(g_tuning, 1);
        sim.ResetCar(0, 0.0f, -20.0f, 0.0f);
        sim.ResetBall({0.0f, 0.93f, 0.0f});
        sim.SetCarBoost(0, 100.0f);
        Settle(sim);
        float carSpeedAtHit = 0;
        int hits = 0;
        float ballAfter = 0;
        Run(sim, Ticks(4.0f), [&](int, const RlWorldState& s, Inputs& in) {
            in[0].throttle = 1.0f;
            in[0].boost = true;
            if (hits == 0 && s.ball.linearVelocity.z > 0.5f) carSpeedAtHit = Len(V(s.cars[0].linearVelocity));
            ballAfter = std::max(ballAfter, Len(V(s.ball.linearVelocity)));
        });
        hits = sim.State().lastBallToucher == 0 ? 1 : 0;
        Note("car %.1f m/s into a ball at rest: ball top speed %.1f m/s", carSpeedAtHit, ballAfter);
        CHECK(hits == 1);
        CHECK(ballAfter > carSpeedAtHit * 1.2f);
        CHECK(ballAfter <= 60.5f);
    }

    // ---------------------------------------------------------------- demolition

    void TestDemolish()
    {
        std::printf("demolition\n");
        for (int fast = 0; fast < 2; ++fast)
        {
            RlSim sim(g_tuning, 2);
            sim.ResetCar(0, 0.0f, -40.0f, 0.0f);
            sim.ResetCar(1, 0.0f, 0.0f, 0.0f);
            sim.SetCarBoost(0, 100.0f);
            Settle(sim);
            bool demolished = false;
            float maxSpeed = 0;
            float respawnAt = -1;
            int demolitions = 0;
            Run(sim, Ticks(7.0f), [&](int t, const RlWorldState& s, Inputs& in) {
                in[0].throttle = 1.0f;
                in[0].boost = fast == 1;
                maxSpeed = std::max(maxSpeed, Len(V(s.cars[0].linearVelocity)));
                if (s.cars[1].demolished && !demolished)
                {
                    demolished = true;
                }
                if (demolished && !s.cars[1].demolished && respawnAt < 0)
                {
                    respawnAt = static_cast<float>(t) / kTickRate;
                }
            });
            demolitions = demolished ? 1 : 0;
            std::printf("  %s run: top speed %.1f, victim demolished %d, back at %.2f s\n", fast == 1 ? "boost" : "no-boost", maxSpeed, demolitions,
                        respawnAt);
            if (fast == 1)
            {
                CHECK(demolished);
                CHECK(respawnAt > 1.0f);
                CHECK(!sim.State().cars[0].demolished);
            }
            else
            {
                CHECK(!demolished);  // 14 m/s is not supersonic: a bump
            }
        }
    }

    // ---------------------------------------------------------------- determinism and rollback

    // a busy deterministic scenario: 4 cars and the ball with pseudo-random inputs
    void ScenarioInputs(int tick, int car, RlCarInput& in)
    {
        const unsigned h = static_cast<unsigned>(tick * 2654435761u) ^ static_cast<unsigned>((car + 1) * 40503u);
        const unsigned phase = static_cast<unsigned>(tick / 30 + car * 7);
        in.throttle = (phase % 5 == 4) ? -1.0f : 1.0f;
        in.steer = static_cast<float>(static_cast<int>(phase * 2654435761u >> 28) % 3 - 1);
        in.boost = (phase % 3) != 0;
        in.jump = (h >> 5) % 23 == 0 || ((tick / 4) % 40 < 3 && car % 2 == 0);
        in.pitch = static_cast<float>(static_cast<int>((h >> 9) % 3) - 1);
        in.handbrake = (phase % 7) == 6;
        in.roll = static_cast<float>(static_cast<int>((h >> 13) % 3) - 1);
    }

    void SetupScenario(RlSim& sim)
    {
        sim.ResetCar(0, -10.0f, -30.0f, 0.2f);
        sim.ResetCar(1, 10.0f, -30.0f, -0.2f);
        sim.ResetCar(2, -10.0f, 30.0f, 3.0f);
        sim.ResetCar(3, 10.0f, 30.0f, 3.3f);
        sim.ResetBall({0.0f, 4.0f, 0.0f}, {1.0f, 0.0f, 2.0f});
    }

    void RunScenario(RlSim& sim, int fromTick, int ticks)
    {
        Run(sim, ticks, [&](int t, const RlWorldState&, Inputs& in) {
            for (int car = 0; car < 4; ++car) ScenarioInputs(fromTick + t, car, in[static_cast<size_t>(car)]);
        });
    }

    void TestDeterminism()
    {
        std::printf("determinism: the same inputs give the same state\n");
        RlSim a(g_tuning, 4);
        RlSim b(g_tuning, 4);
        SetupScenario(a);
        SetupScenario(b);
        int diverged = -1;
        std::vector<uint8_t> sa, sb;
        for (int block = 0; block < 12 && diverged < 0; ++block)
        {
            RunScenario(a, block * 120, 120);
            RunScenario(b, block * 120, 120);
            a.SaveState(sa);
            b.SaveState(sb);
            if (sa != sb) diverged = block;
        }
        Note("first diverging second: %.0f (-1 = none); hash %.0f", diverged, static_cast<double>(RlSim::HashState(sa) % 100000));
        CHECK(diverged < 0);
        const RlWorldState& s = a.State();
        float distance = 0;
        for (int i = 0; i < 4; ++i) distance += Len(V(s.cars[i].position));
        CHECK(distance > 5.0f);  // the scenario moved the cars
    }

    void TestRollback()
    {
        std::printf("rollback: SaveState, N ticks, RestoreState, the same N ticks\n");
        RlSim sim(g_tuning, 4);
        SetupScenario(sim);
        RunScenario(sim, 0, 300);

        std::vector<uint8_t> snapshot;
        sim.SaveState(snapshot);
        RunScenario(sim, 300, 360);
        std::vector<uint8_t> first;
        sim.SaveState(first);

        CHECK(sim.RestoreState(snapshot));
        std::vector<uint8_t> again;
        sim.SaveState(again);
        CHECK(again == snapshot);  // restoring gives the saved bytes back
        RunScenario(sim, 300, 360);
        std::vector<uint8_t> second;
        sim.SaveState(second);
        Note("snapshot %.0f bytes; after 360 ticks equal: %.0f", static_cast<double>(snapshot.size()), first == second ? 1.0 : 0.0);
        CHECK(first == second);

        // the ring of snapshots, with a rollback in the middle of a demolition
        RlStateRing ring(8);
        RlSim fresh(g_tuning, 2);
        fresh.ResetCar(0, 0.0f, -40.0f, 0.0f);
        fresh.ResetCar(1, 0.0f, 0.0f, 0.0f);
        fresh.SetCarBoost(0, 100.0f);
        uint32_t tick = 0;
        const auto drive = [&](int ticks) {
            Run(fresh, ticks, [&](int, const RlWorldState&, Inputs& in) {
                in[0].throttle = 1.0f;
                in[0].boost = true;
            });
        };
        drive(Ticks(1.0f));
        for (int i = 0; i < 12; ++i)
        {
            drive(Ticks(0.25f));
            ++tick;
            ring.Push(tick, fresh);
        }
        std::vector<uint8_t> end;
        fresh.SaveState(end);
        const bool victimDown = fresh.State().cars[1].demolished;
        CHECK(ring.Restore(6, fresh));
        for (uint32_t t = 6; t < 12; ++t) drive(Ticks(0.25f));
        std::vector<uint8_t> replay;
        fresh.SaveState(replay);
        Note("ring rollback across a demolition (victim down: %.0f): equal %.0f", victimDown ? 1.0 : 0.0, end == replay ? 1.0 : 0.0);
        CHECK(end == replay);
    }

    // ---------------------------------------------------------------- tuning and speed

    // the render mesh in assets/models is the arena of the defaults: collision and picture are the same triangles
    void TestArenaObjInSync()
    {
        std::printf("assets/models/rl_arena.obj is the arena of the tuning\n");
        std::ifstream file(std::filesystem::u8path(RL_ARENA_OBJ), std::ios::binary);
        CHECK(static_cast<bool>(file));
        const std::string onDisk((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        CHECK(onDisk == RlArena(g_tuning.arena).ToRenderObj());
    }

    void TestTuning()
    {
        std::printf("tuning json\n");
        RlTuning loaded;
        std::string error;
        const bool ok = loaded.LoadFromFile(RL_TUNING_JSON, &error);
        if (!ok) std::printf("  %s\n", error.c_str());
        CHECK(ok);
        CHECK(loaded.ToJson() == g_tuning.ToJson());  // the file in assets/data is the defaults, nothing is missing from it
        RlTuning changed;
        CHECK(changed.LoadFromText("{\"car\": {\"mass\": 200.5}, \"gravity\": 9.0}"));
        CHECK(changed.car.mass == 200.5f && changed.gravity == 9.0f && changed.ball.radius == g_tuning.ball.radius);
        CHECK(!changed.LoadFromText("not json"));
    }

    void TestSpeed()
    {
        std::printf("tick cost with 8 cars\n");
        RlSim sim(g_tuning, 8);
        for (int i = 0; i < 8; ++i)
        {
            sim.ResetCar(i, -28.0f + 8.0f * static_cast<float>(i), (i % 2 == 0) ? -30.0f : 30.0f, (i % 2 == 0) ? 0.0f : 3.14159f);
            sim.SetCarBoost(i, 100.0f);
        }
        sim.ResetBall({0.0f, 4.0f, 0.0f}, {1.0f, 0.0f, 2.0f});
        Inputs inputs(8);
        const int warm = 240;
        const int measured = 1200;
        double worst = 0, total = 0;
        for (int t = 0; t < warm + measured; ++t)
        {
            for (int car = 0; car < 8; ++car)
            {
                RlCarInput in;
                ScenarioInputs(t, car, in);
                inputs[static_cast<size_t>(car)] = in;
            }
            const auto start = std::chrono::steady_clock::now();
            sim.Step(inputs.data());
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (t >= warm)
            {
                total += ms;
                worst = std::max(worst, ms);
            }
        }
        Note("8 cars + ball: average %.3f ms per tick, worst %.3f ms (a tick is 8.33 ms)", total / measured, worst);
        CHECK(total / measured < 4.0);
    }
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--verbose") == 0)
        {
            g_verbose = true;
        }
        else if (std::strcmp(argv[i], "--probe") == 0 && i + 1 < argc)
        {
            Probe(static_cast<float>(std::atof(argv[i + 1])));
            return 0;
        }
        else if (std::strcmp(argv[i], "--sweep-dodge") == 0)
        {
            SweepDodge();
            return 0;
        }
        else if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc)
        {
            g_trace = true;
            g_traceDirection = std::atoi(argv[i + 1]);
        }
        else if (std::strcmp(argv[i], "--write-arena") == 0 && i + 1 < argc)
        {
            return RlArena(RlTuning{}.arena).WriteRenderObj(argv[i + 1]) ? 0 : 1;
        }
        else if (std::strcmp(argv[i], "--write-tuning") == 0 && i + 1 < argc)
        {
            std::ofstream file(argv[i + 1], std::ios::binary | std::ios::trunc);
            const std::string text = RlTuning{}.ToJson() + "\n";
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            return file ? 0 : 1;
        }
    }
    (void)g_verbose;

    TestArenaShell();
    TestTuning();
    TestArenaObjInSync();
    TestSettle();
    TestAcceleration();
    TestSteering();
    TestJumps();
    TestWalls();
    TestFlipFromRoof();
    TestBallBounce();
    TestBallStaysInside();
    TestBallHit();
    TestDemolish();
    TestDeterminism();
    TestRollback();
    TestSpeed();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
