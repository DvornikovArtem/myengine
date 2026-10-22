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
        const float rollFollow = std::clamp(t.rollFollow, 0.0f, 1.0f);

        // ---- ground or air: a quarter of a second between them, so that a wheel that lets go for a tick does not make the camera jump
        const float groundTarget = in.onSurface ? 1.0f : 0.0f;
        if (!state.ready)
        {
            state.groundBlend = groundTarget;
        }
        else
        {
            const float step = dt / 0.25f;
            state.groundBlend += std::clamp(groundTarget - state.groundBlend, -step, step);
        }

        // ---- the up of the camera: the world up (the horizon is level), unless "Wall tilt" lets it lean towards the surface under the car (and only on the ground)
        V3 up = FromRl(state.up);
        const V3 surfaceUp = Norm(FromRl(in.surfaceNormal), worldUp);
        const V3 targetUp = Norm(Lerp(worldUp, surfaceUp, rollFollow * state.groundBlend), worldUp);
        const float upRate = state.groundBlend * t.upFollow + (1.0f - state.groundBlend) * t.airUpFollow;
        up = !state.ready ? targetUp : Norm(Lerp(up, targetUp, 1.0f - std::exp(-dt * upRate)), targetUp);

        // ---- the heading: where the camera looks along. The nose of the car on the floor; along the motion on a wall, on the ceiling and in the air (the nose of a
        // car that climbs a wall or flips points anywhere, the motion is what the player follows); the last heading when there is nothing to go by.
        const float blendTarget = in.ballCam ? 1.0f : 0.0f;
        const float blendStep = t.transitionSpeed * 1.5f * dt;
        state.ballBlend = state.ready ? state.ballBlend + std::clamp(blendTarget - state.ballBlend, -blendStep, blendStep) : blendTarget;  // the first frame has no blend to make

        const V3 lastHeading = Norm(OnPlane(FromRl(state.heading), up), {0.0f, 0.0f, 1.0f});
        const V3 carForward = Rotate(in.carRotation, {0.0f, 0.0f, 1.0f});
        const V3 noseHeading = OnPlane(carForward, up);
        const bool noseOk = Len(noseHeading) > 0.35f;  // nose straight up or down: it says nothing about the heading
        const V3 nose = noseOk ? Norm(noseHeading, lastHeading) : lastHeading;
        const V3 motion = OnPlane(FromRl(in.carVelocity), up);
        const float motionSpeed = Len(motion);
        const V3 motionDirection = Norm(motion, nose);
        // how much the motion counts: nothing when the car stands still
        const float motionWeight = std::clamp((motionSpeed - 2.0f) / 4.0f, 0.0f, 1.0f);
        // on a wall or the ceiling the motion decides (more so the less the camera is meant to lean with the surface)
        const float wallness = std::clamp((0.85f - surfaceUp.y) / 0.5f, 0.0f, 1.0f);
        const V3 groundHeading = Norm(Lerp(nose, motionDirection, wallness * (1.0f - rollFollow) * motionWeight), nose);
        const V3 airHeading = Norm(Lerp(nose, motionDirection, motionWeight), nose);
        V3 target = Norm(Lerp(airHeading, groundHeading, state.groundBlend), nose);

        // the ball cam: from the car towards the ball (on a wall with the horizon level this is as stable as on the floor)
        const float ballWeight = state.ballBlend * std::clamp((up.y - 0.2f) / 0.5f, 0.0f, 1.0f);
        const V3 toBall = OnPlane(ball - carPosition, up);
        if (ballWeight > 0.001f && Len(toBall) > 0.5f)
        {
            target = Norm(Lerp(target, Norm(toBall, target), ballWeight), target);
        }
        V3 heading = lastHeading;
        if (!state.ready)
        {
            heading = target;
        }
        else
        {
            const float rate = t.swivelSpeed * t.stiffness * 2.0f * (state.groundBlend + (1.0f - state.groundBlend) * t.airSwivelScale);
            const V3 wanted = Norm(Lerp(heading, target, 1.0f - std::exp(-dt * rate)), target);
            // and never faster than the limit, whatever the car does
            const float cosine = std::clamp(Dot(heading, wanted), -1.0f, 1.0f);
            const float angle = std::acos(cosine);
            const float limit = std::max(t.maxTurnRate, 10.0f) * kDegToRad * dt;
            heading = (angle > limit && angle > 1e-5f) ? Norm(Lerp(heading, wanted, limit / angle), wanted) : wanted;
        }
        heading = Norm(OnPlane(heading, up), nose);
        state.ready = true;
        state.heading = ToRl(heading);
        state.up = ToRl(up);

        // ---- the place: behind the car and above it, then pulled in so that it stays inside the arena
        // (the pivot sits on the side of the surface that is inside the arena: under a car on the ceiling, so the ray to the camera starts inside)
        const V3 inside = Norm(Lerp(up, surfaceUp, state.groundBlend), up);
        const V3 pivot = carPosition + inside * 0.35f;
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

        // ---- where it looks: ahead along the heading, tilted by the camera pitch. The car stays where the player expects it in the picture: when it climbs or falls
        // away from the level of the camera the camera tilts after it (smoothly), past a small dead zone.
        const V3 toCar = pivot - eye;
        const float elevation = std::atan2(Dot(toCar, up), Len(OnPlane(toCar, up)));
        const float nominal = std::atan2(0.35f - t.height, t.distance);
        constexpr float kDeadZone = 8.0f * kDegToRad;
        const float delta = elevation - nominal;
        const float extra = delta > 0.0f ? std::max(delta - kDeadZone, 0.0f) : std::min(delta + kDeadZone, 0.0f);
        const float wantedOffset = std::clamp(extra, -45.0f * kDegToRad, 60.0f * kDegToRad);
        state.pitchOffset += (wantedOffset - state.pitchOffset) * (1.0f - std::exp(-dt * 6.0f));
        const float pitch = t.pitchDegrees * kDegToRad + state.pitchOffset;
        V3 forward = Norm(heading * std::cos(pitch) + up * std::sin(pitch), heading);
        if (ballWeight > 0.001f)
        {
            V3 toBallFromEye = Norm(ball - eye, forward);
            // not steeper than the limit, and not so steep that the car leaves the picture (a ball high above the car lifts the camera only as far as the car
            // allows, a ball far below it - when the car is on the ceiling - lowers the camera only as far as the car allows)
            const V3 flat = Norm(OnPlane(toBallFromEye, up), heading);
            const float height = Dot(toBallFromEye, up);
            const float halfVertical = std::atan(std::tan(t.fov * 0.5f * kDegToRad) * (9.0f / 16.0f));
            const float highest = std::sin(std::min(t.ballCamMaxPitch * kDegToRad, std::max(elevation + halfVertical * 0.9f, 5.0f * kDegToRad)));
            const float lowest = std::sin(std::max(std::min(elevation - halfVertical * 0.9f, -5.0f * kDegToRad), -80.0f * kDegToRad));
            const float clamped0 = std::clamp(height, lowest, highest);
            // the angle follows smoothly: the ball flies over the car, the car climbs, the limit moves
            state.ballPitch = ballWeight < 0.01f ? clamped0 : state.ballPitch + (clamped0 - state.ballPitch) * (1.0f - std::exp(-dt * 7.0f));
            const float clamped = std::clamp(state.ballPitch, lowest, highest);
            toBallFromEye = Norm(flat * std::sqrt(std::max(1.0f - clamped * clamped, 0.0f)) + up * clamped, flat);
            forward = Norm(Lerp(forward, toBallFromEye, ballWeight), forward);
        }
        const V3 right = Norm(Cross(up, forward), Cross(up, heading));
        const V3 cameraUp = Cross(forward, right);
        return {ToRl(eye), ToRl(forward), ToRl(cameraUp), ToRl(right)};
    }
}
