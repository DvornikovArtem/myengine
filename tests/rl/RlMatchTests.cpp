// RlMatchTests.cpp
// Headless tests of the Rocket League match (game/rl): kick-off, goals, clock, overtime, boost pads, demolition and respawn, the bots,
// determinism and rollback. `--verbose` prints more.

#include <rl/RlControls.h>
#include <rl/RlMatch.h>
#include <rl/RlTuning.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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

    template <typename... Args>
    void Note(const char* format, Args... args)
    {
        std::printf("  ");
        std::printf(format, static_cast<double>(args)...);
        std::printf("\n");
    }

    int Ticks(float seconds) { return static_cast<int>(std::lround(seconds * static_cast<float>(kTickRate))); }
    float Dist2(const RlVec3& a, float x, float z) { return std::sqrt((a.x - x) * (a.x - x) + (a.z - z) * (a.z - z)); }

    RlTuning g_tuning;

    // steps with neutral inputs
    void Idle(RlMatch& match, int ticks)
    {
        std::vector<RlCarInput> inputs(static_cast<size_t>(match.SlotCount()));
        for (int t = 0; t < ticks; ++t)
        {
            match.Step(inputs.data());
        }
    }

    RlMatchConfig HumanConfig(int mode, uint32_t mask = 0xFFu)
    {
        RlMatchConfig c;
        c.mode = mode;
        c.humanMask = mask;
        return c;
    }

    bool Finite(const RlWorldState& w)
    {
        const auto ok = [](const RlVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        for (int i = 0; i < w.carCount; ++i)
        {
            if (!ok(w.cars[i].position) || !ok(w.cars[i].linearVelocity) || !ok(w.cars[i].angularVelocity))
            {
                return false;
            }
        }
        return ok(w.ball.position) && ok(w.ball.linearVelocity);
    }

    // ---------------------------------------------------------------- kick-off

    void TestKickoff()
    {
        std::printf("kick-off positions\n");
        for (int mode = 1; mode <= 4; ++mode)
        {
            RlMatch match(g_tuning, HumanConfig(mode));
            const RlWorldState& w = match.World();
            CHECK(match.SlotCount() == 2 * mode);
            CHECK(w.carCount == 2 * mode);
            CHECK(w.match.phase == RlMatchPhase::Countdown);
            int onSpot = 0;
            for (int slot = 0; slot < match.SlotCount(); ++slot)
            {
                const int team = match.Team(slot);
                CHECK(team == (slot < mode ? 0 : 1));
                bool found = false;
                for (int order = 0; order < 5; ++order)
                {
                    const RlSpot s = RlKickoffSpot(team, order);
                    if (Dist2(w.cars[slot].position, s.x, s.z) < 0.05f)
                    {
                        found = true;
                    }
                }
                onSpot += found ? 1 : 0;
                // blue stands on -Z, orange on +Z, both look at the middle
                CHECK(team == 0 ? w.cars[slot].position.z < 0.0f : w.cars[slot].position.z > 0.0f);
            }
            CHECK(onSpot == 2 * mode);
            Note("%.0fv%.0f: %.0f cars on RL kick-off spots", mode, mode, onSpot);
            CHECK(std::fabs(w.ball.position.x) < 0.01f && std::fabs(w.ball.position.z) < 0.01f);
        }
        // the spots of the wiki (uu / 100): blue right corner (-20.48, -25.6), orange the same turned half a turn
        const RlSpot blue = RlKickoffSpot(0, 0);
        const RlSpot orange = RlKickoffSpot(1, 0);
        CHECK(std::fabs(blue.x + 20.48f) < 1e-3f && std::fabs(blue.z + 25.6f) < 1e-3f);
        CHECK(std::fabs(orange.x - 20.48f) < 1e-3f && std::fabs(orange.z - 25.6f) < 1e-3f);
        // 4v4 uses the two corners and both back spots of each team
        RlMatch four(g_tuning, HumanConfig(4));
        CHECK(Dist2(four.World().cars[0].position, -20.48f, -25.6f) < 0.05f);
        CHECK(Dist2(four.World().cars[1].position, 20.48f, -25.6f) < 0.05f);
        CHECK(Dist2(four.World().cars[2].position, -2.56f, -38.4f) < 0.05f);
        CHECK(Dist2(four.World().cars[3].position, 2.56f, -38.4f) < 0.05f);
        CHECK(Dist2(four.World().cars[7].position, -2.56f, 38.4f) < 0.05f);
        // the cars look at the middle of the field: blue's right corner faces (+x, +z)
        const RlQuat q = four.World().cars[0].rotation;
        const float fx = 2.0f * (q.x * q.z + q.w * q.y);
        const float fz = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
        CHECK(fx > 0.6f && fz > 0.6f);
    }

    void TestCountdownAndClock()
    {
        std::printf("countdown, clock, goal, overtime\n");
        RlTuning tuning = g_tuning;
        tuning.match.matchSeconds = 4.0f;
        RlMatch match(tuning, HumanConfig(1, 0x3u));
        std::vector<RlCarInput> inputs(2);
        inputs[0].throttle = 1.0f;
        inputs[0].boost = true;
        const RlVec3 start = match.World().cars[0].position;
        for (int t = 0; t < Ticks(2.5f); ++t)
        {
            match.Step(inputs.data());
        }
        const RlVec3 p = match.World().cars[0].position;
        Note("2.5 s of throttle in the countdown: moved %.3f m, phase %.0f, clock %.2f", Dist2(p, start.x, start.z), static_cast<int>(match.World().match.phase),
             match.World().match.clock);
        CHECK(Dist2(p, start.x, start.z) < 0.05f);
        CHECK(match.World().match.phase == RlMatchPhase::Countdown);
        CHECK(match.World().match.clock == 4.0f);
        for (int t = 0; t < Ticks(0.6f); ++t)
        {
            match.Step(inputs.data());
        }
        CHECK(match.World().match.phase == RlMatchPhase::Playing);

        // the clock runs while playing
        for (int t = 0; t < Ticks(1.0f); ++t)
        {
            match.Step(inputs.data());
        }
        Note("1 s of play: clock %.3f (of 4)", match.World().match.clock);
        CHECK(std::fabs(match.World().match.clock - (4.0f - 1.0f - 0.1f)) < 0.2f);

        // a goal: ball and clock stop, score, scorer, then a new kick-off
        RlMatch goal(tuning, HumanConfig(1, 0x3u));
        Idle(goal, Ticks(3.2f));
        goal.MutableSim().ResetBall({0.0f, 1.2f, 51.2f + 0.5f});  // not past the line by a radius: no goal
        Idle(goal, Ticks(0.3f));
        CHECK(goal.World().match.score[0] == 0 && goal.World().match.phase == RlMatchPhase::Playing);
        goal.MutableSim().ResetBall({0.0f, 1.2f, 51.2f + 1.3f});  // the centre is more than a radius past the line
        Idle(goal, 2);
        const RlMatchState& m = goal.World().match;
        CHECK(m.score[0] == 1 && m.score[1] == 0);
        CHECK(m.phase == RlMatchPhase::GoalScored && m.lastGoalTeam == 0);
        const float clockAtGoal = m.clock;
        Idle(goal, Ticks(1.0f));
        CHECK(goal.World().match.clock == clockAtGoal);  // stopped
        Idle(goal, Ticks(2.2f));
        CHECK(goal.World().match.phase == RlMatchPhase::Countdown);
        CHECK(Dist2(goal.World().ball.position, 0.0f, 0.0f) < 0.1f);
        CHECK(goal.World().match.clock == clockAtGoal);
        Note("goal: 1:0 for blue, clock stopped at %.2f, then a new kick-off", clockAtGoal);

        // the end of time with the goal ahead: Finished, the leader wins
        Idle(goal, Ticks(3.0f + 4.0f));
        CHECK(goal.World().match.phase == RlMatchPhase::Finished && goal.World().match.winner == 0);

        // a draw at the end of time: overtime without a limit, the first goal wins
        RlMatch draw(tuning, HumanConfig(1, 0x3u));
        Idle(draw, Ticks(3.0f + 4.0f + 0.5f));
        CHECK(draw.World().match.overtime != 0 && draw.World().match.phase == RlMatchPhase::Playing);
        const float ot0 = draw.World().match.clock;
        Idle(draw, Ticks(1.0f));
        Note("overtime clock counts up: %.2f -> %.2f", ot0, draw.World().match.clock);
        CHECK(draw.World().match.clock > ot0 + 0.9f);
        draw.MutableSim().ResetBall({0.0f, 1.2f, -(51.2f + 1.3f)});
        Idle(draw, 2);
        CHECK(draw.World().match.phase == RlMatchPhase::GoalScored && draw.World().match.score[1] == 1);
        Idle(draw, Ticks(3.2f));
        CHECK(draw.World().match.phase == RlMatchPhase::Finished && draw.World().match.winner == 1);
        // nothing moves after the end
        const RlVec3 end = draw.World().cars[0].position;
        std::vector<RlCarInput> go(2);
        go[0].throttle = 1.0f;
        for (int t = 0; t < Ticks(1.0f); ++t) draw.Step(go.data());
        CHECK(Dist2(draw.World().cars[0].position, end.x, end.z) < 0.05f);

        // practice: no clock, a goal only means a new kick-off
        RlMatchConfig practiceConfig = HumanConfig(0, 1u);
        RlMatch practice(tuning, practiceConfig);
        CHECK(practice.SlotCount() == 1);
        Idle(practice, Ticks(3.2f));
        practice.MutableSim().ResetBall({0.0f, 1.2f, 51.2f + 1.3f});
        Idle(practice, 2);
        CHECK(practice.World().match.phase == RlMatchPhase::GoalScored);
        Idle(practice, Ticks(3.2f));
        CHECK(practice.World().match.phase == RlMatchPhase::Countdown);
        Idle(practice, Ticks(40.0f));
        CHECK(practice.World().match.phase == RlMatchPhase::Playing && practice.World().match.overtime == 0);
    }

    // ---------------------------------------------------------------- pads

    void TestPads()
    {
        std::printf("boost pads\n");
        RlMatch match(g_tuning, HumanConfig(1, 0x3u));
        Idle(match, Ticks(3.2f));
        CHECK(match.World().match.phase == RlMatchPhase::Playing);

        // the pad layout of RLBot: 6 big, 28 small
        int big = 0;
        for (int i = 0; i < kPadCount; ++i)
        {
            float x, z;
            big += RlPadSpot(i, x, z) ? 1 : 0;
        }
        CHECK(big == 6);

        RlSim& sim = match.MutableSim();
        float sx, sz;
        RlPadSpot(0, sx, sz);  // small pad at (0, -42.4)
        sim.SetCarBoost(0, 20.0f);
        sim.SetCarPose(0, {sx, 0.32f, sz}, {0, 0, 0, 1}, {});
        Idle(match, 6);
        Note("small pad: boost 20 -> %.1f, respawn in %.2f s", match.World().cars[0].boost, match.World().match.pads[0].respawnLeft);
        CHECK(std::fabs(match.World().cars[0].boost - 32.0f) < 0.1f);
        CHECK(std::fabs(match.World().match.pads[0].respawnLeft - 4.0f) < 0.1f);
        // it does not give boost twice, and it is back after 4 s
        sim.SetCarPose(0, {sx + 6.0f, 0.32f, sz}, {0, 0, 0, 1}, {});
        Idle(match, Ticks(3.9f));
        CHECK(match.World().match.pads[0].respawnLeft > 0.0f);
        Idle(match, Ticks(0.3f));
        CHECK(match.World().match.pads[0].respawnLeft == 0.0f);

        // a big pad: full boost, 10 s
        float bx, bz;
        const bool isBig = RlPadSpot(3, bx, bz);
        CHECK(isBig);
        sim.SetCarBoost(0, 5.0f);
        sim.SetCarPose(0, {bx, 0.32f, bz}, {0, 0, 0, 1}, {});
        Idle(match, 6);
        Note("big pad: boost 5 -> %.1f, respawn in %.2f s", match.World().cars[0].boost, match.World().match.pads[3].respawnLeft);
        CHECK(match.World().cars[0].boost == 100.0f);
        CHECK(std::fabs(match.World().match.pads[3].respawnLeft - 10.0f) < 0.1f);

        // a car with full boost leaves the pad alone; a car in the air above it too
        float ax, az;
        RlPadSpot(1, ax, az);
        sim.SetCarBoost(0, 100.0f);
        sim.SetCarPose(0, {ax, 0.32f, az}, {0, 0, 0, 1}, {});
        Idle(match, 6);
        CHECK(match.World().match.pads[1].respawnLeft == 0.0f);
        sim.SetCarBoost(0, 10.0f);
        sim.SetCarPose(0, {ax, 4.0f, az}, {0, 0, 0, 1}, {});
        Idle(match, 6);
        CHECK(match.World().match.pads[1].respawnLeft == 0.0f);
    }

    // ---------------------------------------------------------------- demolition

    void TestDemolishRespawn()
    {
        std::printf("demolition and respawn\n");
        RlMatch match(g_tuning, HumanConfig(1, 0x3u));
        Idle(match, Ticks(3.2f));
        RlSim& sim = match.MutableSim();
        // blue (slot 0) runs at 23 m/s into the standing orange car (slot 1)
        sim.SetCarPose(0, {-30.0f, 0.32f, -10.0f}, {0, 0.70710678f, 0, 0.70710678f}, {23.0f, 0.0f, 0.0f});
        sim.SetCarPose(1, {-24.0f, 0.32f, -10.0f}, {0, 0.70710678f, 0, 0.70710678f}, {});
        bool demolished = false;
        int downAt = -1;
        int backAt = -1;
        std::vector<RlCarInput> inputs(2);
        inputs[0].throttle = 1.0f;
        inputs[0].boost = true;
        for (int t = 0; t < Ticks(8.0f); ++t)
        {
            match.Step(inputs.data());
            const RlCarState& victim = match.World().cars[1];
            if (victim.demolished && !demolished)
            {
                demolished = true;
                downAt = t;
            }
            if (demolished && !victim.demolished && backAt < 0)
            {
                backAt = t;
            }
        }
        Note("demolished at tick %.0f, back after %.2f s", downAt, static_cast<float>(backAt - downAt) / kTickRate);
        CHECK(demolished);
        CHECK(backAt > 0 && std::fabs(static_cast<float>(backAt - downAt) / kTickRate - 3.0f) < 0.1f);
        // back on one of the respawn spots of orange (on +Z, x = +-23.04 or +-26.88) with the starting boost (it may have moved a little since)
        const RlCarState& back = match.World().cars[1];
        CHECK(back.position.z > 40.0f);
        CHECK(!match.World().cars[0].demolished);
        Note("orange came back at (%.1f, %.1f)", back.position.x, back.position.z);
    }

    // ---------------------------------------------------------------- the statistics of the table (G1.0)

    RlQuat YawQuat(float radians) { return {0.0f, std::sin(radians * 0.5f), 0.0f, std::cos(radians * 0.5f)}; }

    // a match in the Playing phase with every slot human (neutral inputs), ready for the staging of a play through MutableSim
    std::unique_ptr<RlMatch> PlayingMatch(int mode, uint64_t seed = 1)
    {
        RlMatchConfig config = HumanConfig(mode);
        config.seed = seed;
        auto match = std::make_unique<RlMatch>(g_tuning, config);
        Idle(*match, Ticks(3.2f));
        return match;
    }

    // Steps with slot `slot` at full throttle (the others idle) until `done` is true or `limit` ticks have passed; returns the ticks.
    template <typename Done>
    int RunPlay(RlMatch& match, int slot, int limit, Done done, bool boost = false)
    {
        std::vector<RlCarInput> inputs(static_cast<size_t>(match.SlotCount()));
        inputs[static_cast<size_t>(slot)].throttle = 1.0f;
        inputs[static_cast<size_t>(slot)].boost = boost;
        for (int t = 0; t < limit; ++t)
        {
            match.Step(inputs.data());
            if (done(match.World()))
            {
                return t + 1;
            }
        }
        return limit;
    }

    bool HasEvent(const RlWorldState& w, RlMatchEventType type, int slot, int other)
    {
        const int total = w.match.eventHead;
        for (int n = std::max(0, total - kEventRing); n < total; ++n)
        {
            const RlMatchEvent& e = w.match.events[n % kEventRing];
            if (e.type == static_cast<int32_t>(type) && e.slot == slot && (other == -2 || e.other == other))
            {
                return true;
            }
        }
        return false;
    }

    // blue slot 0 runs at a ball at rest on the floor in front of the goal at +Z and scores
    struct GoalPlay
    {
        bool scored = false;
        int goalTick = 0;
        float ballSpeedAtGoal = 0.0f;
    };

    // the cars that do not take part stand at the touchline, away from the ball and the goals
    void ParkOthers(RlMatch& match, int keep)
    {
        for (int i = 0; i < match.SlotCount(); ++i)
        {
            if (i != keep)
            {
                match.MutableSim().SetCarPose(i, {-36.0f + 3.0f * static_cast<float>(i), 0.3776f, 0.0f}, {0, 0, 0, 1}, {});
            }
        }
    }

    GoalPlay ScoreFrom(RlMatch& match, int slot, float x, float z, float yaw, float speed, float ballZ)
    {
        RlSim& sim = match.MutableSim();
        ParkOthers(match, slot);
        sim.ResetBall({0.0f, g_tuning.ball.radius + 0.01f, ballZ});
        sim.SetCarPose(slot, {x, 0.3776f, z}, YawQuat(yaw), {std::sin(yaw) * speed, 0.0f, std::cos(yaw) * speed});
        GoalPlay play;
        RunPlay(match, slot, Ticks(4.0f), [&](const RlWorldState& w) {
            if (w.match.phase == RlMatchPhase::GoalScored)
            {
                play.scored = true;
                play.goalTick = static_cast<int>(w.tick);
                const RlVec3 v = w.ball.linearVelocity;
                play.ballSpeedAtGoal = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                return true;
            }
            return false;
        });
        return play;
    }

    int PointsOf(const RlMatchState& m, int slot)
    {
        return 100 * m.goals[slot] + 50 * m.assists[slot] + 50 * m.saves[slot] + 20 * m.shots[slot] + 25 * m.demos[slot] + 2 * m.touches[slot];
    }

    void TestMatchStatistics()
    {
        std::printf("statistics of the match: points, assists, shots, saves, demolitions, events\n");

        // ---- the goal: +100 points and a goal for the scorer, nothing for the mate who did not touch, the speed of the ball, the events
        {
            auto match = PlayingMatch(2);
            const GoalPlay play = ScoreFrom(*match, 0, 0.0f, 40.0f, 0.0f, 18.0f, 47.0f);
            const RlMatchState& m = match->World().match;
            Note("goal at tick %.0f, ball %.1f m/s (%.0f km/h), points %.0f, touches %.0f, shots %.0f", play.goalTick, play.ballSpeedAtGoal, play.ballSpeedAtGoal * 3.6f,
                 m.points[0], m.touches[0], m.shots[0]);
            CHECK(play.scored);
            CHECK(m.score[0] == 1 && m.goals[0] == 1 && m.lastGoalTeam == 0 && m.lastGoalScorer == 0 && m.lastGoalOwn == 0);
            CHECK(m.points[0] == PointsOf(m, 0) && m.points[0] >= 100);
            CHECK(m.points[1] == 0 && m.assists[0] == 0 && m.lastGoalAssist == -1);
            CHECK(std::fabs(m.lastGoalSpeed - play.ballSpeedAtGoal) < 1e-4f);  // the speed of the ball at the tick of the goal
            CHECK(HasEvent(match->World(), RlMatchEventType::Goal, 0, -1));
        }

        // ---- an own goal: the score goes to the other team, the player gets no points for it
        {
            auto match = PlayingMatch(1, 2);
            const GoalPlay play = ScoreFrom(*match, 0, 0.0f, -40.0f, 3.14159265f, 18.0f, -47.0f);  // blue runs at the ball and puts it into its own goal
            const RlMatchState& m = match->World().match;
            CHECK(play.scored);
            CHECK(m.score[1] == 1 && m.score[0] == 0 && m.lastGoalOwn == 1 && m.lastGoalScorer == 0);
            CHECK(m.goals[0] == 0 && m.points[0] == PointsOf(m, 0) && m.points[0] < 100);
            CHECK(HasEvent(match->World(), RlMatchEventType::OwnGoal, 0, -1));
        }

        // ---- the assist: a mate's touch counts when it is at most 5 s before the goal (and before the touch of the scorer)
        {
            struct Outcome
            {
                bool scored = false;
                int assist = -2;
                int gap = 0;  // ticks between the touch of the mate and the goal
                int points = 0, assists = 0;
            };
            const auto play = [&](int waitTicks) {
                auto match = PlayingMatch(2);
                RlSim& sim = match->MutableSim();
                // the mate (slot 1) touches the ball in the middle of the field
                ParkOthers(*match, 1);
                sim.ResetBall({10.0f, g_tuning.ball.radius + 0.01f, 0.0f});
                sim.SetCarPose(1, {10.0f, 0.3776f, -6.0f}, YawQuat(0.0f), {0.0f, 0.0f, 8.0f});
                int touchTick = -1;
                RunPlay(*match, 1, Ticks(2.0f), [&](const RlWorldState& w) {
                    if (w.match.touches[1] > 0)
                    {
                        touchTick = static_cast<int>(w.tick);
                        return true;
                    }
                    return false;
                });
                sim.ResetBall({30.0f, g_tuning.ball.radius + 0.01f, 0.0f});  // the ball rests until the scorer comes
                Idle(*match, waitTicks);
                const GoalPlay goal = ScoreFrom(*match, 0, 0.0f, 40.0f, 0.0f, 18.0f, 47.0f);
                Outcome o;
                o.scored = goal.scored;
                o.assist = match->World().match.lastGoalAssist;
                o.gap = goal.goalTick - touchTick;
                o.points = match->World().match.points[1];
                o.assists = match->World().match.assists[1];
                return o;
            };
            const Outcome probe = play(60);
            const int base = probe.gap - 60;  // what the run to the goal takes
            const Outcome inside = play(588 - base);   // 4.9 s
            const Outcome outside = play(612 - base);  // 5.1 s
            Note("the mate's touch %.0f ticks before the goal: assist slot %.0f; %.0f ticks: assist slot %.0f", inside.gap, inside.assist, outside.gap, outside.assist);
            CHECK(inside.scored && outside.scored);
            CHECK(inside.gap >= 586 && inside.gap <= 590);
            CHECK(inside.assist == 1 && inside.assists == 1 && inside.points >= 50);
            CHECK(outside.assist == -1 && outside.assists == 0);
        }

        // ---- a shot on goal: a touch after which the ball flies at the goal counts, a touch that misses does not
        for (int miss = 0; miss < 2; ++miss)
        {
            auto match = PlayingMatch(1, 3);
            RlSim& sim = match->MutableSim();
            const float x = miss != 0 ? 30.0f : 0.0f;
            ParkOthers(*match, 0);
            sim.ResetBall({x, g_tuning.ball.radius + 0.01f, 25.0f});
            sim.SetCarPose(0, {x, 0.3776f, 18.0f}, YawQuat(0.0f), {0.0f, 0.0f, 18.0f});
            RunPlay(*match, 0, Ticks(1.0f), [&](const RlWorldState& w) { return w.match.touches[0] > 0 && w.tick > 0; });
            Idle(*match, 6);
            const RlMatchState& m = match->World().match;
            Note("%.0f = touch wide of the goal, 0 = at the goal: shots %.0f, points %.0f", miss, m.shots[0], m.points[0]);
            CHECK(m.touches[0] >= 1);
            CHECK(m.shots[0] == (miss != 0 ? 0 : 1));
            CHECK(HasEvent(match->World(), RlMatchEventType::Shot, 0, -1) == (miss == 0));
        }

        // ---- a save: the ball heads for the own goal, the touch of a defender sends it away (+50); a touch that leaves it heading in is no save
        for (int still = 0; still < 2; ++still)
        {
            auto match = PlayingMatch(1, 4);
            RlSim& sim = match->MutableSim();
            ParkOthers(*match, 0);
            if (still == 0)
            {
                sim.ResetBall({0.0f, 1.2f, -30.0f}, {0.0f, 0.0f, -18.0f});
                sim.SetCarPose(0, {0.0f, 0.3776f, -38.0f}, YawQuat(0.0f), {});
                Idle(*match, Ticks(1.0f));
            }
            else
            {
                sim.ResetBall({0.0f, g_tuning.ball.radius + 0.01f, -30.0f}, {0.0f, 0.0f, -10.0f});
                sim.SetCarPose(0, {0.0f, 0.3776f, -22.0f}, YawQuat(3.14159265f), {0.0f, 0.0f, -22.0f});
                RunPlay(*match, 0, Ticks(1.5f), [&](const RlWorldState& w) { return w.match.touches[0] > 0; });
                Idle(*match, 6);
            }
            const RlMatchState& m = match->World().match;
            Note("%.0f = a touch that pushes it on, 0 = a defender in the way: saves %.0f, touches %.0f, ball velocity z %.1f", still, m.saves[0], m.touches[0],
                 match->World().ball.linearVelocity.z);
            CHECK(m.touches[0] >= 1);
            if (still == 0)
            {
                CHECK(m.saves[0] == 1 && m.points[0] >= 50 && HasEvent(match->World(), RlMatchEventType::Save, 0, -1));
            }
            else
            {
                CHECK(m.saves[0] == 0);
            }
        }

        // ---- a demolition: +1 demo, +25, the event with who and whom
        {
            RlMatch match(g_tuning, HumanConfig(1, 0x3u));
            Idle(match, Ticks(3.2f));
            RlSim& sim = match.MutableSim();
            sim.SetCarPose(0, {-30.0f, 0.32f, -10.0f}, {0, 0.70710678f, 0, 0.70710678f}, {23.0f, 0.0f, 0.0f});
            sim.SetCarPose(1, {-24.0f, 0.32f, -10.0f}, {0, 0.70710678f, 0, 0.70710678f}, {});
            RunPlay(match, 0, Ticks(3.0f), [&](const RlWorldState& w) { return w.cars[1].demolished; }, true);
            const RlMatchState& m = match.World().match;
            CHECK(match.World().cars[1].demolished);
            CHECK(m.demos[0] == 1 && m.demos[1] == 0 && m.points[0] >= 25);
            CHECK(HasEvent(match.World(), RlMatchEventType::Demo, 0, 1));
        }

        // ---- the rules hold over whole matches of bots: the points are exactly the sum of the rules, the ring keeps the last events, two runs agree
        {
            std::vector<uint8_t> first;
            for (int run = 0; run < 2; ++run)
            {
                RlMatchConfig config;
                config.mode = 4;
                config.seed = 21;
                config.humanMask = 0;
                config.botLevel = 1;
                RlMatch match(g_tuning, config);
                std::vector<RlCarInput> inputs(8);
                for (int t = 0; t < Ticks(300.0f); ++t)
                {
                    match.Step(inputs.data());
                }
                const RlMatchState& m = match.World().match;
                int sum = 0;
                for (int slot = 0; slot < 8; ++slot)
                {
                    CHECK(m.points[slot] == PointsOf(m, slot));
                    sum += m.points[slot];
                }
                if (run == 0)
                {
                    Note("4v4 bots, 5 min: %.0f events, %.0f points in all, score %.0f : %.0f", m.eventHead, sum, m.score[0], m.score[1]);
                    CHECK(m.eventHead >= 1 && sum > 0);
                    const auto* bytes = reinterpret_cast<const uint8_t*>(&m);
                    first.assign(bytes, bytes + sizeof(RlMatchState));
                }
                else
                {
                    CHECK(std::memcmp(first.data(), &m, sizeof(RlMatchState)) == 0);  // the same run, the same bytes (the statistics, the events, the names)
                }
            }
        }

        // ---- the ring: the oldest events are overwritten, the order is kept
        {
            auto match = PlayingMatch(1, 5);
            const RlWorldState& w = match->World();
            // goals one after another: each kick-off the ball is put in front of the goal again
            int goals = 0;
            for (int n = 0; n < kEventRing + 4 && goals < kEventRing + 4; ++n)
            {
                Idle(*match, Ticks(3.5f) + Ticks(3.2f));
                if (match->World().match.phase != RlMatchPhase::Playing)
                {
                    break;
                }
                const GoalPlay p = ScoreFrom(*match, 0, 0.0f, 40.0f, 0.0f, 18.0f, 47.0f);
                goals += p.scored ? 1 : 0;
            }
            Note("%.0f goals in a row, %.0f events: the ring holds the last %.0f", goals, w.match.eventHead, kEventRing);
            CHECK(goals >= 2);
            if (w.match.eventHead > kEventRing)
            {
                uint32_t lastTick = 0;
                bool ordered = true;
                for (int n = w.match.eventHead - kEventRing; n < w.match.eventHead; ++n)
                {
                    const RlMatchEvent& e = w.match.events[n % kEventRing];
                    ordered = ordered && e.tick >= lastTick;
                    lastTick = e.tick;
                }
                CHECK(ordered);
            }
        }

        // ---- the names: the given ones, "Player" for a human, the names of the bots in order, a long name is cut at a character
        {
            RlMatchConfig config = HumanConfig(2, 0x1u);
            config.names[0] = "Artem";
            config.names[3] = "Very long name of a player that does not fit in the field of the table, \xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD";
            RlMatch match(g_tuning, config);
            const RlMatchState& m = match.World().match;
            CHECK(std::strcmp(m.names[0], "Artem") == 0);
            CHECK(std::strcmp(m.names[1], "Comet") == 0 && std::strcmp(m.names[2], "Blaze") == 0);
            CHECK(std::strlen(m.names[3]) == static_cast<size_t>(kNameBytes - 1) && m.names[3][kNameBytes - 1] == 0);
            RlMatch other(g_tuning, HumanConfig(1, 0x1u));
            CHECK(std::strcmp(other.World().match.names[0], "Player") == 0 && std::strcmp(other.World().match.names[1], "Comet") == 0);
        }

        // ---- the prediction of the shots: straight at the goal, wide, too slow, over the bar, with a bounce
        {
            RlBallState ball;
            ball.position = {0.0f, 1.0f, 30.0f};
            ball.linearVelocity = {0.0f, 0.0f, 25.0f};
            CHECK(RlBallHeadsIntoGoal(g_tuning, ball, 1));
            CHECK(!RlBallHeadsIntoGoal(g_tuning, ball, -1));
            ball.position.x = 20.0f;
            CHECK(!RlBallHeadsIntoGoal(g_tuning, ball, 1));  // wide of the posts
            ball.position = {0.0f, 1.0f, 30.0f};
            ball.linearVelocity = {0.0f, 0.0f, 3.0f};
            CHECK(!RlBallHeadsIntoGoal(g_tuning, ball, 1));  // too slow for 2 s
            ball.linearVelocity = {0.0f, 12.0f, 25.0f};
            CHECK(!RlBallHeadsIntoGoal(g_tuning, ball, 1));  // over the bar
            ball.position = {0.0f, 6.0f, 25.0f};
            ball.linearVelocity = {0.0f, -3.0f, 15.0f};
            CHECK(RlBallHeadsIntoGoal(g_tuning, ball, 1));  // comes down, bounces on the floor and goes in
            ball.position = {0.0f, 1.0f, -30.0f};
            ball.linearVelocity = {0.0f, 0.0f, -25.0f};
            CHECK(RlBallHeadsIntoGoal(g_tuning, ball, -1));
        }

        // ---- a snapshot and the way back: the statistics are part of the state, the same ticks again give the same bytes
        {
            RlMatchConfig config;
            config.mode = 2;
            config.seed = 8;
            config.humanMask = 0;
            RlMatch match(g_tuning, config);
            std::vector<RlCarInput> inputs(4);
            for (int t = 0; t < Ticks(40.0f); ++t)
            {
                match.Step(inputs.data());
            }
            std::vector<uint8_t> snapshot;
            match.SaveState(snapshot);
            for (int t = 0; t < Ticks(40.0f); ++t)
            {
                match.Step(inputs.data());
            }
            const RlMatchState forward = match.World().match;
            CHECK(match.RestoreState(snapshot));
            for (int t = 0; t < Ticks(40.0f); ++t)
            {
                match.Step(inputs.data());
            }
            CHECK(std::memcmp(&forward, &match.World().match, sizeof(RlMatchState)) == 0);
        }
    }

    // ---------------------------------------------------------------- the bots

    struct BotRun
    {
        int goals[2] = {0, 0};
        int ownGoals = 0;
        int demolitions = 0;
        int ballHits = 0;
        float maxStillSeconds = 0.0f;
        bool finite = true;
        double msPerTick = 0.0;
        double worstMs = 0.0;
        uint32_t ticks = 0;
        std::vector<uint8_t> finalState;
    };

    BotRun RunBots(int mode, uint64_t seed, int level, float seconds, bool measure)
    {
        RlMatchConfig config;
        config.mode = mode;
        config.seed = seed;
        config.humanMask = 0;
        config.botLevel = level;
        RlMatch match(g_tuning, config);
        BotRun run;
        std::vector<RlCarInput> inputs(static_cast<size_t>(match.SlotCount()));
        float still[kMaxCars] = {};
        int lastScore[2] = {0, 0};
        double total = 0.0;
        const int count = Ticks(seconds);
        for (int t = 0; t < count; ++t)
        {
            const auto start = std::chrono::steady_clock::now();
            match.Step(inputs.data());
            if (measure)
            {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                total += ms;
                run.worstMs = std::max(run.worstMs, ms);
            }
            const RlWorldState& w = match.World();
            run.demolitions += match.LastEvents().demolitions;
            run.ballHits += match.LastEvents().ballHits;
            if (t % 60 == 0 && !Finite(w))
            {
                run.finite = false;
            }
            for (int team = 0; team < 2; ++team)
            {
                if (w.match.score[team] > lastScore[team])
                {
                    run.goals[team] += w.match.score[team] - lastScore[team];
                    lastScore[team] = w.match.score[team];
                    run.ownGoals += w.match.lastGoalOwn;
                }
            }
            for (int i = 0; i < w.carCount; ++i)
            {
                const RlCarState& c = w.cars[i];
                const float v = std::sqrt(c.linearVelocity.x * c.linearVelocity.x + c.linearVelocity.y * c.linearVelocity.y +
                                          c.linearVelocity.z * c.linearVelocity.z);
                if (w.match.phase == RlMatchPhase::Playing && !c.demolished && v < 1.0f)
                {
                    still[i] += kTickDt;
                    run.maxStillSeconds = std::max(run.maxStillSeconds, still[i]);
                    if (g_verbose && t % 120 == 0 && still[i] > 5.0f)
                    {
                        std::printf("    t=%.0f s slot %d still %.0f s at (%.1f %.1f %.1f) up.y=%.2f contacts=%d ball (%.1f %.1f %.1f) boost %.0f\n", t / 120.0, i, still[i], c.position.x, c.position.y, c.position.z, 0.0, c.wheelContacts, w.ball.position.x, w.ball.position.y, w.ball.position.z, c.boost);
                    }
                }
                else
                {
                    still[i] = 0.0f;
                }
            }
        }
        run.ticks = match.World().tick;
        run.msPerTick = total / std::max(count, 1);
        match.SaveState(run.finalState);
        return run;
    }

    // the numbers of the bots over several seeds (the report of G4b compares them before and after a change of the bots)
    void BotStats()
    {
        std::printf("bot statistics, 5 seeds each (touches = hits of the ball, own = own goals)\n");
        for (const int mode : {4, 1})
        {
            int hits = 0, goals = 0, own = 0, demolitions = 0;
            float standstill = 0.0f;
            const float seconds = mode == 4 ? 300.0f : 180.0f;
            for (int seed = 1; seed <= 5; ++seed)
            {
                const BotRun r = RunBots(mode, static_cast<uint64_t>(seed) * 101, 1, seconds, false);
                hits += r.ballHits;
                goals += r.goals[0] + r.goals[1];
                own += r.ownGoals;
                demolitions += r.demolitions;
                standstill = std::max(standstill, r.maxStillSeconds);
            }
            std::printf("  %dv%d, %.0f s: touches %.1f, goals %.1f (own %.1f), demolitions %.1f per match, longest standstill %.1f s\n", mode, mode,
                        static_cast<double>(seconds), hits / 5.0, goals / 5.0, own / 5.0, demolitions / 5.0, static_cast<double>(standstill));
        }
    }

    void TraceBots(int mode, int level)
    {
        RlMatchConfig config;
        config.mode = mode;
        config.seed = 101;
        config.humanMask = 0;
        config.botLevel = level;
        RlMatch match(g_tuning, config);
        std::vector<RlCarInput> inputs(static_cast<size_t>(match.SlotCount()));
        int lastHits = 0;
        for (int t = 0; t < Ticks(60.0f); ++t)
        {
            match.Step(inputs.data());
            lastHits += match.LastEvents().ballHits;
            if (t % 120 == 0)
            {
                const RlWorldState& w = match.World();
                std::printf("t=%2d hits %d ball (%.0f %.1f %.0f) v %.0f |", t / 120, lastHits, static_cast<double>(w.ball.position.x),
                            static_cast<double>(w.ball.position.y), static_cast<double>(w.ball.position.z),
                            static_cast<double>(std::sqrt(w.ball.linearVelocity.x * w.ball.linearVelocity.x + w.ball.linearVelocity.z * w.ball.linearVelocity.z)));
                for (int i = 0; i < w.carCount; ++i)
                {
                    const RlCarState& c = w.cars[i];
                    const float d = std::sqrt((c.position.x - w.ball.position.x) * (c.position.x - w.ball.position.x) + (c.position.z - w.ball.position.z) * (c.position.z - w.ball.position.z));
                    std::printf(" [%d d%.0f v%.0f b%.0f%s]", i, static_cast<double>(d), static_cast<double>(std::sqrt(c.linearVelocity.x * c.linearVelocity.x + c.linearVelocity.z * c.linearVelocity.z)),
                                static_cast<double>(c.boost), c.onGround ? "" : " air");
                }
                std::printf("%c", 10);
            }
        }
    }

    void TestBotMatches()
    {
        std::printf("bot matches\n");
        for (int level = 0; level <= 2; ++level)
        {
            const BotRun r = RunBots(2, 11 + static_cast<uint64_t>(level), level, 120.0f, false);
            Note("2v2 level %.0f, 2 min: goals blue %.0f orange %.0f, ball hits %.0f, demolitions %.0f, longest standstill %.1f s", level, r.goals[0], r.goals[1],
                 r.ballHits, r.demolitions, r.maxStillSeconds);
            CHECK(r.finite);
            CHECK(r.maxStillSeconds < 10.0f);
        }
        const BotRun full = RunBots(4, 5, 1, 300.0f, true);
        Note("4v4 level 1, the whole 5 minutes: goals %.0f : %.0f, ball hits %.0f, demolitions %.0f", full.goals[0], full.goals[1], full.ballHits, full.demolitions);
        Note("longest standstill of a car %.1f s; tick with 8 bots: average %.4f ms, worst %.3f ms", full.maxStillSeconds, full.msPerTick, full.worstMs);
        CHECK(full.finite);
        CHECK(full.maxStillSeconds < 10.0f);
        CHECK(full.goals[0] + full.goals[1] >= 1);
        CHECK(full.ballHits > 20);
        // 1v1 on the hard level: three matches of 3 minutes, goals in total
        int oneGoals = 0;
        int oneHits = 0;
        float oneStill = 0.0f;
        bool oneFinite = true;
        for (uint64_t seed = 3; seed <= 5; ++seed)
        {
            const BotRun one = RunBots(1, seed, 2, 180.0f, false);
            oneGoals += one.goals[0] + one.goals[1];
            oneHits += one.ballHits;
            oneStill = std::max(oneStill, one.maxStillSeconds);
            oneFinite = oneFinite && one.finite;
        }
        Note("1v1 level 2, 3 matches of 3 min: goals %.0f, ball hits %.0f, longest standstill %.1f s", oneGoals, oneHits, oneStill);
        CHECK(oneFinite && oneStill < 10.0f);
        CHECK(oneGoals >= 1);
        CHECK(oneHits > 40);
    }

    void TestDeterminismAndRollback()
    {
        std::printf("determinism and rollback\n");
        const BotRun a = RunBots(4, 77, 1, 90.0f, false);
        const BotRun b = RunBots(4, 77, 1, 90.0f, false);
        const BotRun c = RunBots(4, 78, 1, 90.0f, false);
        CHECK(a.finalState == b.finalState);
        CHECK(a.finalState != c.finalState);
        Note("same seed: identical %.0f; other seed: different %.0f; snapshot %.0f bytes", a.finalState == b.finalState ? 1.0 : 0.0,
             a.finalState != c.finalState ? 1.0 : 0.0, static_cast<double>(a.finalState.size()));

        // a rollback in the middle of the match: the same inputs again, the same end
        RlMatchConfig config;
        config.mode = 4;
        config.seed = 77;
        config.humanMask = 1;  // slot 0 is a human with scripted input, the rest are bots
        RlMatch match(g_tuning, config);
        std::vector<RlCarInput> inputs(8);
        const auto drive = [&](int from, int ticks) {
            for (int t = from; t < from + ticks; ++t)
            {
                inputs[0] = RlCarInput{};
                inputs[0].throttle = 1.0f;
                inputs[0].steer = ((t / 90) % 3 == 0) ? 0.6f : -0.2f;
                inputs[0].boost = (t / 50) % 2 == 0;
                inputs[0].jump = (t % 200) < 6;
                match.Step(inputs.data());
            }
        };
        drive(0, Ticks(20.0f));
        std::vector<uint8_t> snapshot;
        match.SaveState(snapshot);
        drive(Ticks(20.0f), Ticks(40.0f));
        std::vector<uint8_t> first;
        match.SaveState(first);
        CHECK(match.RestoreState(snapshot));
        std::vector<uint8_t> again;
        match.SaveState(again);
        CHECK(again == snapshot);
        drive(Ticks(20.0f), Ticks(40.0f));
        std::vector<uint8_t> second;
        match.SaveState(second);
        Note("rollback of 40 s in a 4v4 match: equal %.0f (%.0f bytes)", first == second ? 1.0 : 0.0, static_cast<double>(first.size()));
        CHECK(first == second);

        // through the ring, across a goal
        RlMatch goalMatch(g_tuning, HumanConfig(1, 0x3u));
        RlMatchRing ring(16);
        std::vector<RlCarInput> idle(2);
        Idle(goalMatch, Ticks(3.2f));
        uint32_t tick = 0;
        for (int i = 0; i < 10; ++i)
        {
            if (i == 3)
            {
                goalMatch.MutableSim().ResetBall({0.0f, 1.2f, 51.2f + 1.3f});
            }
            Idle(goalMatch, Ticks(0.5f));
            ring.Push(++tick, goalMatch);
        }
        std::vector<uint8_t> end;
        goalMatch.SaveState(end);
        CHECK(ring.Restore(2, goalMatch));
        CHECK(goalMatch.World().match.score[0] == 0);  // before the goal
        for (uint32_t t = 2; t < 10; ++t)
        {
            if (t == 3)
            {
                goalMatch.MutableSim().ResetBall({0.0f, 1.2f, 51.2f + 1.3f});
            }
            Idle(goalMatch, Ticks(0.5f));
        }
        std::vector<uint8_t> replay;
        goalMatch.SaveState(replay);
        Note("ring rollback across a goal: equal %.0f", end == replay ? 1.0 : 0.0);
        CHECK(end == replay);
    }

    // ---------------------------------------------------------------- controls

    RlRawInput Raw() { return RlRawInput{}; }

    // a tap shorter than a frame (and a frame with no tick in it) is not lost: the latch keeps it until a tick takes it
    void TestInputLatch()
    {
        std::printf("input latch: a tap is never lost between two ticks\n");
        RlInputLatch latch;
        RlCarInput held;
        // nothing pressed: the tick sees what is held
        held.throttle = 1.0f;
        RlCarInput tick = latch.Take(held);
        CHECK(tick.throttle == 1.0f && !tick.jump && !latch.Pending());
        // a click of the jump button inside a frame, let go before the tick: the tick still gets it, the next one does not
        RlCarInput tapped;
        tapped.jump = true;
        latch.Press(tapped);
        CHECK(latch.Pending());
        tick = latch.Take(held);
        CHECK(tick.jump && tick.throttle == 1.0f);
        tick = latch.Take(held);
        CHECK(!tick.jump && !latch.Pending());
        // two frames with no tick between them: both taps are kept (the larger steering, both buttons)
        RlCarInput first, second;
        first.steer = -0.4f;
        first.boost = true;
        second.steer = 1.0f;
        second.jump = true;
        latch.Press(first);
        latch.Press(second);
        RlCarInput heldSteer;
        heldSteer.steer = 0.2f;
        tick = latch.Take(heldSteer);
        CHECK(tick.steer == 1.0f && tick.boost && tick.jump);
        // a stick held the other way beyond the tap wins (the larger value)
        RlCarInput small;
        small.steer = 0.3f;
        latch.Press(small);
        RlCarInput heldFull;
        heldFull.steer = -1.0f;
        tick = latch.Take(heldFull);
        CHECK(tick.steer == -1.0f);
        // the whole way: a key that goes down and up inside a frame (the "pressed" events of the controls) gives one tick of its action
        const RlControls controls = RlControls::Defaults();
        RlRawInput none;
        RlRawInput pressedD;
        pressedD.keys['D'] = true;  // the event of the frame; the key is no longer down at the end of it
        latch.Press(controls.Evaluate(pressedD).car);
        tick = latch.Take(controls.Evaluate(none).car);
        CHECK(tick.steer > 0.9f);
        tick = latch.Take(controls.Evaluate(none).car);
        CHECK(tick.steer == 0.0f);
    }

    void TestControls()
    {
        std::printf("controls as data\n");
        const RlControls controls = RlControls::Defaults();

        // the PC layout of Rocket League
        RlRawInput r = Raw();
        r.mouse[0] = true;
        CHECK(controls.Evaluate(r).car.boost && !controls.Evaluate(r).car.jump);           // left mouse button: boost
        r = Raw();
        r.mouse[1] = true;
        CHECK(controls.Evaluate(r).car.jump && !controls.Evaluate(r).car.boost);           // right mouse button: jump
        r = Raw();
        r.keys[0x20] = true;
        CHECK(controls.Evaluate(r).Down(RlButtonAction::BallCam));                          // Space: ball cam
        r = Raw();
        r.keys[0x10] = true;
        CHECK(controls.Evaluate(r).car.handbrake && controls.Evaluate(r).car.airRoll);      // Shift: powerslide and air roll
        r = Raw();
        r.keys['W'] = true;
        CHECK(controls.Evaluate(r).car.throttle == 1.0f && controls.Evaluate(r).car.pitch == -1.0f);
        r = Raw();
        r.keys['S'] = true;
        CHECK(controls.Evaluate(r).car.throttle == -1.0f && controls.Evaluate(r).car.pitch == 1.0f);
        r = Raw();
        r.keys['D'] = true;
        CHECK(controls.Evaluate(r).car.steer == 1.0f);
        r.keys['A'] = true;
        CHECK(controls.Evaluate(r).car.steer == 0.0f);  // both cancel
        r = Raw();
        r.keys['E'] = true;
        CHECK(controls.Evaluate(r).car.roll == 1.0f);
        r = Raw();
        r.keys['Q'] = true;
        CHECK(controls.Evaluate(r).car.roll == -1.0f);
        r = Raw();
        r.keys[0x09] = true;
        CHECK(controls.Evaluate(r).Down(RlButtonAction::Scoreboard));
        r = Raw();
        r.keys['M'] = true;
        CHECK(controls.Evaluate(r).Down(RlButtonAction::Menu));

        // the Xbox controller
        r = Raw();
        r.padAxes[PadRightTrigger] = 1.0f;
        CHECK(controls.Evaluate(r).car.throttle == 1.0f);
        r = Raw();
        r.padAxes[PadLeftTrigger] = 1.0f;
        CHECK(controls.Evaluate(r).car.throttle == -1.0f);
        r = Raw();
        r.padAxes[PadLeftX] = 0.6f;
        r.padAxes[PadLeftY] = 1.0f;  // stick right and forward
        CHECK(controls.Evaluate(r).car.steer > 0.4f && controls.Evaluate(r).car.pitch == -1.0f);
        r = Raw();
        r.padAxes[PadLeftX] = 0.1f;  // inside the dead zone
        CHECK(controls.Evaluate(r).car.steer == 0.0f);
        const struct
        {
            RlPadButton button;
            RlButtonAction action;
        } pad[] = {{PadA, RlButtonAction::Jump},      {PadB, RlButtonAction::Boost},      {PadX, RlButtonAction::Powerslide},
                   {PadY, RlButtonAction::BallCam},   {PadBack, RlButtonAction::Scoreboard}, {PadStart, RlButtonAction::Menu}};
        for (const auto& p : pad)
        {
            r = Raw();
            r.padButtons = 1u << p.button;
            CHECK(controls.Evaluate(r).Down(p.action));
        }
        r = Raw();
        r.padButtons = 1u << PadRB;
        CHECK(controls.Evaluate(r).car.roll == 1.0f);
        r = Raw();
        r.padButtons = 1u << PadLB;
        CHECK(controls.Evaluate(r).car.roll == -1.0f);
        // keyboard and pad together do not exceed the range
        r = Raw();
        r.keys['W'] = true;
        r.padAxes[PadRightTrigger] = 1.0f;
        CHECK(controls.Evaluate(r).car.throttle == 1.0f);

        // everything at once: gas, steer, pitch, roll, boost, jump, powerslide from the keyboard, the mouse and the pad together
        r = Raw();
        r.keys['W'] = true;
        r.keys['D'] = true;
        r.keys['E'] = true;
        r.keys[0x10] = true;       // Shift
        r.mouse[0] = true;         // left button: boost
        r.mouse[1] = true;         // right button: jump
        r.padAxes[PadLeftX] = -0.5f;  // the stick the other way: the steer is the sum, not the last device
        {
            const RlControlState all = controls.Evaluate(r);
            CHECK(all.car.throttle == 1.0f && all.car.pitch == -1.0f && all.car.roll == 1.0f);
            CHECK(all.car.steer > 0.5f && all.car.steer < 0.8f);
            CHECK(all.car.boost && all.car.jump && all.car.handbrake && all.car.airRoll);
        }
        // every pair of the controls of the car works with every other (a held boost does not block the steer, and so on)
        {
            const char* const keys[] = {"W", "S", "A", "D", "Q", "E", "Shift", "Space", "Tab", "M"};
            int bad = 0;
            for (const char* a : keys)
            {
                for (const char* b : keys)
                {
                    for (int mouse = 0; mouse < 3; ++mouse)
                    {
                        RlRawInput x = Raw();
                        RlControls c = RlControls::Defaults();
                        std::string ignore;
                        // find the codes through the bindings: the tokens of the actions
                        const auto code = [&](const char* name) { return std::string(name) == "Shift" ? 0x10 : std::string(name) == "Space" ? 0x20 : std::string(name) == "Tab" ? 0x09 : name[0]; };
                        x.keys[code(a)] = true;
                        x.keys[code(b)] = true;
                        if (mouse > 0) x.mouse[mouse - 1] = true;
                        const RlControlState one = controls.Evaluate(x);
                        RlRawInput onlyA = Raw();
                        onlyA.keys[code(a)] = true;
                        RlRawInput onlyB = Raw();
                        onlyB.keys[code(b)] = true;
                        RlRawInput onlyM = Raw();
                        if (mouse > 0) onlyM.mouse[mouse - 1] = true;
                        const RlControlState sa = controls.Evaluate(onlyA), sb = controls.Evaluate(onlyB), sm = controls.Evaluate(onlyM);
                        // the boolean actions of the pair are the union of the single ones
                        for (int i = 0; i < static_cast<int>(RlButtonAction::Count); ++i)
                        {
                            if (one.buttons[i] != (sa.buttons[i] || sb.buttons[i] || sm.buttons[i])) ++bad;
                        }
                        (void)c;
                    }
                }
            }
            CHECK(bad == 0);
        }

        // the shipped file is the defaults, nothing is missing from it
        std::string error;
        RlControls shipped;
        const bool loaded = shipped.LoadFromFile(RL_CONTROLS_JSON, &error);
        if (!loaded) std::printf("  %s\n", error.c_str());
        CHECK(loaded);
        CHECK(shipped.ToJson() == controls.ToJson());

        // layers: the defaults, the shipped file, the file of the player (one action changed); a missing file is fine
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "myengine_rl_controls_test";
        std::filesystem::create_directories(dir);
        const std::string userFile = (dir / "user.json").generic_u8string();
        {
            std::ofstream file(dir / "user.json", std::ios::binary | std::ios::trunc);
            file << "{ \"buttons\": { \"jump\": [\"Space\", \"MouseRight\"], \"ballCam\": [\"B\"] }, \"axes\": { \"roll\": [\"X\", \"-Z\"] } }";
        }
        RlControls layered;
        CHECK(layered.LoadLayered(RL_CONTROLS_JSON, userFile, &error));
        r = Raw();
        r.keys[0x20] = true;
        CHECK(layered.Evaluate(r).car.jump && !layered.Evaluate(r).Down(RlButtonAction::BallCam));  // Space jumps now
        r = Raw();
        r.keys['B'] = true;
        CHECK(layered.Evaluate(r).Down(RlButtonAction::BallCam));
        r = Raw();
        r.mouse[1] = true;
        CHECK(layered.Evaluate(r).car.jump);
        r = Raw();
        r.mouse[0] = true;
        CHECK(layered.Evaluate(r).car.boost);  // not mentioned: the default stays
        r = Raw();
        r.keys['X'] = true;
        CHECK(layered.Evaluate(r).car.roll == 1.0f);
        r = Raw();
        r.keys['E'] = true;
        CHECK(layered.Evaluate(r).car.roll == 0.0f);  // the old binding of the changed action is gone
        RlControls noUser;
        CHECK(noUser.LoadLayered(RL_CONTROLS_JSON, (dir / "missing.json").generic_u8string(), &error));
        CHECK(noUser.ToJson() == controls.ToJson());

        // mistakes get a message with the action and the binding; the controls stay as they were
        {
            std::ofstream file(dir / "bad.json", std::ios::binary | std::ios::trunc);
            file << "{ \"buttons\": { \"boost\": [\"MouseLeft\", \"Banana\"] } }";
        }
        RlControls broken = controls;
        CHECK(!broken.LoadFromFile((dir / "bad.json").generic_u8string(), &error));
        std::printf("  an unknown key: %s\n", error.c_str());
        CHECK(error.find("boost") != std::string::npos && error.find("Banana") != std::string::npos);
        CHECK(broken.ToJson() == controls.ToJson());
        CHECK(!broken.LoadFromText("not json", &error) && !error.empty());
        CHECK(!broken.LoadFromText("{ \"axes\": { \"throttle\": \"W\" } }", &error) && error.find("throttle") != std::string::npos);
        CHECK(!layered.LoadLayered(RL_CONTROLS_JSON, (dir / "bad.json").generic_u8string(), &error));

        // save and load again
        RlControls edited = controls;
        CHECK(edited.SetButton(RlButtonAction::Boost, {"MouseMiddle", "F"}, &error));
        CHECK(edited.SaveToFile((dir / "saved" / "rl_controls.json").generic_u8string(), &error));
        RlControls again;
        CHECK(again.LoadFromFile((dir / "saved" / "rl_controls.json").generic_u8string(), &error));
        CHECK(again.ToJson() == edited.ToJson());
        CHECK(again.ButtonTokens(RlButtonAction::Boost) == std::vector<std::string>({"MouseMiddle", "F"}));
        std::filesystem::remove_all(dir);
    }

    void TestTuningFile()
    {
        std::printf("match tuning\n");
        RlTuning t;
        CHECK(t.LoadFromText("{\"match\": {\"matchSeconds\": 120, \"padSmallRespawn\": 2}}"));
        CHECK(t.match.matchSeconds == 120.0f && t.match.padSmallRespawn == 2.0f && t.match.padBigBoost == 100.0f);
        RlRng a(5), b(5), c(6);
        CHECK(a.NextU64() == b.NextU64() && a.NextU64() == b.NextU64());
        CHECK(c.NextU64() != RlRng(5).NextU64());
        float lo = 1.0f, hi = 0.0f;
        for (int i = 0; i < 1000; ++i)
        {
            const float f = a.Float01();
            lo = std::min(lo, f);
            hi = std::max(hi, f);
        }
        CHECK(lo >= 0.0f && hi < 1.0f && hi - lo > 0.9f);
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--find-stuck") == 0)
    {
        const int mode = argc > 2 ? std::atoi(argv[2]) : 1;
        const uint64_t seed = argc > 3 ? static_cast<uint64_t>(std::atoi(argv[3])) : 3;
        const int level = argc > 4 ? std::atoi(argv[4]) : 2;
        RlMatchConfig config;
        config.mode = mode;
        config.seed = seed;
        config.humanMask = 0;
        config.botLevel = level;
        RlMatch match(g_tuning, config);
        std::vector<RlCarInput> inputs(static_cast<size_t>(match.SlotCount()));
        float still[8] = {};
        for (int t = 0; t < Ticks(180.0f); ++t)
        {
            match.Step(inputs.data());
            const RlWorldState& w = match.World();
            for (int i = 0; i < w.carCount; ++i)
            {
                const RlCarState& c = w.cars[i];
                const float v = std::sqrt(c.linearVelocity.x * c.linearVelocity.x + c.linearVelocity.y * c.linearVelocity.y + c.linearVelocity.z * c.linearVelocity.z);
                still[i] = (v < 0.5f && !c.demolished && w.match.phase == RlMatchPhase::Playing) ? still[i] + kTickDt : 0.0f;
                if (still[i] > 5.0f && t % 60 == 0)
                {
                    const float upY = 1.0f - 2.0f * (c.rotation.x * c.rotation.x + c.rotation.z * c.rotation.z);
                    std::printf("t %.1f car %d still %.1f s pos (%.1f %.2f %.1f) up.y %.2f wheels %d boost %.0f ball (%.1f %.1f %.1f)\n", static_cast<double>(t) / 120.0, i, static_cast<double>(still[i]),
                                static_cast<double>(c.position.x), static_cast<double>(c.position.y), static_cast<double>(c.position.z), static_cast<double>(upY), c.wheelContacts, static_cast<double>(c.boost),
                                static_cast<double>(w.ball.position.x), static_cast<double>(w.ball.position.y), static_cast<double>(w.ball.position.z));
                }
            }
        }
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--trace-bots") == 0)
    {
        TraceBots(argc > 2 ? std::atoi(argv[2]) : 1, 1);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--bot-stats") == 0)
    {
        BotStats();
        return 0;
    }
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--verbose") == 0)
        {
            g_verbose = true;
        }
    }
    TestControls();
    TestInputLatch();
    TestKickoff();
    TestCountdownAndClock();
    TestPads();
    TestDemolishRespawn();
    TestMatchStatistics();
    TestTuningFile();
    TestBotMatches();
    TestDeterminismAndRollback();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
