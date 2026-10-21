// RlCarController.h
// What a Rocket League car does in one tick: four suspension rays, grip, throttle, steering, the pull into the surface, jump, double jump,
// dodge, air control and boost. Everything is a force or a velocity change on the Jolt body; nothing sets the pose.
// The model (air control, jump, dodge, suspension, sticky force) follows RocketSim by ZealanL (MIT), see docs/game/rl-physics.md.

#pragma once

#include "RlJolt.h"

#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

namespace rl::detail
{
    // The part of a car that lives outside Jolt. Plain old data: a snapshot copies the bytes (the sim zero-fills it first so the
    // padding is the same everywhere).
    struct CarRuntime
    {
        float boost = 0.0f;
        float respawnTimer = 0.0f;
        float airTime = 0.0f;       // seconds since the push of the first jump ended (or since the car left the ground without a jump)
        float jumpTime = 1000.0f;   // seconds since the first jump started
        float flipTime = 0.0f;      // seconds since the dodge started
        float flipPitch = 0.0f;     // direction of the flip: about the car's X (+ = nose down, a front flip) ...
        float flipRoll = 0.0f;      // ... and about its Z (+ = rolls left)
        float rightSpinLeft = 0.0f; // seconds the turn of a car lying on its roof or side still runs
        float rightSpinX = 0.0f;    // its local angular velocity (rad/s)
        float rightSpinZ = 0.0f;
        float bodyNormalX = 0.0f;   // normal of the surface the hitbox touches (towards the car), valid while bodyContact
        float bodyNormalY = 1.0f;
        float bodyNormalZ = 0.0f;
        float gripScale = 1.0f;     // grip of the rear tyres: drops with the powerslide, comes back over handbrakeRecoverTime
        float landingTimer = 0.0f;  // seconds the landing grip is still stronger
        float spawnX = 0.0f;
        float spawnY = 0.0f;
        float spawnZ = 0.0f;
        float spawnYaw = 0.0f;
        int32_t wheelContacts = 0;
        int32_t ballHitCooldown = 0;
        uint8_t demolished = 0;
        uint8_t jumpPrev = 0;
        uint8_t jumpUsed = 0;       // the first jump was made
        uint8_t secondUsed = 0;     // the double jump or the dodge was made
        uint8_t isJumping = 0;      // the push of the first jump still runs
        uint8_t flipped = 0;        // a dodge was made (the pitch stick stays off a little after it)
        uint8_t dodging = 0;        // the flip spin is being driven
        uint8_t bodyContact = 0;    // the hitbox touched the arena in the last tick
        uint8_t slideLatch = 0;     // the powerslide button was held in the air: it is the air roll until it is let go
        uint8_t wasOnSurface = 0;
        uint8_t selfRight = 0;      // the turn of a car lying on its roof or side runs
    };

    struct CarContext
    {
        JPH::PhysicsSystem& physics;
        const RlTuning& tuning;
        const JPH::Shape* arenaShape;
    };

    // Results the sim copies into RlCarState.
    struct CarFrame
    {
        int wheelContacts = 0;
        float wheelCompression[kWheelCount] = {};
        JPH::Vec3 surfaceNormal = JPH::Vec3::sZero();
    };

    // Applies the input of one tick to the body (call before PhysicsSystem::Update, not while a body is locked).
    void ControlCar(const CarContext& context, JPH::Body& body, const RlCarInput& input, CarRuntime& runtime, CarFrame& frame);
}
