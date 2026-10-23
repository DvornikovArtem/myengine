// RlWheels.h
// The look of the wheels of the cars: the spin about the axle and the turn of the front pair. The simulation keeps neither (a wheel in it is a ray with
// a spring), so the picture works them out from what every machine of a match has: the state of the car. The local player, a bot, a car that the network
// brings and a car of a replay all arrive as an RlCarState, so one class animates them all (docs/game/rl-cars.md, "Колёса").
// Angles are radians in the frame of RlTypes.h: a positive spin is the top of the wheel moving forward (the car rolls forward), a positive steer
// turns the front of the wheel to the right.

#pragma once

#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

namespace rl
{
    struct RlWheelPose
    {
        float spin = 0.0f;   // about the axle (the car's X), wrapped to -pi..pi
        float steer = 0.0f;  // about the car's up; always 0 for the rear wheels
    };

    class RlWheelAnimator
    {
    public:
        // Wheel indices are those of RlCarState::wheelCompression: 0 front left, 1 front right, 2 rear left, 3 rear right.
        static bool IsFront(int wheel) { return wheel < 2; }
        static bool IsRight(int wheel) { return (wheel & 1) != 0; }

        // The wheels of every car stand still and straight.
        void Reset();
        // The wheels of one car stand still and straight (a car put on a kick-off spot, a car that comes back after a demolition).
        void ResetCar(int slot);

        // Moves the wheels of one car on by `dt` seconds of the picture. `car` is the state to show (already interpolated) and `tuning` the numbers
        // of the car of the slot (wheel radii and places, the largest steering angle). dt <= 0 changes nothing, so a paused picture stays still.
        //
        // Wheels on the ground roll without slipping: the speed of the wheel along its heading over its radius. Wheels in the air keep the spin they
        // had and slow down with a time constant of kAirSpinDecay. The front wheels turn to the angle that makes the car yaw as fast as it does
        // (the bicycle model, tan(steer) = yaw rate * wheelbase / speed), which needs speed: a car that stands keeps its wheels straight.
        void Update(int slot, const RlCarState& car, const RlCarTuning& tuning, float dt);

        RlWheelPose Pose(int slot, int wheel) const;

        // Seconds in which the spin of a wheel in the air falls to 1/e.
        static constexpr float kAirSpinDecay = 1.5f;

    private:
        struct CarWheels
        {
            float spin[kWheelCount] = {};
            float omega[kWheelCount] = {};  // rad/s, positive forward
            float steer = 0.0f;
        };
        CarWheels cars[kMaxCars];
    };
}
