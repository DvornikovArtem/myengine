// The training of the Rocket League game (G4g), without a window: the packs of assets/data/training (they load, the exercises are sane, every
// exercise is a miss when the player does nothing and the shots of the goalie really go into the goal), free play (the endless boost, the ball back
// to the car and into the air, the counters of the hits), the exercises on scripted input (a goal, a save, a touch above N metres, a kick-off
// against the bot), the timer, the attempts, next / previous / retry, and the results file (saved after every attempt, read again, a broken
// file is no results).

#include <myengine/core/AppPaths.h>
#include <myengine/game/RlTrainingStore.h>

#include <rl/RlMatch.h>
#include <rl/RlTraining.h>
#include <rl/RlTuning.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef TRAINING_PACKS_DIR
#error TRAINING_PACKS_DIR is not defined
#endif

namespace
{
    using namespace rl;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    float Length(const RlVec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

    std::vector<RlTrainingPack> Packs()
    {
        std::vector<std::string> errors;
        std::vector<RlTrainingPack> packs = RlLoadTrainingPacks(TRAINING_PACKS_DIR, &errors);
        for (const std::string& error : errors)
        {
            std::cerr << "pack error: " << error << "\n";
        }
        Check(errors.empty(), "every pack of the folder reads");
        return packs;
    }

    const RlTrainingPack& PackById(const std::vector<RlTrainingPack>& packs, const std::string& id)
    {
        for (const RlTrainingPack& pack : packs)
        {
            if (pack.id == id)
            {
                return pack;
            }
        }
        throw std::runtime_error("pack " + id + " is missing");
    }

    RlCarInput Drive(const bool boost = true)
    {
        RlCarInput input;
        input.throttle = 1.0f;
        input.boost = boost;
        return input;
    }

    // Steps until the phase leaves `phase` (or `maxTicks` pass); returns the ticks made.
    int StepWhile(RlTraining& training, const RlCarInput& input, const RlTrainingPhase phase, const int maxTicks)
    {
        int ticks = 0;
        while (training.Phase() == phase && ticks < maxTicks)
        {
            training.Step(input);
            ++ticks;
        }
        return ticks;
    }

    // Past the "get ready" stretch of an exercise, into the time of play.
    void ToRunning(RlTraining& training)
    {
        const int ticks = StepWhile(training, RlCarInput{}, RlTrainingPhase::Ready, 600);
        Check(training.Phase() == RlTrainingPhase::Running, "the exercise reaches Running (" + std::to_string(ticks) + " ticks)");
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    void TestPacks()
    {
        const std::vector<RlTrainingPack> packs = Packs();
        Check(packs.size() == 4, "four packs");
        Check(packs[0].id == "striker" && packs[1].id == "goalie" && packs[2].id == "aerial" && packs[3].id == "kickoff", "the packs follow their order");
        const RlTuning tuning;
        for (const RlTrainingPack& pack : packs)
        {
            Check(!pack.name.empty() && !pack.description.empty(), pack.id + ": a name and a description");
            Check(pack.exercises.size() >= 5 && pack.exercises.size() <= 10, pack.id + ": 5 to 10 exercises");
            for (std::size_t i = 0; i < pack.exercises.size(); ++i)
            {
                const RlTrainingExercise& e = pack.exercises[i];
                const std::string where = pack.id + "/" + e.id;
                Check(!e.name.empty() && !e.hint.empty() && e.timeLimit >= 5.0f && e.attempts >= 1, where + ": name, hint, time and attempts");
                for (std::size_t j = i + 1; j < pack.exercises.size(); ++j)
                {
                    Check(pack.exercises[j].id != e.id, where + ": the id is unique");
                }
                if (e.kickoff)
                {
                    Check(e.botLevel == 2 && e.condition == RlTrainingCondition::KickoffWin, where + ": a kick-off against the bot of level 2");
                    continue;
                }
                Check(std::fabs(e.carX) < tuning.arena.halfWidth && std::fabs(e.carZ) < tuning.arena.halfLength, where + ": the car starts inside the arena");
                Check(std::fabs(e.ballPosition.x) < tuning.arena.halfWidth && std::fabs(e.ballPosition.z) < tuning.arena.halfLength &&
                          e.ballPosition.y >= tuning.ball.radius - 1e-3f && e.ballPosition.y < tuning.arena.height - 2.0f,
                      where + ": the ball starts inside the arena");
                // the car does not start in the ball
                const float dx = e.carX - e.ballPosition.x;
                const float dz = e.carZ - e.ballPosition.z;
                Check(std::sqrt(dx * dx + dz * dz) > 4.0f || e.ballPosition.y > 3.0f, where + ": the car and the ball are apart");
            }
        }
        // the kinds of the packs
        for (const RlTrainingExercise& e : PackById(packs, "goalie").exercises)
        {
            Check(e.condition == RlTrainingCondition::Saved, "goalie: a save");
        }
        for (const RlTrainingExercise& e : PackById(packs, "aerial").exercises)
        {
            Check(e.condition == RlTrainingCondition::TouchAbove && e.height >= 4.0f, "aerial: a touch above 4 m or more");
        }
        for (const RlTrainingExercise& e : PackById(packs, "striker").exercises)
        {
            Check(e.condition == RlTrainingCondition::Goal, "striker: a goal");
        }
    }

    void TestPackParser()
    {
        RlTrainingPack pack;
        std::string error;
        const std::string good = R"({"id":"p","name":"P","exercises":[{"id":"a","car":{"x":0,"z":-10},"ball":{"position":[0,1,0]},"success":{"type":"goal"}}]})";
        Check(RlLoadTrainingPackFromText(good, pack, &error), "a minimal pack reads: " + error);
        Check(pack.exercises.size() == 1 && pack.exercises[0].attempts == 3 && pack.exercises[0].timeLimit == 10.0f, "the defaults");

        const char* bad[] = {
            "not json",
            R"({"id":"p","exercises":[]})",
            R"({"exercises":[{"id":"a"}]})",
            R"({"id":"p","exercises":[{"id":"a","car":{"x":0,"z":0},"ball":{"position":[0,1,0]},"success":{"type":"nonsense"}}]})",
            R"({"id":"p","exercises":[{"id":"a","car":{"x":0,"z":0},"ball":{"position":[0,1]},"success":{"type":"goal"}}]})",
            R"({"id":"p","exercises":[{"id":"a","car":{"x":0,"z":0},"ball":{"position":[0,1,0]},"success":{"type":"goal"},"timeLimit":0}]})",
            R"({"id":"p","exercises":[{"id":"a","car":{"x":0,"z":900},"ball":{"position":[0,1,0]},"success":{"type":"goal"}}]})",
            R"({"id":"p","exercises":[{"id":"a","success":{"type":"goal"}}]})",
            R"({"id":"p","exercises":[{"id":"a","success":{"type":"kickoffWin"}}]})",
            R"({"id":"p","exercises":[{"id":"a","car":{"x":0,"z":0},"ball":{"position":[0,1,0]},"success":{"type":"goal"}},{"id":"a","car":{"x":0,"z":0},"ball":{"position":[0,1,0]},"success":{"type":"goal"}}]})",
        };
        for (const char* text : bad)
        {
            error.clear();
            Check(!RlLoadTrainingPackFromText(text, pack, &error) && !error.empty(), std::string("a bad pack is refused with a reason: ") + text);
        }
        error.clear();
        Check(!RlLoadTrainingPackFromFile("no/such/file.json", pack, &error) && !error.empty(), "a missing file is an error");
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    void TestFreePlay()
    {
        const RlTuning tuning;
        RlTraining training(tuning);
        Check(training.Mode() == RlTrainingMode::FreePlay && training.Phase() == RlTrainingPhase::Running, "free play begins running");
        Check(training.World().carCount == 1, "one car");

        // the endless boost: three seconds of boost leave the tank full; without it the tank empties
        training.SetInfiniteBoost(true);
        for (int i = 0; i < 360; ++i)
        {
            training.Step(Drive());
        }
        Check(training.World().cars[0].boost > 99.0f, "endless boost: the tank stays full");
        Check(Length(training.World().cars[0].linearVelocity) > 15.0f, "the car drove");
        training.SetInfiniteBoost(false);
        for (int i = 0; i < 240; ++i)
        {
            training.Step(Drive());
        }
        Check(training.World().cars[0].boost < 50.0f, "without it the boost is spent");

        // the ball back to the car, on the floor in front of it
        training.ResetCar();
        training.ResetBallToCar(6.0f);
        {
            const RlWorldState& w = training.World();
            const float dx = w.ball.position.x - w.cars[0].position.x;
            const float dz = w.ball.position.z - w.cars[0].position.z;
            Check(std::fabs(std::sqrt(dx * dx + dz * dz) - 6.0f) < 0.05f, "the ball is 6 m from the car");
            Check(Length(w.ball.linearVelocity) < 1e-3f && w.ball.position.y < 1.2f, "the ball rests on the floor");
            // in front: the car of the kick-off spot looks along its own +Z, the ball is where the car looks
            const RlSpot spot = RlKickoffSpot(0, 0);
            const float lookX = std::sin(spot.yaw);
            const float lookZ = std::cos(spot.yaw);
            Check(dx * lookX + dz * lookZ > 5.9f, "the ball is in front of the car");
        }

        // the ball in the air in front of the car
        training.BallInAir(14.0f, 9.0f);
        Check(training.World().ball.position.y > 8.9f, "the ball is in the air");
        float apex = 0.0f;
        for (int i = 0; i < 120 * 4; ++i)
        {
            training.Step(RlCarInput{});
            apex = std::max(apex, training.World().ball.position.y);
        }
        Check(apex > 9.0f, "the ball flew up first");

        // the counters: the hit of a rolling ball by the car
        training.ResetCar();
        training.ResetBallToCar(5.0f);
        training.ResetCounters();
        Check(training.Info().hits == 0, "the counters are cleared");
        for (int i = 0; i < 120 * 4 && training.Info().hits == 0; ++i)
        {
            training.Step(Drive());
        }
        const RlTrainingInfo info = training.Info();
        Check(info.hits >= 1, "the hit was counted");
        Check(info.lastHitSpeed > 5.0f && info.bestHitSpeed >= info.lastHitSpeed, "the speed of the hit");
        Check(info.maxTouchHeight > 0.5f && info.maxTouchHeight < 2.0f, "the height of a touch on the floor");
        training.ResetCounters();
        Check(training.Info().hits == 0 && training.Info().bestHitSpeed == 0.0f, "the counters are reset");

        // a pack command in free play does nothing
        training.Next();
        Check(training.Mode() == RlTrainingMode::FreePlay, "Next means nothing in free play");
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    std::string TempFile(const char* name)
    {
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "myengine_rl_training_test";
        std::filesystem::create_directories(folder);
        return (folder / name).generic_u8string();
    }

    void TestStrikerGoal()
    {
        const std::vector<RlTrainingPack> packs = Packs();
        const RlTrainingPack& striker = PackById(packs, "striker");
        const RlTuning tuning;
        RlTraining training(tuning);
        Check(training.StartPack(striker), "the striker pack starts");
        Check(training.Mode() == RlTrainingMode::Pack && training.Phase() == RlTrainingPhase::Ready, "the exercise waits first");
        RlTrainingInfo info = training.Info();
        Check(info.exerciseIndex == 0 && info.exerciseCount == static_cast<int>(striker.exercises.size()) && info.attempt == 1, "exercise 1 of N, attempt 1");
        Check(info.packName == "Striker" && info.exerciseName == striker.exercises[0].name, "the names");
        Check(info.timeLeft == striker.exercises[0].timeLimit && info.elapsed == 0.0f, "the full time");

        // the world stands still while the exercise waits: the ball is where it was put
        const RlVec3 start = training.World().ball.position;
        for (int i = 0; i < 30; ++i)
        {
            training.Step(Drive());
        }
        Check(training.World().ball.position.x == start.x && training.World().ball.position.z == start.z, "the ball waits while the exercise is ready");
        Check(training.World().cars[0].boost == striker.exercises[0].carBoost, "the boost is the one of the exercise");
        Check(std::fabs(training.World().cars[0].position.z - striker.exercises[0].carZ) < 0.5f, "the car stands at its start");

        // drive straight through the ball: a goal
        ToRunning(training);
        const int ticks = StepWhile(training, Drive(), RlTrainingPhase::Running, 120 * 10);
        Check(training.Phase() == RlTrainingPhase::Success, "driving through the ball scores");
        info = training.Info();
        Check(info.elapsed > 1.0f && info.elapsed < striker.exercises[0].timeLimit, "the time of the goal");
        Check(std::fabs(info.elapsed - static_cast<float>(ticks) / 120.0f) < 0.05f, "the elapsed time is the ticks of play");
        Check(info.hits >= 1 && info.lastHitSpeed > 5.0f, "the hit speed of the shot");
        Check(info.newBest, "the first success is a record");
        Check(info.best.successes == 1 && info.best.attempts == 1 && info.best.bestTime > 0.0f && info.best.bestHitSpeed > 5.0f, "the results of the exercise");
        const float time = info.elapsed;

        // the banner stays 1.8 s, then the next exercise begins by itself
        const int banner = StepWhile(training, RlCarInput{}, RlTrainingPhase::Success, 120 * 5);
        Check(banner > 120 * 1.7f && banner < 120 * 1.9f, "the banner time");
        info = training.Info();
        Check(info.exerciseIndex == 1 && training.Phase() == RlTrainingPhase::Ready && info.attempt == 1, "a success moves on to the next exercise");

        // the same exercise again: the best time and the counters
        training.Previous();
        Check(training.Info().exerciseIndex == 0 && training.Phase() == RlTrainingPhase::Ready, "Previous goes back");
        Check(training.Info().best.successes == 1 && std::fabs(training.Info().best.bestTime - time) < 1e-4f, "the results are still there");
        ToRunning(training);
        StepWhile(training, Drive(), RlTrainingPhase::Running, 120 * 10);
        Check(training.Phase() == RlTrainingPhase::Success, "the second goal");
        info = training.Info();
        Check(info.best.successes == 2 && info.best.attempts == 2, "two successes");
        Check(info.best.bestTime <= time + 1e-4f, "the best time never grows");
    }

    void TestMissAndAttempts()
    {
        const std::vector<RlTrainingPack> packs = Packs();
        const RlTrainingPack& striker = PackById(packs, "striker");
        const RlTuning tuning;
        RlTraining training(tuning);
        training.StartPack(striker);
        const RlTrainingExercise& e = striker.exercises[0];
        Check(e.attempts == 3, "three attempts");

        for (int attempt = 1; attempt <= e.attempts; ++attempt)
        {
            Check(training.Info().exerciseIndex == 0 && training.Info().attempt == attempt, "attempt " + std::to_string(attempt));
            ToRunning(training);
            // the timer: it runs down while the player does nothing
            for (int i = 0; i < 120 * 3; ++i)
            {
                training.Step(RlCarInput{});
            }
            const RlTrainingInfo mid = training.Info();
            Check(std::fabs(mid.elapsed - 3.0f) < 0.02f && std::fabs(mid.timeLeft - (e.timeLimit - 3.0f)) < 0.02f, "the timer runs");
            const int ticks = StepWhile(training, RlCarInput{}, RlTrainingPhase::Running, 120 * 20);
            Check(training.Phase() == RlTrainingPhase::Miss, "the time is up: a miss");
            Check(std::fabs(static_cast<float>(ticks + 360) / 120.0f - e.timeLimit) < 0.05f, "the miss comes when the time limit is reached");
            const RlTrainingInfo info = training.Info();
            Check(info.missReason == RlTrainingMiss::TimeUp, "the reason is the time");
            Check(info.outOfAttempts == (attempt == e.attempts), "the last miss spends the last attempt");
            Check(info.attempt == attempt, "the banner names the attempt that was missed");
            StepWhile(training, RlCarInput{}, RlTrainingPhase::Miss, 120 * 5);
        }
        // no attempts left: on to the next exercise
        Check(training.Info().exerciseIndex == 1 && training.Info().attempt == 1, "out of attempts: the next exercise");
        const RlTrainingExerciseResult r = training.Results().Get(striker.id, e.id);
        Check(r.attempts == 3 && r.successes == 0 && r.bestTime == 0.0f, "three misses are in the results");

        // a manual retry spends nothing
        ToRunning(training);
        for (int i = 0; i < 120; ++i)
        {
            training.Step(RlCarInput{});
        }
        training.Retry();
        Check(training.Phase() == RlTrainingPhase::Ready && training.Info().attempt == 1 && training.Info().elapsed == 0.0f, "Retry starts the attempt again for free");
        // after a miss the next attempt begins by itself, with the exercise set up again
        ToRunning(training);
        StepWhile(training, RlCarInput{}, RlTrainingPhase::Running, 120 * 20);
        StepWhile(training, RlCarInput{}, RlTrainingPhase::Miss, 120 * 5);
        Check(training.Info().exerciseIndex == 1 && training.Info().attempt == 2 && training.Phase() == RlTrainingPhase::Ready, "a miss with attempts left: the same exercise again");
        const RlTrainingExercise& e2 = striker.exercises[1];
        Check(std::fabs(training.World().ball.position.z - e2.ballPosition.z) < 0.01f, "the ball is put back");

        // next / previous run through the whole pack and the summary comes
        for (std::size_t i = 2; i < striker.exercises.size(); ++i)
        {
            training.Next();
            Check(training.Info().exerciseIndex == static_cast<int>(i), "Next");
        }
        training.Next();
        Check(training.Phase() == RlTrainingPhase::PackDone, "after the last exercise the summary");
        Check(training.Info().exerciseIndex == static_cast<int>(striker.exercises.size()) - 1 && training.CurrentExercise() == nullptr, "the summary has no exercise");
        training.Step(RlCarInput{});
        training.Previous();
        Check(training.Phase() == RlTrainingPhase::Ready && training.Info().exerciseIndex == static_cast<int>(striker.exercises.size()) - 1, "Previous leaves the summary for the last exercise");
        training.Next();
        training.Retry();
        Check(training.Phase() == RlTrainingPhase::Ready && training.Info().exerciseIndex == 0, "Retry in the summary starts the pack over");
        training.Previous();
        Check(training.Info().exerciseIndex == 0, "Previous at the first exercise stays");
    }

    void TestDoNothing()
    {
        const std::vector<RlTrainingPack> packs = Packs();
        const RlTuning tuning;
        RlTraining training(tuning);
        for (const RlTrainingPack& pack : packs)
        {
            if (pack.id == "kickoff")
            {
                continue;
            }
            for (std::size_t i = 0; i < pack.exercises.size(); ++i)
            {
                const RlTrainingExercise& e = pack.exercises[i];
                const std::string where = pack.id + "/" + e.id;
                training.StartPack(pack, static_cast<int>(i));
                ToRunning(training);
                // the ball must stay inside the arena and the exercise must end by itself
                const int ticks = StepWhile(training, RlCarInput{}, RlTrainingPhase::Running, 120 * 130);
                Check(training.Phase() == RlTrainingPhase::Miss, where + ": nothing done is a miss");
                const RlTrainingMiss reason = training.Info().missReason;
                if (e.condition == RlTrainingCondition::Saved)
                {
                    // the shots of the goalie are on target: untouched, every one of them is a goal against
                    Check(reason == RlTrainingMiss::OwnGoal, where + ": the shot goes into the goal");
                    Check(static_cast<float>(ticks) / 120.0f < e.timeLimit - 0.5f, where + ": it arrives before the time is up");
                }
                else
                {
                    Check(reason == RlTrainingMiss::TimeUp, where + ": the time is up");
                    Check(std::fabs(static_cast<float>(ticks) / 120.0f - e.timeLimit) < 0.05f, where + ": at the time limit");
                }
            }
        }
    }

    void TestSaved()
    {
        const std::vector<RlTrainingPack> packs = Packs();
        const RlTrainingPack& goalie = PackById(packs, "goalie");
        const RlTuning tuning;
        RlTraining training(tuning);
        training.StartPack(goalie, 0);
        // the car goes to the line of the shot: the ball hits it
        training.MutableSim().ResetCar(0, 0.0f, -40.0f, 0.0f);
        ToRunning(training);
        const int ticks = StepWhile(training, RlCarInput{}, RlTrainingPhase::Running, 120 * 12);
        Check(training.Phase() == RlTrainingPhase::Success, "the ball hit the car: saved");
        Check(training.Info().hits >= 1, "the touch is counted");
        Check(ticks > 120 && ticks < 120 * 8, "the save takes some time");
        Check(training.Info().best.successes == 1, "the save is in the results");

        // a touch that does not stop the shot is not a save: the goal against ends the exercise
        training.Retry();
        training.MutableSim().ResetCar(0, 0.0f, -40.0f, 0.0f);
        ToRunning(training);
        // wait for the shot, then fly the car away just before the contact, after a touch at once: put the ball into the goal
        for (int i = 0; i < 60; ++i)
        {
            training.Step(RlCarInput{});
        }
        training.MutableSim().ResetBall({0.0f, 1.0f, -52.5f}, {0.0f, 0.0f, -5.0f});
        StepWhile(training, RlCarInput{}, RlTrainingPhase::Running, 120 * 3);
        Check(training.Phase() == RlTrainingPhase::Miss && training.Info().missReason == RlTrainingMiss::OwnGoal, "a ball in the own goal is a miss");
    }

    void TestTouchAbove()
    {
        const std::vector<RlTrainingPack> packs = Packs();
        const RlTrainingPack& aerial = PackById(packs, "aerial");
        const RlTuning tuning;
        const RlTrainingExercise& e = aerial.exercises[0];
        Check(e.height >= 4.0f, "the first aerial asks for 4 m");

        // a touch low down does not count (the ball is at 3 m at the start)
        {
            RlTraining training(tuning);
            training.StartPack(aerial, 0);
            ToRunning(training);
            const RlVec3 ball = training.World().ball.position;
            training.MutableSim().SetCarPose(0, {ball.x, ball.y, ball.z - 1.6f}, RlQuat{}, {0.0f, 0.0f, 8.0f});
            for (int i = 0; i < 40; ++i)
            {
                training.Step(RlCarInput{});
            }
            Check(training.Info().hits >= 1, "the car hit the ball");
            Check(training.Info().maxTouchHeight < e.height, "the ball was low at the touch");
            Check(training.Phase() == RlTrainingPhase::Running, "a low touch is not a success");
        }
        // the same touch when the ball is high counts
        {
            RlTraining training(tuning);
            training.StartPack(aerial, 0);
            ToRunning(training);
            for (int i = 0; i < 60; ++i)
            {
                training.Step(RlCarInput{});  // the ball rises
            }
            const RlWorldState& w = training.World();
            Check(w.ball.position.y > e.height + 1.0f, "the ball is high now");
            const RlVec3 ball = w.ball.position;
            const RlVec3 ballVelocity = w.ball.linearVelocity;
            training.MutableSim().SetCarPose(0, {ball.x, ball.y, ball.z - 1.6f}, RlQuat{}, {ballVelocity.x, ballVelocity.y, ballVelocity.z + 8.0f});
            StepWhile(training, RlCarInput{}, RlTrainingPhase::Running, 60);
            Check(training.Phase() == RlTrainingPhase::Success, "a touch above the height is a success");
            Check(training.Info().maxTouchHeight >= e.height, "the height of the touch");
        }
    }

    void TestKickoff()
    {
        const std::vector<RlTrainingPack> packs = Packs();
        const RlTrainingPack& kickoff = PackById(packs, "kickoff");
        const RlTuning tuning;

        auto run = [&](const int exercise, const bool drive) {
            RlTraining training(tuning);
            training.StartPack(kickoff, exercise);
            Check(training.Phase() == RlTrainingPhase::Ready, "the kick-off waits for the countdown");
            Check(training.World().carCount == 2 && training.World().match.phase == RlMatchPhase::Countdown, "two cars and the countdown of the match");
            const RlVec3 botStart = training.World().cars[1].position;
            const int countdown = StepWhile(training, drive ? Drive() : RlCarInput{}, RlTrainingPhase::Ready, 120 * 6);
            Check(countdown > 120 * 2.5f && countdown < 120 * 3.5f, "the countdown of the match is about 3 s");
            Check(training.Phase() == RlTrainingPhase::Running && training.Info().elapsed == 0.0f, "the clock starts after the countdown");
            const float limit = kickoff.exercises[static_cast<std::size_t>(exercise)].timeLimit;
            const int ticks = StepWhile(training, drive ? Drive() : RlCarInput{}, RlTrainingPhase::Running, static_cast<int>(limit * 120.0f) + 120);
            Check(training.Phase() == RlTrainingPhase::Success || training.Phase() == RlTrainingPhase::Miss, "the kick-off ends");
            Check(static_cast<float>(ticks) / 120.0f <= limit + 0.05f, "inside the time limit");
            const RlVec3 botNow = training.World().cars[1].position;
            Check(std::fabs(botNow.x - botStart.x) + std::fabs(botNow.z - botStart.z) > 3.0f, "the bot drove to the ball");
            return std::make_pair(training.Phase(), training.Info());
        };

        // the player does nothing: the bot is first to the ball
        for (int exercise = 0; exercise < 5; ++exercise)
        {
            const auto result = run(exercise, false);
            Check(result.first == RlTrainingPhase::Miss, "kick-off " + std::to_string(exercise) + ": the bot wins it when the player stands still");
        }
        // the same run twice gives the same end
        const auto a = run(4, true);
        const auto b = run(4, true);
        Check(a.first == b.first && a.second.elapsed == b.second.elapsed && a.second.missReason == b.second.missReason && a.second.lastHitSpeed == b.second.lastHitSpeed,
              "a kick-off run twice with the same input ends the same");
        Check(a.second.hits >= 1 || a.first == RlTrainingPhase::Miss, "a success needs the touch of the car");
    }

    void TestResultsFile()
    {
        const std::vector<RlTrainingPack> packs = Packs();
        const RlTrainingPack& striker = PackById(packs, "striker");
        const RlTuning tuning;
        const std::string path = TempFile("training.json");
        std::filesystem::remove(path);

        {
            RlTraining training(tuning);
            training.SetResultsFile(path);  // the file is not there yet: no results
            Check(training.Results().Get("striker", "open-net").attempts == 0, "no results at first");
            training.StartPack(striker);
            ToRunning(training);
            StepWhile(training, Drive(), RlTrainingPhase::Running, 120 * 10);
            Check(training.Phase() == RlTrainingPhase::Success, "a goal");
            Check(std::filesystem::exists(std::filesystem::u8path(path)), "the file is written after the attempt, not at the end of the session");
        }
        RlTrainingResults saved;
        std::string error;
        Check(saved.LoadFromFile(path, &error), "the file reads: " + error);
        const RlTrainingExerciseResult r = saved.Get("striker", "open-net");
        Check(r.successes == 1 && r.attempts == 1 && r.bestTime > 1.0f && r.bestHitSpeed > 5.0f, "the saved results");
        Check(saved.PackSuccesses(striker) == 1, "one exercise of the pack done");
        Check(saved.Get("striker", "rolling-ball").attempts == 0 && saved.Get("goalie", "open-net").attempts == 0, "other exercises are empty");

        // a new session reads them and goes on
        {
            RlTraining training(tuning);
            training.SetResultsFile(path);
            Check(training.Results().Get("striker", "open-net").successes == 1, "the next session reads the results");
            training.StartPack(striker);
            Check(training.Info().best.successes == 1 && training.Info().packDoneSuccesses == 1, "the HUD data has them");
            ToRunning(training);
            StepWhile(training, RlCarInput{}, RlTrainingPhase::Running, 120 * 20);  // a miss
            Check(training.Results().Get("striker", "open-net").attempts == 2 && training.Results().Get("striker", "open-net").successes == 1, "a miss only counts the attempt");
        }
        RlTrainingResults again;
        Check(again.LoadFromFile(path, &error) && again.Get("striker", "open-net").attempts == 2, "and it is saved");
        Check(std::fabs(again.Get("striker", "open-net").bestTime - r.bestTime) < 2e-3f, "the best time is kept (to the millisecond of the file)");

        // text round trip
        RlTrainingResults copy;
        Check(copy.LoadFromText(again.ToText(), &error), "the text reads: " + error);
        Check(copy.Get("striker", "open-net").attempts == 2 && copy.Get("striker", "open-net").successes == 1, "the text round trip");

        // a broken file is no results, not a crash
        {
            std::ofstream(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc) << "{ this is not json";
            RlTrainingResults broken;
            Check(!broken.LoadFromFile(path, &error) && broken.Get("striker", "open-net").attempts == 0, "a broken file: no results");
            RlTraining training(tuning);
            training.SetResultsFile(path);
            Check(training.Results().Get("striker", "open-net").attempts == 0, "the training starts with no results");
            training.StartPack(striker);
            ToRunning(training);
            StepWhile(training, Drive(), RlTrainingPhase::Running, 120 * 10);
            RlTrainingResults fixed;
            Check(fixed.LoadFromFile(path, &error) && fixed.Get("striker", "open-net").successes == 1, "the file is written anew over the broken one");
        }
        std::filesystem::remove_all(std::filesystem::temp_directory_path() / "myengine_rl_training_test");
    }

    void TestStore()
    {
        using namespace myengine;
        Check(game::TrainingResultsPath() == core::apppaths::UserDataDirectory() / "training.json", "the results are in the data folder of the user");
        const RlTuning tuning;
        RlTraining training(tuning);
        game::AttachTrainingResults(training);  // reads the file of the user if there is one; nothing is written before an attempt ends
        Check(training.Mode() == RlTrainingMode::FreePlay, "attaching the results does not start a pack");
    }
}

int main()
{
    try
    {
        TestPackParser();
        TestPacks();
        TestFreePlay();
        TestStrikerGoal();
        TestMissAndAttempts();
        TestDoNothing();
        TestSaved();
        TestTouchAbove();
        TestKickoff();
        TestResultsFile();
        TestStore();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "rl_training tests passed\n";
    return 0;
}
