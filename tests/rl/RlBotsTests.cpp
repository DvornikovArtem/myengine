// RlBotsTests.cpp
// The bots of the Rocket League match (game/rl, G4f) measured the way the task asks: whole matches without a window, bots on both teams,
// 5 minutes (and overtime) each, several seeds, the two teams swapping sides so that neither side nor spawn gets an advantage.
//   myengine_rl_bots_tests                 the checks of the task (long)
//   myengine_rl_bots_tests --quick         one short series, prints the table only
//   myengine_rl_bots_tests --vs A B [mode] [matches]   A and B are "<version><level>", e.g. 22 (v2 level 2) or 11 (v1 level 1)

#include <rl/RlBotForecast.h>
#include <rl/RlMatch.h>
#include <rl/RlRng.h>
#include <rl/RlSim.h>
#include <rl/RlTuning.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

    RlTuning g_tuning;
    bool g_trace = false;

    struct Kind
    {
        int version = 2;
        int level = 1;
    };

    struct Result
    {
        int score[2] = {0, 0};
        int winner = -1;               // 0 blue, 1 orange, -1 not finished
        int hits[2] = {0, 0};
        int ownGoals[2] = {0, 0};      // own goals made by the team
        int demolitions = 0;
        float longestStand = 0.0f;     // seconds a car stood still while the ball was in play
        float longestStuck[2] = {0.0f, 0.0f};  // seconds a car stood still on its roof, on its side or wedged (not just waiting on its wheels)
        int saves[2] = {0, 0};         // balls on their way into the net that a defender kept out (no goal in the next 2 s)
        double msPerTick = 0.0;
        double msWorstTick = 0.0;
        int ticks = 0;
    };

    // Plays one match: team 0 (blue) with `a`, team 1 (orange) with `b`
    Result Play(int mode, Kind a, Kind b, uint64_t seed, float maxSeconds = 900.0f)
    {
        RlMatchConfig config;
        config.mode = mode;
        config.humanMask = 0;
        config.seed = seed;
        config.botVersion = a.version;
        config.botLevel = a.level;
        config.botVersionOrange = b.version;
        config.botLevelOrange = b.level;
        RlMatch match(g_tuning, config);
        const int slots = match.SlotCount();
        std::vector<RlCarInput> inputs(static_cast<size_t>(slots));
        Result r;
        std::vector<float> standing(static_cast<size_t>(slots), 0.0f);
        std::vector<float> stuck(static_cast<size_t>(slots), 0.0f);
        struct PendingSave
        {
            int tick;
            int team;
            int scoreOther;
        };
        std::vector<PendingSave> pending;
        int previousGoals[2] = {0, 0};
        const int maxTicks = static_cast<int>(maxSeconds * static_cast<float>(kTickRate));
        double total = 0.0;
        for (int t = 0; t < maxTicks; ++t)
        {
            const auto begin = std::chrono::steady_clock::now();
            match.Step(inputs.data());
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            total += ms;
            r.msWorstTick = std::max(r.msWorstTick, ms);
            ++r.ticks;
            const RlWorldState& w = match.World();
            r.demolitions += match.LastEvents().demolitions;
            for (int team = 0; team < 2; ++team)
            {
                if (w.match.score[team] > previousGoals[team])
                {
                    previousGoals[team] = w.match.score[team];
                    if (w.match.lastGoalOwn != 0)
                    {
                        ++r.ownGoals[1 - w.match.lastGoalTeam];  // credited to `lastGoalTeam`, made by the other one
                        if (g_trace)
                        {
                            const int who = w.match.lastGoalScorer;
                            const RlCarState& c = w.cars[who];
                            std::printf("  own goal at %.1f s by slot %d (v%d L%d): car (%.1f %.1f %.1f) v (%.1f %.1f %.1f)  ball (%.1f %.1f %.1f) v (%.1f %.1f %.1f)\n", t / 120.0, who,
                                        who < std::max(mode, 1) ? a.version : b.version, who < std::max(mode, 1) ? a.level : b.level, static_cast<double>(c.position.x),
                                        static_cast<double>(c.position.y), static_cast<double>(c.position.z), static_cast<double>(c.linearVelocity.x), static_cast<double>(c.linearVelocity.y),
                                        static_cast<double>(c.linearVelocity.z), static_cast<double>(w.ball.position.x), static_cast<double>(w.ball.position.y), static_cast<double>(w.ball.position.z),
                                        static_cast<double>(w.ball.linearVelocity.x), static_cast<double>(w.ball.linearVelocity.y), static_cast<double>(w.ball.linearVelocity.z));
                        }
                    }
                }
            }
            // a save: a defender touches a ball that was heading for its goal fast and near
            if (match.LastEvents().ballHits > 0 && w.lastBallToucher >= 0 && w.match.phase == RlMatchPhase::Playing)
            {
                const int team = w.lastBallToucher / std::max(mode, 1);
                const RlBallState& before = match.PreviousWorld().ball;
                const float towardsGoal = (team == 0 ? -before.linearVelocity.z : before.linearVelocity.z);
                const float distance = g_tuning.arena.halfLength - (team == 0 ? -before.position.z : before.position.z);
                if (towardsGoal > 5.0f && distance < 30.0f && std::fabs(before.position.x) < g_tuning.arena.goalHalfWidth + 4.0f)
                {
                    pending.push_back({t, team, w.match.score[1 - team]});
                }
            }
            for (size_t k = 0; k < pending.size();)
            {
                if (t - pending[k].tick >= 240)
                {
                    if (w.match.score[1 - pending[k].team] == pending[k].scoreOther)
                    {
                        ++r.saves[pending[k].team];
                    }
                    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(k));
                }
                else
                {
                    ++k;
                }
            }
            if (w.match.phase == RlMatchPhase::Playing)
            {
                for (int i = 0; i < slots; ++i)
                {
                    const RlCarState& c = w.cars[i];
                    const float upY = 1.0f - 2.0f * (c.rotation.x * c.rotation.x + c.rotation.z * c.rotation.z);
                    const float flatSpeed = std::sqrt(c.linearVelocity.x * c.linearVelocity.x + c.linearVelocity.z * c.linearVelocity.z);
                    stuck[static_cast<size_t>(i)] = (flatSpeed < 1.0f && !c.demolished && (upY < 0.7f || c.wheelContacts < 3)) ? stuck[static_cast<size_t>(i)] + kTickDt : 0.0f;
                    if (g_trace && stuck[static_cast<size_t>(i)] > 5.0f && stuck[static_cast<size_t>(i)] < 5.0f + kTickDt)
                    {
                        std::printf("  stuck for 5 s: slot %d at %.1f s, pos (%.1f %.1f %.1f) up.y %.2f wheels %d boost %.0f, ball (%.1f %.1f %.1f)\n", i, t / 120.0, static_cast<double>(c.position.x),
                                    static_cast<double>(c.position.y), static_cast<double>(c.position.z), static_cast<double>(upY), c.wheelContacts, static_cast<double>(c.boost),
                                    static_cast<double>(w.ball.position.x), static_cast<double>(w.ball.position.y), static_cast<double>(w.ball.position.z));
                    }
                    r.longestStuck[i / std::max(mode, 1)] = std::max(r.longestStuck[i / std::max(mode, 1)], stuck[static_cast<size_t>(i)]);
                    const float speed = std::sqrt(c.linearVelocity.x * c.linearVelocity.x + c.linearVelocity.z * c.linearVelocity.z);
                    standing[static_cast<size_t>(i)] = (speed < 1.0f && !c.demolished) ? standing[static_cast<size_t>(i)] + kTickDt : 0.0f;
                    r.longestStand = std::max(r.longestStand, standing[static_cast<size_t>(i)]);
                }
            }
            if (w.match.phase == RlMatchPhase::Finished)
            {
                r.winner = w.match.winner;
                break;
            }
        }
        const RlWorldState& w = match.World();
        r.score[0] = w.match.score[0];
        r.score[1] = w.match.score[1];
        const int teamSize = std::max(mode, 1);
        for (int i = 0; i < slots; ++i)
        {
            r.hits[i / teamSize] += w.match.touches[i];
        }
        r.msPerTick = total / std::max(r.ticks, 1);
        return r;
    }

    struct Series
    {
        int winsA = 0;
        int winsB = 0;
        int undecided = 0;
        double goalsA = 0.0;
        double goalsB = 0.0;
        double hitsA = 0.0;
        double hitsB = 0.0;
        double ownA = 0.0;
        double ownB = 0.0;
        double savesA = 0.0;
        double savesB = 0.0;
        double longestStuck = 0.0;
        double demolitions = 0.0;
        double longestStand = 0.0;
        double msPerTick = 0.0;
        double msWorst = 0.0;
        int matches = 0;

        double WinRate() const { return (winsA + 0.5 * undecided) / std::max(matches, 1); }
    };

    // `matches` matches, A and B swapping sides every second one
    Series Compare(int mode, Kind a, Kind b, int matches, uint64_t firstSeed = 100)
    {
        Series s;
        for (int m = 0; m < matches; ++m)
        {
            const bool swap = (m % 2) == 1;
            const uint64_t seed = firstSeed + static_cast<uint64_t>(m / 2) * 7919u + static_cast<uint64_t>(mode) * 104729u;
            const Result r = swap ? Play(mode, b, a, seed) : Play(mode, a, b, seed);
            const int sideA = swap ? 1 : 0;
            const int sideB = 1 - sideA;
            ++s.matches;
            if (r.winner < 0)
            {
                ++s.undecided;
            }
            else if (r.winner == sideA)
            {
                ++s.winsA;
            }
            else
            {
                ++s.winsB;
            }
            s.goalsA += r.score[sideA];
            s.goalsB += r.score[sideB];
            s.hitsA += r.hits[sideA];
            s.hitsB += r.hits[sideB];
            s.ownA += r.ownGoals[sideA];
            s.ownB += r.ownGoals[sideB];
            s.savesA += r.saves[sideA];
            s.savesB += r.saves[sideB];
            s.longestStuck = std::max<double>(s.longestStuck, r.longestStuck[sideA]);
            s.demolitions += r.demolitions;
            s.longestStand = std::max<double>(s.longestStand, r.longestStand);
            s.msPerTick += r.msPerTick;
            s.msWorst = std::max(s.msWorst, r.msWorstTick);
        }
        const double n = std::max(s.matches, 1);
        s.goalsA /= n;
        s.goalsB /= n;
        s.hitsA /= n;
        s.hitsB /= n;
        s.ownA /= n;
        s.ownB /= n;
        s.savesA /= n;
        s.savesB /= n;
        s.demolitions /= n;
        s.msPerTick /= n;
        return s;
    }

    void Print(const char* title, int mode, Kind a, Kind b, const Series& s)
    {
        std::printf("%s  (%dv%d, A = v%d L%d, B = v%d L%d, %d matches)\n", title, mode, mode, a.version, a.level, b.version, b.level, s.matches);
        std::printf("  A wins %d, B wins %d, undecided %d  ->  A %.0f%%;  goals per match A %.1f : B %.1f;  hits A %.0f : B %.0f;  own goals A %.2f : B %.2f\n",
                    s.winsA, s.winsB, s.undecided, s.WinRate() * 100.0, s.goalsA, s.goalsB, s.hitsA, s.hitsB, s.ownA, s.ownB);
        std::printf("  saves A %.1f : B %.1f per match; demolitions %.1f per match; longest stand-still %.1f s, longest stuck of team A (roof, side, wedged) %.1f s; tick %.3f ms on average (worst %.2f ms)\n",
                    s.savesA, s.savesB, s.demolitions, s.longestStand, s.longestStuck, s.msPerTick, s.msWorst);
    }

    // The forecast of the ball against the ball of the real simulation: random shots from all over the arena
    void TestForecast()
    {
        std::printf("forecast of the ball against the real simulation (random shots, no cars)\n");
        RlRng rng(5);
        const float times[5] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f};
        std::vector<float> errors[5];
        double forecastMs = 0.0;
        const int shots = 60;
        for (int shot = 0; shot < shots; ++shot)
        {
            RlSim sim(g_tuning, 0);
            RlBallState ball;
            ball.position = {rng.Signed() * 30.0f, 1.0f + rng.Float01() * 12.0f, rng.Signed() * 42.0f};
            ball.linearVelocity = {rng.Signed() * 22.0f, rng.Signed() * 12.0f, rng.Signed() * 28.0f};
            sim.ResetBall(ball.position, ball.linearVelocity, {});
            RlForecast forecast;
            const auto begin = std::chrono::steady_clock::now();
            RlForecastBall(g_tuning, sim.State().ball, 0, forecast);
            forecastMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            int tick = 0;
            for (int i = 0; i < 5; ++i)
            {
                const int target = static_cast<int>(times[i] * static_cast<float>(kTickRate));
                while (tick < target)
                {
                    sim.Step(nullptr);
                    ++tick;
                }
                const RlVec3 truth = sim.State().ball.position;
                const RlVec3 guess = RlForecastAt(forecast, 0, times[i]);
                if (std::fabs(truth.z) > g_tuning.arena.halfLength - 1.0f)
                {
                    continue;  // in a goal: the forecast stops there, the real ball goes on bouncing in the net
                }
                errors[i].push_back(std::sqrt((truth.x - guess.x) * (truth.x - guess.x) + (truth.y - guess.y) * (truth.y - guess.y) + (truth.z - guess.z) * (truth.z - guess.z)));
            }
        }
        for (int i = 0; i < 5; ++i)
        {
            std::sort(errors[i].begin(), errors[i].end());
            const size_t n = errors[i].size();
            double mean = 0.0;
            for (const float e : errors[i])
            {
                mean += e;
            }
            mean /= std::max<size_t>(n, 1);
            std::printf("  after %.1f s: mean error %.2f m, median %.2f m, 90th percentile %.2f m (%zu shots)\n", static_cast<double>(times[i]), mean,
                        static_cast<double>(errors[i][n / 2]), static_cast<double>(errors[i][n * 9 / 10]), n);
            if (i == 1)
            {
                CHECK(errors[i][n / 2] < 1.0f);
            }
        }
        std::printf("  the forecast itself takes %.1f us\n", forecastMs / shots * 1000.0);
    }

    // one shot printed step by step
    void ForecastTrace(float x, float y, float z, float vx, float vy, float vz)
    {
        RlSim sim(g_tuning, 0);
        sim.ResetBall({x, y, z}, {vx, vy, vz}, {});
        RlForecast forecast;
        RlForecastBall(g_tuning, sim.State().ball, 0, forecast);
        int tick = 0;
        for (int i = 1; i <= 12; ++i)
        {
            const float t = 0.25f * static_cast<float>(i);
            while (tick < static_cast<int>(t * 120.0f))
            {
                sim.Step(nullptr);
                ++tick;
            }
            const RlVec3 truth = sim.State().ball.position;
            const RlVec3 guess = RlForecastAt(forecast, 0, t);
            std::printf("  t %.2f  real (%.2f %.2f %.2f)  forecast (%.2f %.2f %.2f)  spin %.2f%s", static_cast<double>(t), static_cast<double>(truth.x), static_cast<double>(truth.y),
                        static_cast<double>(truth.z), static_cast<double>(guess.x), static_cast<double>(guess.y), static_cast<double>(guess.z),
                        static_cast<double>(std::sqrt(sim.State().ball.angularVelocity.x * sim.State().ball.angularVelocity.x + sim.State().ball.angularVelocity.z * sim.State().ball.angularVelocity.z)), "\n");
        }
    }

    // a team of bots against a team that stands still: how fast it scores, how often it touches the ball
    void AgainstIdle(Kind kind, int mode, float seconds, uint64_t seed)
    {
        RlMatchConfig config;
        config.mode = mode;
        config.seed = seed;
        config.botVersion = kind.version;
        config.botLevel = kind.level;
        uint32_t mask = 0;
        for (int i = mode; i < 2 * mode; ++i)
        {
            mask |= 1u << i;  // the orange cars are "human" with no input: they stand
        }
        config.humanMask = mask;
        RlMatch match(g_tuning, config);
        std::vector<RlCarInput> inputs(static_cast<size_t>(match.SlotCount()));
        int firstTouch = -1;
        int goalAt[16] = {};
        int goals = 0;
        double speedSum = 0.0;
        int speedN = 0;
        const int ticks = static_cast<int>(seconds * 120.0f);
        for (int t = 0; t < ticks; ++t)
        {
            match.Step(inputs.data());
            const RlWorldState& w = match.World();
            int touches = 0;
            for (int i = 0; i < match.SlotCount(); ++i)
            {
                touches += w.match.touches[i];
            }
            if (firstTouch < 0 && touches > 0)
            {
                firstTouch = t;
            }
            if (w.match.score[0] > goals && goals < 16)
            {
                goalAt[goals++] = t;
            }
            for (int i = 0; i < mode; ++i)
            {
                const RlVec3& v = w.cars[i].linearVelocity;
                speedSum += std::sqrt(v.x * v.x + v.z * v.z);
                ++speedN;
            }
            if (g_trace && t % 60 == 0 && t > 360 && t < 120 * 36)
            {
                const RlCarState& c = w.cars[0];
                std::printf("  t %5.1f  car (%6.1f %4.1f %6.1f) v %5.1f boost %3.0f %s%s  ball (%6.1f %4.1f %6.1f) v (%5.1f %5.1f %5.1f)  touches %d\n", t / 120.0, static_cast<double>(c.position.x),
                            static_cast<double>(c.position.y), static_cast<double>(c.position.z),
                            static_cast<double>(std::sqrt(c.linearVelocity.x * c.linearVelocity.x + c.linearVelocity.y * c.linearVelocity.y + c.linearVelocity.z * c.linearVelocity.z)),
                            static_cast<double>(c.boost), c.onGround ? "gnd" : "air", c.demolished ? " DEMO" : "", static_cast<double>(w.ball.position.x),
                            static_cast<double>(w.ball.position.y), static_cast<double>(w.ball.position.z), static_cast<double>(w.ball.linearVelocity.x),
                            static_cast<double>(w.ball.linearVelocity.y), static_cast<double>(w.ball.linearVelocity.z), w.match.touches[0]);
                if (mode > 1)
                {
                    const RlCarState& c1 = w.cars[1];
                    std::printf("        car1 (%6.1f %4.1f %6.1f) v %5.1f boost %3.0f\n", static_cast<double>(c1.position.x), static_cast<double>(c1.position.y), static_cast<double>(c1.position.z),
                                static_cast<double>(std::sqrt(c1.linearVelocity.x * c1.linearVelocity.x + c1.linearVelocity.z * c1.linearVelocity.z)), static_cast<double>(c1.boost));
                }
            }
        }
        const RlWorldState& w = match.World();
        int touches = 0;
        for (int i = 0; i < match.SlotCount(); ++i)
        {
            touches += w.match.touches[i];
        }
        std::printf("v%d L%d %dv%d vs idle, %.0f s: score %d:%d, touches %d, first touch at %.1f s, mean speed %.1f m/s, goals at", kind.version, kind.level, mode, mode,
                    static_cast<double>(seconds), w.match.score[0], w.match.score[1], touches, firstTouch < 0 ? -1.0 : firstTouch / 120.0, speedSum / std::max(speedN, 1));
        for (int i = 0; i < goals; ++i)
        {
            std::printf(" %.0f", goalAt[i] / 120.0);
        }
        std::printf(" s\n");
    }

    // high balls: the ball is put in the air near a bot, how often does the bot touch it within 6 s, and how high was it when it did
    void AerialTrials(Kind kind, int trials)
    {
        RlRng rng(77);
        int touched = 0;
        int touchedHigh = 0;
        double firstSum = 0.0;
        for (int trial = 0; trial < trials; ++trial)
        {
            RlMatchConfig config;
            config.mode = 1;
            config.seed = 500 + static_cast<uint64_t>(trial);
            config.botVersion = kind.version;
            config.botLevel = kind.level;
            config.humanMask = 2u;  // the orange car is a "human" that stands
            RlMatch match(g_tuning, config);
            std::vector<RlCarInput> inputs(2);
            for (int t = 0; t < 120 * 6; ++t)
            {
                match.Step(inputs.data());  // the countdown
            }
            match.MutableSim().SetCarBoost(0, 100.0f);
            // the car stands on the floor facing +Z with a full tank, the ball hangs high in front of it and falls
            const float carX = rng.Signed() * 6.0f;
            match.MutableSim().SetCarPose(0, {carX, 0.38f, -6.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f});
            match.MutableSim().ResetBall({carX + rng.Signed() * 3.0f, 7.0f + rng.Float01() * 2.0f, 8.0f + rng.Float01() * 4.0f},
                                         {rng.Signed() * 1.0f, 14.0f + rng.Float01() * 3.0f, 2.0f + rng.Float01() * 2.0f});
            const int before = match.World().match.touches[0];
            double first = -1.0;
            bool high = false;
            for (int t = 0; t < 120 * 6; ++t)
            {
                match.Step(inputs.data());
                if (g_trace && trial < 2 && t % 12 == 0)
                {
                    const RlCarState& c = match.World().cars[0];
                    const RlBallState& bl = match.World().ball;
                    std::printf("  trial %d t %.2f car (%.1f %.1f %.1f) v (%.1f %.1f %.1f) boost %.0f %s | ball (%.1f %.1f %.1f) v (%.1f %.1f %.1f)\n", trial, t / 120.0,
                                static_cast<double>(c.position.x), static_cast<double>(c.position.y), static_cast<double>(c.position.z), static_cast<double>(c.linearVelocity.x),
                                static_cast<double>(c.linearVelocity.y), static_cast<double>(c.linearVelocity.z), static_cast<double>(c.boost), c.onGround ? "gnd" : "AIR",
                                static_cast<double>(bl.position.x), static_cast<double>(bl.position.y), static_cast<double>(bl.position.z), static_cast<double>(bl.linearVelocity.x),
                                static_cast<double>(bl.linearVelocity.y), static_cast<double>(bl.linearVelocity.z));
                }
                if (first < 0.0 && match.World().match.touches[0] > before)
                {
                    first = t / 120.0;
                    high = match.World().ball.position.y > 2.8f;
                }
            }
            touched += first >= 0.0 ? 1 : 0;
            touchedHigh += high ? 1 : 0;
            firstSum += first >= 0.0 ? first : 0.0;
        }
        std::printf("v%d L%d: high balls touched %d of %d within 6 s (%d of them while the ball was above 2.8 m), first touch after %.1f s on average\n", kind.version,
                    kind.level, touched, trials, touchedHigh, firstSum / std::max(touched, 1));
    }

    Kind ParseKind(const char* text)
    {
        Kind k;
        if (std::strlen(text) >= 2)
        {
            k.version = text[0] - '0';
            k.level = text[1] - '0';
        }
        return k;
    }
}

int main(int argc, char** argv)
{
    std::string error;
    if (!g_tuning.LoadFromFile(RL_TUNING_JSON, &error))
    {
        std::printf("FAIL: cannot read %s: %s\n", RL_TUNING_JSON, error.c_str());
        return 1;
    }

    if (argc > 7 && std::strcmp(argv[1], "--trace") == 0)
    {
        ForecastTrace(static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])), static_cast<float>(std::atof(argv[4])),
                      static_cast<float>(std::atof(argv[5])), static_cast<float>(std::atof(argv[6])), static_cast<float>(std::atof(argv[7])));
        return 0;
    }
    if (argc > 2 && std::strcmp(argv[1], "--aerial") == 0)
    {
        g_trace = argc > 4;
        AerialTrials(ParseKind(argv[2]), argc > 3 ? std::atoi(argv[3]) : 20);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--owngoals") == 0)
    {
        g_trace = true;
        const int mode = argc > 2 ? std::atoi(argv[2]) : 2;
        const Kind a = argc > 4 ? ParseKind(argv[4]) : Kind{2, 2};
        const Kind b = argc > 5 ? ParseKind(argv[5]) : Kind{1, 1};
        Print("own goals", mode, a, b, Compare(mode, a, b, argc > 3 ? std::atoi(argv[3]) : 16));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--suite") == 0)
    {
        // the quick look: level 2 and level 1 of v2 against the bots of G4b in 2v2 and 4v4, 16 matches each
        const int matches = argc > 2 ? std::atoi(argv[2]) : 16;
        const int modes[2] = {2, 4};
        for (const int mode : modes)
        {
            const Series s22 = Compare(mode, Kind{2, 2}, Kind{1, 1}, matches);
            const Series s21 = Compare(mode, Kind{2, 1}, Kind{1, 1}, matches);
            const Series s20 = Compare(mode, Kind{2, 2}, Kind{2, 0}, matches);
            std::printf("%dv%d: v2L2-v1L1 %3.0f%% (goals %.1f:%.1f, own %.2f)   v2L1-v1L1 %3.0f%% (goals %.1f:%.1f)   v2L2-v2L0 %3.0f%% (goals %.1f:%.1f)   tick %.3f ms\n", mode, mode,
                        s22.WinRate() * 100.0, s22.goalsA, s22.goalsB, s22.ownA, s21.WinRate() * 100.0, s21.goalsA, s21.goalsB, s20.WinRate() * 100.0, s20.goalsA,
                        s20.goalsB, s22.msPerTick);
        }
        return 0;
    }
    if (argc > 4 && std::strcmp(argv[1], "--idle") == 0)
    {
        g_trace = argc > 6;
        AgainstIdle(ParseKind(argv[2]), std::atoi(argv[3]), static_cast<float>(std::atof(argv[4])), argc > 5 ? static_cast<uint64_t>(std::atoi(argv[5])) : 1u);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--forecast") == 0)
    {
        TestForecast();
        return g_failures == 0 ? 0 : 1;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--vs") == 0)
    {
        const Kind a = ParseKind(argv[2]);
        const Kind b = ParseKind(argv[3]);
        const int mode = argc > 4 ? std::atoi(argv[4]) : 2;
        const int matches = argc > 5 ? std::atoi(argv[5]) : 10;
        Print("series", mode, a, b, Compare(mode, a, b, matches));
        return 0;
    }
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;

    const Kind v2L0{2, 0};
    const Kind v2L1{2, 1};
    const Kind v2L2{2, 2};
    const Kind v1L1{1, 1};

    if (quick)
    {
        Print("v2 level 2 against v2 level 0", 2, v2L2, v2L0, Compare(2, v2L2, v2L0, 4));
        return 0;
    }

    TestForecast();

    const int modes[2] = {2, 4};
    for (const int mode : modes)
    {
        const Series strong = Compare(mode, v2L2, v2L0, 10);
        Print("level 2 against level 0", mode, v2L2, v2L0, strong);
        CHECK(strong.WinRate() >= 0.80);
        CHECK(strong.longestStuck < 10.0);

        const Series upgrade = Compare(mode, v2L1, v1L1, 10);
        Print("v2 against the bots of G4b (both level 1)", mode, v2L1, v1L1, upgrade);
        CHECK(upgrade.WinRate() >= 0.70);

        const Series best = Compare(mode, v2L2, v1L1, 10);
        Print("v2 level 2 against the bots of G4b (level 1)", mode, v2L2, v1L1, best);
        CHECK(best.WinRate() >= 0.70);

        // few own goals: the team of v2 does not score into its own net more often than it concedes by the opponent's work
        CHECK(strong.ownA <= 0.5);
        // the cost of the thinking of eight bots
        CHECK(strong.msPerTick <= 0.5);
    }

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "OK" : "FAILED", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
