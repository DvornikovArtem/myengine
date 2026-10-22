// RlMatch.cpp

#include <rl/RlMatch.h>
#include <rl/RlName.h>

#include "RlBots.h"
#include "RlBotsV1.h"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <cstring>
#include <type_traits>

namespace rl
{
    namespace
    {
        constexpr float kPi = 3.14159265f;
        constexpr uint32_t kMatchMagic = 0x314D4C52;  // "RLM1"
        constexpr float kRoleInterval = 0.3f;
        constexpr float kRoleInterval2 = 0.2f;

        // RLBot field info, uu: kick-off spots of blue (orange is the same turned by half a turn)
        struct SpotUu
        {
            float x;
            float y;
            float yawPi;  // RL yaw / pi, counter-clockwise from +x
        };
        constexpr SpotUu kKickoffUu[5] = {
            {-2048.0f, -2560.0f, 0.25f},  // right corner
            {2048.0f, -2560.0f, 0.75f},   // left corner
            {-256.0f, -3840.0f, 0.5f},    // back right
            {256.0f, -3840.0f, 0.5f},     // back left
            {0.0f, -4608.0f, 0.5f},       // far back centre
        };
        constexpr SpotUu kRespawnUu[4] = {
            {-2304.0f, -4608.0f, 0.5f},  // right inside
            {-2688.0f, -4608.0f, 0.5f},  // right outside
            {2304.0f, -4608.0f, 0.5f},   // left inside
            {2688.0f, -4608.0f, 0.5f},   // left outside
        };

        RlSpot ToSpot(const SpotUu& s, int team)
        {
            // RL: (x, y) on the floor, yaw from +x towards +y. Here: z = y, yaw from +z towards +x, which is pi/2 - yaw.
            float x = s.x * 0.01f;
            float z = s.y * 0.01f;
            float yaw = 0.5f * kPi - s.yawPi * kPi;
            if (team != 0)
            {
                x = -x;
                z = -z;
                yaw += kPi;
            }
            return {x, z, yaw};
        }

        // RLBot index order, uu (x, y); the big pads are 3, 4, 15, 18, 29, 30
        constexpr float kPadsUu[kPadCount][2] = {
            {0, -4240},     {-1792, -4184}, {1792, -4184},  {-3072, -4096}, {3072, -4096}, {-940, -3308}, {940, -3308},  {0, -2816},   {-3584, -2484},
            {3584, -2484},  {-1788, -2300}, {1788, -2300},  {-2048, -1036}, {0, -1024},    {2048, -1036}, {-3584, 0},    {-1024, 0},   {1024, 0},
            {3584, 0},      {-2048, 1036},  {0, 1024},      {2048, 1036},   {-1788, 2300}, {1788, 2300},  {-3584, 2484}, {3584, 2484}, {0, 2816},
            {-940, 3310},   {940, 3308},    {-3072, 4096},  {3072, 4096},   {-1792, 4184}, {1792, 4184},  {0, 4240},
        };

        bool IsBigPad(int index) { return index == 3 || index == 4 || index == 15 || index == 18 || index == 29 || index == 30; }
    }

    bool RlBallHeadsIntoGoal(const RlTuning& tuning, const RlBallState& ball, int goalSide, float horizon)
    {
        const float line = tuning.arena.halfLength + tuning.ball.radius;
        const float step = 1.0f / 60.0f;
        float x = ball.position.x, y = ball.position.y, z = ball.position.z;
        float vx = ball.linearVelocity.x, vy = ball.linearVelocity.y, vz = ball.linearVelocity.z;
        for (float t = 0.0f; t < horizon; t += step)
        {
            vy -= tuning.gravity * step;
            const float lastZ = z;
            x += vx * step;
            y += vy * step;
            z += vz * step;
            if (y < tuning.ball.radius && vy < 0.0f)
            {
                y = tuning.ball.radius;
                vy = -vy * tuning.ball.restitution;
            }
            const float side = static_cast<float>(goalSide);
            if (lastZ * side < line && z * side >= line)
            {
                return std::fabs(x) < tuning.arena.goalHalfWidth && y < tuning.arena.goalHeight;
            }
        }
        return false;
    }

    RlSpot RlKickoffSpot(int team, int order) { return ToSpot(kKickoffUu[std::clamp(order, 0, 4)], team); }
    RlSpot RlRespawnSpot(int team, int order) { return ToSpot(kRespawnUu[std::clamp(order, 0, 3)], team); }

    bool RlPadSpot(int index, float& x, float& z)
    {
        index = std::clamp(index, 0, kPadCount - 1);
        x = kPadsUu[index][0] * 0.01f;
        z = kPadsUu[index][1] * 0.01f;
        return IsBigPad(index);
    }

    namespace
    {
        constexpr float kWedgedSeconds = 7.0f;
        constexpr int kTouchGapTicks = 60;      // a touch counts once in half a second
        constexpr int kAssistTicks = 600;       // an assist: the touch of a mate at most 5 s before the goal
        constexpr int kPointsGoal = 100;
        constexpr int kPointsAssist = 50;
        constexpr int kPointsSave = 50;
        constexpr int kPointsShot = 20;
        constexpr int kPointsDemo = 25;
        constexpr int kPointsTouch = 2;

        // The part of the match that lives outside RlSim and outside RlWorldState: plain data for the snapshot.
        struct MatchRuntime
        {
            RlRng rng;
            RlMatchState match;
            RlBotMemory bots[kMaxCars];
            v1::RlBotMemory botsV1[kMaxCars];
            RlBotTeamPlan plans[2];
            float roleTimer[2];
            int32_t wasDemolished[kMaxCars];
            float stillSeconds[kMaxCars];  // how long a bot has not moved (the rescue of a wedged bot)
            int32_t lastTouchTick[kMaxCars];      // the tick of the last contact of the car with the ball (assists)
            int32_t countedTouchTick[kMaxCars];   // the tick of the last touch that was counted (a touch counts once in half a second)
        };
        static_assert(std::is_trivially_copyable_v<MatchRuntime>, "the match runtime is copied as bytes");
        static_assert(std::is_trivially_copyable_v<RlRng>, "the generator is copied as bytes");
    }

    struct RlMatch::Impl
    {
        RlTuning tuning;
        RlMatchConfig config;
        int slots = 0;
        int teamSize = 0;
        std::unique_ptr<RlSim> sim;
        MatchRuntime rt;
        RlWorldState world;
        RlWorldState previous;
        RlEvents events;
        RlForecast forecast;           // made again from the world at every tick when a v2 bot needs it: not part of the state
        int64_t forecastTick = -1;

        Impl(const RlTuning& t, const RlMatchConfig& c)
            : tuning(t)
            , config(c)
        {
            std::memset(&rt, 0, sizeof(rt));
            rt.rng = RlRng(c.seed);
            config.mode = std::clamp(c.mode, 0, 4);
            teamSize = std::max(config.mode, 1);
            slots = config.mode == 0 ? 1 : 2 * config.mode;
            sim = std::make_unique<RlSim>(tuning, slots);
            for (int slot = 0; slot < slots; ++slot)
            {
                sim->SetCarTeam(slot, TeamOf(slot));  // cars of one team bump each other, only the other team is demolished
            }
            for (int i = 0; i < kMaxCars; ++i)
            {
                rt.bots[i] = RlBotMemory{};
                rt.botsV1[i] = v1::RlBotMemory{};
                rt.wasDemolished[i] = 0;
            }
            rt.roleTimer[0] = rt.roleTimer[1] = 0.0f;
            for (int i = 0; i < kMaxCars; ++i)
            {
                rt.lastTouchTick[i] = -100000;
                rt.countedTouchTick[i] = -100000;
            }
            rt.match = RlMatchState{};
            rt.match.mode = config.mode;
            SetNames();
            rt.match.clock = config.mode == 0 ? 0.0f : tuning.match.matchSeconds;
            BeginKickoff();
            RefreshWorld();
            previous = world;
        }

        int TeamOf(int slot) const { return config.mode == 0 ? 0 : (slot < teamSize ? 0 : 1); }
        bool IsBot(int slot) const { return (config.humanMask & (1u << slot)) == 0; }
        int BotVersionOf(int team) const { return team == 1 && config.botVersionOrange >= 0 ? config.botVersionOrange : config.botVersion; }
        int BotLevelOf(int team) const { return team == 1 && config.botLevelOrange >= 0 ? config.botLevelOrange : config.botLevel; }

        // The forecast of the ball for this tick (a pure function of the ball, see RlForecastBall)
        void RefreshForecast(bool needed)
        {
            if (!needed || forecastTick == static_cast<int64_t>(world.tick))
            {
                return;
            }
            ZoneScopedN("Rl::BallForecast");
            RlForecastBall(tuning, world.ball, world.tick, forecast);
            forecastTick = static_cast<int64_t>(world.tick);
        }

        void RefreshWorld()
        {
            world = sim->State();
            world.match = rt.match;
        }

        // The names of the table: the given ones, "Player" for a human and the names of the bots in the order of the bot slots.
        void SetNames()
        {
            static const char* const kBotNames[] = {"Comet", "Blaze", "Nova", "Spark", "Bolt", "Drift", "Flux"};
            int bots = 0;
            for (int slot = 0; slot < slots; ++slot)
            {
                std::string name = SanitizePlayerName(config.names[slot]);
                if (name.empty())
                {
                    name = IsBot(slot) ? kBotNames[bots % 7] : "Player";
                }
                if (IsBot(slot))
                {
                    ++bots;
                }
                size_t length = std::min(name.size(), static_cast<size_t>(kNameBytes - 1));
                while (length > 0 && length < name.size() && (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80)
                {
                    --length;  // not in the middle of a character
                }
                std::memset(rt.match.names[slot], 0, kNameBytes);
                std::memcpy(rt.match.names[slot], name.data(), length);
            }
        }

        void PushEvent(RlMatchEventType type, int slot, int other, uint32_t tick)
        {
            RlMatchState& m = rt.match;
            RlMatchEvent& e = m.events[static_cast<uint32_t>(m.eventHead) % kEventRing];
            e.tick = tick;
            e.type = static_cast<int32_t>(type);
            e.slot = slot;
            e.other = other;
            ++m.eventHead;
        }

        // The touch of a car: the points, the shot on goal and the save (the rules of docs/game/rl-match.md).
        void OnTouch(int slot, const RlBallState& before, const RlBallState& after, uint32_t tick)
        {
            RlMatchState& m = rt.match;
            const int now = static_cast<int>(tick);
            rt.lastTouchTick[slot] = now;
            if (now - rt.countedTouchTick[slot] < kTouchGapTicks)
            {
                return;
            }
            rt.countedTouchTick[slot] = now;
            ++m.touches[slot];
            m.points[slot] += kPointsTouch;
            if (config.mode == 0)
            {
                return;
            }
            const int team = TeamOf(slot);
            const int attacked = team == 0 ? 1 : -1;  // the side of the goal this team shoots at
            if (RlBallHeadsIntoGoal(tuning, after, attacked) && !RlBallHeadsIntoGoal(tuning, before, attacked))
            {
                ++m.shots[slot];
                m.points[slot] += kPointsShot;
                PushEvent(RlMatchEventType::Shot, slot, -1, tick);
            }
            if (RlBallHeadsIntoGoal(tuning, before, -attacked) && !RlBallHeadsIntoGoal(tuning, after, -attacked))
            {
                ++m.saves[slot];
                m.points[slot] += kPointsSave;
                PushEvent(RlMatchEventType::Save, slot, -1, tick);
            }
        }

        void BeginKickoff()
        {
            RlMatchState& m = rt.match;
            m.phase = RlMatchPhase::Countdown;
            m.phaseTimer = tuning.match.countdownSeconds;
            ++m.kickoffs;
            for (RlPadState& pad : m.pads)
            {
                pad.respawnLeft = 0.0f;
            }
            int order[4] = {0, 1, 2, 3};
            int single = 0;
            if (config.mode == 1)
            {
                single = rt.rng.Range(5);
            }
            else if (config.mode == 3)
            {
                order[2] = 2 + rt.rng.Range(2);
            }
            for (int slot = 0; slot < slots; ++slot)
            {
                const int team = TeamOf(slot);
                const int index = config.mode == 0 ? 0 : slot % teamSize;
                const RlSpot spot = RlKickoffSpot(team, config.mode == 1 ? single : order[index]);
                sim->ResetCar(slot, spot.x, spot.z, spot.yaw);
                const RlSpot back = RlRespawnSpot(team, rt.rng.Range(4));
                sim->SetRespawnPoint(slot, back.x, back.z, back.yaw);
                rt.wasDemolished[slot] = 0;
                rt.bots[slot].decisionTimer = 0.0f;
                rt.bots[slot].recoverStage = 0;
                rt.bots[slot].stuckTimer = 0.0f;
                rt.bots[slot].speedFlip = 0;
                rt.bots[slot].aerialMode = 0;
                rt.bots[slot].kickoffMode = 0;
                rt.bots[slot].dodgeCooldown = 0.0f;
                rt.botsV1[slot].decisionTimer = 0.0f;
                rt.botsV1[slot].recoverStage = 0;
                rt.botsV1[slot].stuckTimer = 0.0f;
            }
            sim->ResetBall({0.0f, tuning.ball.radius + 0.05f, 0.0f});
            forecastTick = -1;
        }

        void Step(const RlCarInput* inputs)
        {
            ZoneScopedN("Rl::Match");
            previous = world;
            events = {};
            RlMatchState& m = rt.match;
            const bool live = m.phase == RlMatchPhase::Playing || m.phase == RlMatchPhase::GoalScored;

            RlCarInput effective[kMaxCars];
            if (live)
            {
                ZoneScopedN("Rl::Bots");
                bool needsForecast = false;
                for (int slot = 0; slot < slots; ++slot)
                {
                    needsForecast = needsForecast || (IsBot(slot) && BotVersionOf(TeamOf(slot)) >= 2);
                }
                RefreshForecast(needsForecast);
                for (int team = 0; team < 2 && config.mode != 0; ++team)
                {
                    rt.roleTimer[team] -= kTickDt;
                    if (rt.roleTimer[team] <= 0.0f)
                    {
                        rt.roleTimer[team] = BotVersionOf(team) >= 2 ? kRoleInterval2 : kRoleInterval;
                        if (BotVersionOf(team) >= 2)
                        {
                            RlBotsPlan(tuning, world, forecast, team, teamSize, BotLevelOf(team), rt.plans[team], rt.bots);
                        }
                        else
                        {
                            v1::RlBotsAssignRoles(tuning, world, team, teamSize, rt.botsV1);
                        }
                    }
                }
                for (int slot = 0; slot < slots; ++slot)
                {
                    if (IsBot(slot))
                    {
                        const int team = TeamOf(slot);
                        if (BotVersionOf(team) >= 2)
                        {
                            const RlBotContext context{tuning, world, forecast, rt.plans[team], slot, team, teamSize, BotLevelOf(team)};
                            effective[slot] = RlBotThink(context, rt.bots[slot], rt.rng);
                        }
                        else
                        {
                            const v1::RlBotContext context{tuning, world, slot, team, teamSize, BotLevelOf(team)};
                            effective[slot] = v1::RlBotThink(context, rt.botsV1[slot], rt.rng);
                        }
                    }
                    else
                    {
                        effective[slot] = inputs[slot];
                    }
                }
            }
            sim->Step(effective);
            events = sim->LastEvents();
            if (m.phase == RlMatchPhase::Playing)
            {
                if (events.ballHits > 0 && sim->State().lastBallToucher >= 0)
                {
                    OnTouch(sim->State().lastBallToucher, world.ball, sim->State().ball, sim->State().tick);
                }
                for (int i = 0; i < std::min(events.demolitions, kMaxCars); ++i)
                {
                    const int by = events.demoBy[i];
                    if (by >= 0 && by < slots)
                    {
                        ++m.demos[by];
                        m.points[by] += kPointsDemo;
                        PushEvent(RlMatchEventType::Demo, by, events.demoVictim[i], sim->State().tick);
                    }
                }
            }

            // a car that was just demolished comes back at one of the respawn spots of its team
            const RlWorldState& now = sim->State();
            for (int slot = 0; slot < slots; ++slot)
            {
                const bool down = now.cars[slot].demolished;
                if (down && rt.wasDemolished[slot] == 0)
                {
                    const RlSpot spot = RlRespawnSpot(TeamOf(slot), rt.rng.Range(4));
                    sim->SetRespawnPoint(slot, spot.x, spot.z, spot.yaw);
                }
                rt.wasDemolished[slot] = down ? 1 : 0;
            }

            // A bot that has been wedged for a while (on a post of the goal, between a ramp and a wall) is put back on a respawn spot of its
            // team: a safeguard for the bots, nothing of the rules (a player gets out of such a place with a flip or leaves the match).
            if (m.phase == RlMatchPhase::Playing)
            {
                for (int slot = 0; slot < slots; ++slot)
                {
                    const RlCarState& car = now.cars[slot];
                    const float speed = std::sqrt(car.linearVelocity.x * car.linearVelocity.x + car.linearVelocity.y * car.linearVelocity.y +
                                                  car.linearVelocity.z * car.linearVelocity.z);
                    rt.stillSeconds[slot] = (IsBot(slot) && !car.demolished && speed < 0.5f) ? rt.stillSeconds[slot] + kTickDt : 0.0f;
                    if (rt.stillSeconds[slot] > kWedgedSeconds)
                    {
                        rt.stillSeconds[slot] = 0.0f;
                        const RlSpot spot = RlRespawnSpot(TeamOf(slot), rt.rng.Range(4));
                        const float boost = car.boost;
                        sim->ResetCar(slot, spot.x, spot.z, spot.yaw);
                        sim->SetCarBoost(slot, boost);
                    }
                }
            }

            if (live)
            {
                UpdatePads();
            }

            const RlWorldState& state = sim->State();
            switch (m.phase)
            {
            case RlMatchPhase::Countdown:
                m.phaseTimer -= kTickDt;
                if (m.phaseTimer <= 0.0f)
                {
                    m.phase = RlMatchPhase::Playing;
                    m.phaseTimer = 0.0f;
                }
                break;
            case RlMatchPhase::Playing:
                if (!CheckGoal(state) && config.mode != 0)
                {
                    if (m.overtime != 0)
                    {
                        m.clock += kTickDt;
                    }
                    else
                    {
                        m.clock -= kTickDt;
                        if (m.clock <= 0.0f)
                        {
                            m.clock = 0.0f;
                            if (m.score[0] == m.score[1])
                            {
                                m.overtime = 1;
                            }
                            else
                            {
                                Finish();
                            }
                        }
                    }
                }
                break;
            case RlMatchPhase::GoalScored:
                m.phaseTimer -= kTickDt;
                if (m.phaseTimer <= 0.0f)
                {
                    if (config.mode != 0 && m.overtime != 0)
                    {
                        Finish();
                    }
                    else if (config.mode != 0 && m.clock <= 0.0f)
                    {
                        if (m.score[0] == m.score[1])
                        {
                            m.overtime = 1;
                            BeginKickoff();
                        }
                        else
                        {
                            Finish();
                        }
                    }
                    else
                    {
                        BeginKickoff();
                    }
                }
                break;
            case RlMatchPhase::Finished:
                break;
            }
            RefreshWorld();
        }

        void Finish()
        {
            RlMatchState& m = rt.match;
            m.phase = RlMatchPhase::Finished;
            m.winner = m.score[0] > m.score[1] ? 0 : 1;
            m.phaseTimer = 0.0f;
        }

        // The ball is a goal when its centre is a radius past the goal line inside the posts and under the bar.
        bool CheckGoal(const RlWorldState& state)
        {
            const RlVec3 p = state.ball.position;
            const float line = tuning.arena.halfLength + tuning.ball.radius + tuning.match.goalLineMargin;
            if (std::fabs(p.z) > line && std::fabs(p.x) < tuning.arena.goalHalfWidth && p.y < tuning.arena.goalHeight)
            {
                RlMatchState& m = rt.match;
                const int team = p.z > 0.0f ? 0 : 1;  // blue attacks +Z
                ++m.score[team];
                m.lastGoalTeam = team;
                m.lastGoalScorer = state.lastBallToucher;
                m.lastGoalOwn = (state.lastBallToucher >= 0 && TeamOf(state.lastBallToucher) != team) ? 1 : 0;
                const RlVec3 v = state.ball.linearVelocity;
                m.lastGoalSpeed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                m.lastGoalAssist = -1;
                const int scorer = state.lastBallToucher;
                if (scorer >= 0 && m.lastGoalOwn == 0)
                {
                    ++m.goals[scorer];
                    m.points[scorer] += kPointsGoal;
                    // the assist: the last touch of a mate before the touch of the scorer, at most 5 s before the goal
                    int assist = -1;
                    for (int slot = 0; slot < slots; ++slot)
                    {
                        if (slot == scorer || TeamOf(slot) != team)
                        {
                            continue;
                        }
                        const int touched = rt.lastTouchTick[slot];
                        if (touched < rt.lastTouchTick[scorer] && static_cast<int>(state.tick) - touched <= kAssistTicks &&
                            (assist < 0 || touched > rt.lastTouchTick[assist]))
                        {
                            assist = slot;
                        }
                    }
                    m.lastGoalAssist = assist;
                    PushEvent(RlMatchEventType::Goal, scorer, assist, state.tick);
                    if (assist >= 0)
                    {
                        ++m.assists[assist];
                        m.points[assist] += kPointsAssist;
                        PushEvent(RlMatchEventType::Assist, assist, scorer, state.tick);
                    }
                }
                else if (scorer >= 0)
                {
                    PushEvent(RlMatchEventType::OwnGoal, scorer, -1, state.tick);
                }
                else
                {
                    PushEvent(RlMatchEventType::Goal, -1, -1, state.tick);
                }
                m.phase = RlMatchPhase::GoalScored;
                m.phaseTimer = tuning.match.goalSeconds;
                return true;
            }
            return false;
        }

        void UpdatePads()
        {
            RlMatchState& m = rt.match;
            for (int i = 0; i < kPadCount; ++i)
            {
                RlPadState& pad = m.pads[i];
                if (pad.respawnLeft > 0.0f)
                {
                    pad.respawnLeft = std::max(pad.respawnLeft - kTickDt, 0.0f);
                    continue;
                }
                float px;
                float pz;
                const bool big = RlPadSpot(i, px, pz);
                const float radius = big ? tuning.match.padBigRadius : tuning.match.padSmallRadius;
                for (int slot = 0; slot < slots; ++slot)
                {
                    const RlCarState& car = sim->State().cars[slot];
                    if (car.demolished || car.position.y > tuning.match.padMaxHeight || car.boost >= tuning.car.boostMax - 0.001f)
                    {
                        continue;
                    }
                    const float dx = car.position.x - px;
                    const float dz = car.position.z - pz;
                    if (dx * dx + dz * dz <= radius * radius)
                    {
                        const float gain = big ? tuning.match.padBigBoost : tuning.match.padSmallBoost;
                        sim->SetCarBoost(slot, std::min(car.boost + gain, tuning.car.boostMax));
                        pad.respawnLeft = big ? tuning.match.padBigRespawn : tuning.match.padSmallRespawn;
                        break;
                    }
                }
            }
        }

        struct SnapshotHeader
        {
            uint32_t magic;
            uint32_t simBytes;
        };

        void Save(std::vector<uint8_t>& out) const
        {
            std::vector<uint8_t> simBytes;
            sim->SaveState(simBytes);
            out.clear();
            const SnapshotHeader header{kMatchMagic, static_cast<uint32_t>(simBytes.size())};
            const auto* h = reinterpret_cast<const uint8_t*>(&header);
            out.insert(out.end(), h, h + sizeof(header));
            out.insert(out.end(), simBytes.begin(), simBytes.end());
            const auto* r = reinterpret_cast<const uint8_t*>(&rt);
            out.insert(out.end(), r, r + sizeof(MatchRuntime));
        }

        bool Restore(const std::vector<uint8_t>& in)
        {
            if (in.size() < sizeof(SnapshotHeader))
            {
                return false;
            }
            SnapshotHeader header{};
            std::memcpy(&header, in.data(), sizeof(header));
            if (header.magic != kMatchMagic || in.size() != sizeof(SnapshotHeader) + header.simBytes + sizeof(MatchRuntime))
            {
                return false;
            }
            const std::vector<uint8_t> simBytes(in.begin() + sizeof(SnapshotHeader), in.begin() + sizeof(SnapshotHeader) + header.simBytes);
            if (!sim->RestoreState(simBytes))
            {
                return false;
            }
            std::memcpy(&rt, in.data() + sizeof(SnapshotHeader) + header.simBytes, sizeof(MatchRuntime));
            RefreshWorld();
            previous = world;
            events = {};
            forecastTick = -1;
            return true;
        }
    };

    RlMatch::RlMatch(const RlTuning& tuning, const RlMatchConfig& config)
        : impl_(std::make_unique<Impl>(tuning, config))
    {
    }

    RlMatch::~RlMatch() = default;

    int RlMatch::SlotCount() const { return impl_->slots; }
    int RlMatch::Team(int slot) const { return impl_->TeamOf(slot); }
    bool RlMatch::IsBot(int slot) const { return impl_->IsBot(slot); }
    uint32_t RlMatch::HumanMask() const { return impl_->config.humanMask; }
    void RlMatch::SetHumanMask(uint32_t mask) { impl_->config.humanMask = mask; }

    void RlMatch::SetStatistics(const RlMatchState& from)
    {
        RlMatchState* targets[2] = {&impl_->rt.match, &impl_->world.match};
        for (RlMatchState* target : targets)
        {
            std::memcpy(target->goals, from.goals, sizeof(from.goals));
            std::memcpy(target->points, from.points, sizeof(from.points));
            std::memcpy(target->assists, from.assists, sizeof(from.assists));
            std::memcpy(target->saves, from.saves, sizeof(from.saves));
            std::memcpy(target->shots, from.shots, sizeof(from.shots));
            std::memcpy(target->demos, from.demos, sizeof(from.demos));
            target->lastGoalSpeed = from.lastGoalSpeed;
            target->lastGoalAssist = from.lastGoalAssist;
        }
    }

    void RlMatch::SetPlayerNames(const std::string* names)
    {
        for (int slot = 0; slot < kMaxCars; ++slot)
        {
            impl_->config.names[slot] = names[slot];
        }
        impl_->SetNames();
        std::memcpy(impl_->world.match.names, impl_->rt.match.names, sizeof(impl_->rt.match.names));
        std::memcpy(impl_->previous.match.names, impl_->rt.match.names, sizeof(impl_->rt.match.names));
    }
    const RlTuning& RlMatch::Tuning() const { return impl_->tuning; }

    void RlMatch::SetTuning(const RlTuning& tuning)
    {
        impl_->sim->SetTuning(tuning);
        impl_->tuning = impl_->sim->Tuning();  // with the numbers that stay fixed put back
        impl_->forecastTick = -1;
    }
    const RlSim& RlMatch::Sim() const { return *impl_->sim; }
    RlSim& RlMatch::MutableSim() { return *impl_->sim; }
    void RlMatch::Step(const RlCarInput* inputs) { impl_->Step(inputs); }
    const RlWorldState& RlMatch::World() const { return impl_->world; }
    const RlWorldState& RlMatch::PreviousWorld() const { return impl_->previous; }
    const RlEvents& RlMatch::LastEvents() const { return impl_->events; }
    void RlMatch::SaveState(std::vector<uint8_t>& out) const { impl_->Save(out); }
    bool RlMatch::RestoreState(const std::vector<uint8_t>& in) { return impl_->Restore(in); }
}
