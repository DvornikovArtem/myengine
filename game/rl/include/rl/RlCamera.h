// RlCamera.h
// The camera of Rocket League without a renderer: it follows a car (car cam) or looks over it at the ball (ball cam), its up follows
// the surface under the car so that walls and the ceiling feel like a floor, it swings round smoothly, and it never leaves the arena:
// the way from the car to the camera is cast against the arena mesh and the camera is pulled in.

#pragma once

#include <rl/RlSim.h>
#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

namespace rl
{
    struct RlCameraState
    {
        RlVec3 heading{0.0f, 0.0f, 1.0f};  // the horizontal direction (in the plane of `up`) the camera looks along
        RlVec3 up{0.0f, 1.0f, 0.0f};
        float ballBlend = 0.0f;            // 0 car cam, 1 ball cam
        float groundBlend = 1.0f;          // 1 the car drives on a surface, 0 it is in the air (changes over a quarter of a second)
        float ballPitch = 0.0f;            // the sine of the angle the ball cam looks up (smoothed)
        float pitchOffset = 0.0f;          // radians the camera tilts to keep the car in the picture (a wall, a fall)
        RlVec3 ballHeading{0.0f, 0.0f, 1.0f};  // the horizontal direction of the ball cam (heading is the one of the car cam; the camera looks along a mix of the two)
        float pull = 0.0f;                 // how far the arena has pulled the camera in (0: the whole distance, 1: 0.6 of it)
        bool lowering = false;             // the way that the camera was pulled in by: down to the level of the car (true) or up
        float stretch = 0.0f;              // 0..1: how fast the car goes (the camera is a little farther on speed)
        bool ready = false;
    };

    struct RlCameraInput
    {
        RlVec3 carPosition;
        RlQuat carRotation;
        bool onSurface = false;            // three or more wheels touch a surface
        RlVec3 surfaceNormal{0.0f, 1.0f, 0.0f};
        RlVec3 ballPosition;
        bool ballCam = false;
        RlVec3 carVelocity;                // the direction of the motion decides where the camera looks on a wall and in the air
    };

    struct RlCameraView
    {
        RlVec3 position;
        RlVec3 forward;  // unit vectors: the camera's own axes
        RlVec3 up;
        RlVec3 right;
    };

    // One camera step of `dt` seconds. `sim` is used for the arena ray only.
    RlCameraView RlCameraUpdate(const RlCameraTuning& tuning, RlCameraState& state, const RlSim& sim, const RlCameraInput& input, float dt);
}
