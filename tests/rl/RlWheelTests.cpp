// RlWheelTests.cpp
// The look of the wheels of the cars (RL-B2, docs/game/rl-cars.md, "Колёса"): the spin and the steer that RlWheelAnimator works out from the state of a
// car. The state is all a picture has of a car, whoever drives it: the local player, a bot, a car that the network brings, a car of a replay. So the checks
// run on states of the sim (driving forward and back, steering, standing, six different cars), on states made by hand (the air, a stopped picture) and
// on the world that a network client draws for a car of another player.

#include <rl/RlCars.h>
#include <rl/RlMatch.h>
#include <rl/RlNet.h>
#include <rl/RlSim.h>
#include <rl/RlTuning.h>
#include <rl/RlWheels.h>
#include <rl/net/NetLink.h>
#include <rl/net/NetReliable.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace
{
    using namespace rl;

    int g_failures = 0;
    int g_checks = 0;

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

    constexpr float kPi = 3.14159265f;
    constexpr float kDt = kTickDt;

    bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

    // the angle of a step of the spin, -pi..pi (a wheel turns less than half a turn between two frames)
    float Delta(float from, float to)
    {
        float d = to - from;
        while (d > kPi)
        {
            d -= 2.0f * kPi;
        }
        while (d < -kPi)
        {
            d += 2.0f * kPi;
        }
        return d;
    }

    // Follows the animation of one car, the frames of the picture, and adds up what the wheels turned (the pose alone is wrapped)
    struct Follow
    {
        RlWheelAnimator animator;
        int slot = 0;
        float spin[kWheelCount] = {};
        float last[kWheelCount] = {};
        float steerMin = 0.0f;
        float steerMax = 0.0f;
        float rearSteerAbs = 0.0f;
        float path = 0.0f;       // metres the car travelled while measuring
        RlVec3 lastPosition{};
        bool started = false;

        void Frame(const RlCarState& car, const RlCarTuning& tuning, float dt)
        {
            animator.Update(slot, car, tuning, dt);
            for (int w = 0; w < kWheelCount; ++w)
            {
                const RlWheelPose pose = animator.Pose(slot, w);
                if (started)
                {
                    spin[w] += Delta(last[w], pose.spin);
                }
                last[w] = pose.spin;
                if (RlWheelAnimator::IsFront(w))
                {
                    steerMin = std::min(steerMin, pose.steer);
                    steerMax = std::max(steerMax, pose.steer);
                }
                else
                {
                    rearSteerAbs = std::max(rearSteerAbs, std::fabs(pose.steer));
                }
            }
            if (started)
            {
                const float dx = car.position.x - lastPosition.x;
                const float dy = car.position.y - lastPosition.y;
                const float dz = car.position.z - lastPosition.z;
                path += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            lastPosition = car.position;
            started = true;
        }

        // forget what was added up (the animation goes on)
        void Restart()
        {
            for (int w = 0; w < kWheelCount; ++w)
            {
                spin[w] = 0.0f;
            }
            steerMin = steerMax = rearSteerAbs = path = 0.0f;
            started = false;
        }
    };

    // The positive rotation about the axle of the wheel that the picture uses (DirectXMath: the same convention as the transform of the entity) moves the
    // top of the wheel to the front of the car
    void TestConvention()
    {
        std::printf("the sign of the spin\n");
        const DirectX::XMVECTOR q = DirectX::XMQuaternionRotationAxis(DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), 0.1f);
        const DirectX::XMVECTOR top = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), q);
        CHECK(DirectX::XMVectorGetZ(top) > 0.05f);  // +Z is the front of the car
        const DirectX::XMVECTOR qy = DirectX::XMQuaternionRotationAxis(DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), 0.1f);
        const DirectX::XMVECTOR nose = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), qy);
        CHECK(DirectX::XMVectorGetX(nose) > 0.05f);  // a positive steer turns the front of the wheel to the right (+X)
    }

    // ---- the sim ------------------------------------------------------------------------------------------------------------------------

    void TestDriving()
    {
        std::printf("driving forward\n");
        const RlTuning tuning;
        RlSim sim(tuning, 1);
        sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
        const RlCarTuning& car = sim.CarTuning(0);
        Follow follow;
        RlCarInput drive;
        drive.throttle = 1.0f;
        float yawRate = 0.0f;
        for (int t = 0; t < 360; ++t)
        {
            sim.Step(&drive);
            if (t == 120)
            {
                follow.Restart();
            }
            follow.Frame(sim.State().cars[0], car, kDt);
            yawRate = std::max(yawRate, std::fabs(sim.State().cars[0].angularVelocity.y));
        }
        const float distance = follow.path;
        CHECK(distance > 15.0f);
        // the wheels roll without slipping: the spin times the radius is the way (the rear and the front pair have their own radius)
        const float rear = (follow.spin[2] + follow.spin[3]) * 0.5f * car.wheelRadiusRear;
        const float front = (follow.spin[0] + follow.spin[1]) * 0.5f * car.wheelRadiusFront;
        std::printf("  way %.2f m, rear wheels roll %.2f m, front wheels %.2f m\n", distance, rear, front);
        CHECK(follow.spin[0] > 10.0f && follow.spin[1] > 10.0f && follow.spin[2] > 10.0f && follow.spin[3] > 10.0f);  // forward is positive
        CHECK(Near(rear, distance, distance * 0.05f));
        CHECK(Near(front, distance, distance * 0.05f));
        // the smaller front wheels turn faster in the same ratio as the radii
        CHECK(Near(follow.spin[0] / follow.spin[2], car.wheelRadiusRear / car.wheelRadiusFront, 0.05f));
        // left and right wheels of an axle turn alike on a straight line
        CHECK(Near(follow.spin[0], follow.spin[1], std::fabs(follow.spin[0]) * 0.03f));
        CHECK(Near(follow.spin[2], follow.spin[3], std::fabs(follow.spin[2]) * 0.03f));
        // straight: no steering to speak of; the rear wheels never turn
        CHECK(follow.steerMax < 0.03f && follow.steerMin > -0.03f);
        CHECK(follow.rearSteerAbs == 0.0f);
    }

    void TestReverse()
    {
        std::printf("driving back\n");
        const RlTuning tuning;
        RlSim sim(tuning, 1);
        sim.ResetCar(0, 0.0f, 20.0f, 0.0f);
        const RlCarTuning& car = sim.CarTuning(0);
        Follow follow;
        RlCarInput reverse;
        reverse.throttle = -1.0f;
        for (int t = 0; t < 240; ++t)
        {
            sim.Step(&reverse);
            follow.Frame(sim.State().cars[0], car, kDt);
        }
        CHECK(sim.State().cars[0].linearVelocity.z < -5.0f);
        for (int w = 0; w < kWheelCount; ++w)
        {
            CHECK(follow.spin[w] < -10.0f);  // backwards is negative
        }
        const float rear = -(follow.spin[2] + follow.spin[3]) * 0.5f * car.wheelRadiusRear;
        CHECK(Near(rear, follow.path, follow.path * 0.06f));
    }

    void TestStanding()
    {
        std::printf("standing still\n");
        const RlTuning tuning;
        RlSim sim(tuning, 1);
        sim.ResetCar(0, 0.0f, -20.0f, 0.0f);
        Follow follow;
        RlCarInput hold;
        hold.steer = 1.0f;  // the stick is turned but the car does not move: the picture has nothing to show it with
        for (int t = 0; t < 240; ++t)
        {
            sim.Step(&hold);
            follow.Frame(sim.State().cars[0], sim.CarTuning(0), kDt);
        }
        for (int w = 0; w < kWheelCount; ++w)
        {
            CHECK(std::fabs(follow.spin[w]) < 0.05f);
        }
        CHECK(follow.steerMax == 0.0f && follow.steerMin == 0.0f);
    }

    void TestSteering()
    {
        std::printf("steering\n");
        const RlTuning tuning;
        for (const float side : {1.0f, -1.0f})
        {
            RlSim sim(tuning, 1);
            sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
            const RlCarTuning& car = sim.CarTuning(0);
            Follow follow;
            RlCarInput drive;
            drive.throttle = 1.0f;
            for (int t = 0; t < 240; ++t)
            {
                sim.Step(&drive);
                follow.Frame(sim.State().cars[0], car, kDt);
            }
            follow.Restart();
            drive.steer = side;  // +1 to the right
            float yawRate = 0.0f;
            float speed = 0.0f;
            for (int t = 0; t < 90; ++t)
            {
                sim.Step(&drive);
                follow.Frame(sim.State().cars[0], car, kDt);
                yawRate = sim.State().cars[0].angularVelocity.y;
                const RlVec3& v = sim.State().cars[0].linearVelocity;
                speed = std::sqrt(v.x * v.x + v.z * v.z);
            }
            const float shown = side > 0.0f ? follow.steerMax : follow.steerMin;
            const float other = side > 0.0f ? follow.steerMin : follow.steerMax;
            std::printf("  stick %+.0f: the car yaws at %+.3f rad/s, %.1f m/s, the wheels turn to %+.3f rad (steer table %.3f rad at 10 m/s)\n", side, yawRate,
                        speed, shown, car.steerAngle[2]);
            CHECK(yawRate * side > 0.05f);                // the stick to the right turns the car to the right (the sign of the yaw)
            CHECK(shown * side > 0.04f);                  // and the front wheels to the same side
            CHECK(std::fabs(shown) <= car.steerAngle[0] + 1e-4f);  // never beyond the widest angle of the car
            CHECK(Near(std::fabs(shown), car.steerAngle[3], car.steerAngle[3] * 0.6f + 0.03f));  // about what the table gives at this speed
            CHECK(other * side > -0.02f);                 // it did not go the wrong way first
            CHECK(follow.rearSteerAbs == 0.0f);           // the rear wheels stay straight
            // the wheel of the outer side of the bend rolls faster than the inner one
            const float inner = side > 0.0f ? follow.spin[3] : follow.spin[2];
            const float outer = side > 0.0f ? follow.spin[2] : follow.spin[3];
            CHECK(outer > inner);
        }
    }

    void TestSixCars()
    {
        std::printf("six cars, each with its own wheels\n");
        RlCarCatalog catalog;
        std::string error;
        if (!catalog.LoadDirectory(RL_CARS_DIR, &error))
        {
            std::printf("  cannot load the cars: %s\n", error.c_str());
            CHECK(false);
            return;
        }
        const RlTuning tuning;
        RlCarVariant variants[6];
        for (int i = 0; i < 6; ++i)
        {
            variants[i] = catalog.Spec(i).variant;
        }
        RlSim sim(tuning, 6, variants);
        for (int i = 0; i < 6; ++i)
        {
            sim.ResetCar(i, -20.0f + 8.0f * static_cast<float>(i), -40.0f, 0.0f);
        }
        Follow follow[6];
        RlCarInput drive[6];
        for (RlCarInput& d : drive)
        {
            d.throttle = 1.0f;
        }
        for (int t = 0; t < 300; ++t)
        {
            sim.Step(drive);
            if (t == 100)
            {
                for (Follow& f : follow)
                {
                    f.Restart();
                }
            }
            for (int i = 0; i < 6; ++i)
            {
                follow[i].slot = i;
                follow[i].Frame(sim.State().cars[i], sim.CarTuning(i), kDt);
            }
        }
        for (int i = 0; i < 6; ++i)
        {
            const RlCarTuning& car = sim.CarTuning(i);
            const float rear = (follow[i].spin[2] + follow[i].spin[3]) * 0.5f * car.wheelRadiusRear;
            const float front = (follow[i].spin[0] + follow[i].spin[1]) * 0.5f * car.wheelRadiusFront;
            std::printf("  %-8s way %.2f m, rear %.2f m (r %.3f), front %.2f m (r %.3f)\n", catalog.Spec(i).id.c_str(), follow[i].path, rear, car.wheelRadiusRear, front,
                        car.wheelRadiusFront);
            CHECK(follow[i].path > 10.0f);
            CHECK(Near(rear, follow[i].path, follow[i].path * 0.05f));
            CHECK(Near(front, follow[i].path, follow[i].path * 0.05f));
        }
    }

    // ---- states made by hand --------------------------------------------------------------------------------------------------------------

    RlCarState Rolling(float speed, bool onGround)
    {
        RlCarState car;
        car.linearVelocity = {0.0f, 0.0f, speed};
        for (float& c : car.wheelCompression)
        {
            c = onGround ? 0.05f : 0.0f;
        }
        car.wheelContacts = onGround ? 4 : 0;
        car.onGround = onGround;
        return car;
    }

    void TestAirAndPause()
    {
        std::printf("the air, a stopped picture, a car put elsewhere\n");
        const RlCarTuning tuning;
        RlWheelAnimator animator;

        // on the ground at 10 m/s the rear wheel turns at speed / radius
        RlCarState car = Rolling(10.0f, true);
        for (int t = 0; t < 60; ++t)
        {
            animator.Update(0, car, tuning, kDt);
        }
        float before = animator.Pose(0, 2).spin;
        animator.Update(0, car, tuning, kDt);
        const float rate = Delta(before, animator.Pose(0, 2).spin) / kDt;
        CHECK(Near(rate, 10.0f / tuning.wheelRadiusRear, 1.5f));

        // a picture that stands (a paused match, dt = 0) leaves everything as it is
        const RlWheelPose frozen = animator.Pose(0, 2);
        animator.Update(0, car, tuning, 0.0f);
        animator.Update(0, car, tuning, -1.0f);
        CHECK(animator.Pose(0, 2).spin == frozen.spin);

        // in the air the wheels keep the spin that they had, no matter how fast the car flies, and slow down by the time constant
        car = Rolling(30.0f, false);
        float start = 0.0f;
        float rateAfter = 0.0f;
        float previous = animator.Pose(0, 2).spin;
        const int airTicks = static_cast<int>(RlWheelAnimator::kAirSpinDecay * static_cast<float>(kTickRate));
        for (int t = 0; t < airTicks; ++t)
        {
            animator.Update(0, car, tuning, kDt);
            const float now = animator.Pose(0, 2).spin;
            if (t == 0)
            {
                start = Delta(previous, now) / kDt;
            }
            rateAfter = Delta(previous, now) / kDt;
            previous = now;
        }
        std::printf("  the spin at the start of a flight %.1f rad/s, after %.1f s of it %.1f rad/s\n", start, RlWheelAnimator::kAirSpinDecay, rateAfter);
        CHECK(start > 0.9f * rate && start <= rate + 0.5f);            // not the speed of the flight
        CHECK(rateAfter > 0.30f * start && rateAfter < 0.45f * start);  // 1/e after one time constant
        // it lands: the wheels take the speed of the road in a blink
        car = Rolling(10.0f, true);
        for (int t = 0; t < 12; ++t)
        {
            animator.Update(0, car, tuning, kDt);
        }
        previous = animator.Pose(0, 2).spin;
        animator.Update(0, car, tuning, kDt);
        CHECK(Near(Delta(previous, animator.Pose(0, 2).spin) / kDt, 10.0f / tuning.wheelRadiusRear, 1.5f));

        // a car put on a spot (a kick-off, a respawn): the wheels stand straight and still
        animator.ResetCar(0);
        for (int w = 0; w < kWheelCount; ++w)
        {
            CHECK(animator.Pose(0, w).spin == 0.0f && animator.Pose(0, w).steer == 0.0f);
        }

        // nonsense does not break it
        animator.Update(-1, car, tuning, kDt);
        animator.Update(kMaxCars, car, tuning, kDt);
        CHECK(animator.Pose(-1, 0).spin == 0.0f && animator.Pose(0, 9).spin == 0.0f);
    }

    // ---- a car of another player over the net -------------------------------------------------------------------------------------------

    void TestRemoteCar(const RlTuning& tuning)
    {
        std::printf("the car of the host, seen by a client\n");
        net::LoopbackHub hub;
        net::NetSimParams simParams;
        simParams.latencyMs = 25.0f;
        simParams.jitterMs = 8.0f;
        auto hostTransport = net::MakeLoopbackTransport(hub, simParams, 1);
        auto clientTransport = net::MakeLoopbackTransport(hub, simParams, 100);
        RlNetHostOptions options;
        options.match.mode = 1;
        options.match.seed = 5;
        RlNetHost host(*hostTransport, tuning, options);
        RlNetClient client(*clientTransport, tuning, nullptr);
        std::string error;
        CHECK(host.Listen(5000, error));
        CHECK(client.Connect("5000", "guest", error));

        double now = 0.0;
        const double step = 1.0 / 240.0;
        const auto tick = [&](const RlCarInput& hostInput)
        {
            now += step;
            host.SetLocalInput(hostInput);
            client.SetLocalInput(RlCarInput{});
            host.Update(now);
            client.Update(now);
        };
        for (int i = 0; i < 240 * 10 && client.State() != RlNetClientState::Lobby; ++i)
        {
            tick(RlCarInput{});
        }
        CHECK(client.State() == RlNetClientState::Lobby);
        CHECK(host.StartMatch(now));

        // the host drives (slot 0), the client only draws it: the picture of the client has what its own prediction knows of that car
        RlCarInput drive;
        drive.throttle = 1.0f;
        drive.steer = 0.4f;
        Follow follow;
        follow.slot = 0;
        float rollSpin = 0.0f;  // what the rear wheels turned and the car travelled in the frames of rolling (not in the air, not at a stop)
        float rollPath = 0.0f;
        float steerSeen = 0.0f;
        int framesRolling = 0;
        for (int i = 0; i < 240 * 14; ++i)
        {
            tick(drive);
            if (client.State() != RlNetClientState::Playing)
            {
                continue;
            }
            const RlWorldState world = client.RenderWorld();
            const RlCarState& car = world.cars[0];
            bool allOnGround = !car.demolished;
            for (const float c : car.wheelCompression)
            {
                allOnGround = allOnGround && c > 0.0f;
            }
            const float speed = std::sqrt(car.linearVelocity.x * car.linearVelocity.x + car.linearVelocity.z * car.linearVelocity.z);
            const float spinBefore = follow.spin[2] + follow.spin[3];
            const float pathBefore = follow.path;
            follow.Frame(car, client.Match()->Sim().CarTuning(0), static_cast<float>(step));
            if (!allOnGround || speed < 3.0f)
            {
                continue;
            }
            ++framesRolling;
            rollSpin += follow.spin[2] + follow.spin[3] - spinBefore;
            rollPath += follow.path - pathBefore;
            steerSeen = std::max(steerSeen, follow.steerMax);
        }
        const RlCarTuning& car = client.Match()->Sim().CarTuning(0);
        std::printf("  %d frames of rolling, rear wheels turned %.1f rad over %.2f m, front wheels turned to %.3f rad\n", framesRolling, rollSpin * 0.5f, rollPath,
                    steerSeen);
        CHECK(client.State() == RlNetClientState::Playing);
        CHECK(framesRolling > 240);
        CHECK(rollSpin > 0.0f);
        CHECK(rollPath > 5.0f);
        CHECK(Near(rollSpin * 0.5f * car.wheelRadiusRear, rollPath, rollPath * 0.08f));
        CHECK(steerSeen > 0.03f);  // the host holds the stick to the right
        CHECK(follow.rearSteerAbs == 0.0f);
    }
}

int main()
{
    RlTuning tuning;
    std::string error;
    if (!tuning.LoadFromFile(RL_TUNING_JSON, &error))
    {
        std::printf("FAIL: cannot read %s: %s\n", RL_TUNING_JSON, error.c_str());
        return 1;
    }
    TestConvention();
    TestDriving();
    TestReverse();
    TestStanding();
    TestSteering();
    TestSixCars();
    TestAirAndPause();
    TestRemoteCar(tuning);
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "OK" : "FAILED", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
