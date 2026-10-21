// RlCarController.cpp

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

        const Vec3 position = body.GetPosition();
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
            float distance;
            bool contact;
        };
        Wheel wheels[kWheelCount];
        int contacts = 0;
        Vec3 normalSum = Vec3::sZero();
        const float rayLength = T.suspensionRest + T.suspensionContactMargin;
        const JPH::SpecifiedBroadPhaseLayerFilter broadFilter(BroadPhase::kStatic);
        const JPH::SpecifiedObjectLayerFilter objectFilter(Layers::kStatic);
        for (int w = 0; w < kWheelCount; ++w)
        {
            const float side = (w & 1) ? 1.0f : -1.0f;  // 0 front left, 1 front right, 2 rear left, 3 rear right
            const float z = (w < 2) ? T.wheelFrontZ : T.wheelRearZ;
            Wheel& wheel = wheels[w];
            wheel.origin = position + basis.Multiply3x3(Vec3(side * T.wheelMountX, T.wheelMountYOffset, z));
            wheel.contact = false;
            wheel.distance = rayLength;
            wheel.normal = up;
            const JPH::RRayCast ray(wheel.origin, -up * rayLength);
            JPH::RayCastResult hit;
            if (ctx.physics.GetNarrowPhaseQuery().CastRay(ray, hit, broadFilter, objectFilter))
            {
                wheel.contact = true;
                wheel.distance = hit.mFraction * rayLength;
                wheel.normal = ctx.arenaShape->GetSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));
                ++contacts;
                normalSum += wheel.normal;
            }
            frame.wheelCompression[w] = 0.0f;
        }
        const Vec3 surfaceNormal = contacts > 0 ? SafeNormalized(normalSum, up) : Vec3::sZero();
        frame.wheelContacts = contacts;
        frame.surfaceNormal = surfaceNormal;
        rt.wheelContacts = contacts;

        rt.sinceJump += dt;
        rt.ballHitCooldown = std::max(rt.ballHitCooldown - 1, 0);

        // ---- 2. what the jump clock says: the ground gives the jump back, leaving it (or the roof) starts the flip window
        const bool onRoof = contacts == 0 && rt.bodyContact != 0 && up.GetY() < 0.0f;
        if (contacts >= 1)
        {
            if (rt.sinceJump > 0.12f)
            {
                rt.jumpUsed = 0;
                rt.secondUsed = 0;
                rt.airTime = 0.0f;
                rt.dodging = 0;
                rt.dodgeSpinLeft = 0.0f;
            }
        }
        else if (rt.jumpUsed == 0)
        {
            rt.jumpUsed = 1;  // drove off an edge: the first jump is gone, a second one is still allowed
            rt.secondUsed = 0;
            rt.airTime = 0.0f;
        }
        if (onRoof)
        {
            rt.jumpUsed = 1;
            rt.secondUsed = 0;
            rt.airTime = 0.0f;
        }
        if (rt.jumpUsed != 0)
        {
            rt.airTime += dt;
        }

        // ---- 3. jump, double jump, dodge
        const bool jumpPressed = input.jump && rt.jumpPrev == 0;
        rt.jumpPrev = input.jump ? 1 : 0;
        if (jumpPressed)
        {
            if (onRoof || (rt.jumpUsed == 0 && contacts >= 1))
            {
                // from the wheels the car jumps along its own up, from the roof it hops straight up and keeps its flip
                const Vec3 direction = onRoof ? worldUp : up;
                rt.jumpDirX = direction.GetX();
                rt.jumpDirY = direction.GetY();
                rt.jumpDirZ = direction.GetZ();
                body.SetLinearVelocityClamped(body.GetLinearVelocity() + direction * T.jumpImpulse);
                rt.jumpUsed = 1;
                rt.secondUsed = 0;
                rt.airTime = 0.0f;
                rt.sinceJump = 0.0f;
                rt.jumpHold = T.jumpHoldTime;
            }
            else if (rt.jumpUsed != 0 && rt.secondUsed == 0 && rt.airTime < T.dodgeWindow)
            {
                rt.secondUsed = 1;
                rt.jumpHold = 0.0f;
                const float forwardInput = -input.pitch;
                const float sideInput = input.steer;
                const float magnitude = std::sqrt(forwardInput * forwardInput + sideInput * sideInput);
                Vec3 velocity = body.GetLinearVelocity();
                if (magnitude < T.dodgeDeadzone)
                {
                    const Vec3 jumpDirection = up.GetY() >= 0.0f ? up : worldUp;
                    velocity += jumpDirection * T.doubleJumpImpulse;
                }
                else
                {
                    const float df = forwardInput / magnitude;
                    const float ds = sideInput / magnitude;
                    const Vec3 forwardFlat = SafeNormalized(Vec3(forward.GetX(), 0.0f, forward.GetZ()), Vec3::sAxisZ());
                    const Vec3 rightFlat = worldUp.Cross(forwardFlat);
                    const float speedAhead = std::max(velocity.Dot(forwardFlat), 0.0f);
                    const float bonus = df > 0.0f ? 1.0f + T.dodgeForwardSpeedBonus * Clamp(speedAhead / T.maxSpeed, 0.0f, 1.0f) : 1.0f;
                    velocity += forwardFlat * (T.dodgeImpulse * df * bonus) + rightFlat * (T.dodgeImpulse * ds);
                    if (velocity.GetY() > 0.0f)
                    {
                        velocity.SetY(velocity.GetY() * (1.0f - T.dodgeVerticalCancel * std::fabs(df)));
                    }
                    rt.dodging = 1;
                    rt.dodgeSpinLeft = T.dodgeSpinTime;
                    rt.dodgeLock = T.dodgeLockTime;
                    rt.dodgeSpinX = T.dodgeSpin * df;    // + about X pitches the nose down: a front flip for df > 0
                    rt.dodgeSpinZ = -T.dodgeSpin * ds;   // + about Z rolls left: a dodge to the right rolls the other way
                }
                body.SetLinearVelocityClamped(velocity);
            }
        }
        if (rt.jumpHold > 0.0f)
        {
            if (input.jump)
            {
                body.SetLinearVelocityClamped(body.GetLinearVelocity() + Vec3(rt.jumpDirX, rt.jumpDirY, rt.jumpDirZ) * (T.jumpHoldAccel * dt));
                rt.jumpHold -= dt;
            }
            else
            {
                rt.jumpHold = 0.0f;
            }
        }

        // ---- 4. suspension and grip of every wheel on the ground
        float driveScale = 0.0f;
        if (contacts > 0)
        {
            driveScale = std::min(static_cast<float>(contacts), 3.0f) / 3.0f;
            const Vec3 rightOnSurface = SafeNormalized(right - surfaceNormal * right.Dot(surfaceNormal), right);
            const Vec3 forwardOnSurface = SafeNormalized(rightOnSurface.Cross(surfaceNormal), forward);
            const float forwardSpeed = body.GetLinearVelocity().Dot(forwardOnSurface);

            // Steering turns the front wheels (a bicycle model); the car then turns because the tyres do not slide sideways.
            // The angle comes from RL's curvature-by-speed table: tan(angle) = curvature * wheelbase.
            const float wheelBase = std::max(T.wheelFrontZ - T.wheelRearZ, 0.1f);
            const float curvature = Interp(T.curvatureSpeed, T.curvatureValue, 6, std::fabs(forwardSpeed)) * (input.handbrake ? T.handbrakeSteerScale : 1.0f);
            const float steerAngle = Clamp(std::atan(Clamp(input.steer, -1.0f, 1.0f) * curvature * wheelBase), -T.maxSteerAngle, T.maxSteerAngle);
            const float steerSin = std::sin(steerAngle);
            const float steerCos = std::cos(steerAngle);
            const float wheelMass = mass * 0.25f;
            for (int w = 0; w < kWheelCount; ++w)
            {
                const Wheel& wheel = wheels[w];
                if (!wheel.contact)
                {
                    continue;
                }
                const float compression = std::max(T.suspensionRest - wheel.distance, 0.0f);
                frame.wheelCompression[w] = compression;
                if (compression > 0.0f)
                {
                    const float approach = body.GetPointVelocity(wheel.origin).Dot(-up);
                    const float force = std::max(T.springStiffness * compression + T.springDamping * approach, 0.0f);
                    body.AddImpulse(up * (force * dt), wheel.origin);
                }

                // the sideways axis of this tyre on the surface it stands on
                const bool front = w < 2;
                const Vec3 n = wheel.normal;
                const Vec3 wheelRight = SafeNormalized(right - n * right.Dot(n), right);
                Vec3 sideAxis = wheelRight;
                if (front)
                {
                    const Vec3 wheelForward = SafeNormalized(wheelRight.Cross(n), forward);
                    sideAxis = wheelRight * steerCos - wheelForward * steerSin;
                }
                // the handbrake lets go of the rear tyres: the tail slides
                const float grip = (!front && input.handbrake) ? T.handbrakeLateralScale : 1.0f;
                const float sideSpeed = body.GetPointVelocity(wheel.origin).Dot(sideAxis);
                const float cap = wheelMass * T.lateralAccel * grip * dt;
                const float impulse = Clamp(-sideSpeed * wheelMass * T.lateralGain, -cap, cap);
                body.AddImpulse(sideAxis * impulse, wheel.origin);
            }

            // throttle, brake, coasting along the surface
            Vec3 velocity = body.GetLinearVelocity();
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
            velocity += forwardOnSurface * (deltaSpeed * driveScale);
            body.SetLinearVelocityClamped(velocity);

            if (contacts >= 3)
            {
                // pitch and roll about the surface die out: the car settles on its springs
                const Vec3 angular = body.GetAngularVelocity();
                const float keep = std::exp(-T.groundAngularDamping * dt);
                const Vec3 along = surfaceNormal * angular.Dot(surfaceNormal);
                body.SetAngularVelocityClamped(along + (angular - along) * keep);
            }

            // walls and ceiling: a pull into the surface, as strong as the surface is steep
            if (static_cast<float>(contacts) >= T.stickMinContacts && rt.sinceJump > T.jumpStickGrace)
            {
                const float steep = Clamp(1.0f - surfaceNormal.GetY(), 0.0f, 1.0f);
                body.AddImpulse(-surfaceNormal * (mass * T.stickAccel * steep * dt));
            }
        }

        // ---- 5. in the air: the flip spin, then the air control with its damping
        if (contacts >= 2)
        {
            rt.dodging = 0;
            rt.dodgeSpinLeft = 0.0f;
            rt.dodgeLock = 0.0f;
        }
        if (contacts == 0)
        {
            Vec3 local = basis.Multiply3x3Transposed(body.GetAngularVelocity());
            if (rt.dodging != 0 && rt.dodgeSpinLeft > 0.0f)
            {
                local.SetX(rt.dodgeSpinX);
                local.SetZ(rt.dodgeSpinZ);
                rt.dodgeSpinLeft -= dt;
                if (rt.dodgeSpinLeft <= 0.0f)
                {
                    rt.dodging = 0;
                }
            }
            else
            {
                const bool locked = rt.dodgeLock > 0.0f;
                float pitch = locked ? 0.0f : Clamp(input.pitch, -1.0f, 1.0f);
                float yaw = locked ? 0.0f : Clamp(input.steer, -1.0f, 1.0f);
                float roll = locked ? 0.0f : Clamp(input.roll, -1.0f, 1.0f);
                if (input.airRoll && !locked)
                {
                    roll = Clamp(roll + input.steer, -1.0f, 1.0f);
                    yaw = 0.0f;
                }
                const float ax = -pitch * T.airPitchTorque - T.airPitchDamping * local.GetX() * (1.0f - std::fabs(pitch));
                const float ay = yaw * T.airYawTorque - T.airYawDamping * local.GetY() * (1.0f - std::fabs(yaw));
                const float az = -roll * T.airRollTorque - T.airRollDamping * local.GetZ() * (1.0f - std::fabs(roll));
                local += Vec3(ax, ay, az) * dt;
            }
            body.SetAngularVelocityClamped(basis.Multiply3x3(local));
        }
        rt.dodgeLock = std::max(rt.dodgeLock - dt, 0.0f);

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
        Vec3 velocity = body.GetLinearVelocity();
        const float speed = velocity.Length();
        if (speed > T.maxSpeed)
        {
            body.SetLinearVelocityClamped(velocity * (T.maxSpeed / speed));
        }
    }
}
