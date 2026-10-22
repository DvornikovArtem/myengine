// RlCarController.cpp
//
// The model of the car follows RocketSim (https://github.com/ZealanL/RocketSim, MIT license, Copyright (c) 2022 ZealanL): the tyres and the
// throttle (Car::_UpdateWheels, btVehicleRL::calcFrictionImpulses), the air control as an angular acceleration in the car's axes
// (Car::_UpdateAirTorque), jump, double jump and dodge (Car::_UpdateJump, Car::_UpdateDoubleJumpOrFlip, from the dodge notes of RLUtilities
// by Samuel P. Mish), the suspension of four rays (btVehicleRL), the pull into the surface ("sticky force"), the auto-roll, the auto-flip
// and the boost. The numbers live in RlTuning.h; this file writes the formulas in the units and axes of this engine (metres, Y up, the
// car's +Z forward, +X right). Bullet works in units of 0.5 m with a tick of 1/120 s, the constants of RocketSim that depend on that are
// converted where they are used (the comments say how).

#include "RlCarController.h"

#include <algorithm>
#include <cmath>

namespace rl::detail
{
    namespace
    {
        using JPH::Mat44;
        using JPH::Vec3;

        constexpr float kPi = 3.14159265f;

        float Clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
        float Sign(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

        // piecewise linear table, clamped at both ends (RocketSim LinearPieceCurve)
        float Interp(const float* xs, const float* ys, int count, float x)
        {
            if (x <= xs[0])
            {
                return ys[0];
            }
            for (int i = 1; i < count; ++i)
            {
                if (x < xs[i])
                {
                    const float t = (x - xs[i - 1]) / std::max(xs[i] - xs[i - 1], 1e-6f);
                    return ys[i - 1] + (ys[i] - ys[i - 1]) * t;
                }
            }
            return ys[count - 1];
        }

        Vec3 SafeNormalized(Vec3 v, Vec3 fallback)
        {
            const float length = v.Length();
            return length > 1e-6f ? v / length : fallback;
        }

        // the signed angle that carries the up of an upright car with this heading to the up of the car (about its nose): the roll
        float RollAngle(Vec3 forward, Vec3 up)
        {
            const Vec3 worldUp = Vec3::sAxisY();
            const Vec3 reference = SafeNormalized(worldUp - forward * forward.Dot(worldUp), up);
            return std::atan2(forward.Dot(reference.Cross(up)), reference.Dot(up));
        }
    }

    void ControlCar(const CarContext& ctx, JPH::Body& body, const RlCarInput& input, CarRuntime& rt, CarFrame& frame)
    {
        const RlCarTuning& T = ctx.car;
        const float dt = kTickDt;
        const float mass = T.mass;

        const Vec3 position = body.GetPosition();  // the origin of the body: the centre of the hitbox
        const Mat44 basis = Mat44::sRotation(body.GetRotation());
        const Vec3 right = basis.GetAxisX();
        const Vec3 up = basis.GetAxisY();
        const Vec3 forward = basis.GetAxisZ();
        const Vec3 worldUp = Vec3::sAxisY();
        const Vec3 centerOfMass = Vec3(body.GetCenterOfMassPosition());
        const Vec3 startVelocity = body.GetLinearVelocity();
        const Vec3 startAngular = body.GetAngularVelocity();

        // ---- 1. the four wheel rays
        struct Wheel
        {
            Vec3 origin;
            Vec3 normal;
            Vec3 point;
            float length;      // the spring, clamped to its travel
            float rest;
            bool contact;
        };
        Wheel wheels[kWheelCount];
        int contacts = 0;
        Vec3 normalSum = Vec3::sZero();
        const JPH::SpecifiedBroadPhaseLayerFilter broadFilter(BroadPhase::kStatic);
        const JPH::SpecifiedObjectLayerFilter objectFilter(Layers::kStatic);
        for (int w = 0; w < kWheelCount; ++w)
        {
            const bool front = w < 2;
            const float side = (w & 1) ? 1.0f : -1.0f;  // 0 front left, 1 front right, 2 rear left, 3 rear right
            const float z = front ? T.wheelFrontZ : T.wheelRearZ;
            const float mountX = front ? T.wheelMountXFront : T.wheelMountXRear;
            const float radius = front ? T.wheelRadiusFront : T.wheelRadiusRear;
            Wheel& wheel = wheels[w];
            wheel.rest = front ? T.suspensionRestFront : T.suspensionRestRear;
            wheel.origin = position + basis.Multiply3x3(Vec3(side * mountX, T.wheelMountYOffset, z));
            wheel.contact = false;
            wheel.length = wheel.rest + T.suspensionTravel;
            wheel.normal = up;
            wheel.point = wheel.origin;
            const float rayLength = wheel.rest + T.suspensionTravel + radius - T.suspensionSubtraction;
            const JPH::RRayCast ray(wheel.origin, -up * rayLength);
            JPH::RayCastResult hit;
            if (ctx.physics.GetNarrowPhaseQuery().CastRay(ray, hit, broadFilter, objectFilter))
            {
                wheel.contact = true;
                wheel.point = ray.GetPointOnRay(hit.mFraction);
                wheel.length = Clamp(hit.mFraction * rayLength - radius, wheel.rest - T.suspensionTravel, wheel.rest + T.suspensionTravel);
                wheel.normal = ctx.arenaShape->GetSurfaceNormal(hit.mSubShapeID2, wheel.point);
                ++contacts;
                normalSum += wheel.normal;
            }
            frame.wheelCompression[w] = 0.0f;
        }
        const Vec3 surfaceNormal = contacts > 0 ? SafeNormalized(normalSum, up) : Vec3::sZero();
        const bool onGround = contacts >= 3;  // RocketSim: a car is on the ground with three wheels on it
        frame.wheelContacts = contacts;
        frame.surfaceNormal = surfaceNormal;
        rt.wheelContacts = contacts;

        // The powerslide button is also the air roll here (the controls of the game bind both to Shift): held in the air as the air roll it
        // is no handbrake after the landing until it is let go and pressed again, or every landing with Shift held would start a slide.
        if (contacts == 0)
        {
            rt.slideLatch = (input.handbrake && input.airRoll) ? 1 : 0;
        }
        else if (!input.handbrake)
        {
            rt.slideLatch = 0;
        }
        const bool handbrake = input.handbrake && rt.slideLatch == 0;

        // the player's stick in the three axes: the air roll button turns the steering into a roll
        const float stickPitch = Clamp(input.pitch, -1.0f, 1.0f);
        float stickYaw = Clamp(input.steer, -1.0f, 1.0f);
        float stickRoll = Clamp(input.roll, -1.0f, 1.0f);
        const float steerIn = stickYaw;
        if (input.airRoll)
        {
            stickRoll = Clamp(stickRoll + stickYaw, -1.0f, 1.0f);
            stickYaw = 0.0f;
        }
        const float throttleIn = Clamp(input.throttle, -1.0f, 1.0f);
        const bool hasBodyContact = rt.bodyContact != 0;
        const Vec3 bodyNormal = SafeNormalized(Vec3(rt.bodyNormalX, rt.bodyNormalY, rt.bodyNormalZ), worldUp);
        rt.bodyContact = 0;  // read once, the next step writes it again

        // ---- 2. the jump (RocketSim Car::_UpdateJump): an instant speed along the up of the car, then a push while the button is held
        const bool jumpPressed = input.jump && rt.jumpPrev == 0;
        rt.jumpPrev = input.jump ? 1 : 0;
        Vec3 velocity = startVelocity;
        bool velocityChanged = false;

        if (onGround && rt.isJumping == 0)
        {
            if (rt.jumpUsed != 0 && rt.jumpTime < T.jumpMinTime + 0.025f)
            {
                // the wheels have not left the ground yet after a short jump: the jump is not given back yet
            }
            else
            {
                rt.jumpUsed = 0;
                rt.jumpTime = 0.0f;
            }
        }
        if (onGround)
        {
            rt.secondUsed = 0;
            rt.flipped = 0;
            rt.airTime = 0.0f;
            rt.flipTime = 0.0f;
            rt.dodging = 0;
        }
        // RocketSim runs the air torque before it decides about a new dodge: the flip starts to turn the tick after it was made
        const bool dodgingAtStart = rt.dodging != 0;
        const bool flippedAtStart = rt.flipped != 0;
        const float flipTimeAtStart = rt.flipTime;

        if (rt.isJumping != 0)
        {
            rt.isJumping = (rt.jumpTime < T.jumpMinTime || (input.jump && rt.jumpTime < T.jumpHoldTime)) ? 1 : 0;
        }
        else if (onGround && jumpPressed)
        {
            rt.isJumping = 1;
            rt.jumpTime = 0.0f;
            velocity += up * T.jumpImpulse;
            velocityChanged = true;
        }
        if (rt.isJumping != 0)
        {
            rt.jumpUsed = 1;
            float accel = T.jumpHoldAccel;
            if (rt.jumpTime < T.jumpMinTime)
            {
                accel *= T.jumpMinAccelScale;
            }
            velocity += up * (accel * dt);
            velocityChanged = true;
        }
        if (rt.isJumping != 0 || rt.jumpUsed != 0)
        {
            rt.jumpTime += dt;
        }

        // ---- 2b. the auto-flip (RocketSim Car::_UpdateAutoFlip): a jump of a car on its roof hops it off the floor and rolls it back onto its wheels
        Vec3 angularAdd = Vec3::sZero();
        if (jumpPressed && hasBodyContact && bodyNormal.GetY() > T.autoFlipNormalY)
        {
            const float roll = RollAngle(forward, up);
            if (std::fabs(roll) > T.autoFlipRollThreshold)
            {
                rt.autoFlipTimer = T.autoFlipTime * (std::fabs(roll) / kPi);
                rt.autoFlipScale = roll > 0.0f ? -1.0f : 1.0f;  // the roll about the nose that takes the roll angle towards zero
                rt.autoFlipping = 1;
                velocity += -up * T.autoFlipImpulse;
                velocityChanged = true;
            }
        }
        if (rt.autoFlipping != 0)
        {
            if (rt.autoFlipTimer <= 0.0f)
            {
                rt.autoFlipping = 0;
                rt.autoFlipTimer = 0.0f;
            }
            else
            {
                angularAdd += forward * (T.autoFlipTorque * rt.autoFlipScale * dt);
                rt.autoFlipTimer -= dt;
            }
        }

        // ---- 3. double jump or dodge in the air (RocketSim Car::_UpdateDoubleJumpOrFlip)
        if (!onGround)
        {
            if (rt.jumpUsed == 0 || rt.isJumping == 0)
            {
                rt.airTime += dt;
            }
            else
            {
                rt.airTime = 0.0f;
            }

            if (jumpPressed && rt.airTime < T.dodgeWindow && rt.secondUsed == 0 && rt.autoFlipping == 0)
            {
                const float inputMagnitude = std::fabs(stickYaw) + std::fabs(stickPitch) + std::fabs(stickRoll);
                rt.secondUsed = 1;
                if (inputMagnitude >= T.dodgeDeadzone)
                {
                    rt.flipped = 1;
                    rt.dodging = 1;
                    rt.flipTime = 0.0f;

                    // the direction of the dodge: forward = -pitch, to the right = yaw + roll
                    float dirForward = -stickPitch;
                    float dirRight = stickYaw + stickRoll;
                    if (std::fabs(dirRight) < 0.1f && std::fabs(dirForward) < 0.1f)
                    {
                        dirForward = 0.0f;
                        dirRight = 0.0f;
                    }
                    else
                    {
                        const float length = std::sqrt(dirForward * dirForward + dirRight * dirRight);
                        dirForward /= length;
                        dirRight /= length;
                    }
                    rt.flipPitch = dirForward;   // about the car's X: + noses down
                    rt.flipRoll = -dirRight;     // about its Z: + rolls left
                    if (std::fabs(dirForward) < 0.1f)
                    {
                        dirForward = 0.0f;
                    }
                    if (std::fabs(dirRight) < 0.1f)
                    {
                        dirRight = 0.0f;
                    }
                    if (dirForward != 0.0f || dirRight != 0.0f)
                    {
                        const float dodgeForwardSpeed = velocity.Dot(forward);
                        const float speedRatio = std::fabs(dodgeForwardSpeed) / T.maxSpeed;
                        bool backwards;
                        if (std::fabs(dodgeForwardSpeed) < 1.0f)
                        {
                            backwards = dirForward < 0.0f;
                        }
                        else
                        {
                            backwards = (dirForward >= 0.0f) != (dodgeForwardSpeed >= 0.0f);
                        }
                        float impulseForward = dirForward * T.dodgeImpulse;
                        float impulseRight = dirRight * T.dodgeImpulse;
                        const float scaleForward = backwards ? T.dodgeBackSpeedScale : T.dodgeForwardSpeedScale;
                        impulseForward *= (scaleForward - 1.0f) * speedRatio + 1.0f;
                        impulseRight *= (T.dodgeSideSpeedScale - 1.0f) * speedRatio + 1.0f;
                        if (backwards)
                        {
                            impulseForward *= T.dodgeBackImpulseScale;
                        }
                        const Vec3 forwardFlat = SafeNormalized(Vec3(forward.GetX(), 0.0f, forward.GetZ()), Vec3::sAxisZ());
                        const Vec3 rightFlat = worldUp.Cross(forwardFlat);
                        velocity += forwardFlat * impulseForward + rightFlat * impulseRight;
                    }
                }
                else
                {
                    velocity += up * T.doubleJumpImpulse;
                }
                velocityChanged = true;
            }
        }

        // ---- 4. the throttle, the brake, the steering and the tyres (RocketSim Car::_UpdateWheels and btVehicleRL::calcFrictionImpulses)
        // Everything is worked out from the velocities the tick started with (the way RocketSim does in updateVehicleFirst); applied one by
        // one the impulse of the first wheel would change what the next one sees.
        const float forwardSpeed = startVelocity.Dot(forward);
        const float absForwardSpeed = std::fabs(forwardSpeed);
        Vec3 tyreImpulse[kWheelCount];
        Vec3 tyrePoint[kWheelCount];
        Vec3 springImpulse[kWheelCount];
        for (int w = 0; w < kWheelCount; ++w)
        {
            tyreImpulse[w] = Vec3::sZero();
            tyrePoint[w] = centerOfMass;
            springImpulse[w] = Vec3::sZero();
        }

        // the handbrake builds up and lets go over time
        rt.handbrakeVal = Clamp(rt.handbrakeVal + (handbrake ? T.handbrakeRise : -T.handbrakeFall) * dt, 0.0f, 1.0f);

        float realThrottle = throttleIn;
        float realBrake = 0.0f;
        if (input.boost && rt.boost > 0.0f)
        {
            realThrottle = 1.0f;
        }
        float engineThrottle = realThrottle;
        float driveSpeedScale = Interp(T.driveSpeed, T.driveFactor, 3, absForwardSpeed);
        if (!handbrake)
        {
            if (std::fabs(realThrottle) >= T.throttleDeadzone)
            {
                if (absForwardSpeed > T.stopSpeed && Sign(realThrottle) != Sign(forwardSpeed))
                {
                    realBrake = 1.0f;  // the full brake against the way the car goes
                    if (absForwardSpeed > 0.0001f)
                    {
                        engineThrottle = 0.0f;
                    }
                }
            }
            else
            {
                engineThrottle = 0.0f;  // coasting
                realBrake = absForwardSpeed < T.stopSpeed ? 1.0f : T.coastBrakeFactor;
            }
        }
        if (contacts < 3)
        {
            driveSpeedScale /= 4.0f;
        }
        const float engineImpulse = engineThrottle * T.throttleAccel * (mass * 0.25f) * dt * driveSpeedScale;  // per wheel
        const float brakeMaxImpulse = realBrake * T.brakeAccel * (mass * 0.25f) * dt;                         // per wheel

        // the angle of the front wheels
        float steerAngle = Interp(T.steerSpeed, T.steerAngle, 6, absForwardSpeed);
        if (rt.handbrakeVal > 0.0f)
        {
            steerAngle += (Interp(T.powerslideSteerSpeed, T.powerslideSteerAngle, 2, absForwardSpeed) - steerAngle) * rt.handbrakeVal;
        }
        steerAngle *= steerIn;
        // what the tyres do now was set in the tick before; what is worked out here is for the next one
        const float steerSin = std::sin(rt.tyreSteer);
        const float steerCos = std::cos(rt.tyreSteer);
        const float tyreEngine = rt.tyreEngine;
        const float tyreBrake = rt.tyreBrake;
        float nextLat[kWheelCount];
        float nextLong[kWheelCount];
        for (int w = 0; w < kWheelCount; ++w)
        {
            nextLat[w] = rt.tyreLat[w];
            nextLong[w] = rt.tyreLong[w];
        }

        if (contacts > 0)
        {
            const float invMass = body.GetMotionProperties()->GetInverseMass();
            const Mat44 inverseInertia = body.GetInverseInertia();
            for (int w = 0; w < kWheelCount; ++w)
            {
                const Wheel& wheel = wheels[w];
                if (!wheel.contact)
                {
                    continue;
                }
                const bool front = w < 2;
                const Vec3 n = wheel.normal;

                // the spring and its damper push along the normal of the surface in the point where the wheel touches it (btVehicleRL::updateSuspension)
                const float compression = wheel.rest - wheel.length;
                frame.wheelCompression[w] = std::max(compression, 0.0f);
                {
                    const float denominator = n.Dot(up);
                    float relativeVelocity = 0.0f;
                    float inverse = 10.0f;
                    if (denominator > 0.1f)
                    {
                        inverse = 1.0f / denominator;
                        relativeVelocity = n.Dot(body.GetPointVelocity(wheel.point)) * inverse;
                    }
                    const float damping = relativeVelocity < 0.0f ? T.suspensionDampCompression : T.suspensionDampRelaxation;
                    float force = (compression * T.suspensionStiffness * inverse - damping * relativeVelocity) *
                                  (front ? T.suspensionScaleFront : T.suspensionScaleRear);
                    force = std::max(force, 0.0f);  // RL never pulls the car down with a spring

                    // a wheel that sinks deeper than its rest (less the ray's shortening) hits the ground like a body does: the part of the
                    // impulse that stops it is shared by the four wheels (btVehicleRL::rayCast, resolveSingleCollision of RocketSim)
                    float extra = 0.0f;
                    const float radius = front ? T.wheelRadiusFront : T.wheelRadiusRear;
                    const float traceLength = (wheel.origin - wheel.point).Dot(up);
                    const float pushbackThreshold = wheel.rest + radius - T.suspensionSubtraction;
                    if (traceLength < pushbackThreshold)
                    {
                        const Vec3 arm = wheel.point - centerOfMass;
                        const Vec3 moment = arm.Cross(n);
                        const float meff = 1.0f / (invMass + moment.Dot(inverseInertia.Multiply3x3(moment)));
                        const float approach = n.Dot(body.GetPointVelocity(wheel.point));
                        const float impulse = (T.pushbackErp * static_cast<float>(kTickRate) * (pushbackThreshold - traceLength) - approach) * meff;
                        extra = std::max(impulse, 0.0f) / static_cast<float>(kWheelCount);
                    }
                    springImpulse[w] = force > 0.0f ? n * (force * dt + extra) : Vec3::sZero();
                }

                // the axle of this wheel (the front wheels turn with the steering), the grip curves by how much the wheel slides
                Vec3 axle = right;
                if (front)
                {
                    axle = right * steerCos - forward * steerSin;
                }
                const Vec3 slideLong = axle.Cross(n);
                const Vec3 wheelVelocity = startVelocity + startAngular.Cross(wheel.origin - centerOfMass);
                const float baseFriction = std::fabs(wheelVelocity.Dot(axle));
                float slip = 0.0f;
                if (baseFriction > T.slipMinSpeed)
                {
                    slip = baseFriction / (std::fabs(wheelVelocity.Dot(slideLong)) + baseFriction);  // the sideways share of the wheel's speed
                }
                float latFriction = Interp(T.latFrictionSlip, T.latFriction, 2, slip);
                float longFriction = 1.0f;
                if (rt.handbrakeVal > 0.0f)
                {
                    latFriction *= (T.handbrakeLatFriction - 1.0f) * rt.handbrakeVal + 1.0f;
                    longFriction *= (Interp(T.handbrakeLongSlip, T.handbrakeLongFriction, 2, slip) - 1.0f) * rt.handbrakeVal + 1.0f;
                }
                if (realThrottle == 0.0f)
                {
                    // no throttle: the tyres hold less, the less the surface is floor
                    const float nonSticky = Interp(T.nonStickyNormal, T.nonStickyScale, 3, n.GetY());
                    latFriction *= nonSticky;
                    longFriction *= nonSticky;
                }
                nextLat[w] = latFriction;  // for the next tick
                nextLong[w] = longFriction;
                latFriction = rt.tyreLat[w];  // this tick holds with what the tick before set
                longFriction = rt.tyreLong[w];

                // the axle in the plane of the surface and the direction the wheel rolls
                const Vec3 axleOnSurface = SafeNormalized(axle - n * axle.Dot(n), right);
                const Vec3 rollDir = SafeNormalized(axleOnSurface.Cross(n), forward);
                const Vec3 relative = wheel.point - centerOfMass;
                const Vec3 contactVelocity = startVelocity + startAngular.Cross(relative);

                // the tyre holds the car against the sideways speed at the contact point: the impulse is a share of the one that stops it
                const Vec3 lever = relative.Cross(axleOnSurface);
                const float effectiveMass = 1.0f / (invMass + lever.Dot(inverseInertia.Multiply3x3(lever)));
                const float sideImpulse = -(T.frictionDamping * 0.5f) * contactVelocity.Dot(axleOnSurface) * effectiveMass * latFriction;

                float rolling = 0.0f;
                if (tyreEngine == 0.0f)
                {
                    if (tyreBrake != 0.0f)
                    {
                        rolling = Clamp(-T.brakeStiffness * contactVelocity.Dot(rollDir), -tyreBrake, tyreBrake);
                    }
                }
                else
                {
                    rolling = tyreEngine;  // the drive pushes along the way the wheel points
                }
                tyreImpulse[w] = rollDir * (rolling * longFriction) + axleOnSurface * sideImpulse;
                // RL applies the tyre forces at the height of the centre of mass: they do not tip the car over
                tyrePoint[w] = centerOfMass + (relative - up * relative.Dot(up));
            }
        }

        rt.tyreSteer = steerAngle;
        rt.tyreEngine = engineImpulse;
        rt.tyreBrake = brakeMaxImpulse;
        for (int w = 0; w < kWheelCount; ++w)
        {
            rt.tyreLat[w] = nextLat[w];
            rt.tyreLong[w] = nextLong[w];
        }

        // the pull into the surface (RocketSim "sticky force"): half a gravity always, and in proportion to the steepness of the surface
        // another one while the car has the throttle or moves; on the floor that is the weight of the car, on a wall what holds it there
        Vec3 stickImpulse = Vec3::sZero();
        if (contacts > 0)
        {
            const bool fullStick = realThrottle != 0.0f || absForwardSpeed > T.stickStopSpeed;
            float share = T.stickBase;
            if (fullStick)
            {
                share += T.stickSteep * (1.0f - std::fabs(surfaceNormal.GetY()));
            }
            stickImpulse = -surfaceNormal * (mass * ctx.tuning.gravity * share * dt);
        }

        // ---- 5. the auto-roll: with the throttle, while the car does not stand on all four wheels or the hitbox touches a surface,
        // a pull into the surface and a torque that turns the wheels to it (RocketSim Car::_UpdateAutoRoll)
        Vec3 autoRollImpulse = Vec3::sZero();
        if (input.throttle != 0.0f && ((contacts > 0 && contacts < 4) || hasBodyContact))
        {
            const Vec3 groundUp = contacts > 0 ? surfaceNormal : bodyNormal;
            const Vec3 groundDown = -groundUp;
            const Vec3 crossRight = groundUp.Cross(forward);
            const Vec3 crossForward = groundDown.Cross(crossRight);
            const float rightFactor = 1.0f - Clamp(right.Dot(crossRight), 0.0f, 1.0f);
            const float forwardFactor = 1.0f - Clamp(forward.Dot(crossForward), 0.0f, 1.0f);
            const Vec3 torqueRight = forward * (right.Dot(groundUp) >= 0.0f ? -1.0f : 1.0f) * rightFactor;
            const Vec3 torqueForward = right * (forward.Dot(groundUp) >= 0.0f ? 1.0f : -1.0f) * forwardFactor;
            autoRollImpulse = groundDown * (mass * T.autoRollForce * dt);
            angularAdd += (torqueForward + torqueRight) * (T.autoRollTorque * dt);
        }

        // ---- apply what the tick worked out
        if (velocityChanged)
        {
            body.SetLinearVelocityClamped(velocity);
        }
        for (int w = 0; w < kWheelCount; ++w)
        {
            if (!wheels[w].contact)
            {
                continue;
            }
            body.AddImpulse(springImpulse[w], wheels[w].point);
            body.AddImpulse(tyreImpulse[w], tyrePoint[w]);
        }
        if (contacts > 0)
        {
            body.AddImpulse(stickImpulse);
        }
        body.AddImpulse(autoRollImpulse);

        // ---- 6. in the air: the flip spin and the air control (RocketSim Car::_UpdateAirTorque)
        Vec3 local = basis.Multiply3x3Transposed(body.GetAngularVelocity() + angularAdd);
        bool turn = angularAdd.LengthSq() > 0.0f;
        const bool startedNow = rt.dodging != 0 && !dodgingAtStart;
        if (onGround)
        {
            rt.dodging = 0;
        }
        else
        {
            bool airControl = false;
            const bool flipping = dodgingAtStart && flippedAtStart && flipTimeAtStart < T.dodgeTorqueTime;
            if (flipping)
            {
                if (rt.flipPitch != 0.0f || rt.flipRoll != 0.0f)
                {
                    // flip cancel: the pitch stick against the flip takes its pitch spin away (and gives the air control back)
                    float pitchScale = 1.0f;
                    if (rt.flipPitch != 0.0f && stickPitch != 0.0f && Sign(rt.flipPitch) == Sign(stickPitch))
                    {
                        pitchScale = 1.0f - std::min(std::fabs(stickPitch), 1.0f);
                        airControl = true;
                    }
                    local += Vec3(rt.flipPitch * pitchScale * T.dodgeTorquePitch, 0.0f, rt.flipRoll * T.dodgeTorqueRoll) * dt;
                    turn = true;
                }
                else
                {
                    airControl = true;  // a stall: the dodge had no direction
                }
            }
            else
            {
                airControl = true;
            }
            airControl = airControl && rt.autoFlipping == 0;
            if (airControl && contacts == 0)
            {
                float pitchScale = 1.0f;
                Vec3 torque = Vec3::sZero();
                if (stickPitch != 0.0f || stickYaw != 0.0f || stickRoll != 0.0f)
                {
                    if (flipping)
                    {
                        pitchScale = 0.0f;
                    }
                    else if (flippedAtStart && flipTimeAtStart < T.dodgeTorqueTime + T.dodgePitchLockExtra)
                    {
                        pitchScale = 0.0f;  // the pitch stick stays off a moment after a flip
                    }
                    torque = Vec3(-stickPitch * pitchScale * T.airPitchTorque, stickYaw * T.airYawTorque, -stickRoll * T.airRollTorque);
                }
                // the damping of pitch and yaw fades out with the stick, the one of roll does not
                const Vec3 damping(T.airPitchDamping * local.GetX() * (1.0f - std::fabs(stickPitch * pitchScale)),
                                   T.airYawDamping * local.GetY() * (1.0f - std::fabs(stickYaw)), T.airRollDamping * local.GetZ());
                local += (torque - damping) * dt;
                turn = true;
            }
            rt.dodging = (flipping || startedNow) ? 1 : 0;

            // the dodge's clock and what it does to the vertical speed: the flip hangs in the air
            if (rt.dodging != 0)
            {
                rt.flipTime += dt;
                if (rt.flipTime <= T.dodgeTorqueTime && rt.flipTime >= T.dodgeZDampStart)
                {
                    Vec3 v = body.GetLinearVelocity();
                    if (v.GetY() < 0.0f || rt.flipTime < T.dodgeZDampEnd)
                    {
                        v.SetY(v.GetY() * std::pow(1.0f - T.dodgeZDamp, dt * static_cast<float>(kTickRate)));
                        body.SetLinearVelocityClamped(v);
                    }
                }
            }
            else if (rt.flipped != 0)
            {
                rt.flipTime += dt;  // counted on: the pitch lock after the flip needs it
            }

            if (throttleIn != 0.0f)
            {
                body.SetLinearVelocityClamped(body.GetLinearVelocity() + forward * (throttleIn * T.airThrottleAccel * dt));
            }
        }
        if (turn)
        {
            // not limited here: RocketSim limits the angular speed after the step, so a flip that is driven at the limit still turns a
            // tick's worth of its torque faster than the limit (5.5 + 1.9 rad/s)
            body.SetAngularVelocity(basis.Multiply3x3(local));
        }

        // ---- 7. boost (RocketSim Car::_UpdateBoost): a started boost runs at least boostMinTime, along the nose of the car
        {
            if (rt.boost > 0.0f)
            {
                if (rt.boosting != 0)
                {
                    rt.boosting = (input.boost || rt.boostingTime < T.boostMinTime) ? 1 : 0;
                }
                else if (input.boost)
                {
                    rt.boosting = 1;
                }
            }
            else
            {
                rt.boosting = 0;
            }
            rt.boostingTime = rt.boosting != 0 ? rt.boostingTime + dt : 0.0f;
            if (rt.boosting != 0)
            {
                rt.boost = std::max(rt.boost - T.boostUsePerSecond * dt, 0.0f);
                const float accel = onGround ? T.boostAccelGround : T.boostAccelAir;
                body.SetLinearVelocityClamped(body.GetLinearVelocity() + forward * (accel * dt));
            }
        }
    }
}
