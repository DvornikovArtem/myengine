// RlBotsBrain.h
// The decisions of the bots of G4f (private to the library): who does what (the plan of a team), and where a bot drives and why (its target,
// the direction of the hit, the time of the contact). RlBots.cpp turns a decision into the sticks of the car.

#pragma once

#include "RlBots.h"

namespace rl::brain
{
    struct Level
    {
        float interval;      // seconds between two decisions
        float aimError;      // radians of random error in the aim
        float horizon;       // seconds of forecast the bot looks into when it looks for the ball
        float heightMax;     // highest ball (centre, m) it plays from the ground
        bool boostFreely;
        bool dodges;
        bool airControl;
        bool kickoffFlip;    // a flip on the way to the ball at a kick-off (speed)
        bool aerial;
    };

    Level LevelOf(int level);

    // where a car meets the ball (the first moment of the forecast at which it can be there)
    struct Intercept
    {
        bool ok = false;
        float time = 0.0f;   // seconds from now
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    Intercept FindIntercept(const RlTuning& tuning, const RlForecast& forecast, const RlCarState& car, const Level& level);
    // a high ball (3 to 7.5 m) that the car can reach by driving under it and jumping with the boost on
    Intercept FindAerial(const RlTuning& tuning, const RlForecast& forecast, const RlCarState& car, const Level& level);

    // seconds from the jump to the contact at height `y`
    float AerialFlightTime(float y);

    // the plan of one team: roles and the kick-off taker (RlBotsPlan)
    void Plan(const RlTuning& tuning, const RlWorldState& world, const RlForecast& forecast, int team, int teamSize, int level, RlBotTeamPlan& plan,
              RlBotMemory* memories);

    // a new decision of one bot: fills the target, the stop flag, the arrival time, the direction of the hit and the time of the contact
    void Decide(const RlBotContext& context, RlBotMemory& memory, RlRng& rng);
}
