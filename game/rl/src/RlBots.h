// RlBots.h
// The bots of the match (G4f, "v2"): they look at a forecast of the ball (the match plays the ball out on its own ghost world, bounces off
// the real arena), find where and when they can meet it, drive there so that the hit sends the ball towards the other goal, rotate as a
// team (the first car attacks, the second supports, the others defend and fetch boost) and keep clear of the own goal. A bot gives an
// RlCarInput exactly like a player; the car does the rest. Everything a bot remembers is in RlBotMemory and RlBotTeamPlan, everything it
// is told about the ball in RlForecast: plain data, in the snapshot of the match. The bots of G4b live on in RlBotsV1.h.

#pragma once

#include <rl/RlBotForecast.h>
#include <rl/RlRng.h>
#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <cstdint>

namespace rl
{
    enum RlBotRole : int32_t
    {
        BotAttacker = 0,
        BotDefender = 1,
        BotSupport = 2,
        BotBoost = 3,
    };

    struct RlBotMemory
    {
        int32_t role = BotAttacker;        // assigned by RlBotsAssignRoles
        int32_t activeRole = BotAttacker;  // what the last decision made of it (a defender in danger attacks, a support short of boost fetches)
        int32_t stopAtTarget = 0;
        int32_t flipIntent = 0;      // decided with the target: flip into a ball on the ground
        int32_t recoverStage = 0;    // 0 none, 1 reversing, 2 flipping off the roof, 3 hop to get free, 4 flipping in the air after it
        float decisionTimer = 0.0f;
        float targetX = 0.0f;
        float targetZ = 0.0f;
        float aimError = 0.0f;       // radians added to the aim until the next decision (the easy levels are inexact)
        float stuckTimer = 0.0f;
        float recoverTimer = 0.0f;
        float recoverSteer = 0.0f;
        float wallTimer = 0.0f;
        float jumpHold = 0.0f;
        float flipWait = 0.0f;
        float patrolSide = 1.0f;     // -1/+1: the side a waiting bot shifts to, so it does not stand for ever
        float standTimer = 0.0f;
        float arriveTimer = 0.0f;    // > 0: the ball is in the air and lands in this many seconds; the car times its arrival
        float dodgeCooldown = 0.0f;
        float hitDirX = 0.0f;        // the direction the ball should go off the car (flat, unit), set with the target
        float hitDirZ = 0.0f;
        float contactTime = -1.0f;   // seconds from the decision to the meeting with the ball, -1 when there is none
        int32_t kickoffMode = 0;     // the car takes the kick-off
        int32_t speedFlip = 0;       // a flip in the air for speed is on its way
        int32_t aerialMode = 0;      // 0 none, 1 driving under the ball that will be high, 2 in the air after it
        float aerialClock = 0.0f;    // seconds until the planned contact (counts down, may go below 0)
        float aerialY = 0.0f;        // height of the planned contact
        uint8_t jumpPrev = 0;
        uint8_t pad[3] = {};
    };

    // What a team decided for all its cars (the roles come from here, every 0.2 s)
    struct RlBotTeamPlan
    {
        int32_t role[kMaxCars] = {};
        float timer = 0.0f;
        int32_t kickoffId = -1;       // the kick-off (RlMatchState::kickoffs) the choice below is for
        int32_t kickoffTaker = -1;    // slot of the car that goes for the ball at a kick-off
        float pad[2] = {};
    };

    struct RlBotContext
    {
        const RlTuning& tuning;
        const RlWorldState& world;
        const RlForecast& forecast;
        const RlBotTeamPlan& plan;
        int slot;
        int team;
        int teamSize;
        int level;  // 0..2
    };

    // The plan of one team, called every 0.2 s: who attacks, who supports, who defends, who fetches boost, who takes the kick-off.
    void RlBotsPlan(const RlTuning& tuning, const RlWorldState& world, const RlForecast& forecast, int team, int teamSize, int level, RlBotTeamPlan& plan,
                    RlBotMemory* memories);

    // The input of one bot for this tick.
    RlCarInput RlBotThink(const RlBotContext& context, RlBotMemory& memory, RlRng& rng);

    // Where the ball will be in `seconds` (gravity, floor, walls and ceiling, no cars).
    RlVec3 RlPredictBall(const RlTuning& tuning, const RlBallState& ball, float seconds);
}
