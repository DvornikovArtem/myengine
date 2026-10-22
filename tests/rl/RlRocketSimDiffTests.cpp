// RlRocketSimDiffTests.cpp
// The differential test against RocketSim: the same scripted inputs (tests/rl/RlDiffScenarios.h) drive the car of this engine and RocketSim
// (github.com/ZealanL/RocketSim, MIT, THE_VOID mode with a flat floor, no arena meshes of the game); the traces of RocketSim were written
// by tests/rl/rocketsim_diff/gen.cpp and are in tests/rl/rocketsim_diff/traces (see the README there). The test replays every scenario on
// the flat middle of our arena and compares the positions, speeds, angular speeds and orientations tick by tick.
// Arguments: --verbose prints every scenario, --only <name> runs one.
//
// G4i: after the Octane scenarios the same is done for every car of the catalog (assets/data/cars/*.physics.json) with the hitbox and the wheels
// of its class and WITHOUT the differences of ours (RlCarCharacter at 1), against the traces <class>_<scenario>.csv that RocketSim wrote for the
// Dominus, Plank, Breakout, Hybrid and Merc (rsgen <dir> <class>). It runs the scenarios of rldiff::InCarSubset.

#include <rl/RlCars.h>
#include <rl/RlSim.h>
#include <rl/RlTuning.h>

#include "RlDiffScenarios.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
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
        float secondFinalRs = 0, secondFinalOurs = 0;
        // G4j: where the car ends. up.y of the car (1 on its wheels, -1 on its roof, near 0 on its side), the wheels on the ground, and the last tick
        // the car was not standing on its wheels (up.y < 0.8 or fewer than 3 wheels)
        float upRs = 0, upOurs = 0;
        int wheelsRs = 0, wheelsOurs = 0;
        int unsettledRs = -1, unsettledOurs = -1;
    };

    bool g_dump = false;
    bool g_carsFuzz = false;
    int g_fuzzOutcomes = 0, g_fuzzSame = 0, g_fuzzRsWheels = 0, g_fuzzOurWheels = 0;
    float g_ballFriction = -1.0f;
    float g_carRestitution = -1.0f;

    Result Run(int index, const std::vector<Row>& rows, const RlTuning& tuning, const RlCarVariant& variant = RlCarVariant{})
    {
        const rldiff::Scenario sc = rldiff::Get(index);
        const int cars = sc.second ? 2 : 1;
        const RlCarVariant variants[2] = {variant, variant};
        RlSim sim(tuning, cars, variants);
        // a car that stands on its wheels has the origin of the car 17 uu up (RocketSim): the hitbox is that much higher plus its own offset
        const float restY = variant.hasBody ? 0.17f - variant.values.comOffsetY : 0.3776f;
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
            const float y = sc.car.y > 0.0f ? sc.car.y : restY;
            sim.SetCarPose(0, {sc.car.x, y, sc.car.z}, q, {nose.x * sc.car.speed, nose.y * sc.car.speed, nose.z * sc.car.speed});
        }
        sim.SetCarBoost(0, sc.boostStart);
        if (sc.second)
        {
            sim.SetCarPose(1, {sc.secondX, restY, sc.secondZ}, {0, 0, 0, 1}, {});
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
                res.upRs = r.up.y;
                res.upOurs = u.y;
                res.wheelsRs = r.wheels;
                res.wheelsOurs = c.wheelContacts;
                if (r.up.y < 0.8f || r.wheels < 3)
                {
                    res.unsettledRs = done;
                }
                if (u.y < 0.8f || c.wheelContacts < 3)
                {
                    res.unsettledOurs = done;
                }
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
                    res.secondFinalRs = ss;
                    res.secondFinalOurs = Len(FromRl(s.linearVelocity));
                }
                if (sc.second && r.secondDemo != 0 && res.demoTickRs < 0)
                {
                    res.demoTickRs = done;
                }
            }
        }
        return res;
    }

    // G4j: the flights that end with the car rolling or spinning into the floor: a side flip, a diagonal flip, an air roll that is let go of. Bullet and Jolt
    // part ways at the first touchdown of a car that turns fast (Bullet starts with the one point of the hitbox that touches first and the car turns about
    // it, Jolt takes the whole edge or face at once and stops the turn), so the two are compared in two ways: tick for tick until that touchdown, and by how the
    // flight ends (the car on its wheels, how long it took) after it.
    bool IsImpactScenario(const char* name)
    {
        return std::strcmp(name, "flip_right_recover") == 0 || std::strcmp(name, "flip_diag_recover") == 0 || std::strcmp(name, "air_roll_land_throttle") == 0;
    }

    // The first touchdown of RocketSim's car after the flip (from the second 0.8 on): a wheel on the ground, or the velocity changing by more than gravity does
    int ImpactTick(const std::vector<Row>& rows)
    {
        for (size_t i = 1; i < rows.size(); ++i)
        {
            if (rows[i].tick < 96)
            {
                continue;
            }
            if (rows[i].wheels > 0 || Len(Sub(rows[i].vel, rows[i - 1].vel)) > 1.2f)
            {
                return rows[i].tick;
            }
        }
        return -1;
    }

    void CheckImpactScenario(const Result& r, const std::vector<Row>& rows)
    {
        const int impact = ImpactTick(rows);
        CHECK(impact > 0);
        CHECK(r.firstOff < 0 || r.firstOff >= impact - 8);   // the same flight until the touchdown
        const bool rsOn = r.upRs > 0.8f && r.wheelsRs >= 3;
        const bool ourOn = r.upOurs > 0.8f && r.wheelsOurs >= 3;
        CHECK(rsOn && ourOn);                                // both end on their wheels
        CHECK(std::abs(r.unsettledOurs - r.unsettledRs) <= 90);   // ours is on its wheels for good within 0.75 s of RocketSim
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
        else if (std::strcmp(argv[i], "--cars-fuzz") == 0)
        {
            g_carsFuzz = true;
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
        if (sc.ticks <= 0 || (only != nullptr && std::strcmp(only, sc.name) != 0) || std::strncmp(sc.name, "fuzz_", 5) == 0 || std::strncmp(sc.name, "spin_", 5) == 0 || std::strncmp(sc.name, "ext_", 4) == 0)
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
        if (IsImpactScenario(sc.name))
        {
            CheckImpactScenario(r, rows);
        }
        else
        {
            CHECK(r.pos2s <= limit.pos);
            CHECK(r.speedMax <= limit.speed);
            CHECK(r.orientMax <= limit.orient);
        }
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

    // ---- G4j: the flights at random (rldiff::kFuzzFirst ..., rldiff::kSpinFirst ...): where the car ends and how fast it gets onto its wheels, against RocketSim
    for (const auto& family : {std::make_pair(rldiff::kFuzzFirst, 60), std::make_pair(rldiff::kSpinFirst, 60), std::make_pair(rldiff::kSpinFirst + 60, 60), std::make_pair(rldiff::kExtFirst, 100), std::make_pair(rldiff::kExtFirst + 100, 100)})
    {
        int outcomes = 0, sameOutcome = 0, both = 0, slower = 0, faster = 0;
        int rsWheels = 0, ourWheels = 0, rsRoof = 0, ourRoof = 0, rsSide = 0, ourSide = 0;
        std::vector<float> lastOffRs, lastOffOurs;   // ticks from the first touchdown to the last tick off the wheels
        std::printf("random flights from %d (%s): the end of the flight, up.y and the last tick off its wheels\n", family.first,
                    family.first == rldiff::kFuzzFirst ? "flip, roll, recover; the throttle at a random time"
                    : family.first == rldiff::kSpinFirst ? "a long roll, pitch or yaw let go of; the throttle late"
                                                         : family.first == rldiff::kSpinFirst + 60 ? "the same without the throttle: how the car rests when nobody drives it"
                    : family.first == rldiff::kExtFirst ? "100 flights of the same kind, no throttle"
                                                        : "the same 100 flights with the throttle held from the first tick");
        for (int index = family.first; index < family.first + family.second; ++index)
        {
            const rldiff::Scenario sc = rldiff::Get(index);
            std::vector<Row> rows;
            if (sc.ticks <= 0 || !ReadTrace(std::string(RL_DIFF_TRACES) + "/" + sc.name + ".csv", rows))
            {
                continue;
            }
            const Result r = Run(index, rows, tuning);
            const int touch = ImpactTick(rows) > 0 ? ImpactTick(rows) : 0;
            lastOffRs.push_back(static_cast<float>(r.unsettledRs - touch));
            lastOffOurs.push_back(static_cast<float>(r.unsettledOurs - touch));
            const bool rsOn = r.upRs > 0.8f && r.wheelsRs >= 3;
            const bool ourOn = r.upOurs > 0.8f && r.wheelsOurs >= 3;
            ++outcomes;
            rsWheels += rsOn ? 1 : 0;
            ourWheels += ourOn ? 1 : 0;
            rsRoof += r.upRs < -0.5f ? 1 : 0;
            ourRoof += r.upOurs < -0.5f ? 1 : 0;
            rsSide += (!rsOn && r.upRs >= -0.5f) ? 1 : 0;
            ourSide += (!ourOn && r.upOurs >= -0.5f) ? 1 : 0;
            sameOutcome += rsOn == ourOn ? 1 : 0;
            if (rsOn && ourOn)
            {
                ++both;
                slower += r.unsettledOurs > r.unsettledRs + 12 ? 1 : 0;
                faster += r.unsettledOurs + 12 < r.unsettledRs ? 1 : 0;
            }
            if (g_verbose || rsOn != ourOn)
            {
                std::printf("  %-10s RocketSim up %5.2f wheels %d off wheels until %3d | ours up %5.2f wheels %d until %3d | first off at %d%s\n", sc.name,
                            static_cast<double>(r.upRs), r.wheelsRs, r.unsettledRs, static_cast<double>(r.upOurs), r.wheelsOurs, r.unsettledOurs, r.firstOff,
                            rsOn != ourOn ? "   <-- the end differs" : "");
            }
        }
        std::printf("  %d flights: at the end on the wheels RocketSim %d, ours %d; on the roof %d / %d; on the side or in the air %d / %d; the same end in %d;\n"
                    "  of the %d that both end on the wheels ours is more than 0.1 s later in %d, earlier in %d\n",
                    outcomes, rsWheels, ourWheels, rsRoof, ourRoof, rsSide, ourSide, sameOutcome, both, slower, faster);
        {
            const auto quantile = [](std::vector<float> v, float q) {
                std::sort(v.begin(), v.end());
                return v.empty() ? 0.0f : v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<float>(v.size())))];
            };
            std::printf("  ticks from the first touchdown to the last tick off the wheels (median / 90 %% / worst): RocketSim %.0f / %.0f / %.0f, ours %.0f / %.0f / %.0f%c",
                        static_cast<double>(quantile(lastOffRs, 0.5f)), static_cast<double>(quantile(lastOffRs, 0.9f)), static_cast<double>(quantile(lastOffRs, 1.0f)),
                        static_cast<double>(quantile(lastOffOurs, 0.5f)), static_cast<double>(quantile(lastOffOurs, 0.9f)), static_cast<double>(quantile(lastOffOurs, 1.0f)), 10);
        }
        // the flights end the same way in nine of ten, ours is on its wheels as often as RocketSim's (a few per cent less), and it is on them for good as soon
        CHECK(outcomes > 0 && sameOutcome * 100 >= outcomes * 88);
        CHECK(ourWheels * 100 >= rsWheels * 92);
        CHECK(!lastOffRs.empty() && !lastOffOurs.empty());
        if (!lastOffRs.empty() && !lastOffOurs.empty())
        {
            const auto at = [](std::vector<float> v, float q) {
                std::sort(v.begin(), v.end());
                return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<float>(v.size())))];
            };
            CHECK(at(lastOffOurs, 0.5f) <= at(lastOffRs, 0.5f) + 12.0f);   // 0.1 s
            CHECK(at(lastOffOurs, 0.9f) <= at(lastOffRs, 0.9f) + 36.0f);   // 0.3 s
        }
        g_fuzzOutcomes += outcomes;
        g_fuzzSame += sameOutcome;
        g_fuzzRsWheels += rsWheels;
        g_fuzzOurWheels += ourWheels;
    }

    // ---- the cars of the catalog, with the layer of RocketSim only
    RlCarCatalog catalog;
    std::string catalogError;
    if (!catalog.LoadDirectory(RL_CARS_DIR, &catalogError))
    {
        std::printf("  cannot read the cars: %s\n", catalogError.c_str());
        CHECK(false);
    }
    else
    {
        CHECK(catalog.Count() == rldiff::kCarClassCount);
        std::printf("\nthe %d cars of the catalog, the hitbox and the wheels of RocketSim, no differences of ours\n", catalog.Count());
        std::printf("  %-26s %7s %7s %7s %7s %7s %7s %6s\n", "car / scenario", "pos", "pos 2s", "speed%", "vel", "ang", "orient", "off at");
        for (int c = 0; c < catalog.Count(); ++c)
        {
            const RlCarVariant variant = RlBodyOnly(catalog.Spec(c).variant);
            std::string cls = catalog.Spec(c).hitboxClass;
            for (char& ch : cls)
            {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            CHECK(variant.hasBody);
            CHECK(cls == rldiff::CarClassName(c));
            float worstPos = 0.0f, worstSpeed = 0.0f;
            for (int index = 0; index < rldiff::kScenarioCount; ++index)
            {
                const rldiff::Scenario sc = rldiff::Get(index);
                if (sc.ticks <= 0 || !rldiff::InCarSubset(sc.name) || (only != nullptr && std::strcmp(only, sc.name) != 0))
                {
                    continue;
                }
                std::vector<Row> rows;
                const std::string file = (cls == "octane" ? std::string() : cls + "_") + sc.name + ".csv";
                if (!ReadTrace(std::string(RL_DIFF_TRACES) + "/" + file, rows))
                {
                    std::printf("  cannot read %s\n", file.c_str());
                    CHECK(false);
                    continue;
                }
                const Result r = Run(index, rows, tuning, variant);
                const Limit limit = LimitOf(sc.name);
                if (IsImpactScenario(sc.name))
                {
                    CheckImpactScenario(r, rows);
                }
                else
                {
                    CHECK(r.pos2s <= limit.pos);
                    CHECK(r.speedMax <= limit.speed);
                    CHECK(r.orientMax <= limit.orient);
                }
                if (sc.ball)
                {
                    CHECK(r.ballPosMax <= 0.4f);
                    CHECK(r.ballSpeedMax <= 0.02f);
                }
                if (sc.second)
                {
                    // the car that was bumped hops and lands again, and the landing of RocketSim's Hybrid takes 1 m/s more than ours (the others agree to
                    // 0.1 m/s): the position of the second car is allowed 0.8 m after two seconds, its speed 5 %
                    CHECK(r.secondPosMax <= 0.8f);
                    CHECK(r.secondSpeedMax <= 0.05f);
                    CHECK((r.demoTickRs < 0 && r.demoTickOurs < 0) || (r.demoTickOurs > r.demoTickRs - 4 && r.demoTickOurs <= r.demoTickRs));
                }
                worstPos = std::max(worstPos, r.pos2s);
                worstSpeed = std::max(worstSpeed, r.speedMax);
                const bool contactOff = (sc.ball && (r.ballPosMax > 0.4f || r.ballSpeedMax > 0.02f)) || (sc.second && (r.secondPosMax > 0.3f || r.secondSpeedMax > 0.05f));
                if (g_verbose || contactOff || (!IsImpactScenario(sc.name) && (r.pos2s > limit.pos || r.speedMax > limit.speed || r.orientMax > limit.orient)))
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "%s / %s", cls.c_str(), sc.name);
                    char extra[128] = "";
                    if (sc.ball)
                    {
                        std::snprintf(extra, sizeof(extra), "  ball %.2f m %.1f%%", static_cast<double>(r.ballPosMax), static_cast<double>(r.ballSpeedMax * 100.0f));
                    }
                    if (sc.second)
                    {
                        std::snprintf(extra, sizeof(extra), "  second %.2f m %.1f%% (end speed %.2f, RocketSim %.2f), demolished at %d (RocketSim %d)", static_cast<double>(r.secondPosMax),
                                      static_cast<double>(r.secondSpeedMax * 100.0f), static_cast<double>(r.secondFinalOurs), static_cast<double>(r.secondFinalRs), r.demoTickOurs,
                                      r.demoTickRs);
                    }
                    std::printf("  %-26s %7.2f %7.2f %7.1f %7.2f %7.2f %7.1f %6d%s\n", name, static_cast<double>(r.posMax), static_cast<double>(r.pos2s),
                                static_cast<double>(r.speedMax * 100.0f), static_cast<double>(r.velMax), static_cast<double>(r.angMax), static_cast<double>(r.orientMax), r.firstOff, extra);
                }
            }
            std::printf("  %-10s worst of the scenarios: position in the first 2 s %.3f m, speed %.2f%%\n", cls.c_str(), static_cast<double>(worstPos), static_cast<double>(worstSpeed * 100.0f));
        }
    }
    // ---- G4j: how every car of the catalog lands the same random flights: with the layer of RocketSim only and with our factors on top (docs/game/rl-cars.md)
    if (g_carsFuzz)
    {
        RlCarCatalog cars;
        std::string error;
        if (cars.LoadDirectory(RL_CARS_DIR, &error))
        {
            std::printf("\nthe flights of the family without the throttle, car by car: the end on the wheels / on the roof / on the side, the mean last tick off the wheels\n");
            for (int c = 0; c < cars.Count(); ++c)
            {
                for (int layer = 0; layer < 2; ++layer)
                {
                    const RlCarVariant variant = layer == 0 ? RlBodyOnly(cars.Spec(c).variant) : cars.Spec(c).variant;
                    int wheels = 0, roof = 0, side = 0, count = 0;
                    double settled = 0.0;
                    for (int index = rldiff::kSpinFirst + 60; index < rldiff::kSpinFirst + 120; ++index)
                    {
                        const rldiff::Scenario sc = rldiff::Get(index);
                        std::vector<Row> dummy;
                        for (int t = 0; t <= sc.ticks; t += 4)
                        {
                            Row row;
                            row.tick = t;
                            dummy.push_back(row);
                        }
                        const Result r = Run(index, dummy, tuning, variant);
                        const bool on = r.upOurs > 0.8f && r.wheelsOurs >= 3;
                        wheels += on ? 1 : 0;
                        roof += r.upOurs < -0.5f ? 1 : 0;
                        side += (!on && r.upOurs >= -0.5f) ? 1 : 0;
                        settled += r.unsettledOurs;
                        ++count;
                    }
                    std::printf("  %-8s %-26s wheels %2d roof %2d side %2d of %d, off the wheels until tick %.0f\n", cars.Spec(c).id.c_str(),
                                layer == 0 ? "(the layer of RocketSim)" : "(with our factors)", wheels, roof, side, count, settled / std::max(count, 1));
                }
            }
        }
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
