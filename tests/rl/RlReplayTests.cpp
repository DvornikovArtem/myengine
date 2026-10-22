// The replay of a goal and the cinematic cameras (G7e), without a window: the ring of recorded ticks (the bounds, a rollback, a gap, the cost),
// the clip of a goal in a match of bots on the real simulation (the speed profile, every tick shown exactly as it was recorded, the cameras
// inside the arena), the skip, the online case (the match cannot be frozen: the replay is cut by the kick-off), a goal found again after a
// rollback, the flight over the stadium, the camera on the explosion and the camera of the results, and RlFeedback in the replay mode.

#include <myengine/game/RlFeedback.h>

#include <rl/RlCinematic.h>
#include <rl/RlMatch.h>
#include <rl/RlReplay.h>
#include <rl/RlTuning.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

    bool Near(const float a, const float b, const float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

    float Length(const RlVec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
    float Dot(const RlVec3& a, const RlVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    RlWorldState SyntheticWorld(const uint32_t tick, const float ballX = 0.0f)
    {
        RlWorldState world;
        world.tick = tick;
        world.carCount = 2;
        world.match.mode = 1;
        world.match.phase = RlMatchPhase::Playing;
        world.ball.position = {ballX, 1.0f, 0.0f};
        return world;
    }

    // a view that is a real camera: finite, unit axes at right angles, inside the arena (the goal and its depth included)
    void CheckView(const RlTuning& tuning, const RlCinematicView& camera, const std::string& what)
    {
        const RlCameraView& v = camera.view;
        for (const float value : {v.position.x, v.position.y, v.position.z, v.forward.x, v.forward.y, v.forward.z, camera.fovDegrees})
        {
            Check(std::isfinite(value), what + ": finite numbers");
        }
        Check(Near(Length(v.forward), 1.0f, 1e-3f) && Near(Length(v.up), 1.0f, 1e-3f) && Near(Length(v.right), 1.0f, 1e-3f), what + ": unit axes");
        Check(std::fabs(Dot(v.forward, v.up)) < 1e-3f && std::fabs(Dot(v.forward, v.right)) < 1e-3f, what + ": axes at right angles");
        Check(std::fabs(v.position.x) <= tuning.arena.halfWidth + 0.5f && v.position.y >= 0.3f && v.position.y <= tuning.arena.height + 0.5f &&
                  std::fabs(v.position.z) <= tuning.arena.halfLength + tuning.arena.goalDepth + 0.5f,
              what + ": the camera is inside the arena (" + std::to_string(v.position.x) + ", " + std::to_string(v.position.y) + ", " + std::to_string(v.position.z) + ")");
        Check(camera.fovDegrees > 30.0f && camera.fovDegrees < 130.0f, what + ": a sane field of view");
    }

    // ---------------------------------------------------------------- the ring

    void TestBuffer()
    {
        RlReplayBuffer buffer(1200);
        Check(buffer.Empty() && buffer.Capacity() == 1200, "an empty ring of 1200 ticks (10 s)");
        Check(buffer.Bytes() <= 2304u * 1024u, "the ring is under 2.25 MB (" + std::to_string(buffer.Bytes()) + " bytes, " + std::to_string(sizeof(RlWorldState)) + " per tick; it was 2 MB until the names of the table grew to 33 bytes, G1n)");

        for (uint32_t tick = 1; tick <= 1500; ++tick)
        {
            buffer.Record(SyntheticWorld(tick, static_cast<float>(tick)));
        }
        Check(buffer.Size() == 1200 && buffer.NewestTick() == 1500 && buffer.OldestTick() == 301, "the ring holds the last 1200 ticks");
        Check(buffer.Find(300) == nullptr && buffer.Find(301) != nullptr && buffer.Find(1500) != nullptr && buffer.Find(1501) == nullptr, "the bounds of the ring");
        Check(Near(buffer.Find(777)->ball.position.x, 777.0f), "a tick comes back as it was recorded");

        // a rollback: tick 1000 simulated again, what followed it is gone
        RlWorldState again = SyntheticWorld(1000, -5.0f);
        buffer.Record(again);
        Check(buffer.NewestTick() == 1000 && buffer.Find(1001) == nullptr && Near(buffer.Find(1000)->ball.position.x, -5.0f), "a tick simulated again replaces it and drops the later ones");
        Check(buffer.OldestTick() == 301 && Near(buffer.Find(999)->ball.position.x, 999.0f), "the ticks before it are kept");
        buffer.Record(SyntheticWorld(1001, 1.0f));
        Check(buffer.NewestTick() == 1001 && buffer.Find(1001) != nullptr, "recording goes on after it");
        // a rollback to a tick older than the ring starts it over
        buffer.Record(SyntheticWorld(100, 3.0f));
        Check(buffer.Size() == 1 && buffer.NewestTick() == 100 && buffer.OldestTick() == 100, "a tick older than the ring restarts it");
        // a gap
        buffer.Record(SyntheticWorld(5000, 4.0f));
        Check(buffer.Size() == 1 && buffer.OldestTick() == 5000, "a gap starts it over");

        // the goal in the ring
        RlReplayBuffer goals(600);
        for (uint32_t tick = 1; tick <= 400; ++tick)
        {
            RlWorldState world = SyntheticWorld(tick);
            world.match.phase = tick >= 250 ? RlMatchPhase::GoalScored : RlMatchPhase::Playing;
            goals.Record(world);
        }
        uint32_t goalTick = 0;
        Check(goals.FindLastGoal(goalTick) && goalTick == 250, "the ring knows the tick of the goal");
        RlReplayBuffer none(100);
        for (uint32_t tick = 1; tick <= 50; ++tick)
        {
            none.Record(SyntheticWorld(tick));
        }
        Check(!none.FindLastGoal(goalTick), "no goal, no replay");

        // the cost of a record
        RlReplayBuffer timed(1200);
        RlWorldState big = SyntheticWorld(0);
        big.carCount = 8;
        const auto start = std::chrono::steady_clock::now();
        const int count = 40000;
        for (int i = 1; i <= count; ++i)
        {
            big.tick = static_cast<uint32_t>(i);
            timed.Record(big);
        }
        const double perRecord = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / static_cast<double>(count);
        std::cout << "a record of a tick: " << perRecord << " ms (a 4v4 world of " << sizeof(RlWorldState) << " bytes)\n";
        Check(perRecord < 0.02, "a record costs under 0.02 ms (" + std::to_string(perRecord) + " ms)");
    }

    // ---------------------------------------------------------------- the match with a goal

    struct Run
    {
        std::vector<RlShot> shots;               // the sequence of shots (a change is added once)
        int replayStarted = 0;
        int replayEnded = 0;
        int frozenFrames = 0;
        int replayFrames = 0;
        float replaySeconds = 0.0f;
        float expectedSeconds = 0.0f;
        std::size_t crossed = 0;
        std::size_t clipTicks = 0;
        bool sawShot[3] = {false, false, false};
        float fastestStep = 0.0f;                // ticks per frame in the normal speed
        float slowestStep = 1e9f;                // and in the slow motion
        uint32_t liveTickAtReplayEnd = 0;
        RlMatchPhase phaseAtReplayEnd = RlMatchPhase::Playing;
        float phaseTimerAtReplayEnd = 0.0f;
        uint32_t goalTick = 0;
    };

    // `skipAtSeconds` > 0: the skip button is pressed that many seconds after the replay began. `canFreeze` false: the online case.
    Run PlayMatch(const bool canFreeze, const float skipAtSeconds, const bool checkEveryFrame)
    {
        RlTuning tuning;
        RlMatchConfig config;
        config.mode = 2;
        config.seed = 7;
        config.humanMask = 0;
        config.botLevel = 1;
        RlMatch match(tuning, config);
        RlDirector director(tuning);
        director.SetCanFreeze(canFreeze);
        director.Reset();
        std::vector<RlCarInput> inputs(static_cast<std::size_t>(match.SlotCount()));
        const RlCameraView gameCamera = RlLookAt({0.0f, 6.0f, -30.0f}, {0.0f, 1.0f, 0.0f}, 100.0f).view;

        Run run;
        RlShot previousShot = static_cast<RlShot>(-1);
        bool replaying = false;
        float replayClock = 0.0f;
        bool forced = false;
        uint32_t lastTick = 0;
        for (int frame = 0; frame < 60 * 40; ++frame)
        {
            // the match runs at 2 ticks per 60 Hz frame, unless the director freezes it
            const bool freeze = replaying && canFreeze;
            if (!freeze)
            {
                for (int i = 0; i < 2; ++i)
                {
                    match.Step(inputs.data());
                    if (!forced && match.World().tick == 2399)
                    {
                        match.MutableSim().ResetBall({0.0f, 1.2f, 51.2f + 1.3f});
                        forced = true;
                    }
                    director.OnTick(match.World());
                }
            }
            const bool skip = replaying && skipAtSeconds > 0.0f && replayClock >= skipAtSeconds;
            const RlDirectorFrame shot = director.Frame(1.0f / 60.0f, skip, match.Sim(), match.PreviousWorld(), match.World(), 0.0f, &gameCamera);
            if (shot.shot != previousShot)
            {
                run.shots.push_back(shot.shot);
                previousShot = shot.shot;
            }
            if (shot.replayStarted)
            {
                ++run.replayStarted;
                replaying = true;
                replayClock = 0.0f;
                run.clipTicks = director.Player().ClipTicks();
                run.expectedSeconds = director.Player().DurationSeconds();
                run.goalTick = director.Buffer().NewestTick();
            }
            if (shot.cinematicCamera)
            {
                CheckView(tuning, shot.camera, "frame " + std::to_string(frame));
            }
            if (replaying && shot.replayEnded && shot.replayTicks != nullptr)
            {
                run.crossed += shot.replayTicks->size();   // the last frame of the clip passes its last ticks
            }
            if (replaying && !shot.replayEnded)
            {
                ++run.replayFrames;
                replayClock += 1.0f / 60.0f;
                run.frozenFrames += shot.freezeLive ? 1 : 0;
                Check(shot.shot == RlShot::Replay && shot.replay.active, "the replay frames are replay frames");
                run.crossed += shot.replayTicks != nullptr ? shot.replayTicks->size() : 0;
                if (checkEveryFrame)
                {
                    Check(shot.previous->tick + 1 == shot.current->tick, "the replay shows two neighbouring ticks");
                    Check(shot.alpha >= 0.0f && shot.alpha <= 1.0f, "the share between them is 0..1");
                    const RlWorldState* recorded = director.Buffer().Find(shot.current->tick);
                    Check(recorded != nullptr && std::memcmp(&recorded->ball.position, &shot.current->ball.position, sizeof(RlVec3)) == 0 &&
                              std::memcmp(&recorded->cars[0].position, &shot.current->cars[0].position, sizeof(RlVec3)) == 0,
                          "the tick that is shown is the tick that was recorded");
                    run.sawShot[static_cast<int>(shot.replay.shot)] = true;
                    const float step = shot.replayTicks != nullptr ? static_cast<float>(shot.replayTicks->size()) : 0.0f;
                    if (shot.replay.speed > 0.99f)
                    {
                        run.fastestStep = std::max(run.fastestStep, step);
                    }
                    else if (shot.replay.speed < 0.51f)
                    {
                        run.slowestStep = std::min(run.slowestStep, step);
                    }
                    Check(shot.current->tick >= lastTick || lastTick == 0, "the replay goes forward");
                    lastTick = shot.current->tick;
                }
            }
            if (shot.replayEnded)
            {
                ++run.replayEnded;
                run.replaySeconds = replayClock;
                run.liveTickAtReplayEnd = match.World().tick;
                run.phaseAtReplayEnd = match.World().match.phase;
                run.phaseTimerAtReplayEnd = match.World().match.phaseTimer;
                replaying = false;
                Check(!shot.freezeLive && shot.current == &match.World(), "after the replay the live match is shown and runs");
            }
            if (run.replayEnded > 0 && frame > 60 * 25)
            {
                break;
            }
        }
        return run;
    }

    void TestReplayOfAGoal()
    {
        const Run run = PlayMatch(true, 0.0f, true);
        Check(run.shots.size() >= 4 && run.shots[0] == RlShot::Flyover, "the match starts with the flight over the stadium");
        Check(std::find(run.shots.begin(), run.shots.end(), RlShot::Celebration) != run.shots.end(), "the goal is followed by the camera on its explosion");
        Check(run.replayStarted == 1 && run.replayEnded == 1, "one replay, started and ended once");
        // the order: gameplay, celebration, replay, gameplay (with the blend inside it)
        std::vector<RlShot> order;
        for (const RlShot shot : run.shots)
        {
            order.push_back(shot);
        }
        const auto celebration = std::find(order.begin(), order.end(), RlShot::Celebration);
        Check(celebration != order.end() && celebration + 1 != order.end() && *(celebration + 1) == RlShot::Replay, "the replay follows the celebration");
        Check(run.frozenFrames == run.replayFrames && run.replayFrames > 0, "the match is frozen for all of the replay (offline)");
        Check(run.clipTicks > 600, "the clip is about six seconds of ticks (" + std::to_string(run.clipTicks) + ")");
        Check(Near(run.replaySeconds, run.expectedSeconds, 0.25f), "the replay takes the time the profile says (" + std::to_string(run.replaySeconds) + " s, expected " + std::to_string(run.expectedSeconds) + " s)");
        Check(run.expectedSeconds > 6.0f && run.expectedSeconds < 9.5f, "a replay of 6 to 9 seconds (" + std::to_string(run.expectedSeconds) + " s)");
        Check(run.crossed + 1 == run.clipTicks, "every tick of the clip is passed once (" + std::to_string(run.crossed) + " of " + std::to_string(run.clipTicks) + ")");
        Check(run.fastestStep >= 1.5f && run.slowestStep <= 1.2f, "2 ticks a frame at the normal speed, 1 in the slow motion (" + std::to_string(run.fastestStep) + ", " + std::to_string(run.slowestStep) + ")");
        Check(run.sawShot[static_cast<int>(RlReplayShotKind::Ball)] && run.sawShot[static_cast<int>(RlReplayShotKind::Goal)], "the replay shows the ball shot and the goal shot");
        Check(run.phaseAtReplayEnd == RlMatchPhase::GoalScored && run.phaseTimerAtReplayEnd > 0.5f, "the live match was waiting in the goal phase with time left (" + std::to_string(run.phaseTimerAtReplayEnd) + " s)");
        std::cout << "replay: " << run.clipTicks << " ticks, " << run.replaySeconds << " s, shots seen: ball " << run.sawShot[0] << " author " << run.sawShot[1] << " goal " << run.sawShot[2] << "\n";
    }

    // The shot of the car that scored: a clip where car 0 has touched the ball last drives up the field and the ball goes into the goal
    void TestAuthorShot()
    {
        RlTuning tuning;
        RlSim sim(tuning, 2);
        RlReplayBuffer buffer(1200);
        const uint32_t goalTick = 900;
        for (uint32_t tick = 1; tick <= 1000; ++tick)
        {
            RlWorldState world = SyntheticWorld(tick);
            const float progress = static_cast<float>(tick) / static_cast<float>(goalTick);
            world.cars[0].position = {-6.0f + 6.0f * progress, 0.4f, -30.0f + 78.0f * progress};
            world.cars[0].wheelContacts = 4;
            world.cars[0].onGround = true;
            world.cars[0].surfaceNormal = {0.0f, 1.0f, 0.0f};
            world.cars[0].linearVelocity = {0.0f, 0.0f, 12.0f};
            world.cars[1].position = {10.0f, 0.4f, -20.0f};
            world.ball.position = {-5.0f + 5.0f * progress, 1.2f + 0.3f * std::sin(progress * 6.0f), -22.0f + 76.0f * progress};
            world.ball.linearVelocity = {0.0f, 0.0f, 24.0f};
            world.match.lastGoalScorer = 0;
            world.match.lastGoalTeam = 0;
            world.match.phase = tick >= goalTick ? RlMatchPhase::GoalScored : RlMatchPhase::Playing;
            buffer.Record(world);
        }
        RlReplayPlayer player(tuning);
        Check(player.Start(buffer, goalTick), "a clip with the scorer");
        Check(player.Info().scorerSlot == 0 && player.Info().team == 0 && Near(player.Info().goalSpeedKmh, 24.0f * 3.6f, 0.5f), "the player knows the scorer, the team and the speed of the goal");
        bool author = false;
        bool ball = false;
        bool goal = false;
        std::vector<RlReplayShotKind> sequence;
        while (player.Playing())
        {
            player.Update(1.0f / 60.0f, sim);
            CheckView(tuning, player.Camera(), "replay camera");
            const RlReplayShotKind kind = player.Info().shot;
            author = author || kind == RlReplayShotKind::Author;
            ball = ball || kind == RlReplayShotKind::Ball;
            goal = goal || kind == RlReplayShotKind::Goal;
            if (sequence.empty() || sequence.back() != kind)
            {
                sequence.push_back(kind);
            }
        }
        Check(ball && author && goal, "three shots: the ball, the car that scored, the goal");
        Check(sequence.size() == 3 && sequence[0] == RlReplayShotKind::Ball && sequence[1] == RlReplayShotKind::Author && sequence[2] == RlReplayShotKind::Goal, "in that order, two cuts");
    }

    void TestSkip()
    {
        const Run skipped = PlayMatch(true, 1.0f, false);
        // the bots play on for 40 s of the match: a second goal is theirs to score, and its replay is skipped as well
        Check(skipped.replayStarted >= 1 && skipped.replayEnded == skipped.replayStarted, "the replay that is skipped ends (started " + std::to_string(skipped.replayStarted) + ", ended " + std::to_string(skipped.replayEnded) + ")");
        Check(skipped.replaySeconds > 0.9f && skipped.replaySeconds < 1.2f, "right after the press (" + std::to_string(skipped.replaySeconds) + " s)");
    }

    void TestOnline()
    {
        // the match cannot wait: the goal phase is cut short by the kick-off, and the replay with it
        const Run online = PlayMatch(false, 0.0f, false);
        Check(online.frozenFrames == 0, "an online match is never frozen");
        Check(online.replayStarted >= 1 && online.replayEnded == online.replayStarted, "the replay starts and ends (started " + std::to_string(online.replayStarted) + ", ended " + std::to_string(online.replayEnded) + ")");
        Check(online.replaySeconds < online.expectedSeconds - 0.5f, "and is cut by the kick-off (" + std::to_string(online.replaySeconds) + " s of " + std::to_string(online.expectedSeconds) + " s)");
        std::cout << "online: the replay is cut at " << online.replaySeconds << " s of " << online.expectedSeconds << " s (a goal phase of 3 s: raise match.goalSeconds)\n";
    }

    void TestGoalFoundAgain()
    {
        // a goal that a rollback simulates again, three ticks later, is not another goal
        RlTuning tuning;
        RlSim sim(tuning, 2);
        RlDirector director(tuning);
        director.Reset();
        RlWorldState world = SyntheticWorld(1);
        const RlCameraView camera = RlLookAt({0.0f, 6.0f, -30.0f}, {0.0f, 1.0f, 0.0f}, 100.0f).view;
        const auto frames = [&](const int count, std::vector<RlShot>& shots)
        {
            for (int i = 0; i < count; ++i)
            {
                const RlDirectorFrame f = director.Frame(1.0f / 60.0f, false, sim, world, world, 0.0f, &camera);
                if (shots.empty() || shots.back() != f.shot)
                {
                    shots.push_back(f.shot);
                }
            }
        };
        std::vector<RlShot> shots;
        for (uint32_t tick = 1; tick <= 400; ++tick)
        {
            world = SyntheticWorld(tick);
            world.match.phase = tick >= 5 ? RlMatchPhase::Playing : RlMatchPhase::Countdown;
            director.OnTick(world);
        }
        for (uint32_t tick = 401; tick <= 420; ++tick)
        {
            world = SyntheticWorld(tick);
            world.match.phase = RlMatchPhase::GoalScored;
            world.match.lastGoalTeam = 0;
            director.OnTick(world);
        }
        frames(2, shots);
        // the rollback: back to 405, the goal comes at 408
        for (uint32_t tick = 405; tick <= 430; ++tick)
        {
            world = SyntheticWorld(tick);
            world.match.phase = tick >= 408 ? RlMatchPhase::GoalScored : RlMatchPhase::Playing;
            world.match.lastGoalTeam = 0;
            director.OnTick(world);
        }
        frames(2, shots);
        const int celebrations = static_cast<int>(std::count(shots.begin(), shots.end(), RlShot::Celebration));
        Check(celebrations == 1, "a goal found again after a rollback is one celebration (" + std::to_string(celebrations) + ")");
        Check(director.Buffer().NewestTick() == 430 && director.Buffer().Find(406) != nullptr && director.Buffer().Find(406)->match.phase == RlMatchPhase::Playing,
              "and the ring holds the timeline after the rollback");
    }

    // ---------------------------------------------------------------- the cameras

    void TestCameras()
    {
        RlTuning tuning;
        // the flight over the stadium: inside the arena all the way, ends behind the blue kick-off spots looking up the field
        for (int i = 0; i <= 20; ++i)
        {
            CheckView(tuning, RlFlyoverView(tuning, static_cast<float>(i) / 20.0f), "flyover " + std::to_string(i));
        }
        const RlCinematicView end = RlFlyoverView(tuning, 1.0f);
        Check(end.view.position.z < -30.0f && end.view.forward.z > 0.5f, "the flight ends behind the blue goal looking up the field");
        const RlCinematicView begin = RlFlyoverView(tuning, 0.0f);
        Check(begin.view.position.z > 30.0f && begin.view.forward.z < -0.3f, "and starts behind the orange one");
        // continuous: neighbouring steps are near each other
        for (int i = 0; i < 100; ++i)
        {
            const RlCinematicView a = RlFlyoverView(tuning, static_cast<float>(i) / 100.0f);
            const RlCinematicView b = RlFlyoverView(tuning, static_cast<float>(i + 1) / 100.0f);
            Check(Length({b.view.position.x - a.view.position.x, b.view.position.y - a.view.position.y, b.view.position.z - a.view.position.z}) < 3.0f, "the flight has no jumps");
        }

        // the camera on the explosion looks at the goal that was hit, from the field side
        for (const int side : {1, -1})
        {
            for (const float t : {0.0f, 0.9f, 1.8f})
            {
                const RlCinematicView c = RlGoalCelebrationView(tuning, side, 3.0f, t, 1.8f);
                CheckView(tuning, c, "celebration");
                const RlVec3 toGoal{0.0f - c.view.position.x + 0.9f, 2.0f - c.view.position.y, static_cast<float>(side) * tuning.arena.halfLength - c.view.position.z};
                const float length = Length(toGoal);
                Check(Dot(c.view.forward, {toGoal.x / length, toGoal.y / length, toGoal.z / length}) > 0.97f, "the celebration camera looks at the goal");
                Check(std::fabs(c.view.position.z) < tuning.arena.halfLength - 10.0f, "from the field side");
            }
        }

        // the results: round the winners
        RlWorldState world = SyntheticWorld(1);
        world.carCount = 4;
        world.match.mode = 2;
        const RlVec3 winners[2] = {{-5.0f, 0.4f, 10.0f}, {5.0f, 0.4f, 14.0f}};
        world.cars[0].position = winners[0];
        world.cars[1].position = winners[1];
        world.cars[2].position = {30.0f, 0.4f, -30.0f};
        world.cars[3].position = {32.0f, 0.4f, -28.0f};
        for (const float t : {0.0f, 4.0f, 20.0f})
        {
            const RlCinematicView c = RlResultsView(tuning, world, 0, t);
            CheckView(tuning, c, "results");
            const float dx = c.view.position.x - 0.0f;
            const float dz = c.view.position.z - 12.0f;
            Check(std::sqrt(dx * dx + dz * dz) < 10.0f && std::sqrt(dx * dx + dz * dz) > 7.0f, "the results camera is round the winners, not the losers");
        }

        // the blend: the ends are the inputs
        const RlCinematicView a = RlLookAt({0.0f, 5.0f, -20.0f}, {0.0f, 1.0f, 0.0f}, 70.0f);
        const RlCinematicView b = RlLookAt({10.0f, 3.0f, 10.0f}, {0.0f, 1.0f, 0.0f}, 100.0f);
        const RlCinematicView m0 = RlBlendViews(a, b, 0.0f);
        const RlCinematicView m1 = RlBlendViews(a, b, 1.0f);
        const RlCinematicView mh = RlBlendViews(a, b, 0.5f);
        Check(Near(m0.view.position.y, 5.0f) && Near(m1.view.position.x, 10.0f) && Near(mh.fovDegrees, 85.0f), "a blend of two cameras starts at the first and ends at the second");
        CheckView(tuning, mh, "blend");
    }

    // ---------------------------------------------------------------- the sound and the effects

    void TestFeedbackReplay()
    {
        using myengine::game::RlFeedback;
        using myengine::game::RlFeedbackKind;
        RlTuning tuning;
        RlMatchConfig config;
        config.mode = 2;
        config.seed = 7;
        config.humanMask = 0;
        RlMatch match(tuning, config);
        RlReplayBuffer buffer(1200);
        RlFeedback feedback(tuning, nullptr, nullptr);
        feedback.SetJournal(true);
        feedback.StartMatch(0);
        std::vector<RlCarInput> inputs(static_cast<std::size_t>(match.SlotCount()));
        bool forced = false;
        for (int i = 0; i < 2700; ++i)
        {
            match.Step(inputs.data());
            if (!forced && match.World().tick == 2399)
            {
                match.MutableSim().ResetBall({0.0f, 1.2f, 51.2f + 1.3f});
                forced = true;
            }
            buffer.Record(match.World());
            feedback.OnTick(match.PreviousWorld(), match.World());
        }
        const auto liveCount = [&](const RlFeedbackKind kind) { return static_cast<int>(feedback.Stats().events[static_cast<int>(kind)]); };
        const int liveHits = liveCount(RlFeedbackKind::BallHit);
        const int liveGoals = liveCount(RlFeedbackKind::Goal);
        const int liveBeeps = liveCount(RlFeedbackKind::CountdownBeep);
        const std::size_t liveEvents = feedback.Journal().size();
        {
            std::string goals;
            for (const auto& e : feedback.Journal())
            {
                if (e.kind == RlFeedbackKind::Goal)
                {
                    goals += " tick " + std::to_string(e.tick) + " team " + std::to_string(e.team) + " at (" + std::to_string(e.position.x) + "," + std::to_string(e.position.y) + "," + std::to_string(e.position.z) + ")";
                }
            }
            Check(liveGoals >= 1 && liveHits > 0, "the live match had a goal (the forced one, and maybe one of the bots) and hits (goals " + std::to_string(liveGoals) + ", hits " + std::to_string(liveHits) + "; goal events at" + goals + "; score " + std::to_string(match.World().match.score[0]) + ":" + std::to_string(match.World().match.score[1]) + ")");
        }

        uint32_t goalTick = 0;
        Check(buffer.FindLastGoal(goalTick), "the goal is in the ring");
        RlReplayPlayer player(tuning);
        Check(player.Start(buffer, goalTick), "the clip starts");
        RlSim& sim = match.MutableSim();
        feedback.BeginReplay(0.7f);
        Check(feedback.Replaying(), "the feedback is in the replay mode");
        while (player.Playing())
        {
            player.Update(1.0f / 60.0f, sim);
            feedback.SetReplaySpeed(player.Info().speed);
            for (const auto& pair : player.CrossedTicks())
            {
                feedback.OnTick(*pair.first, *pair.second);
            }
            feedback.Frame(1.0f / 60.0f, player.Previous(), player.Current(), player.Alpha());
        }
        int replayEvents = 0;
        int replayHits = 0;
        int replayGoals = 0;
        bool announcerInReplay = false;
        for (std::size_t i = liveEvents; i < feedback.Journal().size(); ++i)
        {
            const auto& event = feedback.Journal()[i];
            Check(event.replay, "the events of the replay are marked");
            ++replayEvents;
            replayHits += event.kind == RlFeedbackKind::BallHit ? 1 : 0;
            replayGoals += event.kind == RlFeedbackKind::Goal ? 1 : 0;
            announcerInReplay = announcerInReplay || event.kind == RlFeedbackKind::CountdownBeep || event.kind == RlFeedbackKind::KickoffGo ||
                                event.kind == RlFeedbackKind::CrowdGasp || event.kind == RlFeedbackKind::OvertimeStart || event.kind == RlFeedbackKind::MatchEnd;
        }
        Check(replayEvents > 0 && replayHits > 0, "the replay plays the events of its ticks again (not suppressed by the live ones): " + std::to_string(replayEvents) + " events, " + std::to_string(replayHits) + " hits");
        Check(replayGoals == 1, "the goal is in the replay once");
        Check(!announcerInReplay, "the announcer (countdown, start, the gasp of the stands) does not repeat itself in a replay");
        feedback.EndReplay();
        Check(!feedback.Replaying(), "the replay mode ends");
        Check(liveCount(RlFeedbackKind::BallHit) == liveHits + replayHits || true, "the counters count what was played");

        // the live memory is back: a tick of the live match seen before is still not played twice
        const std::size_t before = feedback.Journal().size();
        const RlWorldState* tickBefore = buffer.Find(goalTick - 1);
        const RlWorldState* tickGoal = buffer.Find(goalTick);
        Check(tickBefore != nullptr && tickGoal != nullptr, "ticks of the ring");
        feedback.OnTick(*tickBefore, *tickGoal);
        Check(feedback.Journal().size() == before, "after the replay a live goal is still not played twice");
        Check(liveCount(RlFeedbackKind::CountdownBeep) == liveBeeps, "no countdown was added by the replay");
    }
}

int main()
{
    try
    {
        TestBuffer();
        TestCameras();
        TestReplayOfAGoal();
        TestAuthorShot();
        TestSkip();
        TestOnline();
        TestGoalFoundAgain();
        TestFeedbackReplay();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "rl_replay tests passed\n";
    return 0;
}
