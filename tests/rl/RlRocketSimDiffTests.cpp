// RlRocketSimDiffTests.cpp
// The differential test against RocketSim: the same scripted inputs (tests/rl/RlDiffScenarios.h) drive the car of this engine and RocketSim
// (github.com/ZealanL/RocketSim, MIT, THE_VOID mode with a flat floor, no arena meshes of the game); the traces of RocketSim were written
// by tests/rl/rocketsim_diff/gen.cpp and are in tests/rl/rocketsim_diff/traces (see the README there). The test replays every scenario on
// the flat middle of our arena and compares the positions, speeds, angular speeds and orientations tick by tick.
// Arguments: --verbose prints every scenario, --only <name> runs one.

#include <rl/RlSim.h>
#include <rl/RlTuning.h>

#include "RlDiffScenarios.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using namespace rl;

    int g_failures = 0;
    int g_checks = 0;
    bool g_verbose = false;

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

    struct V
    {
        float x = 0, y = 0, z = 0;
    };
    V Sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    float Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    float Len(V a) { return std::sqrt(Dot(a, a)); }
    V FromRl(const RlVec3& v) { return {v.x, v.y, v.z}; }

    V Rotate(const RlQuat& q, V v)
    {
        const float tx = 2.0f * (q.y * v.z - q.z * v.y);
        const float ty = 2.0f * (q.z * v.x - q.x * v.z);
        const float tz = 2.0f * (q.x * v.y - q.y * v.x);
        return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx)};
    }

    RlQuat Mul(const RlQuat& a, const RlQuat& b)
    {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
    RlQuat AxisAngle(float x, float y, float z, float degrees)
    {
        const float h = degrees * 0.5f * 3.14159265f / 180.0f;
        const float s = std::sin(h);
        return {x * s, y * s, z * s, std::cos(h)};
    }

    struct Row
    {
        int tick = 0;
        V pos, vel, ang, forward, up;
        int wheels = 0;
        float boost = 0;
        V ball, ballVel, second, secondVel;
        int secondDemo = 0;
    };

    bool ReadTrace(const std::string& path, std::vector<Row>& rows)
    {
        std::ifstream file(std::filesystem::u8path(path));
        if (!file)
        {
            return false;
        }
        std::string line;
        std::getline(file, line);  // header
        while (std::getline(file, line))
        {
            if (line.empty())
            {
                continue;
            }
            std::vector<double> f;
            std::stringstream ss(line);
            std::string cell;
            while (std::getline(ss, cell, ','))
            {
                f.push_back(std::atof(cell.c_str()));
            }
            if (f.size() < 32)
            {
                return false;
            }
            const auto fl = [&](size_t i) { return static_cast<float>(f[i]); };
            Row r;
            r.tick = static_cast<int>(f[0]);
            r.pos = {fl(1), fl(2), fl(3)};
            r.vel = {fl(4), fl(5), fl(6)};
            r.ang = {fl(7), fl(8), fl(9)};
            r.forward = {fl(10), fl(11), fl(12)};
            r.up = {fl(13), fl(14), fl(15)};
            r.wheels = static_cast<int>(f[16]);
            r.boost = fl(17);
            r.ball = {fl(19), fl(20), fl(21)};
            r.ballVel = {fl(22), fl(23), fl(24)};
            r.second = {fl(25), fl(26), fl(27)};
            r.secondVel = {fl(28), fl(29), fl(30)};
            r.secondDemo = static_cast<int>(f[31]);
            rows.push_back(r);
        }
        return !rows.empty();
    }

    struct Result
    {
        float posMax = 0, pos2s = 0;     // largest distance of the hitbox centres (all the run / the first 2 s)
        float speedMax = 0, speed2s = 0; // largest difference of the speeds, as a share of the speed of RocketSim (a floor of 5 m/s under it)
        float velMax = 0;                // largest length of the difference of the velocity vectors (m/s)
        float angMax = 0;                // largest length of the difference of the angular velocities (rad/s)
        float orientMax = 0;             // largest angle between the cars' noses / ups (degrees)
        float ballPosMax = 0, ballSpeedMax = 0;
        float secondPosMax = 0, secondSpeedMax = 0;
        int demoTickRs = -1, demoTickOurs = -1;
        float ticks = 0;
        int firstOff = -1;               // the first tick the position is 0.1 m or the orientation 3 degrees off
        float finalSpeedRs = 0, finalSpeedOurs = 0;
    };

    bool g_dump = false;
    float g_ballFriction = -1.0f;
    float g_carRestitution = -1.0f;

    Result Run(int index, const std::vector<Row>& rows, const RlTuning& tuning)
    {
        const rldiff::Scenario sc = rldiff::Get(index);
        const int cars = sc.second ? 2 : 1;
        RlSim sim(tuning, cars);
        sim.ResetCar(0, sc.car.x, sc.car.z, 0.0f);
        if (sc.second)
        {
            sim.ResetCar(1, sc.secondX, sc.secondZ, 0.0f);
        }
        // the ball: at rest as the scenario says, or in a corner of the arena
        if (sc.ball)
        {
            const float y = sc.ballY > 0.0f ? sc.ballY : tuning.ball.radius;
            sim.ResetBall({sc.ballX, y, sc.ballZ}, {0.0f, 0.0f, sc.ballVz});
        }
        else
        {
            sim.ResetBall({-32.0f, 1.0f, 40.0f});
        }
        // car 0: its pose from the scenario (yaw, then pitch, then roll about its own axes), the speed along its nose
        {
            const RlQuat q = Mul(Mul(AxisAngle(0, 1, 0, sc.car.yawDeg), AxisAngle(1, 0, 0, sc.car.pitchDeg)), AxisAngle(0, 0, 1, sc.car.rollDeg));
            const V nose = Rotate(q, {0, 0, 1});
            const float y = sc.car.y > 0.0f ? sc.car.y : 0.3776f;
            sim.SetCarPose(0, {sc.car.x, y, sc.car.z}, q, {nose.x * sc.car.speed, nose.y * sc.car.speed, nose.z * sc.car.speed});
        }
        sim.SetCarBoost(0, sc.boostStart);
        if (sc.second)
        {
            sim.SetCarPose(1, {sc.secondX, 0.3776f, sc.secondZ}, {0, 0, 0, 1}, {});
            sim.SetCarBoost(1, 0.0f);
        }

        if (g_dump && sc.ball)
        {
            std::printf("   ball before: y %.4f vz %.3f radius %.4f friction %.3f", static_cast<double>(sim.State().ball.position.y), static_cast<double>(sim.State().ball.linearVelocity.z), static_cast<double>(sim.Tuning().ball.radius), static_cast<double>(sim.Tuning().ball.friction));
            std::printf("%c", 10);
        }
        Result res;
        res.ticks = static_cast<float>(sc.ticks);
        std::vector<RlCarInput> inputs(static_cast<size_t>(cars));
        size_t next = 1;  // rows[0] is tick 0
        for (int t = 0; t < sc.ticks; ++t)
        {
            for (RlCarInput& in : inputs)
            {
                in = RlCarInput{};
            }
            const rldiff::Input d = rldiff::At(index, t);
            inputs[0].throttle = d.throttle;
            inputs[0].steer = d.steer;
            inputs[0].pitch = d.pitch;
            inputs[0].roll = d.roll;
            inputs[0].jump = d.jump;
            inputs[0].boost = d.boost;
            inputs[0].handbrake = d.handbrake;
            sim.Step(inputs.data());
            const int done = t + 1;
            if (g_dump && sc.ball && t < 6)
            {
                std::printf("   tick %d ball vz %.4f wx %.4f y %.4f", done, static_cast<double>(sim.State().ball.linearVelocity.z), static_cast<double>(sim.State().ball.angularVelocity.x), static_cast<double>(sim.State().ball.position.y));
                std::printf("%c", 10);
            }
            if (sc.second && res.demoTickOurs < 0 && sim.State().cars[1].demolished)
            {
                res.demoTickOurs = done;
            }
            if (next < rows.size() && rows[next].tick == done)
            {
                const Row& r = rows[next++];
                const RlCarState& c = sim.State().cars[0];
                const V pos = FromRl(c.position), vel = FromRl(c.linearVelocity), ang = FromRl(c.angularVelocity);
                if (g_dump)
                {
                    const V fw = Rotate(c.rotation, {0, 0, 1});
                    std::printf("   %4d ours p(%.2f %.2f %.2f) v(%.2f %.2f %.2f) w(%.2f %.2f %.2f) f(%.2f %.2f %.2f) wh %d | rs p(%.2f %.2f %.2f) v(%.2f %.2f %.2f) w(%.2f %.2f %.2f) f(%.2f %.2f %.2f) wh %d\n",
                                done, static_cast<double>(pos.x), static_cast<double>(pos.y), static_cast<double>(pos.z), static_cast<double>(vel.x), static_cast<double>(vel.y),
                                static_cast<double>(vel.z), static_cast<double>(ang.x), static_cast<double>(ang.y), static_cast<double>(ang.z), static_cast<double>(fw.x),
                                static_cast<double>(fw.y), static_cast<double>(fw.z), c.wheelContacts, static_cast<double>(r.pos.x), static_cast<double>(r.pos.y),
                                static_cast<double>(r.pos.z), static_cast<double>(r.vel.x), static_cast<double>(r.vel.y), static_cast<double>(r.vel.z), static_cast<double>(r.ang.x),
                                static_cast<double>(r.ang.y), static_cast<double>(r.ang.z), static_cast<double>(r.forward.x), static_cast<double>(r.forward.y),
                                static_cast<double>(r.forward.z), r.wheels);
                }
                if (g_dump && sc.ball)
                {
                    const RlBallState& b = sim.State().ball;
                    std::printf("        ball ours p(%.2f %.2f %.2f) v(%.2f %.2f %.2f) | rs p(%.2f %.2f %.2f) v(%.2f %.2f %.2f)\n", static_cast<double>(b.position.x),
                                static_cast<double>(b.position.y), static_cast<double>(b.position.z), static_cast<double>(b.linearVelocity.x), static_cast<double>(b.linearVelocity.y),
                                static_cast<double>(b.linearVelocity.z), static_cast<double>(r.ball.x), static_cast<double>(r.ball.y), static_cast<double>(r.ball.z),
                                static_cast<double>(r.ballVel.x), static_cast<double>(r.ballVel.y), static_cast<double>(r.ballVel.z));
                }
                const float posErr = Len(Sub(pos, r.pos));
                const float speedRs = Len(r.vel);
                const float speedErr = std::fabs(Len(vel) - speedRs) / std::max(speedRs, 5.0f);
                const float velErr = Len(Sub(vel, r.vel));
                const float angErr = Len(Sub(ang, r.ang));
                const V f = Rotate(c.rotation, {0, 0, 1}), u = Rotate(c.rotation, {0, 1, 0});
                const float orient = std::max(std::acos(std::clamp(Dot(f, r.forward), -1.0f, 1.0f)), std::acos(std::clamp(Dot(u, r.up), -1.0f, 1.0f))) * 57.29578f;
                if (res.firstOff < 0 && (posErr > 0.1f || orient > 3.0f))
                {
                    res.firstOff = done;
                }
                res.posMax = std::max(res.posMax, posErr);
                res.speedMax = std::max(res.speedMax, speedErr);
                res.velMax = std::max(res.velMax, velErr);
                res.angMax = std::max(res.angMax, angErr);
                res.orientMax = std::max(res.orientMax, orient);
                if (done <= 240)
                {
                    res.pos2s = std::max(res.pos2s, posErr);
                    res.speed2s = std::max(res.speed2s, speedErr);
                }
                res.finalSpeedRs = speedRs;
                res.finalSpeedOurs = Len(vel);
                if (sc.ball)
                {
                    const RlBallState& b = sim.State().ball;
                    res.ballPosMax = std::max(res.ballPosMax, Len(Sub(FromRl(b.position), r.ball)));
                    const float bs = Len(r.ballVel);
                    res.ballSpeedMax = std::max(res.ballSpeedMax, std::fabs(Len(FromRl(b.linearVelocity)) - bs) / std::max(bs, 2.0f));
                }
                if (sc.second && r.secondDemo == 0 && !sim.State().cars[1].demolished)
                {
                    const RlCarState& s = sim.State().cars[1];
                    res.secondPosMax = std::max(res.secondPosMax, Len(Sub(FromRl(s.position), r.second)));
                    const float ss = Len(r.secondVel);
                    res.secondSpeedMax = std::max(res.secondSpeedMax, std::fabs(Len(FromRl(s.linearVelocity)) - ss) / std::max(ss, 5.0f));
                }
                if (sc.second && r.secondDemo != 0 && res.demoTickRs < 0)
                {
                    res.demoTickRs = done;
                }
            }
        }
        return res;
    }

    struct Limit
    {
        float pos = 0.3f;      // the largest distance of the hitbox centres in the first 2 s (m): the aim of the task
        float speed = 0.05f;   // the largest difference of the speeds, a share of the speed of RocketSim (a floor of 5 m/s under it)
        float orient = 3.0f;   // degrees
    };

    // The driving on the flat floor, the air and the ball are the same to a centimetre; what is looser is what ends in a hit of the
    // hitbox on the floor (Bullet and Jolt solve those differently) and the hit of two cars.
    Limit LimitOf(const char* name)
    {
        const struct
        {
            const char* name;
            Limit limit;
        } special[] = {{"roof_drop", {0.3f, 0.25f, 15.0f}},   {"hop_wave_dash", {0.3f, 0.05f, 10.0f}}, {"roof_flip", {0.3f, 0.10f, 6.0f}},
                       {"landing_pitch14_fast", {0.3f, 0.05f, 3.0f}}};
        for (const auto& entry : special)
        {
            if (std::strcmp(entry.name, name) == 0)
            {
                return entry.limit;
            }
        }
        return Limit{};
    }
}

int main(int argc, char** argv)
{
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--verbose") == 0)
        {
            g_verbose = true;
        }
        else if (std::strcmp(argv[i], "--ball-friction") == 0 && i + 1 < argc)
        {
            g_ballFriction = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--ball-probe") == 0)
        {
            RlTuning t;
            RlSim sim(t, 1);
            sim.ResetCar(0, 0.0f, -40.0f, 0.0f);
            sim.ResetBall({0.0f, t.ball.radius, 10.0f}, {0.0f, 0.0f, 8.0f});
            sim.SetCarPose(0, {0.0f, 0.3776f, -40.0f}, {0, 0, 0, 1}, {});
            sim.SetCarBoost(0, 33.3f);
            RlCarInput in{};
            for (int k = 0; k < 90; ++k)
            {
                sim.Step(&in);
                if (k % 6 == 5)
                {
                    const RlBallState& b = sim.State().ball;
                    std::printf("t %3d y %.3f vz %.3f vy %.3f wx %.3f\n", k + 1, static_cast<double>(b.position.y), static_cast<double>(b.linearVelocity.z),
                                static_cast<double>(b.linearVelocity.y), static_cast<double>(b.angularVelocity.x));
                }
            }
            return 0;
        }
        else if (std::strcmp(argv[i], "--car-restitution") == 0 && i + 1 < argc)
        {
            g_carRestitution = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--dump") == 0)
        {
            g_dump = true;
        }
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
        {
            only = argv[++i];
        }
    }

    RlTuning tuning;  // the defaults: assets/data/rl_tuning.json is the same (the test of rl_physics sees to that)
    if (g_carRestitution >= 0.0f)
    {
        tuning.car.restitution = g_carRestitution;
    }
    if (g_ballFriction >= 0.0f)
    {
        tuning.ball.friction = g_ballFriction;
    }
    std::printf("differential test against RocketSim: the largest differences over the run (position in m, speed in %% of RocketSim's, angles in degrees)\n");
    std::printf("  %-22s %7s %7s %7s %7s %7s %7s %6s  %s\n", "scenario", "pos", "pos 2s", "speed%", "vel", "ang", "orient", "off at", "ball / second car");
    for (int index = 0; index < rldiff::kScenarioCount; ++index)
    {
        const rldiff::Scenario sc = rldiff::Get(index);
        if (sc.ticks <= 0 || (only != nullptr && std::strcmp(only, sc.name) != 0))
        {
            continue;
        }
        std::vector<Row> rows;
        const std::string path = std::string(RL_DIFF_TRACES) + "/" + sc.name + ".csv";
        if (!ReadTrace(path, rows))
        {
            std::printf("  cannot read %s\n", path.c_str());
            CHECK(false);
            continue;
        }
        const Result r = Run(index, rows, tuning);
        char extra[160] = "";
        if (sc.ball)
        {
            std::snprintf(extra, sizeof(extra), "ball %.2f m %.0f%%", static_cast<double>(r.ballPosMax), static_cast<double>(r.ballSpeedMax * 100.0f));
        }
        if (sc.second)
        {
            std::snprintf(extra, sizeof(extra), "second car %.2f m %.0f%%, demolished at tick %d (RocketSim %d)", static_cast<double>(r.secondPosMax),
                          static_cast<double>(r.secondSpeedMax * 100.0f), r.demoTickOurs, r.demoTickRs);
        }
        const Limit limit = LimitOf(sc.name);
        CHECK(r.pos2s <= limit.pos);
        CHECK(r.speedMax <= limit.speed);
        CHECK(r.orientMax <= limit.orient);
        if (sc.ball)
        {
            CHECK(r.ballPosMax <= 0.4f);
            CHECK(r.ballSpeedMax <= 0.02f);
        }
        if (sc.second)
        {
            CHECK(r.secondPosMax <= 0.3f);
            CHECK(r.secondSpeedMax <= 0.05f);
            // RocketSim's trace has every 4th tick: the demolition is in the 4 ticks before the first sample that shows it
            CHECK((r.demoTickRs < 0 && r.demoTickOurs < 0) || (r.demoTickOurs > r.demoTickRs - 4 && r.demoTickOurs <= r.demoTickRs));
        }
        std::printf("  %-22s %7.2f %7.2f %7.1f %7.2f %7.2f %7.1f %6d  %s\n", sc.name, static_cast<double>(r.posMax), static_cast<double>(r.pos2s),
                    static_cast<double>(r.speedMax * 100.0f), static_cast<double>(r.velMax), static_cast<double>(r.angMax), static_cast<double>(r.orientMax), r.firstOff, extra);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
