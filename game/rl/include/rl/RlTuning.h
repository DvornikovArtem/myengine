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
        float dodgeHitScale = 1.3f;     // the extra push of a ball hit during a dodge
        float roofHitScale = 0.6f;      // and of one with the roof of the car
        int hitCooldownTicks = 6;       // one car can add the extra push once per this many ticks
    };

    // The car follows the model of Rocket League as RocketSim (github.com/ZealanL/RocketSim, MIT, ZealanL) reconstructs it: air control as
    // an angular acceleration in the car's axes (the inertia of the body does not matter), jump and dodge as velocity changes, the
    // suspension of four rays with the RL stiffness, damping and travel, the pull into the surface (sticky force). Lengths in metres
    // (RL unreal units / 100), RL values are named in the comments.
    struct RlCarTuning
    {
        // body
        float mass = 180.0f;
        float halfExtentX = 0.43350f;   // Octane hitbox 120.507 x 86.6994 x 38.6591 uu (the long axis is Z)
        float halfExtentY = 0.19330f;
        float halfExtentZ = 0.60254f;
        float comOffsetY = -0.20755f;   // the centre of mass sits this far below the centre of the hitbox ...
        float comOffsetZ = -0.13876f;   // ... and this far behind it (RL: the hitbox is offset by 13.8757 uu forward, 20.755 uu up)
        float inertiaMultiplier = 1.0f; // 1: the inertia of the box of the hitbox, as RL's
        float friction = 0.3f;          // car vs arena (RL 0.3)
        float restitution = 0.0f;       // RL has 0.1..0.3; 0 here: the hitbox does not bounce off the floor
        float maxSpeed = 23.0f;
        float maxAngularSpeed = 5.5f;
        float supersonicSpeed = 22.0f;
        float throttleMaxSpeed = 14.1f; // the throttle alone does not push beyond it, the boost does

        // suspension: four rays down the car's -Y from the wheel mounts (relative to the centre of the hitbox)
        float wheelMountXFront = 0.259f;   // RL 25.9 uu
        float wheelMountXRear = 0.295f;    // RL 29.5 uu
        float wheelMountYOffset = 0.0f;    // the mounts are at the height of the centre of the hitbox
        float wheelFrontZ = 0.37374f;      // RL 51.25 uu from the origin, 13.8757 uu hitbox offset
        float wheelRearZ = -0.47626f;      // RL -33.75 uu
        float wheelRadiusFront = 0.125f;
        float wheelRadiusRear = 0.150f;
        float suspensionRestFront = 0.26755f;  // length of a relaxed spring (RL 38.755 uu minus the travel)
        float suspensionRestRear = 0.25055f;
        float suspensionTravel = 0.12f;        // RL 12 uu: the spring is clamped to rest +- travel
        float suspensionSubtraction = 0.025f;  // the ray is this much shorter than rest + travel + radius (RL 0.05 bt = 2.5 uu)
        float suspensionStiffness = 500.0f;    // N/m per unit of the force scale; the force scale makes it 17.9 / 27.1 kN/m per wheel
        float suspensionDampCompression = 25.0f;   // N s/m per unit of the force scale while the spring compresses
        float suspensionDampRelaxation = 40.0f;    // and while it relaxes
        float suspensionScaleFront = 35.75f;
        float suspensionScaleRear = 54.265f;
        float spawnHeight = 0.38f;             // height of the hitbox centre of a car put on the ground

        // ground
        float throttleAccel = 16.0f;       // m/s^2 at standstill, falls linearly to throttleAccelAtMax at throttleMaxSpeed
        float throttleAccelAtMax = 1.6f;
        float brakeAccel = 35.0f;
        float coastDecel = 5.25f;
        float lateralAccel = 38.0f;        // largest sideways acceleration the tyres hold (m/s^2)
        float lateralGain = 0.85f;         // share of the sideways speed a tick takes out
        float handbrakeLateralScale = 0.18f;  // grip of the rear tyres with the handbrake
        float handbrakeRecoverTime = 0.25f; // s the rear tyres need to get their grip back after the powerslide
        float landingGripTime = 0.3f;      // s after a touchdown with stronger side grip
        float landingLateralScale = 2.5f;  // times the side grip in them
        float landingYawDamping = 10.0f;   // 1/s on the yaw rate left over from the air, without steering
        float handbrakeSteerScale = 1.5f;  // the curvature table counts this much more with the handbrake
        float maxSteerAngle = 0.62f;       // rad, the front wheels never turn further
        // steering curvature (1/m) by speed (m/s): yaw rate = steer * curvature * forward speed
        float curvatureSpeed[6] = {0.0f, 5.0f, 10.0f, 15.0f, 17.5f, 23.0f};
        float curvatureValue[6] = {0.69f, 0.398f, 0.235f, 0.1375f, 0.11f, 0.088f};
        float groundAngularDamping = 0.0f; // 1/s, pitch and roll about the surface (0: only the springs act on the tilt of a landed car)

        // the pull into the surface while a wheel touches it (RL "sticky force"): (base + steep * (1 - |normal.y|)) * gravity
        float stickBase = 0.5f;            // always (so the car presses on the floor with 1.5 g in all)
        float stickSteep = 1.0f;           // added in proportion to how steep the surface is, while the car drives or has the throttle
        float stickStopSpeed = 0.25f;      // m/s: slower than this without the throttle the steep part is off (the car slides down a wall)

        // jump, double jump, dodge (RocketSim / RLBot wiki values)
        float jumpImpulse = 2.91667f;      // instant speed along the car's up at the jump
        float jumpHoldAccel = 14.5833f;    // m/s^2 along up while the button is held
        float jumpMinTime = 0.025f;        // the hold push runs at least this long ...
        float jumpMinAccelScale = 0.62f;   // ... at this share
        float jumpHoldTime = 0.2f;         // and at most this long
        float doubleJumpImpulse = 2.91667f;
        float dodgeWindow = 1.25f;         // s after the first jump's push ended (or after leaving the ground) the second jump is allowed
        float dodgeDeadzone = 0.5f;        // |yaw| + |pitch| + |roll| below this makes a double jump instead of a dodge
        float dodgeImpulse = 5.0f;         // m/s of speed along the direction of the dodge
        float dodgeForwardSpeedScale = 1.0f;   // the forward impulse is this much larger at top speed (RL 1.0: not larger)
        float dodgeSideSpeedScale = 1.9f;      // the sideways one
        float dodgeBackSpeedScale = 2.5f;      // a back flip (against the speed)
        float dodgeBackImpulseScale = 1.06667f;  // 16/15 on a back flip
        float dodgeZDamp = 0.35f;          // share of the vertical speed a flip takes out per 1/120 s tick (in the window below)
        float dodgeZDampStart = 0.15f;     // s after the dodge
        float dodgeZDampEnd = 0.21f;       // up to here always, after it only while the car falls
        float dodgeTorqueTime = 0.65f;     // s the flip spin is driven; then the air control is back
        float dodgeTorquePitch = 224.0f;   // rad/s^2 of a front/back flip (the angular speed limit stops it at 5.5)
        float dodgeTorqueRoll = 260.0f;    // and of a side flip
        float dodgePitchLockExtra = 0.3f;  // s after the flip the pitch stick is still off
        float selfRightHop = 2.0f;         // m/s: the small hop of the jump of a car lying on its roof or side
        float selfRightSpin = 5.0f;        // rad/s of the flip that turns a car lying on its roof or side back onto its wheels

        // air control: angular acceleration = T * stick - D * angular speed, in the car's axes (RL 130/95/400 and 30/20/50, times 2 pi / 65536 * 1000)
        float airPitchTorque = 12.4636f;   // rad/s^2
        float airYawTorque = 9.10795f;
        float airRollTorque = 38.3495f;
        float airPitchDamping = 2.87621f;  // 1/s; pitch and yaw damping fade out with the stick
        float airYawDamping = 1.91747f;
        float airRollDamping = 4.79369f;
        float airThrottleAccel = 0.66667f; // m/s^2 along the nose with the throttle in the air

        // boost
        float boostStart = 33.3f;
        float boostMax = 100.0f;
        float boostUsePerSecond = 33.3f;
        float boostAccelGround = 9.92f;
        float boostAccelAir = 10.58f;

        // demolish
        float respawnDelay = 3.0f;
    };

    // The camera of Rocket League (default values of its settings): the distance and the height behind the car, the angle, how stiff it
    // follows the car, how fast it swings round, how fast it changes between the car cam and the ball cam.
    struct RlCameraTuning
    {
        float fov = 110.0f;               // degrees, horizontal, as the field of view of RL (the settings of G1 change it)
        float distance = 2.7f;            // m behind the car
        float height = 1.0f;              // m above the car
        float pitchDegrees = -3.0f;       // the car cam looks this far down
        float stiffness = 0.45f;          // 0 loose .. 1 glued to the heading of the car
        float swivelSpeed = 4.7f;         // how fast the camera swings round to a new heading
        float transitionSpeed = 1.2f;     // car cam <-> ball cam, 1 / s
        float upFollow = 4.0f;            // 1 / s: the camera's up follows the surface under the car
        float airUpFollow = 1.5f;         // and in the air (the car cartwheels, the camera does not)
        float airSwivelScale = 0.5f;      // the heading follows the car this much slower in the air
        float minDistance = 0.45f;        // the camera never comes closer to the car than this, whatever stands behind it
        float wallMargin = 0.3f;          // m kept between the camera and the arena
        float ballCamMaxPitch = 35.0f;    // degrees: the ball cam does not look further up or down than this
    };

    struct RlMatchTuning
    {
        float matchSeconds = 300.0f;
        float countdownSeconds = 3.0f;
        float goalSeconds = 3.0f;
        float goalLineMargin = 0.0f;       // extra metres the ball centre must be past (goal line + ball radius)
        float padBigBoost = 100.0f;
        float padBigRespawn = 10.0f;
        float padBigRadius = 2.08f;
        float padSmallBoost = 12.0f;
        float padSmallRespawn = 4.0f;
        float padSmallRadius = 1.65f;
        float padMaxHeight = 1.7f;         // a car whose centre is higher does not pick the pad up
    };

    // Walks over every number of the tuning in the order of the json file: the RL Tuning window of the editor draws its sliders with it.
    class RlTuningVisitor
    {
    public:
        virtual ~RlTuningVisitor() = default;
        virtual void Begin(const char* group) = 0;  // a group of the json ("ball", "car", ...) ...
        virtual void End() = 0;                     // ... until here
        virtual void Section(const char* title) = 0;  // a heading inside a group (only a hint for the reader of a window)
        virtual void Float(const char* name, float& value) = 0;
        virtual void Int(const char* name, int& value) = 0;
        virtual void Floats(const char* name, float* values, int count) = 0;
    };

    struct RlTuning
    {
        float gravity = 6.5f;
        RlArenaTuning arena;
        RlBallTuning ball;
        RlCarTuning car;
        RlMatchTuning match;
        RlCameraTuning camera;

        // Reads the json file; keys that are missing keep the defaults. Returns false (and fills error) if the file is
        // missing or is not json.
        bool LoadFromFile(const std::string& path, std::string* error = nullptr);
        bool LoadFromText(const std::string& text, std::string* error = nullptr);
        std::string ToJson() const;
        void Visit(RlTuningVisitor& visitor);
    };
}
