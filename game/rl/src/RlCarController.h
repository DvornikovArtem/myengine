// RlCarController.h
// What a Rocket League car does in one tick: four suspension rays, grip, throttle, steering, sticking to walls, jump, double jump,
// dodge, air control and boost. Everything is a force or a velocity change on the Jolt body; nothing sets the pose.

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
        float airTime = 0.0f;       // seconds since the first jump or since the wheels left the ground
        float sinceJump = 1000.0f;  // seconds since the last first jump
        float jumpHold = 0.0f;      // seconds of the jump-hold push that are left
        float dodgeSpinLeft = 0.0f; // seconds the flip spin still runs
        float dodgeLock = 0.0f;     // seconds the air control is still off
        float dodgeSpinX = 0.0f;    // local angular velocity of the flip (rad/s) about the car's X
        float dodgeSpinZ = 0.0f;    // and about its Z
        float jumpDirX = 0.0f;      // where the jump hold pushes (the car's up when the jump was made)
        float jumpDirY = 1.0f;
        float jumpDirZ = 0.0f;
        float spawnX = 0.0f;
        float spawnY = 0.0f;
        float spawnZ = 0.0f;
        float spawnYaw = 0.0f;
        int32_t wheelContacts = 0;
        int32_t ballHitCooldown = 0;
        uint8_t demolished = 0;
        uint8_t jumpPrev = 0;
        uint8_t jumpUsed = 0;       // the first jump was made, or the car left the ground
        uint8_t secondUsed = 0;     // the double jump / dodge was made
        uint8_t bodyContact = 0;    // the hitbox touched the arena in the last tick
        uint8_t dodging = 0;
        uint8_t pad[2] = {};  // keeps the struct free of unnamed padding
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
