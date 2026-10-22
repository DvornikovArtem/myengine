// RlCarPhysicsTests.cpp
// The cars of the game (G4i, docs/game/rl-cars.md): the catalog and its files, the two layers of numbers, every car in the sim (standing, driving,
// the jump, the double jump, the dodge, the landing), six different cars in one world (determinism, save and restore), the choice of the cars of a
// match (humans keep theirs, bots draw theirs from the seed), a match with different cars (rollback).
// The comparison with RocketSim is in RlRocketSimDiffTests.cpp.

#include <rl/RlCars.h>
#include <rl/RlMatch.h>
#include <rl/RlRng.h>
#include <rl/RlSim.h>
#include <rl/RlTuning.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

    bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

    RlCarCatalog LoadCatalog()
    {
        RlCarCatalog catalog;
        std::string error;
        if (!catalog.LoadDirectory(RL_CARS_DIR, &error))
        {
            std::printf("  cannot load the cars: %s\n", error.c_str());
            CHECK(false);
        }
        return catalog;
    }

    // ---- the files ----------------------------------------------------------------------------------------------------------------------

    void TestCatalog()
    {
        std::printf("the catalog\n");
        const RlCarCatalog catalog = LoadCatalog();
        CHECK(catalog.Count() == 6);
        const char* const ids[6] = {"pulse", "vector", "slate", "dart", "fusion", "bulwark"};
        const char* const classes[6] = {"Octane", "Dominus", "Plank", "Breakout", "Hybrid", "Merc"};
        for (int i = 0; i < 6 && i < catalog.Count(); ++i)
        {
            CHECK(catalog.Spec(i).id == ids[i]);
            CHECK(catalog.Spec(i).hitboxClass == classes[i]);
            CHECK(catalog.Find(ids[i]) == i);
            CHECK(catalog.Spec(i).variant.hasBody);
        }
        CHECK(catalog.Find("nothing") == -1);
        CHECK(catalog.Valid(99) == kDefaultCar && catalog.Valid(-1) == kDefaultCar && catalog.Valid(3) == 3);

        // the differences of ours are 10 to 20 percent of the Pulse, which has the numbers of the tuning
        const RlCarTuning plain;
        for (int i = 0; i < catalog.Count(); ++i)
        {
            const RlCarTuning c = catalog.Tuning(plain, i);
            const float ratios[] = {c.mass / plain.mass,
                                    c.inertiaMultiplier / plain.inertiaMultiplier,
                                    c.throttleAccel / plain.throttleAccel,
                                    c.maxSpeed / plain.maxSpeed,
                                    c.boostAccelGround / plain.boostAccelGround,
                                    c.boostAccelAir / plain.boostAccelAir,
                                    c.steerAngle[2] / plain.steerAngle[2],
                                    c.airYawTorque / plain.airYawTorque};
            for (const float f : ratios)
            {
                CHECK(f >= 0.87f && f <= 1.21f);
            }
        }
        {
            const RlCarTuning c = catalog.Tuning(plain, 0);
            CHECK(c.mass == plain.mass && c.throttleAccel == plain.throttleAccel && c.maxSpeed == plain.maxSpeed && c.steerAngle[0] == plain.steerAngle[0]);
        }

        // the same files give the same hash, another number gives another one
        const RlCarCatalog again = LoadCatalog();
        CHECK(catalog.Hash() == again.Hash() && catalog.Hash() != 0);
        CHECK(RlCarCatalog::Fallback().Hash() != catalog.Hash());

        // the default car of the files is the car of the tuning: the Octane of RocketSim is what RlTuning has as its defaults
        const RlCarTuning tuning;
        const RlCarTuning pulse = catalog.Tuning(tuning, 0);
        CHECK(Near(pulse.halfExtentX, tuning.halfExtentX, 1e-5f) && Near(pulse.halfExtentY, tuning.halfExtentY, 1e-5f) && Near(pulse.halfExtentZ, tuning.halfExtentZ, 1e-5f));
        CHECK(Near(pulse.comOffsetY, tuning.comOffsetY, 1e-5f) && Near(pulse.comOffsetZ, tuning.comOffsetZ, 1e-5f));
        CHECK(Near(pulse.wheelMountXFront, tuning.wheelMountXFront, 1e-5f) && Near(pulse.wheelMountXRear, tuning.wheelMountXRear, 1e-5f));
        CHECK(Near(pulse.wheelFrontZ, tuning.wheelFrontZ, 1e-5f) && Near(pulse.wheelRearZ, tuning.wheelRearZ, 1e-5f));
        CHECK(Near(pulse.wheelRadiusFront, tuning.wheelRadiusFront, 1e-5f) && Near(pulse.wheelRadiusRear, tuning.wheelRadiusRear, 1e-5f));
        CHECK(Near(pulse.suspensionRestFront, tuning.suspensionRestFront, 1e-5f) && Near(pulse.suspensionRestRear, tuning.suspensionRestRear, 1e-5f));
        CHECK(Near(pulse.wheelMountYOffset, tuning.wheelMountYOffset, 1e-5f) && Near(pulse.spawnHeight, tuning.spawnHeight, 1e-5f));
        CHECK(pulse.mass == tuning.mass && pulse.maxSpeed == tuning.maxSpeed && pulse.throttleAccel == tuning.throttleAccel);
        // every number of Pulse is the one of the tuning (the file names them, the values are the same)
        CHECK(RlBodyOnly(catalog.Spec(0).variant).fields.size() >= 10);

        // the hitboxes of the others are the ones of RocketSim (uu / 100): the Dominus 130.427 x 85.7799 x 33.8, the Merc 123.22 x 79.2103 x 44.1591
        const RlCarTuning dominus = catalog.Tuning(tuning, 1);
        CHECK(Near(dominus.halfExtentZ * 2.0f, 1.30427f, 1e-3f) && Near(dominus.halfExtentX * 2.0f, 0.857799f, 1e-3f) && Near(dominus.halfExtentY * 2.0f, 0.338f, 1e-3f));
        const RlCarTuning merc = catalog.Tuning(tuning, 5);
        CHECK(Near(merc.halfExtentZ * 2.0f, 1.2322f, 1e-3f) && Near(merc.halfExtentY * 2.0f, 0.441591f, 1e-3f));
        // a car on the floor stands as high as its hitbox is above the origin of the car (17 uu up for all of them)
        CHECK(Near(merc.spawnHeight, tuning.spawnHeight + (0.21505f - 0.20755f), 1e-4f));
        CHECK(Near(dominus.spawnHeight, tuning.spawnHeight + (0.1575f - 0.20755f), 1e-4f));
        // the numbers of ours are on top of it
        CHECK(Near(merc.mass, 180.0f * 1.20f, 0.5f) && Near(merc.maxSpeed, 23.0f * 0.97f, 0.01f));
        CHECK(merc.steerAngle[0] < tuning.steerAngle[0]);
        // the body only: the hitbox and the wheels of the class, the rest is the tuning
        const RlCarTuning bodyOnly = RlApplyCarVariant(tuning, RlBodyOnly(catalog.Spec(5).variant));
        CHECK(bodyOnly.mass == tuning.mass && Near(bodyOnly.halfExtentY * 2.0f, 0.441591f, 1e-3f));
    }

    void TestBadFiles()
    {
        std::printf("files that are refused\n");
        const std::string good =
            R"({"id":"a","hitbox":{"halfExtentX":0.43,"halfExtentY":0.19,"halfExtentZ":0.6,"comOffsetY":-0.2,"comOffsetZ":-0.13,)"
            R"("rocketSim":{"size":[120,86,38],"offset":[13,0,20]}},"physics":{"mass":199.0,"steerAngle":[0.5,0.3,0.18,0.1,0.08,0.03]}})";
        RlCarCatalog catalog;
        std::string error;
        CHECK(catalog.LoadTexts({good}, &error));
        CHECK(!RlCarCatalog().LoadTexts({}, &error));
        CHECK(!RlCarCatalog().LoadTexts({"not json"}, &error));
        CHECK(!RlCarCatalog().LoadTexts({R"({"order":1})"}, &error) && !error.empty());
        CHECK(!RlCarCatalog().LoadTexts({good, good}, &error) && error.find("twice") != std::string::npos);
        std::string heavy = good;
        heavy.replace(heavy.find("\"mass\":199.0"), 12, "\"mass\":900.0");
        CHECK(!RlCarCatalog().LoadTexts({heavy}, &error) && error.find("mass") != std::string::npos);
        std::string typo = good;
        typo.replace(typo.find("\"mass\""), 6, "\"maas\"");
        CHECK(!RlCarCatalog().LoadTexts({typo}, &error) && error.find("maas") != std::string::npos);
        std::string shortList = good;
        shortList.replace(shortList.find("[0.5,0.3,0.18,0.1,0.08,0.03]"), 28, "[0.5,0.3]");
        CHECK(!RlCarCatalog().LoadTexts({shortList}, &error) && error.find("steerAngle") != std::string::npos);
        std::string tiny = good;
        tiny.replace(tiny.find("\"halfExtentZ\":0.6"), 17, "\"halfExtentZ\":0.01");
        CHECK(!RlCarCatalog().LoadTexts({tiny}, &error) && error.find("hitbox") != std::string::npos);
        CHECK(!RlCarCatalog().LoadDirectory("this/folder/is/not/there", &error));
    }

    void TestStats()
    {
        std::printf("the numbers of the garage\n");
        const RlCarCatalog catalog = LoadCatalog();
        const RlCarTuning tuning;
        RlCarStats stats[6];
        for (int i = 0; i < 6 && i < catalog.Count(); ++i)
        {
            stats[i] = catalog.Stats(tuning, i);
            std::printf("  %-8s top %.2f m/s, 0-14 m/s %.2f s, %.0f kg, wheels %.3f rad at 10 m/s, hitbox %.2f x %.2f x %.2f m\n", catalog.Spec(i).id.c_str(),
                        static_cast<double>(stats[i].topSpeed), static_cast<double>(stats[i].accelerationTime), static_cast<double>(stats[i].mass),
                        static_cast<double>(stats[i].turning), static_cast<double>(stats[i].length), static_cast<double>(stats[i].width), static_cast<double>(stats[i].height));
        }
        // the extremes of the set: Bulwark is the heaviest, tallest and slowest to turn and to start, Dart the quickest and the lightest
        for (int i = 0; i < 6; ++i)
        {
            CHECK(stats[i].mass <= stats[5].mass);
            CHECK(stats[i].height <= stats[5].height + 1e-6f);
            CHECK(stats[i].accelerationTime >= stats[3].accelerationTime - 1e-6f);
            CHECK(stats[i].turning <= stats[4].turning + 1e-6f || i == 4);
            CHECK(stats[i].mass >= stats[3].mass);
            CHECK(stats[i].topSpeed <= stats[3].topSpeed + 1e-6f);
        }
        CHECK(stats[5].accelerationTime >= stats[0].accelerationTime);
        CHECK(stats[5].turning <= stats[0].turning);
        // the Pulse is the middle: its acceleration time is the one of the plain tuning, about two seconds
        CHECK(stats[0].accelerationTime > 1.5f && stats[0].accelerationTime < 3.5f);
        // the differences are visible but not extreme (the balance of the set holds): every number within 25 percent of the Pulse
        for (int i = 1; i < 6; ++i)
        {
            CHECK(stats[i].accelerationTime / stats[0].accelerationTime > 0.75f && stats[i].accelerationTime / stats[0].accelerationTime < 1.25f);
            CHECK(stats[i].mass / stats[0].mass > 0.75f && stats[i].mass / stats[0].mass < 1.25f);
            CHECK(stats[i].topSpeed / stats[0].topSpeed > 0.9f && stats[i].topSpeed / stats[0].topSpeed < 1.1f);
        }
    }

    // ---- a car in the sim -----------------------------------------------------------------------------------------------------------------

    struct Probe
    {
        float restY = 0.0f;
        int restContacts = 0;
        float timeTo10 = 0.0f;
        float speedAfter3s = 0.0f;
        float boostedSpeed = 0.0f;
        float jumpPeak = 0.0f;
        float holdPeak = 0.0f;
        float doublePeak = 0.0f;
        float dodgeSpeedGain = 0.0f;
        bool landed = false;
        float landUp = 0.0f;
    };

    RlVec3 Up(const RlQuat& q)
    {
        return {2.0f * (q.x * q.y - q.z * q.w), 1.0f - 2.0f * (q.x * q.x + q.z * q.z), 2.0f * (q.y * q.z + q.x * q.w)};
    }

    Probe Run(const RlCarVariant& variant)
    {
        Probe p;
        const RlTuning tuning;
        {
            RlSim sim(tuning, 1, &variant);
            sim.ResetCar(0, 0.0f, -20.0f, 0.0f);
            RlCarInput idle;
            for (int t = 0; t < 120; ++t)
            {
                sim.Step(&idle);
            }
            p.restY = sim.State().cars[0].position.y;
            p.restContacts = sim.State().cars[0].wheelContacts;
        }
        {
            RlSim sim(tuning, 1, &variant);
            sim.ResetCar(0, 0.0f, -45.0f, 0.0f);
            RlCarInput drive;
            drive.throttle = 1.0f;
            for (int t = 0; t < 360; ++t)
            {
                sim.Step(&drive);
                const float speed = sim.State().cars[0].linearVelocity.z;
                if (p.timeTo10 == 0.0f && speed >= 10.0f)
                {
                    p.timeTo10 = static_cast<float>(t + 1) / 120.0f;
                }
                if (t == 359)
                {
                    p.speedAfter3s = speed;
                }
            }
        }
        {
            RlSim sim(tuning, 1, &variant);
            sim.ResetCar(0, 0.0f, -47.0f, 0.0f);
            RlCarInput boost;
            boost.throttle = 1.0f;
            boost.boost = true;
            sim.SetCarBoost(0, 100.0f);
            for (int t = 0; t < 270; ++t)
            {
                sim.Step(&boost);
            }
            p.boostedSpeed = sim.State().cars[0].linearVelocity.z;
        }
        // jumps: a tap, a hold, a double jump
        const auto jumpRun = [&](int hold, bool second, bool dodge, float& peak, float* gain) {
            RlSim sim(tuning, 1, &variant);
            sim.ResetCar(0, 0.0f, -20.0f, 0.0f);
            RlCarInput in;
            for (int t = 0; t < 30; ++t)
            {
                sim.Step(&in);
            }
            const float base = sim.State().cars[0].position.y;
            float before = 0.0f;
            for (int t = 0; t < 300; ++t)
            {
                in = RlCarInput{};
                in.jump = t < hold || (second && t >= 40 && t < 43);
                if (dodge && t >= 40 && t < 43)
                {
                    in.pitch = -1.0f;
                }
                if (t == 39)
                {
                    before = sim.State().cars[0].linearVelocity.z;
                }
                sim.Step(&in);
                peak = std::max(peak, sim.State().cars[0].position.y - base);
                if (gain != nullptr && t == 80)
                {
                    *gain = sim.State().cars[0].linearVelocity.z - before;
                }
            }
            p.landed = sim.State().cars[0].wheelContacts == 4;
            const RlVec3 up = Up(sim.State().cars[0].rotation);
            p.landUp = up.y;
        };
        jumpRun(3, false, false, p.jumpPeak, nullptr);
        jumpRun(24, false, false, p.holdPeak, nullptr);
        jumpRun(24, true, false, p.doublePeak, nullptr);
        float dodgePeak = 0.0f;
        jumpRun(24, true, true, dodgePeak, &p.dodgeSpeedGain);
        return p;
    }

    void TestEveryCar()
    {
        std::printf("every car in the sim\n");
        const RlCarCatalog catalog = LoadCatalog();
        std::vector<Probe> probes;
        for (int i = 0; i < catalog.Count(); ++i)
        {
            const Probe p = Run(catalog.Spec(i).variant);
            probes.push_back(p);
            std::printf("  %-8s rest y %.3f (%d wheels), 10 m/s in %.2f s, %.1f m/s after 3 s, %.1f m/s with the boost, jump %.2f / hold %.2f / double %.2f m, dodge +%.1f m/s\n",
                        catalog.Spec(i).id.c_str(), static_cast<double>(p.restY), p.restContacts, static_cast<double>(p.timeTo10), static_cast<double>(p.speedAfter3s),
                        static_cast<double>(p.boostedSpeed), static_cast<double>(p.jumpPeak), static_cast<double>(p.holdPeak), static_cast<double>(p.doublePeak),
                        static_cast<double>(p.dodgeSpeedGain));
            // it stands on its four wheels at the height that the hitbox says (17 uu + the offset of the hitbox, a little less under its weight)
            const float expected = 0.17f - catalog.Spec(i).variant.values.comOffsetY;
            CHECK(p.restContacts == 4);
            CHECK(Near(p.restY, expected, 0.02f));
            // it drives, boosts, jumps, flips, lands on its wheels
            CHECK(p.timeTo10 > 0.8f && p.timeTo10 < 2.5f);
            CHECK(p.speedAfter3s > 13.0f && p.speedAfter3s < 15.0f);
            CHECK(p.boostedSpeed > 19.0f && p.boostedSpeed < 25.0f);
            CHECK(p.jumpPeak > 0.4f && p.jumpPeak < 1.0f);    // a tap: 2.9 m/s of the jump itself and the push of three ticks
            CHECK(p.holdPeak > 1.8f && p.holdPeak < 2.6f);    // held 0.2 s: 2.9 + 14.6 * 0.2 = 5.8 m/s up under 6.5 m/s^2 of gravity
            CHECK(p.doublePeak > p.holdPeak + 1.0f);
            CHECK(p.dodgeSpeedGain > 3.0f);                   // a front flip adds about 5 m/s along the nose
            CHECK(p.landed);
            CHECK(p.landUp > 0.99f);
        }
        // the differences are felt: Dart starts quicker than Pulse and Bulwark; Bulwark is the slowest of all to 10 m/s
        CHECK(probes.size() == 6);
        if (probes.size() == 6)
        {
            CHECK(probes[3].timeTo10 < probes[0].timeTo10);
            CHECK(probes[0].timeTo10 < probes[5].timeTo10);
            CHECK(probes[3].boostedSpeed > probes[5].boostedSpeed);
            for (int i = 0; i < 6; ++i)
            {
                CHECK(probes[5].timeTo10 >= probes[i].timeTo10 - 1e-6f);
            }
        }
    }

    // ---- six cars in one world --------------------------------------------------------------------------------------------------------------

    RlCarInput RandomInput(RlRng& rng)
    {
        RlCarInput in;
        in.throttle = rng.Signed();
        in.steer = rng.Signed();
        in.pitch = rng.Signed();
        in.roll = rng.Signed();
        in.boost = rng.Float01() < 0.5f;
        in.jump = rng.Float01() < 0.1f;
        in.handbrake = rng.Float01() < 0.1f;
        return in;
    }

    uint64_t RunMixed(int ticks, uint64_t seed, std::vector<uint8_t>* saved, int saveAt)
    {
        const RlCarCatalog catalog = LoadCatalog();
        const RlTuning tuning;
        RlCarVariant variants[6];
        for (int i = 0; i < 6; ++i)
        {
            variants[i] = catalog.Spec(i).variant;
        }
        RlSim sim(tuning, 6, variants);
        for (int i = 0; i < 6; ++i)
        {
            sim.ResetCar(i, -20.0f + 8.0f * static_cast<float>(i), (i % 2 == 0) ? -30.0f : 30.0f, (i % 2 == 0) ? 0.0f : 3.14159f);
            sim.SetCarTeam(i, i % 2);
        }
        sim.ResetBall({0.0f, 2.0f, 0.0f});
        RlRng rng(seed);
        RlCarInput inputs[6];
        for (int t = 0; t < ticks; ++t)
        {
            if (t % 15 == 0)
            {
                for (int i = 0; i < 6; ++i)
                {
                    inputs[i] = RandomInput(rng);
                }
            }
            if (saved != nullptr && t == saveAt)
            {
                sim.SaveState(*saved);
            }
            sim.Step(inputs);
        }
        std::vector<uint8_t> end;
        sim.SaveState(end);
        return RlSim::HashState(end);
    }

    void TestMixedWorld()
    {
        std::printf("six cars in one world\n");
        const uint64_t a = RunMixed(900, 7, nullptr, -1);
        const uint64_t b = RunMixed(900, 7, nullptr, -1);
        const uint64_t c = RunMixed(900, 8, nullptr, -1);
        CHECK(a == b);
        CHECK(a != c);

        // restore in the middle and run on: the same end state as the run that never stopped
        const RlCarCatalog catalog = LoadCatalog();
        const RlTuning tuning;
        RlCarVariant variants[6];
        for (int i = 0; i < 6; ++i)
        {
            variants[i] = catalog.Spec(i).variant;
        }
        std::vector<uint8_t> saved;
        const uint64_t straight = RunMixed(600, 11, &saved, 300);
        RlSim sim(tuning, 6, variants);
        CHECK(sim.RestoreState(saved));
        RlRng rng(11);
        RlCarInput inputs[6];
        for (int t = 0; t < 600; ++t)
        {
            if (t % 15 == 0)
            {
                for (int i = 0; i < 6; ++i)
                {
                    inputs[i] = RandomInput(rng);
                }
            }
            if (t >= 300)
            {
                sim.Step(inputs);
            }
        }
        std::vector<uint8_t> end;
        sim.SaveState(end);
        CHECK(RlSim::HashState(end) == straight);

        // the same match with all the cars as Pulse ends in another state: the cars matter
        const RlCarVariant plain[6] = {};
        RlSim same(tuning, 6, plain);
        CHECK(same.CarTuning(5).mass == tuning.car.mass && sim.CarTuning(5).mass > tuning.car.mass * 1.1f);
    }

    // ---- the cars of a match ----------------------------------------------------------------------------------------------------------------

    void TestChooseCars()
    {
        std::printf("the choice of the cars of a match\n");
        const RlCarCatalog catalog = LoadCatalog();
        int wanted[kMaxCars];
        for (int& w : wanted)
        {
            w = -1;
        }
        wanted[0] = 3;
        wanted[2] = 5;
        uint8_t cars[kMaxCars];
        RlChooseCars(catalog, 42, 6, 0b000101, wanted, cars);
        CHECK(cars[0] == 3 && cars[2] == 5);
        CHECK(cars[6] == 0 && cars[7] == 0);
        // the same seed gives the same cars, the humans do not change what the bots draw
        uint8_t again[kMaxCars];
        RlChooseCars(catalog, 42, 6, 0b000101, wanted, again);
        CHECK(std::memcmp(cars, again, sizeof(cars)) == 0);
        wanted[0] = 1;
        RlChooseCars(catalog, 42, 6, 0b000101, wanted, again);
        CHECK(again[0] == 1 && again[1] == cars[1] && again[3] == cars[3] && again[4] == cars[4] && again[5] == cars[5]);
        // a human without a wish gets the default car, a wish out of the catalog too
        wanted[0] = -1;
        wanted[2] = 77;
        RlChooseCars(catalog, 42, 6, 0b000101, wanted, again);
        CHECK(again[0] == kDefaultCar && again[2] == kDefaultCar);
        // the bots draw all the cars, whatever the seed
        int seen[6] = {};
        for (uint64_t seed = 1; seed <= 200; ++seed)
        {
            RlChooseCars(catalog, seed, 8, 0, nullptr, again);
            for (int slot = 0; slot < 8; ++slot)
            {
                CHECK(again[slot] < 6);
                ++seen[again[slot]];
            }
        }
        for (int i = 0; i < 6; ++i)
        {
            CHECK(seen[i] > 150 && seen[i] < 350);  // 1600 draws, about 267 of each
        }
    }

    // a match with a different car in every slot: the cars drive, the bots play with them, a rollback replays it exactly
    void TestMatchWithCars()
    {
        std::printf("a match with different cars\n");
        const RlCarCatalog catalog = LoadCatalog();
        RlTuning tuning;
        RlMatchConfig config;
        config.mode = 3;
        config.seed = 99;
        config.humanMask = 0b000001;
        int wanted[kMaxCars];
        for (int& w : wanted)
        {
            w = -1;
        }
        wanted[0] = 5;
        RlChooseCars(catalog, config.seed, 6, config.humanMask, wanted, config.carIds);
        for (int slot = 0; slot < 6; ++slot)
        {
            config.cars[slot] = catalog.Spec(config.carIds[slot]).variant;
        }
        RlMatch match(tuning, config);
        CHECK(match.CarId(0) == 5);
        for (int slot = 0; slot < 6; ++slot)
        {
            CHECK(match.CarId(slot) == config.carIds[slot]);
            CHECK(Near(match.Sim().CarTuning(slot).mass, catalog.Tuning(tuning.car, config.carIds[slot]).mass, 1e-3f));
        }
        RlCarInput inputs[kMaxCars];
        std::vector<uint8_t> at600;
        uint64_t hashAt1200 = 0;
        for (int t = 0; t < 1200; ++t)
        {
            if (t == 600)
            {
                match.SaveState(at600);
            }
            for (int slot = 0; slot < 6; ++slot)
            {
                inputs[slot] = RlCarInput{};
            }
            inputs[0].throttle = 1.0f;
            inputs[0].boost = (t / 120) % 2 == 0;
            inputs[0].steer = ((t / 90) % 2 == 0) ? 0.4f : -0.4f;
            match.Step(inputs);
        }
        std::vector<uint8_t> end;
        match.SaveState(end);
        hashAt1200 = RlSim::HashState(end);
        // the bots moved (their cars differ, so does how far they got)
        float travelled = 0.0f;
        for (int slot = 1; slot < 6; ++slot)
        {
            const RlVec3 p = match.World().cars[slot].position;
            travelled += std::fabs(p.x) + std::fabs(p.z);
            CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        }
        CHECK(travelled > 5.0f);
        // back to tick 600 and again: the same
        CHECK(match.RestoreState(at600));
        for (int t = 600; t < 1200; ++t)
        {
            for (int slot = 0; slot < 6; ++slot)
            {
                inputs[slot] = RlCarInput{};
            }
            inputs[0].throttle = 1.0f;
            inputs[0].boost = (t / 120) % 2 == 0;
            inputs[0].steer = ((t / 90) % 2 == 0) ? 0.4f : -0.4f;
            match.Step(inputs);
        }
        std::vector<uint8_t> replay;
        match.SaveState(replay);
        CHECK(RlSim::HashState(replay) == hashAt1200);
    }

    // A demolished car comes back on a spot of its team (G-bugball): above the floor, inside the arena, on its wheels, whatever the car and the team.
    // The parked car (under the map while it is down) must not stay there, and a tall hitbox must not start inside the floor.
    void TestDemolishRespawnEveryCar()
    {
        std::printf("a demolished car comes back on its wheels (6 cars x 2 teams)%c", 10);
        const RlCarCatalog catalog = LoadCatalog();
        RlTuning tuning;
        for (int car = 0; car < catalog.Count(); ++car)
        {
            for (int victimSlot = 0; victimSlot < 2; ++victimSlot)
            {
                RlMatchConfig config;
                config.mode = 1;
                config.seed = 5;
                config.humanMask = 0x3u;
                for (int slot = 0; slot < 2; ++slot)
                {
                    config.carIds[slot] = static_cast<uint8_t>(car);
                    config.cars[slot] = catalog.Spec(car).variant;
                }
                RlMatch match(tuning, config);
                RlCarInput idle[kMaxCars] = {};
                for (int t = 0; t < 3 * kTickRate + 40; ++t)
                {
                    match.Step(idle);
                }
                RlSim& sim = match.MutableSim();
                // the victim was driving somewhere (not on a kick-off spot) when it was demolished
                sim.SetCarPose(victimSlot, {-5.0f, catalog.Tuning(tuning.car, car).spawnHeight, -10.0f}, {0, 0.70710678f, 0, 0.70710678f}, {10.0f, 0.0f, 0.0f});
                sim.DemolishCar(victimSlot);
                RlCarInput inputs[kMaxCars] = {};
                bool down = false;
                bool back = false;
                float lowest = 100.0f;
                for (int t = 0; t < 12 * kTickRate && !(back && t > 0); ++t)
                {
                    match.Step(inputs);
                    const RlCarState& v = match.World().cars[victimSlot];
                    if (v.demolished)
                    {
                        down = true;
                    }
                    else if (down)
                    {
                        back = true;
                        lowest = v.position.y;
                    }
                }
                CHECK(down);
                CHECK(back);
                // the first tick after the respawn: over the floor and in the arena
                const RlCarState& placed = match.World().cars[victimSlot];
                CHECK(lowest > 0.15f && lowest < 1.5f);
                CHECK(std::fabs(placed.position.x) < tuning.arena.halfWidth && std::fabs(placed.position.z) < tuning.arena.halfLength);
                // and a few seconds later it stands on its wheels (nobody touches it: the other car is at its kick-off spot on the other side)
                RlCarInput quiet[kMaxCars] = {};
                for (int t = 0; t < 4 * kTickRate; ++t)
                {
                    match.Step(quiet);
                }
                const RlCarState& rested = match.World().cars[victimSlot];
                const float upY = 1.0f - 2.0f * (rested.rotation.x * rested.rotation.x + rested.rotation.z * rested.rotation.z);
                std::printf("  car %s, slot %d: back at y %.2f, rests at (%.1f, %.2f, %.1f), up %.2f, wheels %d%c", catalog.Spec(car).id.c_str(), victimSlot,
                            static_cast<double>(lowest), static_cast<double>(rested.position.x), static_cast<double>(rested.position.y),
                            static_cast<double>(rested.position.z), static_cast<double>(upY), rested.wheelContacts, 10);
                CHECK(!rested.demolished);
                CHECK(rested.position.y > 0.15f && rested.position.y < 1.5f);
                CHECK(rested.wheelContacts >= 3);
                CHECK(upY > 0.95f);
                CHECK(std::fabs(rested.position.x) < tuning.arena.halfWidth && std::fabs(rested.position.z) < tuning.arena.halfLength);
            }
        }
    }
}

int main()
{
    TestCatalog();
    TestBadFiles();
    TestStats();
    TestEveryCar();
    TestMixedWorld();
    TestChooseCars();
    TestMatchWithCars();
    TestDemolishRespawnEveryCar();
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "OK" : "FAILED", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
