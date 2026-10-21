// RlTypes.h
// Plain data of the Rocket League simulation: no Jolt, no engine types. Units are metres, seconds, radians; Y is up.
// A car looks along its local +Z, its up is +Y and its right is +X (the engine's left-handed frame; the numbers are the
// same as in Jolt's right-handed math). Positive rotation about local +X pitches the nose down, about +Y yaws to the right,
// about +Z rolls to the left.

#pragma once

#include <cstdint>

namespace rl
{
    constexpr int kMaxCars = 8;
    constexpr int kWheelCount = 4;
    constexpr int kTickRate = 120;
    constexpr float kTickDt = 1.0f / static_cast<float>(kTickRate);

    struct RlVec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct RlQuat
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
    };

    // What one player holds on the controller during a tick.
    struct RlCarInput
    {
        float throttle = 0.0f;   // -1..1, forward +
        float steer = 0.0f;      // -1..1, right +; on the ground it turns the car, in the air it yaws (or rolls with airRoll)
        float pitch = 0.0f;      // -1..1, +1 = stick back = nose up; the flip direction while dodging (-1 is a front flip)
        float roll = 0.0f;       // -1..1, right +; a separate air roll axis (keyboard Q/E)
        bool boost = false;
        bool jump = false;       // held; a new press jumps, double jumps or dodges, holding lengthens the first jump
        bool handbrake = false;  // drift on the ground
        bool airRoll = false;    // while held the steer axis rolls the car instead of yawing it
    };

    struct RlCarState
    {
        RlVec3 position{};
        RlQuat rotation{};
        RlVec3 linearVelocity{};
        RlVec3 angularVelocity{};
        float boost = 0.0f;               // 0..100
        int wheelContacts = 0;            // wheels touching the arena this tick
        bool onGround = false;            // at least one wheel touches
        bool demolished = false;
        bool supersonic = false;
        bool jumped = false;              // the first jump was made (or the car left the ground)
        bool canDodge = false;            // the second jump / dodge is still available
        bool dodging = false;             // the flip torque is still running
        float respawnTimer = 0.0f;        // seconds until a demolished car comes back
        float wheelCompression[kWheelCount] = {};  // 0 = the wheel is in the air, otherwise the suspension compression in metres
        RlVec3 surfaceNormal{};           // average normal under the wheels (zero in the air)
    };

    struct RlBallState
    {
        RlVec3 position{};
        RlQuat rotation{};
        RlVec3 linearVelocity{};
        RlVec3 angularVelocity{};
    };

    constexpr int kPadCount = 34;  // 6 big and 28 small boost pads (RLBot field info order)

    enum class RlMatchPhase : int32_t
    {
        Countdown = 0,  // the cars stand on the kick-off spots, the throttle does nothing
        Playing = 1,
        GoalScored = 2, // the ball and the cars keep living, then the next kick-off (or the end)
        Finished = 3,
    };

    struct RlPadState
    {
        float respawnLeft = 0.0f;  // seconds until the pad is back, 0 = ready
    };

    // Everything of a match outside the physics. A plain structure: a snapshot copies the bytes.
    struct RlMatchState
    {
        RlMatchPhase phase = RlMatchPhase::Countdown;
        int32_t mode = 1;               // players per team (1..4); the practice mode has 1 car and mode 0
        int32_t score[2] = {0, 0};      // blue, orange
        float clock = 0.0f;             // seconds left; in overtime the seconds played in it
        int32_t overtime = 0;           // 1 in overtime (int32: the structure has no padding bytes, a snapshot compares bytewise)
        float phaseTimer = 0.0f;        // seconds left of the countdown / of the goal celebration
        int32_t lastGoalTeam = -1;      // the team that scored last
        int32_t lastGoalScorer = -1;    // slot of the car that touched the ball last before the goal, -1 if nobody
        int32_t lastGoalOwn = 0;
        int32_t winner = -1;            // 0 blue, 1 orange once the match is Finished
        uint32_t kickoffs = 0;
        int32_t goals[kMaxCars] = {};     // per slot: goals scored (own goals not counted)
        int32_t touches[kMaxCars] = {};   // per slot: hits of the ball
        RlPadState pads[kPadCount];
    };

    struct RlWorldState
    {
        uint32_t tick = 0;
        int carCount = 0;
        RlCarState cars[kMaxCars];
        RlBallState ball;
        // Last car that touched the ball, -1 before the first touch (the debug overlay and the goals use it)
        int lastBallToucher = -1;
        RlMatchState match;  // filled by RlMatch, zero for a bare RlSim
    };

    // The things a step reports for the tests and for the future sound / effects.
    struct RlEvents
    {
        int ballHits = 0;
        int demolitions = 0;
    };
}
