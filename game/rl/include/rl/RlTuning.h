// RlTuning.h
// Every number of the Rocket League simulation. The defaults are the values of the table in docs/game/rl-online.md (RLBot wiki,
// uu / 100) and of the feel tuning of G3; assets/data/rl_tuning.json overrides any of them by name.

#pragma once

#include <string>

namespace rl
{
    struct RlArenaTuning
    {
        float halfWidth = 40.96f;       // walls at x = +-halfWidth
        float halfLength = 51.2f;       // end walls at z = +-halfLength
        float height = 20.44f;
        float cornerCut = 11.5f;        // the 45 degree corner chamfer eats this much of both walls
        float floorRampRadius = 3.0f;   // quarter pipe between floor and walls
        float ceilingRampRadius = 3.0f; // the same between walls and ceiling
        int rampSegments = 10;          // straight pieces in a quarter circle
        float goalHalfWidth = 8.93f;
        float goalHeight = 6.43f;
        float goalDepth = 8.8f;
        float friction = 0.4f;
        float restitution = 0.0f;       // Jolt takes the larger of the two restitutions, so the ball's counts
    };

    struct RlBallTuning
    {
        float radius = 0.9275f;
        float mass = 30.0f;
        float restitution = 0.6f;
        float friction = 0.15f;
        float maxSpeed = 60.0f;
        float maxAngularSpeed = 6.0f;
        float linearDamping = 0.0f;
        float angularDamping = 0.0f;
        // RL's extra push on a hit: the ball gets extra = relativeSpeed * scale(relativeSpeed) along the hit direction whose
        // vertical part is scaled by hitNormalYScale. The table is (relative speed, scale) pairs.
        float hitNormalYScale = 0.35f;
        float hitSpeed[4] = {0.0f, 5.0f, 23.0f, 46.0f};
        float hitScale[4] = {0.65f, 0.65f, 0.55f, 0.30f};
        int hitCooldownTicks = 6;       // one car can add the extra push once per this many ticks
    };

    struct RlCarTuning
    {
        // body
        float mass = 180.0f;
        float halfExtentX = 0.42f;      // Octane hitbox 0.84 wide, 0.36 high, 1.18 long (half sizes; the long axis is Z)
        float halfExtentY = 0.18f;
        float halfExtentZ = 0.59f;
        float inertiaMultiplier = 1.6f; // a bigger inertia calms the box down on the springs
        float friction = 0.3f;
        float restitution = 0.0f;
        float maxSpeed = 23.0f;
        float maxAngularSpeed = 5.5f;
        float supersonicSpeed = 22.0f;
        float throttleMaxSpeed = 14.1f; // the throttle alone does not push beyond it, the boost does

        // suspension (four rays down the car's local -Y from the wheel mounts)
        float wheelMountX = 0.39f;
        float wheelMountYOffset = -0.06f;  // mount height in the hitbox
        float wheelFrontZ = 0.50f;
        float wheelRearZ = -0.46f;
        float suspensionRest = 0.30f;      // ray length with a relaxed spring: the wheel hangs this far under the mount
        float suspensionContactMargin = 0.05f; // the wheel still counts as touching this far beyond the rest length
        float springStiffness = 6500.0f;   // N/m per wheel
        float springDamping = 650.0f;      // N s/m per wheel

        // ground
        float throttleAccel = 16.0f;       // m/s^2 at standstill, falls linearly to throttleAccelAtMax at throttleMaxSpeed
        float throttleAccelAtMax = 1.6f;
        float brakeAccel = 35.0f;
        float coastDecel = 5.25f;
        float lateralAccel = 38.0f;        // largest sideways acceleration the tyres hold (m/s^2)
        float lateralGain = 0.85f;         // share of the sideways speed a tick takes out
        float handbrakeLateralScale = 0.18f;  // grip of the rear tyres with the handbrake
        float handbrakeSteerScale = 1.5f;  // the curvature table counts this much more with the handbrake
        float maxSteerAngle = 0.62f;       // rad, the front wheels never turn further
        // steering curvature (1/m) by speed (m/s): yaw rate = steer * curvature * forward speed
        float curvatureSpeed[6] = {0.0f, 5.0f, 10.0f, 15.0f, 17.5f, 23.0f};
        float curvatureValue[6] = {0.69f, 0.398f, 0.235f, 0.1375f, 0.11f, 0.088f};
        float groundAngularDamping = 4.0f; // 1/s, pitch and roll about the surface
        float stickAccel = 13.0f;          // pull into walls and ceiling (m/s^2), scaled by how much the surface is not floor
        float stickMinContacts = 2.0f;
        float jumpStickGrace = 0.20f;      // no stick this long after a jump

        // jump, double jump, dodge
        float jumpImpulse = 2.92f;
        float jumpHoldAccel = 14.58f;
        float jumpHoldTime = 0.2f;
        float doubleJumpImpulse = 2.92f;
        float dodgeWindow = 1.25f;         // seconds after the first jump (or after leaving the ground) the second jump is allowed
        float dodgeDeadzone = 0.5f;        // direction input below this makes a double jump instead of a dodge
        float dodgeImpulse = 5.0f;
        float dodgeForwardSpeedBonus = 0.9f;   // forward dodge impulse * (1 + bonus * speed / maxSpeed)
        float dodgeVerticalCancel = 0.65f;     // share of the vertical speed a front/back flip removes
        float dodgeSpin = 5.5f;            // rad/s the flip spins at
        float dodgeSpinTime = 0.85f;       // seconds the spin is held (about one turn with the damping afterwards)
        float dodgeLockTime = 0.85f;       // seconds the air control is off after a dodge

        // air
        float airPitchTorque = 12.146f;    // rad/s^2
        float airYawTorque = 8.920f;
        float airRollTorque = 36.080f;
        float airPitchDamping = 2.798f;    // 1/s
        float airYawDamping = 1.886f;
        float airRollDamping = 4.472f;

        // boost
        float boostStart = 33.3f;
        float boostMax = 100.0f;
        float boostUsePerSecond = 33.3f;
        float boostAccelGround = 9.92f;
        float boostAccelAir = 10.58f;

        // demolish
        float respawnDelay = 3.0f;
    };

    struct RlTuning
    {
        float gravity = 6.5f;
        RlArenaTuning arena;
        RlBallTuning ball;
        RlCarTuning car;

        // Reads the json file; keys that are missing keep the defaults. Returns false (and fills error) if the file is
        // missing or is not json.
        bool LoadFromFile(const std::string& path, std::string* error = nullptr);
        bool LoadFromText(const std::string& text, std::string* error = nullptr);
        std::string ToJson() const;
    };
}
