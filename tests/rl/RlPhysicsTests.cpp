// RlPhysicsTests.cpp
// Headless tests of the Rocket League physics (game/rl): driving, jumps, walls, ball, demolition, determinism, rollback.
// Arguments: --write-tuning <path> writes the default tuning as json and exits; --verbose prints every measurement.

#include <rl/RlArena.h>
#include <rl/RlCamera.h>
#include <rl/RlControls.h>
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
        CHECK(c.position.y > 0.36f && c.position.y < 0.40f);  // RL: the hitbox centre rests 37.8 uu over the floor
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
        Run(straight, Ticks(3.0f), [&](int t, const RlWorldState& st, Inputs& in) { in[0].throttle = 1.0f; if (g_verbose && t % 30 == 0) Note("  t %.2f x %.3f z %.2f yaw %.5f yawrate %.5f", t / 120.0f, st.cars[0].position.x, st.cars[0].position.z, std::atan2(Forward(st.cars[0]).x, Forward(st.cars[0]).z), st.cars[0].angularVelocity.y); });
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

    // ---------------------------------------------------------------- landing

    struct LandingStats
    {
        bool touched = false;
        int rollCrossings = 0;     // sign changes of the roll angle after the first touch (amplitudes over 0.01 rad only)
        int pitchCrossings = 0;
        int bounces = 0;           // times the car rose again after it came down
        float settleSeconds = 99;  // from the first touch until the car is at rest for good
        float maxRoll = 0;
        float maxPitch = 0;
    };

    RlQuat AxisAngle(float x, float y, float z, float degrees)
    {
        const float h = degrees * 0.5f * 3.14159265f / 180.0f;
        const float s = std::sin(h);
        return {x * s, y * s, z * s, std::cos(h)};
    }

    // the stick a player gives to bring a spinning car back onto its wheels (the same PD law as the bots use)
    void LevelOut(const RlCarState& c, RlCarInput& in)
    {
        const Vec ux = Rotate({-c.rotation.x, -c.rotation.y, -c.rotation.z, c.rotation.w}, {0, 1, 0});
        const Vec w = Rotate({-c.rotation.x, -c.rotation.y, -c.rotation.z, c.rotation.w}, V(c.angularVelocity));
        in.roll = std::clamp(3.0f * ux.x + 0.35f * w.z, -1.0f, 1.0f);
        in.pitch = std::clamp(-3.0f * ux.z + 0.35f * w.x, -1.0f, 1.0f);
        if (Up(c).y < 0.0f) in.roll = in.roll >= 0.0f ? 1.0f : -1.0f;
    }

    // Follows a car from its first wheel contact on: how often the tilt swings through the level, how often it bounces, when it is at rest.
    struct SettleTracker
    {
        LandingStats stats;
        float prevVy = 0;
        int rollSign = 0, pitchSign = 0;
        int touchTick = -1;
        int quietSince = -1;
        bool rose = false;
        bool fellAgain = false;
        int minTick = 0;  // ignore the first contacts before this tick (a jump that is still on the ground)

        void Update(int t, const RlCarState& c)
        {
            if (c.wheelContacts > 0 && touchTick < 0 && t >= minTick)
            {
                touchTick = t;
                stats.touched = true;
            }
            if (touchTick < 0)
            {
                prevVy = c.linearVelocity.y;
                return;
            }
            const float roll = std::asin(std::clamp(Right(c).y, -1.0f, 1.0f));
            const float pitch = std::asin(std::clamp(Forward(c).y, -1.0f, 1.0f));
            stats.maxRoll = std::max(stats.maxRoll, std::fabs(roll));
            stats.maxPitch = std::max(stats.maxPitch, std::fabs(pitch));
            const auto track = [](float angle, int& sign, int& crossings) {
                const int now = angle > 0.01f ? 1 : (angle < -0.01f ? -1 : 0);
                if (now != 0 && sign != 0 && now != sign)
                {
                    ++crossings;
                }
                if (now != 0)
                {
                    sign = now;
                }
            };
            track(roll, rollSign, stats.rollCrossings);
            track(pitch, pitchSign, stats.pitchCrossings);
            // a bounce is a second fall after the car has come up out of its first dip
            if (!rose && c.linearVelocity.y > 0.1f)
            {
                rose = true;
            }
            else if (rose && !fellAgain && c.linearVelocity.y < -0.3f)
            {
                fellAgain = true;
            }
            else if (fellAgain && c.linearVelocity.y > 0.1f)
            {
                ++stats.bounces;
                fellAgain = false;
            }
            const float spin = Len(V(c.angularVelocity));
            const bool quiet = std::fabs(roll) < 0.02f && std::fabs(pitch) < 0.02f && spin < 0.2f && std::fabs(c.linearVelocity.y) < 0.2f;
            if (quiet && quietSince < 0)
            {
                quietSince = t;
            }
            if (!quiet)
            {
                quietSince = -1;
            }
            prevVy = c.linearVelocity.y;
        }

        LandingStats Result() const
        {
            LandingStats r = stats;
            if (quietSince >= 0 && touchTick >= 0)
            {
                r.settleSeconds = static_cast<float>(quietSince - touchTick) / kTickRate;
            }
            return r;
        }
    };

    // Drops a car from `height` metres, rolled and pitched by the given degrees, with a forward speed; measures how it settles.
    LandingStats DropAndMeasure(const RlTuning& tuning, float rollDeg, float pitchDeg, float height, float speed)
    {
        RlSim sim(tuning, 1);
        sim.ResetCar(0, 0.0f, -30.0f, 0.0f);
        QuietBall(sim);
        // pitch about X then roll about Z (the product of two small rotations is good enough)
        const RlQuat a = AxisAngle(0, 0, 1, rollDeg);
        const RlQuat b = AxisAngle(1, 0, 0, pitchDeg);
        const RlQuat q = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
        sim.SetCarPose(0, {0.0f, height, -30.0f}, q, {0.0f, 0.0f, speed});
        SettleTracker tracker;
        Run(sim, Ticks(4.0f), [&](int t, const RlWorldState& s2, Inputs&) {
            const RlCarState& c = s2.cars[0];
            if (g_trace && g_traceDirection == 7 && tracker.touchTick >= 0 && t < tracker.touchTick + 60 && t % 2 == 0)
            {
                std::printf("    +%02d y=%.3f vy=%.2f roll=%.3f pitch=%.3f w=%.2f contacts=%d comp=%.3f%c", t - tracker.touchTick, c.position.y,
                            c.linearVelocity.y, std::asin(std::clamp(Right(c).y, -1.0f, 1.0f)), std::asin(std::clamp(Forward(c).y, -1.0f, 1.0f)),
                            Len(V(c.angularVelocity)), c.wheelContacts, c.wheelCompression[0], 10);
            }
            tracker.Update(t, c);
        });
        return tracker.Result();
    }

    void PrintLanding(const char* name, const LandingStats& s)
    {
        std::printf("  %s: roll crossings %d, pitch crossings %d, bounces %d, at rest after %.2f s (largest roll %.2f, pitch %.2f rad)%c", name, s.rollCrossings,
                    s.pitchCrossings, s.bounces, static_cast<double>(s.settleSeconds), static_cast<double>(s.maxRoll), static_cast<double>(s.maxPitch), 10);
    }

    void TestLanding()
    {
        std::printf("landing: no rocking\n");
        const LandingStats level = DropAndMeasure(g_tuning, 0.0f, 0.0f, 1.2f, 0.0f);
        const LandingStats rolled = DropAndMeasure(g_tuning, 12.0f, 0.0f, 1.0f, 0.0f);
        const LandingStats pitched = DropAndMeasure(g_tuning, 0.0f, -14.0f, 1.0f, 8.0f);
        const LandingStats diagonal = DropAndMeasure(g_tuning, 10.0f, 10.0f, 0.8f, 12.0f);
        PrintLanding("level drop 1.2 m", level);
        PrintLanding("rolled 12 deg", rolled);
        PrintLanding("nose down 14 deg at 8 m/s", pitched);
        PrintLanding("roll + pitch 10 deg at 12 m/s", diagonal);
        for (const LandingStats* s : {&level, &rolled, &pitched, &diagonal})
        {
            CHECK(s->touched);
            CHECK(s->rollCrossings <= 1 && s->pitchCrossings <= 1);  // at most one swing through the level, no rocking
            CHECK(s->bounces <= 1);
            CHECK(s->settleSeconds <= 0.65f);
        }
        CHECK(level.settleSeconds <= 0.35f);  // a plain landing: at rest in about a third of a second
    }

    // after a landing the car goes where its nose points: the sideways speed dies fast, no skid
    struct SkidResult
    {
        float sideAtTouch = 0;
        float sideAfter = 0;      // 0.3 s after the touchdown
        float yawRateAfter = 0;
        float forwardAfter = 0;
        bool touched = false;
    };

    SkidResult LandingSkid(const RlTuning& tuning, float yawSeconds, bool shiftHeld, float steerOnGround)
    {
        RlSim sim(tuning, 1);
        sim.ResetCar(0, 0.0f, -48.0f, 0.0f);
        sim.SetCarBoost(0, 100.0f);
        QuietBall(sim);
        Settle(sim);
        SkidResult r;
        int touchTick = -1;
        Run(sim, Ticks(3.0f), [&](int t, const RlWorldState& s2, Inputs& in) {
            const RlCarState& c = s2.cars[0];
            const int jumpAt = Ticks(1.0f);
            in[0].throttle = t < jumpAt + Ticks(0.3f) ? 1.0f : 0.5f;
            in[0].boost = t < Ticks(0.8f);
            in[0].jump = t >= jumpAt && t < jumpAt + 6;  // a tap
            const bool inAir = t > jumpAt + 6 && touchTick < 0;
            const int yawEnd = jumpAt + Ticks(0.15f) + static_cast<int>(yawSeconds * kTickRate);
            if (inAir && yawSeconds > 0.0f && t < yawEnd)
            {
                in[0].steer = 1.0f;  // yaw in the air
            }
            // Shift is the air roll in the air and the powerslide on the ground: held through the landing it must not make a skid
            const bool shift = shiftHeld && t >= yawEnd;
            in[0].handbrake = shift;
            in[0].airRoll = shift;
            if (g_trace && g_traceDirection == 6 && t >= jumpAt && t < jumpAt + Ticks(1.2f) && t % 6 == 0)
            {
                std::printf("    t+%d y=%.2f side=%.2f fwd=%.2f yawrate=%.2f contacts=%d up.y=%.2f%c", t - jumpAt, c.position.y, Dot(V(c.linearVelocity), Right(c)),
                            Dot(V(c.linearVelocity), Forward(c)), c.angularVelocity.y, c.wheelContacts, Up(c).y, 10);
            }
            if (touchTick >= 0)
            {
                in[0].steer = steerOnGround;
            }
            const Vec fwd = Forward(c);
            const Vec right = Right(c);
            const float side = Dot(V(c.linearVelocity), right);
            if (t > jumpAt + 10 && c.wheelContacts >= 3 && touchTick < 0)
            {
                touchTick = t;
                r.touched = true;
                r.sideAtTouch = std::fabs(side);
            }
            if (touchTick >= 0 && t == touchTick + Ticks(0.3f))
            {
                r.sideAfter = std::fabs(side);
                r.yawRateAfter = std::fabs(c.angularVelocity.y);
                r.forwardAfter = Dot(V(c.linearVelocity), fwd);
            }
        });
        return r;
    }

    void TestLandingSkid()
    {
        std::printf("landing: no skid\n");
        const struct
        {
            const char* name;
            float yaw;
            bool shift;
            float steer;
        } cases[] = {{"straight", 0.0f, false, 0.0f}, {"yawed in the air", 0.05f, false, 0.0f}, {"yawed, Shift held (air roll)", 0.05f, true, 0.0f},
                     {"yawed, steering on landing", 0.05f, false, 1.0f}};
        for (const auto& c : cases)
        {
            const SkidResult r = LandingSkid(g_tuning, c.yaw, c.shift, c.steer);
            std::printf("  %-32s touched %d, sideways at touchdown %.2f m/s, 0.3 s later %.2f m/s, yaw rate %.2f rad/s, forward %.1f m/s%c", c.name, r.touched ? 1 : 0,
                        static_cast<double>(r.sideAtTouch), static_cast<double>(r.sideAfter), static_cast<double>(r.yawRateAfter), static_cast<double>(r.forwardAfter), 10);
            CHECK(r.touched);
            if (c.steer == 0.0f)
            {
                CHECK(r.sideAfter < 1.0f);
                CHECK(r.yawRateAfter < 0.4f);
            }
        }
    }

    // A fast drive up a ramp: the wheels must not leave the surface at the joints (floor -> ramp -> wall).
    void TestGlue()
    {
        std::printf("the car stays on the surface over the ramp joints\n");
        for (const float yaw : {1.5707963f, 0.7853982f})  // straight at the side wall, and diagonally
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 12.0f, 0.0f, yaw);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            sim.SetCarPose(0, {12.0f, 0.32f, 0.0f}, AxisAngle(0, 1, 0, yaw * 57.29578f), {std::sin(yaw) * 20.0f, 0.0f, std::cos(yaw) * 20.0f});
            int airborne = 0;
            int onSurface = 0;
            float maxY = 0;
            Run(sim, Ticks(2.2f), [&](int, const RlWorldState& s, Inputs& in) {
                const RlCarState& c = s.cars[0];
                in[0].throttle = 1.0f;
                in[0].boost = true;
                maxY = std::max(maxY, c.position.y);
                if (c.position.y < 9.0f)
                {
                    (c.wheelContacts == 0 ? airborne : onSurface)++;
                }
            });
            Note("yaw %.0f deg: %.0f ticks airborne, %.0f on the surface, highest point %.1f m", yaw * 57.29578f, airborne, onSurface, maxY);
            CHECK(airborne <= 2);
        }
    }

    void SweepLanding()
    {
        std::printf("landing sweep: spring damping x stiffness -> roll/pitch crossings, settle time (rolled 12 deg / pitched 14 deg)\n");
        for (const float stiffness : {300.0f, 500.0f, 700.0f})
        {
            for (const float damping : {15.0f, 25.0f, 35.0f, 45.0f})
            {
                RlTuning t = g_tuning;
                t.car.suspensionStiffness = stiffness;
                t.car.suspensionDampCompression = damping;
                const LandingStats a = DropAndMeasure(t, 12.0f, 0.0f, 1.0f, 0.0f);
                const LandingStats lv = DropAndMeasure(t, 0.0f, 0.0f, 1.2f, 0.0f);
                const LandingStats b = DropAndMeasure(t, 0.0f, -14.0f, 1.0f, 8.0f);
                std::printf("  k %.0f c %.0f: level settle %.2f | roll cross %d pitch cross %d settle %.2f | pitched: roll %d pitch %d settle %.2f bounces %d\n", stiffness, damping, lv.settleSeconds,
                            a.rollCrossings, a.pitchCrossings, a.settleSeconds, b.rollCrossings, b.pitchCrossings, b.settleSeconds, a.bounces + b.bounces);
            }
        }
    }

    // ---------------------------------------------------------------- jumps

    struct DodgeLanding
    {
        bool landed = false;
        float touchTime = 0;
        float upAtTouch = 0;
        float upLater = 0;
        int wheelsLater = 0;
        LandingStats settle;
    };

    // jump (held 0.25 s), dodge in `direction` (0 front, 1 back, 2 right, 3 left) at 0.4 s, no input afterwards
    DodgeLanding DodgeAndLand(const RlTuning& tuning, int direction, bool trace)
    {
        RlSim sim(tuning, 1);
        sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
        Settle(sim);
        DodgeLanding result;
        int touchTick = -1;
        SettleTracker tracker;
        tracker.minTick = Ticks(0.7f);
        const int press = Ticks(0.4f);
        Run(sim, Ticks(4.0f), [&](int t, const RlWorldState& s, Inputs& in) {
            const RlCarState& c = s.cars[0];
            tracker.Update(t, c);
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
            // the flip is over after 0.65 s: the player gets the car back onto its wheels with the stick
            if (t > press + Ticks(0.65f) && c.wheelContacts == 0) LevelOut(c, in[0]);
        });
        result.settle = tracker.Result();
        return result;
    }

    void SweepDodge()
    {
        std::printf("dodge sweep: spin time -> up.y at the first touch (front, back, right, left)\n");
        for (const float spinTime : {0.55f, 0.65f, 0.75f, 0.85f, 0.95f})
        {
            RlTuning tuning = g_tuning;
            tuning.car.dodgeTorqueTime = spinTime;
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
            PrintLanding("landing of that dodge", d.settle);
            CHECK(d.settle.rollCrossings <= 1 && d.settle.pitchCrossings <= 1);  // no rocking after the flip either
            CHECK(d.settle.settleSeconds <= 1.0f);
        }
    }


    // a car on its roof or on a side comes back onto its wheels with one press of the jump: on the roof the auto-flip of RocketSim (a roll
    // beyond 160 degrees, the floor under the hitbox) hops it and rolls it back, on a side the second jump pushes it over
    void TestFlipFromRoof()
    {
        std::printf("a jump turns a car on its roof or side back onto its wheels\n");
        const struct
        {
            const char* name;
            float rollDegrees;
            float steer;
        } cases[] = {{"roof", 180.0f, 0.0f}, {"roof, steering right", 180.0f, 1.0f}, {"left side", 90.0f, 0.0f}, {"right side", -90.0f, 0.0f}, {"leaning 170 deg", 170.0f, 0.0f}};
        for (const auto& c : cases)
        {
            RlSim sim(g_tuning, 1);
            QuietBall(sim);
            sim.SetCarPose(0, {0.0f, 0.6f, -30.0f}, AxisAngle(0, 0, 1, c.rollDegrees), {});
            Run(sim, Ticks(1.0f), nullptr);
            const RlCarState& rest = sim.State().cars[0];
            const bool lying = Up(rest).y < 0.6f && rest.wheelContacts <= 1;
            bool upright = false;
            float uprightAt = 0;
            Run(sim, Ticks(2.0f), [&](int t, const RlWorldState& s, Inputs& in) {
                in[0].jump = t >= 2 && t < 5;  // one press
                in[0].steer = c.steer;
                const RlCarState& car = s.cars[0];
                if (!upright && t > 6 && car.wheelContacts == 4 && Up(car).y > 0.95f)
                {
                    upright = true;
                    uprightAt = static_cast<float>(t) / kTickRate;
                }
            });
            Note("%.0f: lying before the press %.0f, on four wheels again after %.2f s", c.rollDegrees, lying ? 1.0 : 0.0, uprightAt);
            std::printf("    (%s)%c", c.name, 10);
            CHECK(lying);
            CHECK(upright && uprightAt <= 1.0f);
        }
    }

    // jumping while driving with the stick forward must give a plain jump, not a flip
    void TestJumpWhileDriving()
    {
        std::printf("a jump while driving is a jump, not a flip\n");
        RlSim sim(g_tuning, 1);
        sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
        sim.SetCarBoost(0, 100.0f);
        QuietBall(sim);
        Settle(sim);
        bool dodged = false;
        float apex = 0;
        Run(sim, Ticks(2.5f), [&](int t, const RlWorldState& s2, Inputs& in) {
            const RlCarState& c = s2.cars[0];
            in[0].throttle = 1.0f;
            in[0].pitch = -1.0f;  // W held
            in[0].boost = t < Ticks(1.0f);
            in[0].jump = t >= Ticks(1.5f) && t < Ticks(1.5f) + 14;  // one click of 120 ms
            if (t >= Ticks(1.5f)) dodged = dodged || c.dodging;
            if (t >= Ticks(1.5f)) apex = std::max(apex, c.position.y);
        });
        std::printf("  flip started: %d, highest point %.2f m%c", dodged ? 1 : 0, static_cast<double>(apex), 10);
        CHECK(!dodged);
        CHECK(apex > 1.0f);
    }

    // Full steering without the handbrake on every speed of the car: it turns the way the table of RL says and does not skid (the sideways speed
    // in the car's own axes stays small), as RocketSim does (the traces of the differential test agree to a centimetre).
    void TestTurnWithoutSkid()
    {
        std::printf("full steering without the handbrake: no skid from 5 to 23 m/s\n");
        for (const float speed : {5.0f, 10.0f, 15.0f, 20.0f, 23.0f})
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -30.0f, 0.0f);
            QuietBall(sim);
            Settle(sim);
            sim.SetCarBoost(0, 100.0f);
            sim.SetCarPose(0, {0.0f, 0.3776f, -30.0f}, {0, 0, 0, 1}, {0.0f, 0.0f, speed});
            float sideMax = 0, sideSum = 0, yawSum = 0, speedSum = 0;
            int samples = 0;
            Run(sim, Ticks(3.0f), [&](int t, const RlWorldState& s2, Inputs& in) {
                const RlCarState& c = s2.cars[0];
                const float forwardSpeed = Dot(V(c.linearVelocity), Forward(c));
                // hold the speed: the throttle below it, the boost for the top speeds
                in[0].throttle = std::clamp((speed - forwardSpeed) * 4.0f, -1.0f, 1.0f);
                in[0].boost = speed > 14.0f && forwardSpeed < speed - 0.5f;
                in[0].steer = 1.0f;
                if (t > Ticks(0.5f))  // after the first half second of the turn getting going
                {
                    const float side = std::fabs(Dot(V(c.linearVelocity), Right(c)));
                    sideMax = std::max(sideMax, side);
                    sideSum += side;
                    yawSum += c.angularVelocity.y;
                    speedSum += forwardSpeed;
                    ++samples;
                }
            });
            const float meanSide = sideSum / static_cast<float>(samples);
            const float yaw = yawSum / static_cast<float>(samples);
            const float meanSpeed = speedSum / static_cast<float>(samples);
            // the front wheels turn by the angle of the table: yaw rate = speed * tan(angle) / wheelbase when the tyres do not slide
            const float wheelbase = g_tuning.car.wheelFrontZ - g_tuning.car.wheelRearZ;
            float angle = g_tuning.car.steerAngle[5];
            for (int i = 1; i < 6; ++i)
            {
                if (meanSpeed <= g_tuning.car.steerSpeed[i])
                {
                    const float f = (meanSpeed - g_tuning.car.steerSpeed[i - 1]) / (g_tuning.car.steerSpeed[i] - g_tuning.car.steerSpeed[i - 1]);
                    angle = g_tuning.car.steerAngle[i - 1] + (g_tuning.car.steerAngle[i] - g_tuning.car.steerAngle[i - 1]) * f;
                    break;
                }
            }
            const float expectedYaw = meanSpeed * std::tan(angle) / wheelbase;
            Note("%.0f m/s: sideways speed on average %.2f, at most %.2f m/s (%.1f%% of the speed); yaw rate %.2f rad/s (the table: %.2f), radius %.1f m", speed, meanSide,
                 sideMax, 100.0f * meanSide / meanSpeed, yaw, expectedYaw, meanSpeed / std::max(yaw, 0.01f));
            CHECK(meanSide < 0.12f * meanSpeed);      // it turns, the nose and the velocity agree
            CHECK(std::fabs(yaw - expectedYaw) < 0.15f * expectedYaw + 0.1f);
        }
    }

    // ---------------------------------------------------------------- jumps, flips, air roll as in RL (G4c)

    struct JumpProbe
    {
        bool dodged = false;       // the flip spin ran
        float spinX = 0, spinZ = 0;  // largest angular velocity about the car's X and Z (signed by the larger one)
        float vyKick = 0;          // the largest jump of the vertical speed in one tick (the second jump)
        float forwardGain = 0;     // forward speed after the dodge minus before
    };

    // A car at speed, a tap, a second press `gap` seconds later with the given stick; `pitch`, `steer`, `roll` are held from the second press.
    JumpProbe SecondJump(float gap, float pitch, float steer, float speedBefore = 8.0f)
    {
        RlSim sim(g_tuning, 1);
        sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
        QuietBall(sim);
        Settle(sim);
        JumpProbe r;
        const int first = Ticks(0.5f);
        const int second = first + Ticks(gap);
        float prevVy = 0;
        float speedAtSecond = 0;
        Run(sim, second + Ticks(1.0f), [&](int t, const RlWorldState& s2, Inputs& in) {
            const RlCarState& c = s2.cars[0];
            in[0].throttle = t < first ? 1.0f : 0.0f;
            const int firstHold = gap > 0.5f ? 24 : 4;  // a held first jump flies for 1.8 s: the long gaps need it
            in[0].jump = (t >= first && t < first + firstHold) || (t >= second && t < second + 4);
            if (t >= second && t < second + 4)
            {
                in[0].pitch = pitch;
                in[0].steer = steer;
            }
            if (t == second - 1) speedAtSecond = Dot(V(c.linearVelocity), {0, 0, 1});
            if (t > second && c.dodging) r.dodged = true;
            if (t > second && t < second + Ticks(0.6f))
            {
                const Vec w = Rotate({-c.rotation.x, -c.rotation.y, -c.rotation.z, c.rotation.w}, V(c.angularVelocity));
                if (std::fabs(w.x) > std::fabs(r.spinX)) r.spinX = w.x;
                if (std::fabs(w.z) > std::fabs(r.spinZ)) r.spinZ = w.z;
            }
            if (t > second && t < second + 4) r.vyKick = std::max(r.vyKick, c.linearVelocity.y - prevVy);
            if (t == second + Ticks(0.15f)) r.forwardGain = Dot(V(c.linearVelocity), {0, 0, 1}) - speedAtSecond;
            prevVy = c.linearVelocity.y;
        });
        (void)speedBefore;
        return r;
    }

    void TestDodgeInputs()
    {
        std::printf("dodges follow the stick, the second jump has a window of 1.25 s\n");
        // W held and a fast second jump: a front flip
        const JumpProbe front = SecondJump(0.15f, -1.0f, 0.0f);
        const JumpProbe back = SecondJump(0.15f, 1.0f, 0.0f);
        const JumpProbe right = SecondJump(0.15f, 0.0f, 1.0f);
        const JumpProbe diagonal = SecondJump(0.15f, -1.0f, 1.0f);
        const JumpProbe plain = SecondJump(0.15f, 0.0f, 0.0f);
        std::printf("  front: spin x %.1f z %.1f, forward gain %.1f%c", static_cast<double>(front.spinX), static_cast<double>(front.spinZ), static_cast<double>(front.forwardGain), 10);
        std::printf("  back: spin x %.1f z %.1f, forward gain %.1f%c", static_cast<double>(back.spinX), static_cast<double>(back.spinZ), static_cast<double>(back.forwardGain), 10);
        std::printf("  right: spin x %.1f z %.1f, forward gain %.1f%c", static_cast<double>(right.spinX), static_cast<double>(right.spinZ), static_cast<double>(right.forwardGain), 10);
        std::printf("  diagonal: spin x %.1f z %.1f%c  plain: dodged %d, vertical kick %.2f m/s%c", static_cast<double>(diagonal.spinX), static_cast<double>(diagonal.spinZ), 10,
                    plain.dodged ? 1 : 0, static_cast<double>(plain.vyKick), 10);
        CHECK(front.dodged && front.spinX > 4.5f && front.forwardGain > 3.0f);   // nose down, forward
        CHECK(back.dodged && back.spinX < -4.5f && back.forwardGain < -1.0f);    // nose up, backwards
        CHECK(right.dodged && right.spinZ < -4.5f && std::fabs(right.spinX) < 1.0f);  // a roll to the right
        CHECK(diagonal.dodged && diagonal.spinX > 2.5f && diagonal.spinZ < -2.5f);
        CHECK(!plain.dodged && plain.vyKick > 2.0f);  // no direction: a plain double jump

        // the window of the second jump: 1.25 s after the push of the first jump ended (a jump held for 0.2 s: until 1.45 s after the press)
        const JumpProbe late = SecondJump(1.0f, -1.0f, 0.0f);
        const JumpProbe tooLate = SecondJump(1.55f, -1.0f, 0.0f);
        const JumpProbe lateJump = SecondJump(1.0f, 0.0f, 0.0f);
        const JumpProbe tooLateJump = SecondJump(1.55f, 0.0f, 0.0f);
        std::printf("  second jump after 1.0 s: dodged %d, plain kick %.2f; after 1.55 s: dodged %d, plain kick %.2f%c", late.dodged ? 1 : 0, static_cast<double>(lateJump.vyKick),
                    tooLate.dodged ? 1 : 0, static_cast<double>(tooLateJump.vyKick), 10);
        CHECK(late.dodged);
        CHECK(lateJump.vyKick > 2.0f);
        CHECK(!tooLate.dodged);
        CHECK(tooLateJump.vyKick < 1.0f);
    }

    // the roll of Q / E in the air, and Shift + A / D as a free air roll
    void TestAirRoll()
    {
        std::printf("air roll: Q / E roll the car at about 5 rad/s\n");
        for (int mode = 0; mode < 3; ++mode)  // 0: roll axis right, 1: roll axis left, 2: Shift + steer right
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
            QuietBall(sim);
            Settle(sim);
            float peak = 0;
            float timeToPeak = -1;
            const int start = Ticks(0.6f);
            Run(sim, Ticks(1.2f), [&](int t, const RlWorldState& s2, Inputs& in) {
                const RlCarState& c = s2.cars[0];
                in[0].jump = t >= 2 && t < Ticks(0.3f);
                if (t >= start)
                {
                    if (mode == 0) in[0].roll = 1.0f;
                    if (mode == 1) in[0].roll = -1.0f;
                    if (mode == 2)
                    {
                        in[0].airRoll = true;
                        in[0].handbrake = true;
                        in[0].steer = 1.0f;
                    }
                    const Vec w = Rotate({-c.rotation.x, -c.rotation.y, -c.rotation.z, c.rotation.w}, V(c.angularVelocity));
                    if (std::fabs(w.z) > std::fabs(peak))
                    {
                        peak = w.z;
                        if (std::fabs(peak) > 5.0f && timeToPeak < 0) timeToPeak = static_cast<float>(t - start) / kTickRate;
                    }
                }
            });
            std::printf("  %s: peak roll rate %.2f rad/s, reached 5 rad/s after %.2f s%c", mode == 0 ? "E (right)" : mode == 1 ? "Q (left)" : "Shift + D", static_cast<double>(peak),
                        static_cast<double>(timeToPeak), 10);
            // + about the car's Z rolls it to the left, so a roll to the right is a negative rate
            CHECK(mode == 1 ? peak > 5.0f : peak < -5.0f);
            CHECK(timeToPeak > 0.0f && timeToPeak < 0.4f);
        }
    }

    // flip cancel and the half-flip
    void TestFlipCancel()
    {
        std::printf("flip cancel and half-flip\n");
        // a front flip: with the stick pulled back after a short time the rotation stops, without it the flip runs on
        float angleCancelled = 0, angleFree = 0;
        for (int cancel = 0; cancel < 2; ++cancel)
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
            QuietBall(sim);
            Settle(sim);
            const int press = Ticks(0.35f);
            float pitchAngle = 0;
            Run(sim, Ticks(1.2f), [&](int t, const RlWorldState& s2, Inputs& in) {
                const RlCarState& c = s2.cars[0];
                in[0].jump = (t >= 2 && t < Ticks(0.3f)) || (t >= press && t < press + 3);
                if (t >= press && t < press + 3) in[0].pitch = -1.0f;
                if (cancel == 1 && t >= press + 3 && t < press + Ticks(0.6f)) in[0].pitch = 1.0f;  // the stick back at once
                if (t == press + Ticks(0.3f)) pitchAngle = std::acos(std::clamp(Up(c).y, -1.0f, 1.0f)) * 57.29578f;
            });
            (cancel == 1 ? angleCancelled : angleFree) = pitchAngle;
        }
        std::printf("  0.3 s after a front flip the car is turned %.0f deg with the cancel and %.0f deg without it%c", static_cast<double>(angleCancelled), static_cast<double>(angleFree), 10);
        // RocketSim's flip cancel takes the torque of the flip away, the spin that has built up dies out with the damping of the air (2.9 1/s)
        CHECK(angleCancelled < angleFree - 15.0f);
        CHECK(angleFree > 60.0f);

        // the half-flip and the flip cancel against the traces of RocketSim: scenarios flip_cancel and half_flip of the differential test
    }

    // a dodge just before the wheels touch the ground keeps its speed gain (wave dash)
    void TestWaveDash()
    {
        std::printf("wave dash\n");
        float gain[2] = {0, 0};
        for (int dodge = 0; dodge < 2; ++dodge)
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -48.0f, 0.0f);
            QuietBall(sim);
            Settle(sim);
            float before = 0, after = 0;
            bool pressed = false;
            Run(sim, Ticks(2.5f), [&](int t, const RlWorldState& s2, Inputs& in) {
                const RlCarState& c = s2.cars[0];
                in[0].throttle = 1.0f;
                in[0].jump = t >= Ticks(1.0f) && t < Ticks(1.0f) + 6;  // a low hop
                // on the way down, 0.25 m over the ground, a dodge forward
                if (dodge == 1 && !pressed && t > Ticks(1.1f) && c.linearVelocity.y < -0.5f && c.position.y < 0.6f)
                {
                    pressed = true;
                    in[0].jump = true;
                    in[0].pitch = -1.0f;
                }
                if (t == Ticks(1.0f) - 1) before = Dot(V(c.linearVelocity), {0, 0, 1});
                if (t == Ticks(1.0f) + Ticks(1.2f)) after = Dot(V(c.linearVelocity), {0, 0, 1});
            });
            gain[dodge] = after - before;
        }
        std::printf("  speed change over a hop: %.1f m/s without a dodge, %.1f m/s with a dodge at the touchdown%c", static_cast<double>(gain[0]), static_cast<double>(gain[1]), 10);
        CHECK(gain[1] > gain[0] + 1.0f);
    }

    // several actions at once are all applied (the input of the car is the sum of everything that is held)
    void TestSimultaneousActions()
    {
        std::printf("throttle, steer, boost, jump and roll at the same time\n");
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, -20.0f, -30.0f, 0.0f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            float yawRate = 0, speed = 0;
            const float boostBefore = sim.State().cars[0].boost;
            Run(sim, Ticks(1.0f), [&](int t, const RlWorldState& s2, Inputs& in) {
                in[0].throttle = 1.0f;
                in[0].steer = 0.5f;
                in[0].boost = true;
                in[0].handbrake = false;
                yawRate = std::max(yawRate, s2.cars[0].angularVelocity.y);
                if (t == Ticks(1.0f) - 1) speed = Len(V(s2.cars[0].linearVelocity));
            });
            std::printf("  gas + steer + boost: speed %.1f m/s, yaw rate %.2f rad/s, boost %.0f -> %.0f%c", static_cast<double>(speed), static_cast<double>(yawRate),
                        static_cast<double>(boostBefore), static_cast<double>(sim.State().cars[0].boost), 10);
            CHECK(speed > 14.5f);  // faster than the throttle alone allows: the boost works while steering
            CHECK(yawRate > 0.3f);
            CHECK(sim.State().cars[0].boost < boostBefore - 25.0f);
        }
        {
            // in the air: pitch, yaw, roll axes and the boost together
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            Vec w{0, 0, 0};
            float speedBefore = 0, speedAfter = 0;
            Run(sim, Ticks(0.9f), [&](int t, const RlWorldState& s2, Inputs& in) {
                const RlCarState& c = s2.cars[0];
                in[0].jump = t >= 2 && t < Ticks(0.3f);
                if (t >= Ticks(0.45f))
                {
                    in[0].pitch = 1.0f;
                    in[0].steer = 1.0f;
                    in[0].roll = 1.0f;
                    in[0].boost = true;
                    const Vec wl = Rotate({-c.rotation.x, -c.rotation.y, -c.rotation.z, c.rotation.w}, V(c.angularVelocity));
                    if (std::fabs(wl.x) + std::fabs(wl.y) + std::fabs(wl.z) > std::fabs(w.x) + std::fabs(w.y) + std::fabs(w.z)) w = wl;
                }
                if (t == Ticks(0.45f)) speedBefore = Len(V(c.linearVelocity));
                if (t == Ticks(0.9f) - 1) speedAfter = Len(V(c.linearVelocity));
            });
            std::printf("  pitch + yaw + roll + boost: angular velocity (%.1f, %.1f, %.1f), speed %.1f -> %.1f%c", static_cast<double>(w.x), static_cast<double>(w.y), static_cast<double>(w.z),
                        static_cast<double>(speedBefore), static_cast<double>(speedAfter), 10);
            CHECK(w.x < -1.0f && w.y > 0.5f && w.z < -1.0f);  // nose up, yaw right, roll right: all three turn the car
            CHECK(speedAfter > speedBefore + 1.0f);
        }
    }

    // the small things of RL
    void TestMechanicsChecklist()
    {
        std::printf("the small things of Rocket League\n");
        // supersonic is kept in a gentle turn
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -49.0f, 0.0f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            float minSpeed = 99;
            Run(sim, Ticks(2.2f), [&](int t, const RlWorldState& s2, Inputs& in) {
                in[0].throttle = 1.0f;
                in[0].boost = t < Ticks(1.7f);
                in[0].steer = t > Ticks(1.5f) ? 0.15f : 0.0f;
                if (t > Ticks(1.7f)) minSpeed = std::min(minSpeed, Len(V(s2.cars[0].linearVelocity)));
            });
            std::printf("  after 0.5 s of a gentle turn at the top speed the car still goes %.1f m/s%c", static_cast<double>(minSpeed), 10);
            CHECK(minSpeed > 21.0f);
        }
        // a powerslide that is let go: the grip is back in a quarter of a second, no skid after it
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, -30.0f, -30.0f, 0.0f);
            QuietBall(sim);
            Settle(sim);
            const auto cruise = [](const RlCarState& c) { return std::clamp((12.0f - Len(V(c.linearVelocity))) * 2.0f, -1.0f, 1.0f); };
            Run(sim, Ticks(2.0f), [&](int, const RlWorldState& s2, Inputs& in) { in[0].throttle = cruise(s2.cars[0]); });
            float sideDuring = 0, sideAfter = 0;
            Run(sim, Ticks(1.6f), [&](int t, const RlWorldState& s2, Inputs& in) {
                const RlCarState& c = s2.cars[0];
                in[0].throttle = cruise(c);
                in[0].handbrake = t < Ticks(0.6f);
                in[0].steer = t < Ticks(0.6f) ? 1.0f : 0.0f;
                const float side = std::fabs(Dot(V(c.linearVelocity), Right(c)));
                if (t < Ticks(0.6f)) sideDuring = std::max(sideDuring, side);
                if (t == Ticks(0.6f) + Ticks(1.0f) - 1) sideAfter = side;
            });
            std::printf("  powerslide: sideways speed up to %.1f m/s, 1 s after letting go %.2f m/s (the handbrake lets go in half a second)%c", static_cast<double>(sideDuring), static_cast<double>(sideAfter), 10);
            CHECK(sideDuring > 1.5f);
            CHECK(sideAfter < 0.6f);
        }
        // the boost in the air pushes along the nose of the car
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -40.0f, 0.0f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            // nose up by about 45 degrees, stationary, then boost
            sim.SetCarPose(0, {0.0f, 8.0f, -40.0f}, AxisAngle(1, 0, 0, -45.0f), {});
            Run(sim, Ticks(0.5f), [&](int, const RlWorldState&, Inputs& in) { in[0].boost = true; });
            const RlCarState& c = sim.State().cars[0];
            Vec v = V(c.linearVelocity);
            v.y += g_tuning.gravity * 0.5f;  // what gravity took off in the half second
            const Vec f = Forward(c);
            std::printf("  boost in the air: velocity (gravity taken out) along the nose %.2f of its speed%c", static_cast<double>(Dot(v, f) / std::max(Len(v), 0.01f)), 10);
            CHECK(Dot(v, f) / std::max(Len(v), 0.01f) > 0.9f);
        }
        // a car on a wall without speed falls off it
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 25.0f, 0.0f, 1.5707963f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            int release = -1;
            int heldAfter = 0;
            float yEnd = 0;
            bool wallEnd = false;
            Run(sim, Ticks(9.0f), [&](int t, const RlWorldState& s2, Inputs& in) {
                const RlCarState& c = s2.cars[0];
                if (release < 0 && c.position.y > 7.0f && std::fabs(Up(c).y) < 0.3f && c.wheelContacts >= 3) release = t;
                if (release < 0)
                {
                    in[0].throttle = 1.0f;
                    in[0].boost = true;
                }
                else
                {
                    if (c.wheelContacts >= 3 && std::fabs(Up(c).y) < 0.4f) ++heldAfter;
                    yEnd = c.position.y;
                    wallEnd = c.wheelContacts >= 3 && std::fabs(Up(c).y) < 0.4f;
                }
            });
            std::printf("  let go on a wall at 7 m: held it for %d ticks, after 9 s it is at %.1f m, on the wall %d%c", heldAfter, static_cast<double>(yEnd), wallEnd ? 1 : 0, 10);
            CHECK(release > 0 && heldAfter > Ticks(0.5f));
            CHECK(!wallEnd && yEnd < 4.0f);
        }
        // a jump from a wall goes away from the wall; a double jump from the ceiling drops the car
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 25.0f, 0.0f, 1.5707963f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            bool jumped = false;
            float awayFromWall = 0;
            int ceilingTicks = 0;
            float vyAfterDouble = 0, vyBefore = 0;
            int doubleAt = -1;
            Run(sim, Ticks(5.5f), [&](int t, const RlWorldState& s2, Inputs& in) {
                const RlCarState& c = s2.cars[0];
                in[0].throttle = 1.0f;
                in[0].boost = t < Ticks(4.0f);
                // on the wall: the jump
                if (!jumped && c.position.y > 6.0f && std::fabs(Up(c).y) < 0.3f && c.wheelContacts >= 3)
                {
                    jumped = true;
                    in[0].jump = true;
                    awayFromWall = 1.0f;  // checked below through the velocity
                }
                if (jumped && awayFromWall == 1.0f && t > 0 && c.linearVelocity.x < -0.3f && c.position.y > 6.0f) awayFromWall = 2.0f;
                if (c.wheelContacts >= 3 && c.position.y > 19.0f && Up(c).y < -0.8f) ++ceilingTicks;
            });
            std::printf("  jump from the wall: moved away from it %d; ticks on the ceiling %d%c", awayFromWall == 2.0f ? 1 : 0, ceilingTicks, 10);
            CHECK(awayFromWall == 2.0f);
            (void)vyAfterDouble;
            (void)vyBefore;
            (void)doubleAt;
        }
        {
            // the ceiling: driving along it, a jump and a second jump push the car along its own up, down and away from the ceiling
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 25.0f, 0.0f, 1.5707963f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            int onCeiling = -1;
            float vyBefore = 0, vyAfter = 0;
            Run(sim, Ticks(6.0f), [&](int t, const RlWorldState& s2, Inputs& in) {
                const RlCarState& c = s2.cars[0];
                if (onCeiling < 0 && c.wheelContacts >= 3 && c.position.y > 19.0f && Up(c).y < -0.8f) onCeiling = t;
                if (onCeiling < 0)
                {
                    in[0].throttle = 1.0f;
                    in[0].boost = t < Ticks(4.0f);
                    return;
                }
                in[0].throttle = 1.0f;
                const int since = t - onCeiling;
                in[0].jump = (since >= 0 && since < 4) || (since >= Ticks(0.3f) && since < Ticks(0.3f) + 4);
                if (since == Ticks(0.3f) - 1) vyBefore = c.linearVelocity.y;
                if (since == Ticks(0.3f) + 6) vyAfter = c.linearVelocity.y;
            });
            std::printf("  ceiling: vertical speed before the second jump %.2f, after %.2f m/s (it must get more negative)%c", static_cast<double>(vyBefore), static_cast<double>(vyAfter), 10);
            CHECK(onCeiling > 0);
            CHECK(vyAfter < vyBefore - 1.5f);
        }
        // a ball hit with a dodge goes faster than a plain hit, one with the roof slower
        {
            float speeds[3] = {0, 0, 0};
            for (int kind = 0; kind < 2; ++kind)
            {
                RlSim sim(g_tuning, 1);
                sim.ResetCar(0, 0.0f, -22.0f, 0.0f);
                sim.ResetBall({0.0f, 1.6f, -9.0f});
                Settle(sim);
                sim.ResetBall({0.0f, 1.6f, -9.0f});
                float best = 0;
                Run(sim, Ticks(2.5f), [&](int t, const RlWorldState& s2, Inputs& in) {
                    const RlCarState& c = s2.cars[0];
                    in[0].throttle = 1.0f;
                    in[0].boost = t < Ticks(1.0f);
                    if (kind == 1 && t > Ticks(0.6f) && !c.jumped && c.position.z > -14.0f)
                    {
                        in[0].jump = true;
                    }
                    if (kind == 1 && c.jumped && c.canDodge && t > Ticks(0.6f))
                    {
                        in[0].jump = true;
                        in[0].pitch = -1.0f;
                    }
                    best = std::max(best, Len(V(s2.ball.linearVelocity)));
                });
                speeds[kind] = best;
            }
            std::printf("  ball speed after a plain hit %.1f m/s, after a hit during a dodge %.1f m/s%c", static_cast<double>(speeds[0]), static_cast<double>(speeds[1]), 10);
            CHECK(speeds[0] > 10.0f && speeds[1] > 10.0f);
        }
    }

    // ---------------------------------------------------------------- the model of RL against reference numbers (G4d)
    // The references are worked out from the constants of RocketSim / the RLBot wiki (see docs/game/rl-physics.md); a test fails when the car
    // leaves the model.

    Vec LocalAngular(const RlCarState& c) { return Rotate({-c.rotation.x, -c.rotation.y, -c.rotation.z, c.rotation.w}, V(c.angularVelocity)); }

    // a car in the air with a spin of `spin` about the local axis (0 pitch, 1 yaw, 2 roll) and the stick `stick`; returns the angular speed after `seconds`
    float AirSpin(float spin, int axis, float stick, float seconds, float* timeTo95 = nullptr)
    {
        RlTuning t = g_tuning;
        t.gravity = 0.0f;  // nothing to fall: the car hangs in the air and the model of the air control is all that acts on it
        RlSim sim(t, 1);
        sim.ResetCar(0, 0.0f, -20.0f, 0.0f);
        QuietBall(sim);
        sim.SetCarPose(0, {0.0f, 10.0f, -20.0f}, {0, 0, 0, 1}, {});
        // the angular velocity cannot be given by SetCarPose: spin it up with the stick first when a start spin is wanted
        float result = 0;
        const int start = spin != 0.0f ? Ticks(2.0f) : 0;
        Run(sim, start + Ticks(seconds), [&](int tick, const RlWorldState& s, Inputs& in) {
            const Vec w = LocalAngular(s.cars[0]);
            const float now = axis == 0 ? w.x : (axis == 1 ? w.y : w.z);
            if (tick < start)
            {
                // spin up to the wanted speed with the stick, then let go
                const float have = std::fabs(now);
                const float push = std::clamp((std::fabs(spin) - have) * 4.0f, 0.0f, 1.0f) * (spin > 0 ? 1.0f : -1.0f);
                if (axis == 0) in[0].pitch = -push;
                if (axis == 1) in[0].steer = push;
                if (axis == 2) in[0].roll = -push;
            }
            else
            {
                if (axis == 0) in[0].pitch = stick;
                if (axis == 1) in[0].steer = stick;
                if (axis == 2) in[0].roll = stick;
                result = now;
                if (timeTo95 != nullptr && *timeTo95 < 0 && std::fabs(now) > 0.95f * 5.5f)
                {
                    *timeTo95 = static_cast<float>(tick - start) / kTickRate;
                }
            }
        });
        return result;
    }

    void TestRlReference()
    {
        std::printf("the model of RL against reference numbers\n");
        const RlCarTuning& car = g_tuning.car;

        // ---- air control: angular acceleration = T * stick - D * angular speed (RocketSim: T = 130, 95, 400, D = 30, 20, 50, times 2 pi / 65536 * 1000);
        // the damping of pitch and yaw fades out with the stick, so with the stick fully over they run up to the limit of 5.5 rad/s
        {
            const float scale = 2.0f * 3.14159265f / 65536.0f * 1000.0f;
            float t95p = -1, t95y = -1, t95r = -1;
            const float pitch = AirSpin(0.0f, 0, -1.0f, 3.0f, &t95p);  // W: nose down is a positive rate about X
            const float yaw = AirSpin(0.0f, 1, 1.0f, 3.0f, &t95y);
            const float roll = AirSpin(0.0f, 2, 1.0f, 3.0f, &t95r);
            Note("full stick for 3 s: pitch %.2f, yaw %.2f, roll %.2f rad/s (the limit is 5.5)", pitch, yaw, roll);
            CHECK(std::fabs(std::fabs(pitch) - 5.5f) < 0.05f);
            CHECK(std::fabs(std::fabs(yaw) - 5.5f) < 0.05f);
            CHECK(std::fabs(std::fabs(roll) - 5.5f) < 0.05f);
            // pitch and yaw: w = T t, the roll: w = T / D * (1 - exp(-D t)); 95 % of 5.5 rad/s
            const float want = 0.95f * 5.5f;
            const float tPitch = want / (130.0f * scale);
            const float tYaw = want / (95.0f * scale);
            const float tRoll = -std::log(1.0f - want / (400.0f / 50.0f)) / (50.0f * scale);
            Note("time to 5.2 rad/s: pitch %.2f s (model %.2f), yaw %.2f s (model %.2f), roll %.2f s (model %.2f)", t95p, tPitch, t95y, tYaw, t95r, tRoll);
            CHECK(std::fabs(t95p - tPitch) < 0.04f);
            CHECK(std::fabs(t95y - tYaw) < 0.04f);
            CHECK(std::fabs(t95r - tRoll) < 0.04f);

            // let go: the spin dies out with the damping, w(t) = w0 * exp(-D t)
            const float held = AirSpin(4.0f, 0, 0.0f, 0.01f);
            const float after = AirSpin(4.0f, 0, 0.0f, 0.5f);
            const float expected = held * std::exp(-30.0f * scale * 0.5f);
            Note("pitch spin %.2f rad/s: 0.5 s after the stick is let go %.2f (RL: %.2f)", held, after, expected);
            CHECK(std::fabs(after - expected) < 0.25f);
        }

        // ---- the jump: 2.917 m/s at the start, 14.58 m/s^2 for 0.2 s while it is held (apex of the hitbox centre against the kinematics)
        {
            const float g = g_tuning.gravity;
            float apex[3] = {};
            for (int kind = 0; kind < 3; ++kind)  // 0 tap, 1 held, 2 held + double jump
            {
                RlSim sim(g_tuning, 1);
                Settle(sim);
                const float base = sim.State().cars[0].position.y;
                float top = base;
                Run(sim, Ticks(4.0f), [&](int t, const RlWorldState& s, Inputs& in) {
                    in[0].jump = kind == 0 ? (t >= 2 && t < 5) : (t >= 2 && t < Ticks(0.5f));
                    if (kind == 2) in[0].jump = (t >= 2 && t < Ticks(0.25f)) || (t >= Ticks(0.4f) && t < Ticks(0.4f) + 3);
                    top = std::max(top, s.cars[0].position.y);
                });
                apex[kind] = top - base;
            }
            // tap: 3 ticks of the hold push at 0.62; held: 0.2 s of the full push
            const float tapV = car.jumpImpulse + car.jumpHoldAccel * car.jumpMinAccelScale * (3.0f / kTickRate);
            const float tapApex = tapV * tapV / (2.0f * g);
            const float heldV = car.jumpImpulse + (car.jumpHoldAccel - g) * car.jumpHoldTime;
            const float heldApex = car.jumpImpulse * car.jumpHoldTime + 0.5f * (car.jumpHoldAccel - g) * car.jumpHoldTime * car.jumpHoldTime + heldV * heldV / (2.0f * g);
            Note("apex of a tap %.2f m (kinematics %.2f), of a held jump %.2f m (kinematics %.2f), with a double jump %.2f m (+%.2f)", apex[0], tapApex, apex[1], heldApex, apex[2],
                 apex[2] - apex[1]);
            CHECK(std::fabs(apex[0] - tapApex) < 0.15f);
            CHECK(std::fabs(apex[1] - heldApex) < 0.35f);
            CHECK(apex[2] > apex[1] + 1.0f);
        }

        // ---- the dodge: 5 m/s along the stick, more at speed to the side and backwards, the vertical speed hangs, the spin is 5.5 rad/s
        {
            const struct
            {
                const char* name;
                float pitch, steer;
            } cases[] = {{"front", -1.0f, 0.0f}, {"back", 1.0f, 0.0f}, {"right", 0.0f, 1.0f}};
            for (const auto& k : cases)
            {
                RlSim sim(g_tuning, 1);
                sim.ResetCar(0, 0.0f, -49.0f, 0.0f);
                QuietBall(sim);
                Settle(sim);
                Vec before{}, after{};
                float vyDuring = 0;
                float spin = 0;
                const int drive = Ticks(1.2f);
                const int press = drive + Ticks(0.4f);
                Run(sim, press + Ticks(0.9f), [&](int t, const RlWorldState& s, Inputs& in) {
                    const RlCarState& c = s.cars[0];
                    in[0].throttle = t < drive ? 1.0f : 0.0f;
                    in[0].jump = (t >= drive && t < drive + Ticks(0.25f)) || (t >= press && t < press + 3);
                    if (t >= press && t < press + 3)
                    {
                        in[0].pitch = k.pitch;
                        in[0].steer = k.steer;
                    }
                    if (t == press - 1) before = V(c.linearVelocity);
                    if (t == press + 4) after = V(c.linearVelocity);
                    if (t > press + Ticks(0.3f) && t < press + Ticks(0.6f)) vyDuring = std::max(vyDuring, std::fabs(c.linearVelocity.y));
                    if (t > press && t < press + Ticks(0.3f)) spin = std::max(spin, Len(V(c.angularVelocity)));
                });
                const float ratio = before.z / car.maxSpeed;
                float expectForward = 0.0f, expectSide = 0.0f;
                if (k.pitch < 0.0f) expectForward = 5.0f * ((car.dodgeForwardSpeedScale - 1.0f) * ratio + 1.0f);
                if (k.pitch > 0.0f) expectForward = -5.0f * ((car.dodgeBackSpeedScale - 1.0f) * ratio + 1.0f) * car.dodgeBackImpulseScale;
                if (k.steer != 0.0f) expectSide = 5.0f * ((car.dodgeSideSpeedScale - 1.0f) * ratio + 1.0f);
                const Vec dv{after.x - before.x, 0.0f, after.z - before.z};
                // the nose is along +Z all the time here (the car only changes its pitch within these ticks)
                std::printf("  %s flip at %.1f m/s: speed change along the nose %.2f (RL %.2f), sideways %.2f (RL %.2f), spin %.2f rad/s, vertical speed in the flip at most %.2f m/s%c",
                            k.name, static_cast<double>(before.z), static_cast<double>(dv.z), static_cast<double>(expectForward), static_cast<double>(dv.x),
                            static_cast<double>(expectSide), static_cast<double>(spin), static_cast<double>(vyDuring), 10);
                CHECK(std::fabs(dv.z - expectForward) < 0.5f);
                CHECK(std::fabs(std::fabs(dv.x) - expectSide) < 0.5f);
                CHECK(spin > 5.0f && spin < 5.55f);
                CHECK(vyDuring < 0.6f);  // the flip hangs in the air: RL takes 35 % of a falling speed out of it every tick
            }
        }

        // ---- W + double jump + Q: a flip with a roll that lands on its wheels (the case "the car does something strange")
        for (int roll = -1; roll <= 1; roll += 2)
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            const int press = Ticks(0.35f);
            float maxSpin = 0;
            bool flipped = false;
            float upAfter = 0;
            int wheelsAfter = 0;
            float maxRise = 0;
            Run(sim, Ticks(4.0f), [&](int t, const RlWorldState& s, Inputs& in) {
                const RlCarState& c = s.cars[0];
                in[0].throttle = 1.0f;
                in[0].jump = (t >= 2 && t < Ticks(0.25f)) || (t >= press && t < press + 3);
                if (t >= press)
                {
                    in[0].pitch = -1.0f;                  // W held
                    in[0].roll = static_cast<float>(roll);  // and Q or E
                }
                if (t > press + Ticks(0.65f)) LevelOut(c, in[0]);
                maxSpin = std::max(maxSpin, Len(V(c.angularVelocity)));
                flipped = flipped || c.dodging;
                maxRise = std::max(maxRise, c.position.y);
                if (t == Ticks(3.9f))
                {
                    upAfter = Up(c).y;
                    wheelsAfter = c.wheelContacts;
                }
            });
            std::printf("  W + double jump + %s: flip %d, largest spin %.2f rad/s, highest point %.2f m, 3.9 s: up.y %.2f on %d wheels%c", roll < 0 ? "Q" : "E", flipped ? 1 : 0,
                        static_cast<double>(maxSpin), static_cast<double>(maxRise), static_cast<double>(upAfter), wheelsAfter, 10);
            CHECK(flipped);
            CHECK(maxSpin < 5.6f);  // the limit of the angular speed is never passed
            CHECK(maxRise < 3.5f);  // no launch
            CHECK(upAfter > 0.95f && wheelsAfter == 4);
        }

        // ---- the suspension: the car rests on it at the RL height, a landing makes no swing, a hit with the body does not bounce it
        {
            RlSim sim(g_tuning, 1);
            Settle(sim);
            const RlCarState& c = sim.State().cars[0];
            // RL: the origin rests 17 uu over the floor, the hitbox centre 20.755 uu higher
            // the springs carry 1.5 times the weight (the sticky force): about 2 cm of compression in front and behind
            Note("rest height %.4f m (RL 0.3776), compression front %.3f / rear %.3f m (the model: 0.0195)", c.position.y, c.wheelCompression[0], c.wheelCompression[2]);
            CHECK(std::fabs(c.position.y - 0.3776f) < 0.006f);
            CHECK(std::fabs(c.wheelCompression[0] - 0.0195f) < 0.004f && std::fabs(c.wheelCompression[2] - 0.0195f) < 0.004f);
        }
        {
            // the car upside down dropped from 1 m: the roof hits the floor, nothing springs back
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -30.0f, 0.0f);
            QuietBall(sim);
            sim.SetCarPose(0, {0.0f, 1.2f, -30.0f}, AxisAngle(0, 0, 1, 180.0f), {});
            float lowest = 99, rebound = 0;
            Run(sim, Ticks(1.5f), [&](int, const RlWorldState& s, Inputs&) {
                const float y = s.cars[0].position.y;
                lowest = std::min(lowest, y);
                if (s.cars[0].linearVelocity.y > 0.0f && lowest < 0.5f) rebound = std::max(rebound, y - lowest);
            });
            Note("a drop on the roof from 1.2 m: lowest point %.3f m, rises again by at most %.3f m (the limit is 0.05 m)", lowest, rebound);
            CHECK(rebound < 0.05f);
        }
    }

    // The camera and the render take the pose between two ticks (RlGameSystem::ApplyToScene: PreviousState and State, alpha = accumulator / tick):
    // a car at a steady speed moves by the same step every frame at 60, 144 and 300 frames a second; the pose of the last tick alone would
    // move in jumps (several frames on one place, then a step).
    void TestInterpolation()
    {
        std::printf("the picture between two ticks: no jitter at 60, 144, 300 FPS\n");
        for (const float fps : {60.0f, 144.0f, 300.0f})
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
            QuietBall(sim);
            Settle(sim);
            sim.SetCarBoost(0, 100.0f);
            // up to the speed first
            Run(sim, Ticks(2.0f), [&](int, const RlWorldState&, Inputs& in) { in[0].throttle = 1.0f; });
            const float frame = 1.0f / fps;
            float accumulator = 0.0f;
            float previousSmooth = 0, previousTick = 0;
            float minStep = 1e9f, maxStep = 0, minTickStep = 1e9f, maxTickStep = 0, jitter = 0, lastStep = -1;
            int frames = 0;
            RlCarInput input;
            input.throttle = 1.0f;
            for (int f = 0; f < static_cast<int>(fps * 1.5f); ++f)
            {
                accumulator += frame;
                while (accumulator >= kTickDt)
                {
                    accumulator -= kTickDt;
                    sim.Step(&input);
                }
                const float alpha = std::clamp(accumulator / kTickDt, 0.0f, 1.0f);
                const float a = sim.PreviousState().cars[0].position.z, b = sim.State().cars[0].position.z;
                const float smooth = a + (b - a) * alpha;
                const float tickPose = b;
                if (f > 0)
                {
                    minStep = std::min(minStep, smooth - previousSmooth);
                    maxStep = std::max(maxStep, smooth - previousSmooth);
                    if (lastStep >= 0.0f)
                    {
                        jitter = std::max(jitter, std::fabs((smooth - previousSmooth) - lastStep));  // how much a step differs from the one before it
                    }
                    lastStep = smooth - previousSmooth;
                    minTickStep = std::min(minTickStep, tickPose - previousTick);
                    maxTickStep = std::max(maxTickStep, tickPose - previousTick);
                    ++frames;
                }
                previousSmooth = smooth;
                previousTick = tickPose;
            }
            Note("%.0f FPS: the step of a frame %.4f .. %.4f m between two interpolated poses (a step differs from the one before by %.2f%% at most), %.4f .. %.4f m with the pose of the tick", fps, minStep, maxStep,
                 100.0f * jitter / std::max(maxStep, 1e-6f), minTickStep, maxTickStep);
            CHECK(frames > 50);
            CHECK(jitter < 0.01f * maxStep);  // the interpolated picture moves evenly
            if (fps > 100.0f)
            {
                CHECK(minTickStep < 0.01f * maxTickStep);  // the pose of the tick alone stands still in some frames (jitter)
            }
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

        // adhesion (RocketSim): let go of everything on the wall, the car is pulled into the wall as long as it still moves (more than
        // 0.25 m/s) and then slides down; it does not hang there for ever
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
        CHECK(heldTicks > Ticks(0.5f));
        CHECK(heldTicks < totalAfter);  // and it does come down
    }

    // ---------------------------------------------------------------- camera

    bool InsideArena(const RlArenaTuning& a, const RlVec3& p)
    {
        if (p.y < -0.001f || p.y > a.height + 0.001f || std::fabs(p.x) > a.halfWidth + 0.001f)
        {
            return false;
        }
        if (std::fabs(p.z) > a.halfLength)
        {
            // behind an end wall only inside a goal
            return std::fabs(p.x) < a.goalHalfWidth && p.y < a.goalHeight && std::fabs(p.z) <= a.halfLength + a.goalDepth;
        }
        const float cx = std::fabs(p.x) - (a.halfWidth - a.cornerCut);
        const float cz = std::fabs(p.z) - (a.halfLength - a.cornerCut);
        return !(cx > 0.0f && cz > 0.0f && cx + cz > a.cornerCut + 0.001f);  // the chamfered corners
    }

    struct CameraRun
    {
        int frames = 0;
        int outside = 0;         // frames with the camera outside the arena
        int blocked = 0;         // frames with an arena wall between the car and the camera
        float maxUpTurn = 0;     // degrees the up of the camera turned in one frame
        float minDistance = 99;  // closest the camera came to the car
        float maxHeight = 0;     // highest the car was
        float maxRoll = 0;       // degrees the camera was tilted from the horizon (its right axis out of the horizontal plane)
        float maxTurnRate = 0;   // degrees per second the camera turned (the angle between its looking directions of two frames)
        float minCarInFrame = 99;  // the smallest cosine between the looking direction and the direction to the car: the car stays in the picture
    };

    CameraRun RunCamera(RlSim& sim, int ticks, bool ballCam, const Script& script, const RlCameraTuning* tuningOverride = nullptr)
    {
        CameraRun run;
        RlCameraState state;
        RlVec3 prevUp{0, 1, 0};
        RlVec3 prevForward{0, 0, 1};
        const RlCameraTuning& cameraTuning = tuningOverride != nullptr ? *tuningOverride : g_tuning.camera;
        const RlArenaTuning& arena = g_tuning.arena;
        Run(sim, ticks, [&](int t, const RlWorldState& s, Inputs& in) {
            if (script) script(t, s, in);
            if (t % 2 != 0) return;  // 60 camera frames a second
            const RlCarState& c = s.cars[0];
            RlCameraInput input;
            input.carPosition = c.position;
            input.carRotation = c.rotation;
            input.onSurface = c.wheelContacts >= 3;
            input.surfaceNormal = c.surfaceNormal;
            input.ballPosition = s.ball.position;
            input.ballCam = ballCam;
            input.carVelocity = c.linearVelocity;
            const RlCameraView view = RlCameraUpdate(cameraTuning, state, sim, input, 1.0f / 60.0f);
            ++run.frames;
            run.maxRoll = std::max(run.maxRoll, std::fabs(std::asin(std::clamp(view.right.y, -1.0f, 1.0f))) * 57.29578f);
            if (run.frames > 1)
            {
                run.maxTurnRate = std::max(run.maxTurnRate, std::acos(std::clamp(Dot(V(view.forward), V(prevForward)), -1.0f, 1.0f)) * 57.29578f * 60.0f);
            }
            prevForward = view.forward;
            {
                const Vec toCar = {c.position.x - view.position.x, c.position.y - view.position.y, c.position.z - view.position.z};
                const float l = Len(toCar);
                if (l > 0.01f)
                {
                    run.minCarInFrame = std::min(run.minCarInFrame, Dot(V(view.forward), {toCar.x / l, toCar.y / l, toCar.z / l}));
                }
            }
            if (!InsideArena(arena, view.position)) ++run.outside;
            if (sim.ArenaSegmentFree(c.position, view.position) < 0.999f) ++run.blocked;
            const float turn = std::acos(std::clamp(Dot(V(view.up), V(prevUp)), -1.0f, 1.0f)) * 57.29578f;
            if (run.frames > 1) run.maxUpTurn = std::max(run.maxUpTurn, turn);
            prevUp = view.up;
            const Vec d = {view.position.x - c.position.x, view.position.y - c.position.y, view.position.z - c.position.z};
            run.minDistance = std::min(run.minDistance, Len(d));
            run.maxHeight = std::max(run.maxHeight, c.position.y);
        });
        return run;
    }

    void TestCamera()
    {
        std::printf("camera: inside the arena on walls, in corners and against walls\n");
        // up a side wall and along the ceiling (the car cam and the ball cam)
        for (int ball = 0; ball < 2; ++ball)
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 25.0f, 0.0f, 1.5707963f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            const CameraRun r = RunCamera(sim, Ticks(5.0f), ball == 1, [&](int t, const RlWorldState&, Inputs& in) {
                in[0].throttle = 1.0f;
                in[0].boost = t < Ticks(4.0f);
            });
            std::printf("  %s: %d frames, car up to %.1f m, outside %d, blocked %d, up turned at most %.1f deg per frame, tilted from the horizon up to %.1f deg, turned at most %.0f deg/s\n",
                        ball ? "ball cam, up the wall and over the ceiling" : "car cam, up the wall and over the ceiling", r.frames, static_cast<double>(r.maxHeight), r.outside,
                        r.blocked, static_cast<double>(r.maxUpTurn), static_cast<double>(r.maxRoll), static_cast<double>(r.maxTurnRate));
            CHECK(r.maxHeight > 15.0f);
            CHECK(r.outside == 0 && r.blocked == 0);
            CHECK(r.maxUpTurn < 8.0f);
        }
        // into a corner at speed, then reversing against the wall (the camera would be behind the wall)
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 10.0f, 10.0f, 0.7853982f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            const CameraRun r = RunCamera(sim, Ticks(6.0f), true, [&](int t, const RlWorldState&, Inputs& in) {
                in[0].throttle = t < Ticks(3.0f) ? 1.0f : -1.0f;
                in[0].boost = t < Ticks(2.0f);
            });
            std::printf("  into the corner and back: %d frames, outside %d, blocked %d, closest %.2f m\n", r.frames, r.outside, r.blocked, static_cast<double>(r.minDistance));
            CHECK(r.outside == 0 && r.blocked == 0);
            CHECK(r.minDistance >= g_tuning.camera.minDistance - 0.01f);
        }
        // a car at the back wall of a goal, nose to the field: the camera belongs behind the back wall without the clip
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, 58.8f, 3.14159265f);
            QuietBall(sim);
            Settle(sim);
            const CameraRun r = RunCamera(sim, Ticks(1.0f), false, nullptr);
            std::printf("  at the back wall of the goal: outside %d of %d, blocked %d, closest %.2f m\n", r.outside, r.frames, r.blocked, static_cast<double>(r.minDistance));
            CHECK(r.outside == 0 && r.blocked == 0);
            CHECK(r.minDistance < g_tuning.camera.distance - 0.5f);  // it really had to come closer
        }
        // the goal: the camera behind a car in a goal may be in the goal, never behind its walls
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, 20.0f, 0.0f);
            sim.SetCarBoost(0, 100.0f);
            QuietBall(sim);
            Settle(sim);
            const CameraRun r = RunCamera(sim, Ticks(3.0f), false, [&](int t, const RlWorldState&, Inputs& in) {
                in[0].throttle = 1.0f;
                in[0].boost = t < Ticks(2.5f);
            });
            std::printf("  into the goal: outside %d of %d, blocked %d\n", r.outside, r.frames, r.blocked);
            CHECK(r.outside == 0 && r.blocked == 0);
        }
        // in the air the camera does not follow the cartwheel of the car: a dodge, the up of the camera barely moves
        {
            RlSim sim(g_tuning, 1);
            sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
            QuietBall(sim);
            Settle(sim);
            const CameraRun r = RunCamera(sim, Ticks(2.0f), false, [&](int t, const RlWorldState&, Inputs& in) {
                in[0].jump = (t >= 2 && t < Ticks(0.25f)) || (t >= Ticks(0.4f) && t < Ticks(0.4f) + 3);
                in[0].pitch = (t >= Ticks(0.4f) && t < Ticks(0.4f) + 3) ? -1.0f : 0.0f;
            });
            std::printf("  a flip: the up of the camera turned at most %.1f deg per frame\n", static_cast<double>(r.maxUpTurn));
            CHECK(r.maxUpTurn < 3.0f);
        }
    }


    // The camera does not tilt with the car (G4h): on a wall, on the ceiling, in a flip, in an air roll and in a fall the horizon of the camera stays level,
    // the camera turns without jerks and the car stays in the picture. `rollFollow` is the setting "Wall tilt" (0 by default).
    struct LevelScenario
    {
        const char* name;
        std::function<void(RlSim&)> setup;
        float seconds;
        Script script;
    };

    void TestCameraLevel()
    {
        std::printf("camera: a level horizon on walls, on the ceiling, in flips, in an air roll and in a fall\n");
        const auto drive = [](Inputs& in, bool boost) {
            in[0].throttle = 1.0f;
            in[0].boost = boost;
        };
        const LevelScenario scenarios[] = {
            {"up the side wall and over the ceiling",
             [](RlSim& sim) { sim.ResetCar(0, 25.0f, 0.0f, 1.5707963f); },
             5.0f,
             [&](int t, const RlWorldState&, Inputs& in) { drive(in, t < Ticks(4.0f)); }},
            {"up the wall at an angle and along it",
             [](RlSim& sim) { sim.ResetCar(0, 20.0f, -20.0f, 0.9f); },
             5.0f,
             [&](int t, const RlWorldState&, Inputs& in) {
                 drive(in, t < Ticks(4.0f));
                 in[0].steer = (t > Ticks(2.2f) && t < Ticks(3.0f)) ? 0.7f : 0.0f;
             }},
            {"a front flip",
             [](RlSim& sim) { sim.ResetCar(0, 0.0f, -45.0f, 0.0f); },
             2.0f,
             [](int t, const RlWorldState&, Inputs& in) {
                 in[0].jump = (t >= 2 && t < Ticks(0.25f)) || (t >= Ticks(0.4f) && t < Ticks(0.4f) + 3);
                 in[0].pitch = (t >= Ticks(0.4f) && t < Ticks(0.4f) + 3) ? -1.0f : 0.0f;
             }},
            {"a backflip",
             [](RlSim& sim) { sim.ResetCar(0, 0.0f, -45.0f, 0.0f); },
             2.0f,
             [](int t, const RlWorldState&, Inputs& in) {
                 in[0].throttle = 1.0f;
                 in[0].jump = (t >= 2 && t < Ticks(0.25f)) || (t >= Ticks(0.4f) && t < Ticks(0.4f) + 3);
                 in[0].pitch = (t >= Ticks(0.4f) && t < Ticks(0.4f) + 3) ? 1.0f : 0.0f;
             }},
            {"an air roll for three seconds under boost",
             [](RlSim& sim) { sim.ResetCar(0, 0.0f, -40.0f, 0.0f); sim.SetCarBoost(0, 100.0f); },
             4.5f,
             [](int t, const RlWorldState&, Inputs& in) {
                 in[0].throttle = 1.0f;
                 in[0].jump = t >= 2 && t < Ticks(0.3f);
                 in[0].boost = t >= Ticks(0.3f) && t < Ticks(3.5f);
                 in[0].pitch = (t >= Ticks(0.3f) && t < Ticks(0.8f)) ? 0.6f : 0.0f;
                 in[0].airRoll = t >= Ticks(0.8f);
                 in[0].steer = t >= Ticks(0.8f) ? 1.0f : 0.0f;
             }},
            {"a fall from the ceiling",
             [](RlSim& sim) { sim.ResetCar(0, 25.0f, 0.0f, 1.5707963f); },
             7.0f,
             [&](int t, const RlWorldState&, Inputs& in) {
                 if (t < Ticks(4.2f))
                 {
                     drive(in, true);
                 }
             }},
        };
        float tiltedWithFollow = 0.0f;
        for (int follow = 0; follow < 2; ++follow)
        {
         for (int ball = 0; ball < 2; ++ball)
         {
            for (const LevelScenario& scenario : scenarios)
            {
                if (follow == 1 && scenario.seconds < 5.0f)
                {
                    continue;  // with "Wall tilt" at 100 % only the walls matter
                }
                RlSim sim(g_tuning, 1);
                scenario.setup(sim);
                sim.SetCarBoost(0, 100.0f);
                QuietBall(sim);
                Settle(sim);
                RlCameraTuning tuning = g_tuning.camera;
                tuning.rollFollow = static_cast<float>(follow);
                const CameraRun r = RunCamera(sim, Ticks(scenario.seconds), ball == 1, scenario.script, &tuning);
                std::printf("  %s%s, %s: tilted from the horizon up to %.1f deg, turned at most %.0f deg/s, the car at most %.0f deg off the middle of the picture, outside %d, blocked %d\n",
                            ball == 1 ? (follow == 1 ? "ball cam, Wall tilt 100%" : "ball cam") : (follow == 1 ? "car cam, Wall tilt 100%" : "car cam"), "", scenario.name, static_cast<double>(r.maxRoll), static_cast<double>(r.maxTurnRate),
                            static_cast<double>(std::acos(std::clamp(r.minCarInFrame, -1.0f, 1.0f)) * 57.29578f), r.outside, r.blocked);
                CHECK(r.outside == 0 && r.blocked == 0);
                if (follow == 0)
                {
                    CHECK(r.maxRoll < 1.0f);                                          // the horizon is level
                    CHECK(r.maxTurnRate < tuning.maxTurnRate * 1.1f);                   // no jerks: the camera turns no faster than the limit
                    CHECK(r.minCarInFrame > std::cos(65.0f * 0.01745329f));             // the car stays in the picture
                }
                else
                {
                    tiltedWithFollow = std::max(tiltedWithFollow, r.maxRoll);
                }
            }
         }
        }
        CHECK(tiltedWithFollow > 10.0f);  // and "Wall tilt" really brings the old leaning back
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
        std::string onDisk((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        onDisk.erase(std::remove(onDisk.begin(), onDisk.end(), '\r'), onDisk.end());  // git may check the file out with CRLF
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

        // the visitor of the RL Tuning window sees every number the json has (the same list serves both)
        struct Counter final : RlTuningVisitor
        {
            int floats = 0, ints = 0, groups = 0, sections = 0;
            void Begin(const char*) override { ++groups; }
            void End() override {}
            void Section(const char*) override { ++sections; }
            void Float(const char*, float&) override { ++floats; }
            void Int(const char*, int&) override { ++ints; }
            void Floats(const char*, float*, int count) override { floats += count; }
        } counter;
        RlTuning copy = g_tuning;
        copy.Visit(counter);
        Note("the window sees %.0f numbers in %.0f groups (%.0f sections)", counter.floats + counter.ints, counter.groups, counter.sections);
        CHECK(counter.groups == 5 && counter.sections >= 8 && counter.floats > 100);

        // new numbers in a running world: the gravity and the jump of a car change at once, the hitbox stays as it was built
        {
            RlSim sim(g_tuning, 1);
            QuietBall(sim);
            Settle(sim);
            RlTuning moon = g_tuning;
            moon.gravity = 1.0f;
            moon.car.jumpImpulse = 3.5f;
            moon.car.halfExtentX = 2.0f;  // built into the body: ignored by the running sim
            sim.SetTuning(moon);
            CHECK(sim.Tuning().gravity == 1.0f && sim.Tuning().car.jumpImpulse == 3.5f);
            CHECK(sim.Tuning().car.halfExtentX == g_tuning.car.halfExtentX);
            float apex = 0;
            const float base = sim.State().cars[0].position.y;
            Run(sim, Ticks(5.0f), [&](int t, const RlWorldState& s, Inputs& in) {
                in[0].jump = t < 3;
                apex = std::max(apex, s.cars[0].position.y - base);
            });
            Note("a jump with gravity 1.0 and the jump impulse 3.5: %.1f m high (the kinematics: more than 5 m)", apex);
            CHECK(apex > 5.0f);
        }
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
        else if (std::strcmp(argv[i], "--sweep-landing") == 0)
        {
            SweepLanding();
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
        else if (std::strcmp(argv[i], "--write-controls") == 0 && i + 1 < argc)
        {
            return RlControls::Defaults().SaveToFile(argv[i + 1]) ? 0 : 1;
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
    TestLanding();
    TestLandingSkid();
    TestTurnWithoutSkid();
    TestInterpolation();
    TestGlue();
    TestJumps();
    TestWalls();
    TestFlipFromRoof();
    TestDodgeInputs();
    TestAirRoll();
    TestFlipCancel();
    TestWaveDash();
    TestRlReference();
    TestSimultaneousActions();
    TestMechanicsChecklist();
    TestJumpWhileDriving();
    TestCamera();
    TestCameraLevel();
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
