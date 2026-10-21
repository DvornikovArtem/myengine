// RlBots.h
// The bots of the match: a port of the heuristics of the old Python RL (roles every 0.3 s, a ball forecast, a spot between the ball and
// the own goal, boost pads when short of boost, jumps and dodges into the ball) on the new physics. A bot gives an RlCarInput exactly
// like a player; the car does the rest. Everything it remembers is in RlBotMemory (plain data, in the snapshot of the match).

#pragma once

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
        uint8_t jumpPrev = 0;
        uint8_t pad[3] = {};
    };

    struct RlBotContext
    {
        const RlTuning& tuning;
        const RlWorldState& world;
        int slot;
        int team;
        int teamSize;
        int level;  // 0..2
    };

    // The roles of one team, called every 0.3 s: the car closest to the ball attacks, the one nearest the own goal defends.
    void RlBotsAssignRoles(const RlTuning& tuning, const RlWorldState& world, int team, int teamSize, RlBotMemory* memories);

    // The input of one bot for this tick.
    RlCarInput RlBotThink(const RlBotContext& context, RlBotMemory& memory, RlRng& rng);

    // Where the ball will be in `seconds` (gravity, floor, walls and ceiling, no cars).
    RlVec3 RlPredictBall(const RlTuning& tuning, const RlBallState& ball, float seconds);
}
