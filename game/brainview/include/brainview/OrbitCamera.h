// OrbitCamera.h
// The camera of the 3D brain: it turns slowly around the cloud by itself, the mouse orbits and zooms (and the turning waits a few seconds after the last touch).

#pragma once

#include <myengine/render/RenderTypes.h>

namespace brainview
{
    struct OrbitCamera
    {
        float yaw = 0.6f;                // radians around the long axis of the cloud
        float pitch = 0.18f;             // radians above the horizon
        float distance = 3.3f;           // from the centre; the cloud has a radius of about 1
        float fovY = 0.70f;              // radians
        float centerY = 0.0f;            // the point looked at (the cloud is centred on the origin)
        bool autoRotate = true;
        float autoRotateSpeed = 0.14f;   // radians per second
        float idleSeconds = 10.0f;       // time since the last touch of the mouse

        static constexpr float kResumeAfterSeconds = 3.0f;
        static constexpr float kMinDistance = 1.3f;
        static constexpr float kMaxDistance = 9.0f;

        // Time passes: the camera turns when nobody touches it
        void Advance(double deltaSeconds);
        // The mouse dragged by (dx, dy) pixels
        void Orbit(float dx, float dy);
        // The wheel turned by `steps` notches (positive: closer)
        void Zoom(float steps);
        void Matrices(float aspect, myengine::render::Matrix4& view, myengine::render::Matrix4& projection) const;
    };
}
