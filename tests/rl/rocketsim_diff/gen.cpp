// gen.cpp: writes the reference traces of RocketSim for the differential test of the Rocket League car of this engine.
//
// Built outside of the engine (see README.md next to this file) against RocketSim, https://github.com/ZealanL/RocketSim (MIT,
// Copyright (c) 2022 ZealanL), commit c2baacb8f4b441dd8505e63c2aeb5a1679b60b02. It runs the game mode THE_VOID (no arena meshes of the
// game are loaded) with a flat floor added to its bullet world: a static plane at z = 0 with the friction (0.6) and the restitution (0.3)
// RocketSim gives to the floor of the real arena. The scenarios are in tests/rl/RlDiffScenarios.h and are driven tick by tick.
//
// Usage: rsgen <output directory>. One csv per scenario. The axes of the engine are x right, y up, z forward (RocketSim: x forward,
// y right, z up, in unreal units); everything is converted: metres, engine axes, hitbox centre as the position of the car.

#include "RocketSim.h"

#include "../RlDiffScenarios.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace RocketSim;

namespace
{
    constexpr float kPi = 3.14159265358979f;

    struct V3
    {
        float x, y, z;
    };

    // engine -> RocketSim (the inverse of the cyclic permutation the output uses): forward z -> x, right x -> y, up y -> z
    Vec ToRs(V3 e) { return Vec(e.z * 100.0f, e.x * 100.0f, e.y * 100.0f); }
    V3 FromRs(const Vec& v) { return {v.y * 0.01f, v.z * 0.01f, v.x * 0.01f}; }
    V3 FromRsDir(const Vec& v) { return {v.y, v.z, v.x}; }

    struct Mat
    {
        float m[3][3];
    };
    Mat Mul(const Mat& a, const Mat& b)
    {
        Mat r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    r.m[i][j] += a.m[i][k] * b.m[k][j];
        return r;
    }
    Mat RotY(float deg)
    {
        const float a = deg * kPi / 180.0f, c = std::cos(a), s = std::sin(a);
        return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
    }
    Mat RotX(float deg)
    {
        const float a = deg * kPi / 180.0f, c = std::cos(a), s = std::sin(a);
        return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
    }
    Mat RotZ(float deg)
    {
        const float a = deg * kPi / 180.0f, c = std::cos(a), s = std::sin(a);
        return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
    }
    V3 Column(const Mat& a, int c) { return {a.m[0][c], a.m[1][c], a.m[2][c]}; }

    // the hitbox sits in front of and above the origin of the car (the centre of mass): 13.8757 uu forward and 20.755 uu up for the Octane
    Vec kHitboxOffset(13.8757f, 0.0f, 20.755f);

    const CarConfig& ConfigOf(const std::string& name)
    {
        if (name == "dominus") return CAR_CONFIG_DOMINUS;
        if (name == "plank") return CAR_CONFIG_PLANK;
        if (name == "breakout") return CAR_CONFIG_BREAKOUT;
        if (name == "hybrid") return CAR_CONFIG_HYBRID;
        if (name == "merc") return CAR_CONFIG_MERC;
        return CAR_CONFIG_OCTANE;
    }

    Vec RotateRs(const RotMat& r, const Vec& v) { return r.forward * v.x + r.right * v.y + r.up * v.z; }

    double D(float v) { return static_cast<double>(v); }
}

int main(int argc, char** argv)
{
    // rsgen <output directory> [octane | dominus | plank | breakout | hybrid | merc]: the Octane runs every scenario (traces/<name>.csv), the
    // other classes the scenarios of rldiff::InCarSubset (traces/<class>_<name>.csv)
    const std::string outDir = argc > 1 ? argv[1] : ".";
    const std::string carName = argc > 2 ? argv[2] : "octane";
    const CarConfig& carConfig = ConfigOf(carName);
    const bool octane = carName == "octane";
    kHitboxOffset = carConfig.hitboxPosOffset;
    RocketSim::Init("none", true);

    for (int index = 0; index < rldiff::kScenarioCount; ++index)
    {
        const rldiff::Scenario sc = rldiff::Get(index);
        if (sc.ticks <= 0 || (!octane && !rldiff::InCarSubset(sc.name)))
        {
            continue;
        }

        ArenaConfig config;
        Arena* arena = Arena::Create(GameMode::THE_VOID, config, 120);
        auto* floorShape = new btStaticPlaneShape(btVector3(0, 0, 1), 0);
        btRigidBody* floorBody = arena->_AddStaticCollisionShape(floorShape);
        floorBody->setRestitution(RLConst::ARENA_COLLISION_BASE_RESTITUTION);
        floorBody->setFriction(RLConst::ARENA_COLLISION_BASE_FRICTION);
        floorBody->setRollingFriction(0.0f);

        // the ball: at rest where the scenario says, or far away in a corner of the void
        {
            BallState ball;
            if (sc.ball)
            {
                const float y = sc.ballY > 0.0f ? sc.ballY : 0.9125f;
                ball.pos = ToRs({sc.ballX, y, sc.ballZ});
                ball.vel = ToRs({0.0f, 0.0f, sc.ballVz});
                if (ball.vel.IsZero())
                {
                    ball.vel.z = 0.001f;  // a ball with no velocity at all is put to sleep by RocketSim and does not fall
                }
            }
            else
            {
                ball.pos = Vec(5000, 5000, 100);
            }
            arena->ball->SetState(ball);
        }

        // car 0
        Car* car = arena->AddCar(Team::BLUE, carConfig);
        {
            const Mat r = Mul(Mul(RotY(sc.car.yawDeg), RotX(sc.car.pitchDeg)), RotZ(sc.car.rollDeg));
            const V3 f = Column(r, 2), right = Column(r, 0), up = Column(r, 1);
            const RotMat rot(ToRs(f) * 0.01f, ToRs(right) * 0.01f, ToRs(up) * 0.01f);
            CarState st;
            st.rotMat = rot;
            // origin of the car = centre of the hitbox minus the offset; a car standing on its wheels rests with the origin 17 uu high
            const Vec centre = ToRs({sc.car.x, sc.car.y > 0.0f ? sc.car.y : 0.0f, sc.car.z});
            st.pos = centre - RotateRs(rot, kHitboxOffset);
            if (sc.car.y <= 0.0f)
            {
                st.pos.z = RLConst::CAR_SPAWN_REST_Z;
            }
            st.vel = RotateRs(rot, Vec(sc.car.speed * 100.0f, 0, 0));
            st.boost = sc.boostStart;
            car->SetState(st);
        }
        Car* second = nullptr;
        if (sc.second)
        {
            second = arena->AddCar(Team::ORANGE, carConfig);
            CarState st;
            st.pos = ToRs({sc.secondX, 0.0f, sc.secondZ}) - RotateRs(st.rotMat, kHitboxOffset);  // the centre of the hitbox is where the scenario says
            st.pos.z = RLConst::CAR_SPAWN_REST_Z;
            st.boost = 0;
            second->SetState(st);
        }

        const std::string path = outDir + "/" + (octane ? "" : carName + "_") + sc.name + ".csv";
        FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr)
        {
            std::printf("cannot write %s\n", path.c_str());
            return 1;
        }
        std::fprintf(file, "tick,px,py,pz,vx,vy,vz,wx,wy,wz,fx,fy,fz,ux,uy,uz,wheels,boost,flipping,bpx,bpy,bpz,bvx,bvy,bvz,sx,sy,sz,svx,svy,svz,sdemo\n");

        const auto write = [&](int tick) {
            const CarState s = car->GetState();
            const Vec centre = s.pos + RotateRs(s.rotMat, kHitboxOffset);
            const V3 p = FromRs(centre), v = FromRs(s.vel), w = FromRsDir(s.angVel);
            const V3 f = FromRsDir(s.rotMat.forward), u = FromRsDir(s.rotMat.up);
            int wheels = 0;
            for (int i = 0; i < 4; ++i)
                wheels += s.wheelsWithContact[i] ? 1 : 0;
            const BallState b = arena->ball->GetState();
            const V3 bp = FromRs(b.pos), bv = FromRs(b.vel);
            V3 sp{0, 0, 0}, sv{0, 0, 0};
            int sdemo = 0;
            if (second != nullptr)
            {
                const CarState t = second->GetState();
                sp = FromRs(t.pos + RotateRs(t.rotMat, kHitboxOffset));
                sv = FromRs(t.vel);
                sdemo = t.isDemoed ? 1 : 0;
            }
            std::fprintf(file, "%d,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%d,%.4g,%d,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%d\n",
                         tick, D(p.x), D(p.y), D(p.z), D(v.x), D(v.y), D(v.z), D(w.x), D(w.y), D(w.z), D(f.x), D(f.y), D(f.z), D(u.x), D(u.y), D(u.z), wheels, D(s.boost),
                         s.isFlipping ? 1 : 0, D(bp.x), D(bp.y), D(bp.z), D(bv.x), D(bv.y), D(bv.z), D(sp.x), D(sp.y), D(sp.z), D(sv.x), D(sv.y), D(sv.z), sdemo);
        };

        write(0);
        for (int t = 0; t < sc.ticks; ++t)
        {
            const rldiff::Input in = rldiff::At(index, t);
            car->controls = CarControls();
            car->controls.throttle = in.throttle;
            car->controls.steer = in.steer;
            // in the air the steering axis is the yaw; the roll is its own axis
            car->controls.yaw = in.steer;
            car->controls.pitch = in.pitch;
            car->controls.roll = in.roll;
            car->controls.jump = in.jump;
            car->controls.boost = in.boost;
            car->controls.handbrake = in.handbrake;
            arena->Step(1);
            if ((t + 1) % 4 == 0)
            {
                write(t + 1);
            }
        }
        std::fclose(file);
        std::printf("%s: %d ticks\n", sc.name, sc.ticks);
        delete arena;
    }
    return 0;
}
