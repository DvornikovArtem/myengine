// RlCamera.cpp

#include <rl/RlCamera.h>

#include <algorithm>
#include <cmath>

namespace rl
{
    namespace
    {
        struct V3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        float Len(V3 a) { return std::sqrt(Dot(a, a)); }
        V3 Norm(V3 a, V3 fallback)
        {
            const float l = Len(a);
            return l > 1e-6f ? a * (1.0f / l) : fallback;
        }
        V3 Lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }
        V3 FromRl(const RlVec3& v) { return {v.x, v.y, v.z}; }
        RlVec3 ToRl(V3 v) { return {v.x, v.y, v.z}; }
        // the part of v that lies in the plane with this normal
        V3 OnPlane(V3 v, V3 normal) { return v - normal * Dot(v, normal); }

        V3 Rotate(const RlQuat& q, V3 v)
        {
            const float tx = 2.0f * (q.y * v.z - q.z * v.y);
            const float ty = 2.0f * (q.z * v.x - q.x * v.z);
            const float tz = 2.0f * (q.x * v.y - q.y * v.x);
            return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx)};
        }
    }

    RlCameraView RlCameraUpdate(const RlCameraTuning& t, RlCameraState& state, const RlSim& sim, const RlCameraInput& in, float dt)
    {
        constexpr float kDegToRad = 0.01745329252f;
        const V3 worldUp = {0.0f, 1.0f, 0.0f};
        const V3 carPosition = FromRl(in.carPosition);
        const V3 ball = FromRl(in.ballPosition);
        const bool grounded = in.onSurface;

        // ---- the up of the camera: the surface under the car (slowly), the world up in the air (slower still)
        V3 up = FromRl(state.up);
        const V3 targetUp = grounded ? Norm(FromRl(in.surfaceNormal), worldUp) : worldUp;
        const float upRate = grounded ? t.upFollow : t.airUpFollow;
        if (!state.ready)
        {
            up = targetUp;
        }
        else
        {
            up = Norm(Lerp(up, targetUp, 1.0f - std::exp(-dt * upRate)), targetUp);
        }

        // ---- the heading: the heading of the car (the car cam) or the line from the car to the ball (the ball cam)
        const float blendTarget = in.ballCam ? 1.0f : 0.0f;
        const float blendStep = t.transitionSpeed * 1.5f * dt;
        state.ballBlend += std::clamp(blendTarget - state.ballBlend, -blendStep, blendStep);

        const V3 carForward = Rotate(in.carRotation, {0.0f, 0.0f, 1.0f});
        V3 carHeading = OnPlane(carForward, up);
        const bool carHeadingOk = Len(carHeading) > 0.35f;  // nose straight up or down: the heading of the car means nothing
        carHeading = Norm(carHeading, FromRl(state.heading));
        V3 target = carHeading;
        // the ball cam fades out as the car climbs a wall: on a wall or the ceiling "towards the ball" has no stable meaning
        const float ballWeight = state.ballBlend * std::clamp((up.y - 0.2f) / 0.5f, 0.0f, 1.0f);
        const V3 toBall = OnPlane(ball - carPosition, up);
        if (ballWeight > 0.001f && Len(toBall) > 0.5f)
        {
            target = Norm(Lerp(carHeading, Norm(toBall, carHeading), ballWeight), carHeading);
        }
        V3 heading = FromRl(state.heading);
        if (!state.ready)
        {
            heading = target;
        }
        else if (grounded || carHeadingOk)
        {
            const float rate = t.swivelSpeed * t.stiffness * 2.0f * (grounded ? 1.0f : t.airSwivelScale);
            heading = Norm(Lerp(heading, target, 1.0f - std::exp(-dt * rate)), target);
        }
        heading = Norm(OnPlane(heading, up), carHeading);
        state.ready = true;
        state.heading = ToRl(heading);
        state.up = ToRl(up);

        // ---- the place: behind the car and above it, then pulled in so that it stays inside the arena
        const V3 pivot = carPosition + up * 0.35f;
        V3 eye = carPosition - heading * t.distance + up * t.height;
        {
            const V3 ray = eye - pivot;
            const float rayLength = Len(ray);
            const float free = sim.ArenaSegmentFree(ToRl(pivot), ToRl(eye));
            float keep = rayLength;
            if (free < 1.0f)
            {
                keep = std::max(rayLength * free - t.wallMargin, t.minDistance);
            }
            keep = std::min(keep, rayLength);
            eye = pivot + Norm(ray, up) * keep;
        }

        // ---- where it looks: ahead along the heading, tilted by the camera pitch; the ball cam turns towards the ball
        const float pitch = t.pitchDegrees * kDegToRad;
        V3 forward = Norm(heading * std::cos(pitch) + up * std::sin(pitch), heading);
        if (ballWeight > 0.001f)
        {
            V3 toBallFromEye = Norm(ball - eye, forward);
            // not steeper than the limit
            const V3 flat = Norm(OnPlane(toBallFromEye, up), heading);
            const float height = Dot(toBallFromEye, up);
            const float limit = std::sin(t.ballCamMaxPitch * kDegToRad);
            const float clamped = std::clamp(height, -limit, limit);
            toBallFromEye = Norm(flat * std::sqrt(std::max(1.0f - clamped * clamped, 0.0f)) + up * clamped, flat);
            forward = Norm(Lerp(forward, toBallFromEye, ballWeight), forward);
        }
        const V3 right = Norm(Cross(up, forward), Cross(up, heading));
        const V3 cameraUp = Cross(forward, right);
        return {ToRl(eye), ToRl(forward), ToRl(cameraUp), ToRl(right)};
    }
}
