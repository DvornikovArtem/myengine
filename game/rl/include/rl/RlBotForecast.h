// RlBotForecast.h
// The forecast of the ball that the bots of the match plan with, public so that the tests can measure it against the real simulation.

#pragma once

#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <cstdint>

namespace rl
{
    // Where the ball will be: its centre every 0.05 s for 3 s from the tick the forecast was made at (no cars in the way, the real arena).
    constexpr int kForecastSamples = 61;
    constexpr float kForecastDt = 0.05f;

    struct RlForecast
    {
        int32_t valid = 0;
        uint32_t tick = 0;
        float pos[kForecastSamples][3] = {};
    };

    // Plays the ball out analytically: gravity, the drag of the ball, and the arena as a rounded box with chamfered corners and the real goal
    // mouths, bounces with the restitution of the ball. No cars in the way, no spin. A pure function of the ball (it is not part of the state of
    // the match: the bots make it again from the world at every tick, so a replay after a rollback gives the same numbers).
    void RlForecastBall(const RlTuning& tuning, const RlBallState& ball, uint32_t tick, RlForecast& out);

    // The ball `seconds` after the tick `tick` the forecast was made at: `now` is the current tick of the world.
    RlVec3 RlForecastAt(const RlForecast& forecast, uint32_t now, float seconds);
}
