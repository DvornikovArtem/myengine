// RlCarController.cpp
//
// The model of the car follows RocketSim (https://github.com/ZealanL/RocketSim, MIT license, Copyright (c) 2022 ZealanL): the air control
// as an angular acceleration in the car's axes (Car::_UpdateAirTorque), jump, double jump and dodge (Car::_UpdateJump,
// Car::_UpdateDoubleJumpOrFlip, from the dodge notes of RLUtilities by Samuel P. Mish), the suspension of four rays (btVehicleRL) and the
// pull into the surface (Car::_UpdateWheels, "sticky force"). The numbers live in RlTuning.h; this file writes the formulas in the
// units and axes of this engine (metres, Y up, the car's +Z forward, +X right).

#include "RlCarController.h"

#include <algorithm>
#include <cmath>

namespace rl::detail
{
    namespace
    {
        using JPH::Mat44;
        using JPH::Vec3;

        float Clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
        float Sign(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

        // piecewise linear table, clamped at both ends
        float Interp(const float* xs, const float* ys, int count, float x)
        {
            if (x <= xs[0])
            {
                return ys[0];
            }
            for (int i = 1; i < count; ++i)
            {
                if (x <= xs[i])
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

        float ThrottleAccel(const RlCarTuning& t, float speedAlong)
        {
            if (speedAlong >= t.throttleMaxSpeed)
            {
                return 0.0f;
            }
            const float f = std::max(speedAlong, 0.0f) / t.throttleMaxSpeed;
            return t.throttleAccel + (t.throttleAccelAtMax - t.throttleAccel) * f;
        }
    }

    void ControlCar(const CarContext& ctx, JPH::Body& body, const RlCarInput& input, CarRuntime& rt, CarFrame& frame)
    {
        const RlCarTuning& T = ctx.tuning.car;
        const float dt = kTickDt;
        const float mass = T.mass;

        const Vec3 position = body.GetPosition();  // the origin of the body: the centre of the hitbox
        const Mat44 basis = Mat44::sRotation(body.GetRotation());
        const Vec3 right = basis.GetAxisX();
        const Vec3 up = basis.GetAxisY();
        const Vec3 forward = basis.GetAxisZ();
        const Vec3 worldUp = Vec3::sAxisY();

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

        // The powerslide button is the air roll in the air: held through the landing it is not a powerslide until it is let go (and pressed
        // again), or every landing with Shift held would spin the car round. The grip of the rear tyres falls fast with the slide and
        // comes back over `handbrakeRecoverTime`, so a car that comes out of a slide is not still on the loose.
        if (contacts == 0)
        {
            rt.slideLatch = input.handbrake ? 1 : 0;
        }
        else if (!input.handbrake)
        {
            rt.slideLatch = 0;
        }
        const bool slide = input.handbrake && rt.slideLatch == 0;
        {
            const float target = slide ? T.handbrakeLateralScale : 1.0f;
            if (target < rt.gripScale)
            {
                rt.gripScale = std::max(target, rt.gripScale - dt / 0.08f);
            }
            else
            {
                rt.gripScale = std::min(target, rt.gripScale + dt * (1.0f - T.handbrakeLateralScale) / std::max(T.handbrakeRecoverTime, 1e-3f));
            }
        }
        // a touchdown: stronger side grip for a moment, the yaw left over from the air dies out
        if (onGround && rt.wasOnSurface == 0)
        {
            rt.landingTimer = T.landingGripTime;
        }
        rt.wasOnSurface = onGround ? 1 : 0;
        const bool landing = rt.landingTimer > 0.0f;
        rt.landingTimer = std::max(rt.landingTimer - dt, 0.0f);
        rt.ballHitCooldown = std::max(rt.ballHitCooldown - 1, 0);

        // lying on the roof or on a side: the hitbox rests on a surface, the wheels do not (or hardly) touch it and the car's up is not its normal
        const Vec3 bodyNormal = SafeNormalized(Vec3(rt.bodyNormalX, rt.bodyNormalY, rt.bodyNormalZ), worldUp);
        const bool lying = rt.bodyContact != 0 && contacts <= 1 && up.Dot(bodyNormal) < 0.6f && rt.dodging == 0;

        // the player's stick in the three axes: the powerslide button turns the steering into a roll
        const float stickPitch = Clamp(input.pitch, -1.0f, 1.0f);
        float stickYaw = Clamp(input.steer, -1.0f, 1.0f);
        float stickRoll = Clamp(input.roll, -1.0f, 1.0f);
        if (input.airRoll)
        {
            stickRoll = Clamp(stickRoll + stickYaw, -1.0f, 1.0f);
            stickYaw = 0.0f;
        }

        // ---- 2. the jump (RocketSim Car::_UpdateJump): an instant speed along the up of the car, then a push while the button is held
        const bool jumpPressed = input.jump && rt.jumpPrev == 0;
        rt.jumpPrev = input.jump ? 1 : 0;
        Vec3 velocity = body.GetLinearVelocity();
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
            rt.selfRight = 0;
            rt.rightSpinLeft = 0.0f;
        }
        bool jumpConsumed = false;
        if (jumpPressed && lying)
        {
            // Lying on the roof or a side: the jump hops the car off the surface and turns it back onto its wheels the short way,
            // an impulse and a spin about the axis that carries its up to the surface normal (the spin is a velocity like the one of a
            // dodge; it runs as long as the car needs and the air damping takes the rest).
            jumpConsumed = true;
            const Vec3 n = bodyNormal;
            Vec3 axis = up.Cross(n);
            if (axis.Length() < 0.25f)
            {
                // upside down: there is no short way, roll to the side of the steering (to the left without it)
                axis = forward * (input.steer > 0.2f ? -1.0f : 1.0f);
            }
            axis = SafeNormalized(axis, forward);
            const float angle = std::acos(Clamp(up.Dot(n), -1.0f, 1.0f));
            const Vec3 local = basis.Multiply3x3Transposed(axis) * T.selfRightSpin;
            const float weightX = std::fabs(local.GetX());
            const float weightZ = std::fabs(local.GetZ());
            const float damping = (weightX * T.airPitchDamping + weightZ * T.airRollDamping) / std::max(weightX + weightZ, 1e-3f);
            const float coast = T.selfRightSpin / std::max(damping, 0.5f);  // the angle the air damping adds after the spin
            rt.selfRight = 1;
            rt.rightSpinLeft = Clamp((angle - coast) / T.selfRightSpin, 0.05f, 0.9f);
            rt.rightSpinX = local.GetX();
            rt.rightSpinZ = local.GetZ();
            velocity += n * T.selfRightHop;
            velocityChanged = true;
            rt.jumpUsed = 1;
            rt.secondUsed = 0;
            rt.isJumping = 0;
            rt.airTime = 0.0f;
            rt.jumpTime = 0.0f;
        }

        if (rt.isJumping != 0)
        {
            rt.isJumping = (rt.jumpTime < T.jumpMinTime || (input.jump && rt.jumpTime < T.jumpHoldTime)) ? 1 : 0;
        }
        else if (!jumpConsumed && onGround && jumpPressed)
        {
            jumpConsumed = true;
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

            if (jumpPressed && !jumpConsumed && rt.airTime < T.dodgeWindow && rt.secondUsed == 0 && rt.selfRight == 0)
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
                        const float forwardSpeed = velocity.Dot(forward);
                        const float speedRatio = std::fabs(forwardSpeed) / T.maxSpeed;
                        bool backwards;
                        if (std::fabs(forwardSpeed) < 1.0f)
                        {
                            backwards = dirForward < 0.0f;
                        }
                        else
                        {
                            backwards = (dirForward >= 0.0f) != (forwardSpeed >= 0.0f);
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

        // ---- 4. suspension and grip of every wheel on the ground
        float driveScale = 0.0f;
        const float gravity = ctx.tuning.gravity;
        if (contacts > 0)
        {
            if (velocityChanged)
            {
                body.SetLinearVelocityClamped(velocity);
                velocityChanged = false;
            }
            driveScale = std::min(static_cast<float>(contacts), 3.0f) / 3.0f;
            const Vec3 rightOnSurface = SafeNormalized(right - surfaceNormal * right.Dot(surfaceNormal), right);
            const Vec3 forwardOnSurface = SafeNormalized(rightOnSurface.Cross(surfaceNormal), forward);
            const float forwardSpeed = body.GetLinearVelocity().Dot(forwardOnSurface);

            // Steering turns the front wheels (a bicycle model); the car then turns because the tyres do not slide sideways.
            // The angle comes from RL's curvature-by-speed table: tan(angle) = curvature * wheelbase.
            const float wheelBase = std::max(T.wheelFrontZ - T.wheelRearZ, 0.1f);
            const float curvature = Interp(T.curvatureSpeed, T.curvatureValue, 6, std::fabs(forwardSpeed)) * (slide ? T.handbrakeSteerScale : 1.0f);
            const float steerAngle = Clamp(std::atan(Clamp(input.steer, -1.0f, 1.0f) * curvature * wheelBase), -T.maxSteerAngle, T.maxSteerAngle);
            const float steerSin = std::sin(steerAngle);
            const float steerCos = std::cos(steerAngle);
            const float wheelMass = mass * 0.25f;
            const Vec3 centerOfMass = Vec3(body.GetCenterOfMassPosition());
            Vec3 springImpulse[kWheelCount];
            Vec3 sideImpulse[kWheelCount];
            Vec3 sideAt[kWheelCount];
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
                    springImpulse[w] = n * (force * dt);
                }

                // the sideways axis of this tyre on the surface it stands on
                const Vec3 wheelRight = SafeNormalized(right - n * right.Dot(n), right);
                Vec3 sideAxis = wheelRight;
                if (front)
                {
                    const Vec3 wheelForward = SafeNormalized(wheelRight.Cross(n), forward);
                    sideAxis = wheelRight * steerCos - wheelForward * steerSin;
                }
                // the handbrake lets go of the rear tyres: the tail slides
                const float grip = (front ? 1.0f : rt.gripScale) * (landing ? T.landingLateralScale : 1.0f);
                const float sideSpeed = body.GetPointVelocity(wheel.origin).Dot(sideAxis);
                const float cap = wheelMass * T.lateralAccel * grip * dt;
                const float impulse = Clamp(-sideSpeed * wheelMass * (landing ? 1.0f : T.lateralGain), -cap, cap);
                // RL applies the tyre forces at the height of the centre of mass: they do not tip the car over
                const Vec3 applyAt = wheel.origin - up * (wheel.origin - centerOfMass).Dot(up);
                sideImpulse[w] = sideAxis * impulse;
                sideAt[w] = applyAt;
            }
            // Every impulse of the tick is worked out from the velocities the tick started with (RocketSim computes them in
            // btVehicleRL::updateVehicleFirst); applied one by one the first wheel's impulse would change what the next one sees.
            for (int w = 0; w < kWheelCount; ++w)
            {
                if (wheels[w].contact)
                {
                    body.AddImpulse(springImpulse[w], wheels[w].point);
                    body.AddImpulse(sideImpulse[w], sideAt[w]);
                }
            }

            // throttle, brake, coasting along the surface
            Vec3 ground = body.GetLinearVelocity();
            float deltaSpeed = 0.0f;
            const float throttle = Clamp(input.throttle, -1.0f, 1.0f);
            if (std::fabs(throttle) > 0.01f)
            {
                const float direction = throttle > 0.0f ? 1.0f : -1.0f;
                if (forwardSpeed * direction >= -0.1f)
                {
                    deltaSpeed = direction * ThrottleAccel(T, forwardSpeed * direction) * std::fabs(throttle) * dt;
                }
                else
                {
                    const float brake = std::min(T.brakeAccel * std::fabs(throttle) * dt, std::fabs(forwardSpeed));
                    deltaSpeed = forwardSpeed > 0.0f ? -brake : brake;
                }
            }
            else if (!input.boost)
            {
                const float coast = std::min(T.coastDecel * dt, std::fabs(forwardSpeed));
                deltaSpeed = forwardSpeed > 0.0f ? -coast : coast;
            }
            ground += forwardOnSurface * (deltaSpeed * driveScale);
            body.SetLinearVelocityClamped(ground);

            if (landing && onGround && std::fabs(input.steer) < 0.1f)
            {
                // the yaw rate left over from the air: without steering the car goes where its nose points
                const Vec3 angular = body.GetAngularVelocity();
                const Vec3 along = surfaceNormal * angular.Dot(surfaceNormal);
                body.SetAngularVelocityClamped(angular - along * (1.0f - std::exp(-T.landingYawDamping * dt)));
            }
            if (onGround && T.groundAngularDamping > 0.0f)
            {
                // pitch and roll about the surface die out
                const Vec3 angular = body.GetAngularVelocity();
                const float keep = std::exp(-T.groundAngularDamping * dt);
                const Vec3 along = surfaceNormal * angular.Dot(surfaceNormal);
                body.SetAngularVelocityClamped(along + (angular - along) * keep);
            }

            // The pull into the surface (RocketSim "sticky force"): half a gravity always, and in proportion to the steepness of the
            // surface another one while the car has the throttle or moves. On the floor that is the weight of the car, on a wall what
            // holds it there; a car that stands still on a wall has only the half and slides down.
            {
                const float throttleNow = input.boost && rt.boost > 0.0f ? 1.0f : throttle;
                const bool fullStick = std::fabs(throttleNow) > 0.001f || std::fabs(forwardSpeed) > T.stickStopSpeed;
                float share = T.stickBase;
                if (fullStick)
                {
                    share += T.stickSteep * (1.0f - std::fabs(surfaceNormal.GetY()));
                }
                body.AddImpulse(-surfaceNormal * (mass * gravity * share * dt));
            }
            velocity = body.GetLinearVelocity();
        }
        else if (velocityChanged)
        {
            body.SetLinearVelocityClamped(velocity);
            velocityChanged = false;
        }

        // ---- 5. in the air: the flip spin and the air control (RocketSim Car::_UpdateAirTorque)
        if (onGround)
        {
            rt.dodging = 0;
        }
        else
        {
            Vec3 local = basis.Multiply3x3Transposed(body.GetAngularVelocity());
            bool turn = false;

            if (rt.selfRight != 0)
            {
                // the car lying on its roof or side is turned back by a spin that runs as long as it needs
                if (contacts >= 2 || rt.rightSpinLeft <= 0.0f)
                {
                    rt.selfRight = 0;
                    rt.rightSpinLeft = 0.0f;
                }
                else
                {
                    local.SetX(rt.rightSpinX);
                    local.SetZ(rt.rightSpinZ);
                    rt.rightSpinLeft -= dt;
                    turn = true;
                }
            }

            bool airControl = false;
            if (rt.dodging != 0)
            {
                rt.dodging = (rt.flipped != 0 && rt.flipTime < T.dodgeTorqueTime) ? 1 : 0;
            }
            if (rt.selfRight == 0)
            {
                if (rt.dodging != 0)
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
            }
            if (airControl && contacts == 0)
            {
                float pitchScale = 1.0f;
                Vec3 torque = Vec3::sZero();
                if (stickPitch != 0.0f || stickYaw != 0.0f || stickRoll != 0.0f)
                {
                    if (rt.dodging != 0)
                    {
                        pitchScale = 0.0f;
                    }
                    else if (rt.flipped != 0 && rt.flipTime < T.dodgeTorqueTime + T.dodgePitchLockExtra)
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
            if (turn)
            {
                body.SetAngularVelocityClamped(basis.Multiply3x3(local));
            }

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

            const float throttleAir = Clamp(input.throttle, -1.0f, 1.0f);
            if (throttleAir != 0.0f)
            {
                body.SetLinearVelocityClamped(body.GetLinearVelocity() + forward * (throttleAir * T.airThrottleAccel * dt));
            }
        }

        // ---- 6. boost
        if (input.boost && rt.boost > 0.0f)
        {
            const float use = std::min(rt.boost, T.boostUsePerSecond * dt);
            const float share = use / (T.boostUsePerSecond * dt);
            rt.boost -= use;
            Vec3 direction = forward;
            float accel = T.boostAccelAir;
            if (contacts > 0)
            {
                const Vec3 rightOnSurface = SafeNormalized(right - surfaceNormal * right.Dot(surfaceNormal), right);
                direction = SafeNormalized(rightOnSurface.Cross(surfaceNormal), forward);
                accel = T.boostAccelGround * driveScale;
            }
            body.SetLinearVelocityClamped(body.GetLinearVelocity() + direction * (accel * share * dt));
        }

        // ---- 7. top speed
        const Vec3 final = body.GetLinearVelocity();
        const float speed = final.Length();
        if (speed > T.maxSpeed)
        {
            body.SetLinearVelocityClamped(final * (T.maxSpeed / speed));
        }
    }
}
