// RlWheels.cpp

#include <rl/RlWheels.h>

#include <algorithm>
#include <cmath>

namespace rl
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        constexpr float kMaxStep = 0.1f;          // seconds: a frame that took longer (a stall, a breakpoint) is not worth a jump of the wheels
        constexpr float kGripTime = 0.03f;        // seconds: how fast a wheel on the ground takes the speed of the road
        constexpr float kSteerTime = 0.05f;       // seconds: how fast the front wheels follow the angle that the yaw rate shows
        constexpr float kSteerSpeedLow = 1.0f;    // m/s: below it the yaw rate says nothing about the wheels, they go straight ...
        constexpr float kSteerSpeedFull = 3.0f;   // ... and from here on the angle is taken whole

        struct Vec
        {
            float x, y, z;
        };

        float Dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

        // q v q^-1: the same numbers turn the vector the same way in Jolt's and in DirectXMath's convention
        Vec Rotate(const RlQuat& q, const Vec& v)
        {
            const Vec u{q.x, q.y, q.z};
            const Vec c1{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};  // u x v
            const Vec c2{u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x};  // u x (u x v)
            return {v.x + 2.0f * (q.w * c1.x + c2.x), v.y + 2.0f * (q.w * c1.y + c2.y), v.z + 2.0f * (q.w * c1.z + c2.z)};
        }

        float Wrap(float angle)
        {
            angle = std::fmod(angle + kPi, 2.0f * kPi);
            if (angle < 0.0f)
            {
                angle += 2.0f * kPi;
            }
            return angle - kPi;
        }
    }

    void RlWheelAnimator::Reset()
    {
        for (CarWheels& car : cars)
        {
            car = CarWheels{};
        }
    }

    void RlWheelAnimator::ResetCar(const int slot)
    {
        if (slot >= 0 && slot < kMaxCars)
        {
            cars[slot] = CarWheels{};
        }
    }

    void RlWheelAnimator::Update(const int slot, const RlCarState& car, const RlCarTuning& tuning, float dt)
    {
        if (slot < 0 || slot >= kMaxCars || !(dt > 0.0f))
        {
            return;
        }
        dt = std::min(dt, kMaxStep);
        CarWheels& wheels = cars[slot];

        const Vec forward = Rotate(car.rotation, {0.0f, 0.0f, 1.0f});
        const Vec right = Rotate(car.rotation, {1.0f, 0.0f, 0.0f});
        const Vec up = Rotate(car.rotation, {0.0f, 1.0f, 0.0f});
        const Vec velocity{car.linearVelocity.x, car.linearVelocity.y, car.linearVelocity.z};
        const Vec angular{car.angularVelocity.x, car.angularVelocity.y, car.angularVelocity.z};
        const float speedForward = Dot(velocity, forward);
        const float speedRight = Dot(velocity, right);
        const float yawRate = Dot(angular, up);  // positive: the nose swings to the right

        bool grounded = false;
        for (int w = 0; w < kWheelCount; ++w)
        {
            grounded = grounded || car.wheelCompression[w] > 0.0f;
        }

        // the front wheels: the angle that gives this yaw rate at this speed, nothing in the air or when the car stands
        float steerTarget = 0.0f;
        const float absSpeed = std::fabs(speedForward);
        if (grounded && absSpeed > kSteerSpeedLow)
        {
            const float wheelbase = std::max(tuning.wheelFrontZ - tuning.wheelRearZ, 0.1f);
            const float limit = std::max(tuning.steerAngle[0], 0.1f);
            const float angle = std::clamp(std::atan(yawRate * wheelbase / speedForward), -limit, limit);
            steerTarget = angle * std::clamp((absSpeed - kSteerSpeedLow) / (kSteerSpeedFull - kSteerSpeedLow), 0.0f, 1.0f);
        }
        wheels.steer += (steerTarget - wheels.steer) * (1.0f - std::exp(-dt / kSteerTime));
        const float steerSin = std::sin(wheels.steer);
        const float steerCos = std::cos(wheels.steer);

        for (int w = 0; w < kWheelCount; ++w)
        {
            const bool front = IsFront(w);
            if (car.wheelCompression[w] > 0.0f)
            {
                // the velocity of the point of the car over the mount: the yaw makes the outer wheel of a bend faster than the inner one
                const float mountX = (IsRight(w) ? 1.0f : -1.0f) * (front ? tuning.wheelMountXFront : tuning.wheelMountXRear);
                const float mountZ = front ? tuning.wheelFrontZ : tuning.wheelRearZ;
                const float alongCar = speedForward - yawRate * mountX;
                const float acrossCar = speedRight + yawRate * mountZ;
                const float alongWheel = front ? alongCar * steerCos + acrossCar * steerSin : alongCar;
                const float radius = std::max(front ? tuning.wheelRadiusFront : tuning.wheelRadiusRear, 0.05f);
                wheels.omega[w] += (alongWheel / radius - wheels.omega[w]) * (1.0f - std::exp(-dt / kGripTime));
            }
            else
            {
                wheels.omega[w] *= std::exp(-dt / kAirSpinDecay);
            }
            wheels.spin[w] = Wrap(wheels.spin[w] + wheels.omega[w] * dt);
        }
    }

    RlWheelPose RlWheelAnimator::Pose(const int slot, const int wheel) const
    {
        RlWheelPose pose;
        if (slot >= 0 && slot < kMaxCars && wheel >= 0 && wheel < kWheelCount)
        {
            pose.spin = cars[slot].spin[wheel];
            pose.steer = IsFront(wheel) ? cars[slot].steer : 0.0f;
        }
        return pose;
    }
}
