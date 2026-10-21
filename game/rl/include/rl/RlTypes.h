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

    struct RlWorldState
    {
        uint32_t tick = 0;
        int carCount = 0;
        RlCarState cars[kMaxCars];
        RlBallState ball;
        // Last car that touched the ball, -1 before the first touch (the debug overlay and G4 goals use it)
        int lastBallToucher = -1;
    };

    // The things a hit report carries for the tests and for the future sound / effects.
    struct RlEvents
    {
        int ballHits = 0;
        int demolitions = 0;
    };
}
