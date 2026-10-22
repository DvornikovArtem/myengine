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

        // the direction between a and b (unit vectors) at a share of the way, along the great circle
        V3 Slerp(V3 a, V3 b, float share)
        {
            const float cosine = std::clamp(Dot(a, b), -1.0f, 1.0f);
            const float angle = std::acos(cosine);
            if (angle < 1e-4f)
            {
                return b;
            }
            const float sine = std::sin(angle);
            return Norm(a * (std::sin((1.0f - share) * angle) / sine) + b * (std::sin(share * angle) / sine), b);
        }

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
        const bool first = !state.ready;  // the first frame has nothing to follow: everything starts where it should be

        // ---- ground or air: a quarter of a second between them, so that a wheel that lets go for a tick does not make the camera jump
        const float groundTarget = in.onSurface ? 1.0f : 0.0f;
        if (first)
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
        up = first ? targetUp : Norm(Lerp(up, targetUp, 1.0f - std::exp(-dt * upRate)), targetUp);

        // ---- car cam <-> ball cam: the weight of the ball cam eases out, and it reaches 95 % in 0.24 s at the transition speed 1 (the directions are what is mixed below,
        // not the places of the camera, and each of them is smoothed on its own: the transition is not slowed down by the swivel)
        const float blendTarget = in.ballCam ? 1.0f : 0.0f;
        const float transitionSeconds = 0.24f / std::max(t.transitionSpeed, 0.05f);
        state.ballBlend = first ? blendTarget : state.ballBlend + (blendTarget - state.ballBlend) * (1.0f - std::exp(-dt * 3.0f / transitionSeconds));

        // ---- the heading of the car cam: where the camera looks along. The nose of the car on the floor; along the motion on a wall and on the ceiling (the nose of a car
        // that climbs a wall points anywhere, the motion is what the player follows); in the air the way the car faces (below); the last heading when there is nothing to go by.
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

        // a heading turned by a share of the angle to another one, about the up (both lie in its plane): the angle is what is mixed, not the vectors (the
        // vectors of a half turn pass through nothing and the camera would not move)
        const auto turned = [&](V3 from, V3 target, float share, float limit)
        {
            const float angle = std::atan2(Dot(Cross(from, target), up), Dot(from, target));
            const float step = std::clamp(angle * share, -limit, limit);
            return Norm(from * std::cos(step) + Cross(up, from) * std::sin(step), from);
        };

        // The way the car faces in the air: its nose on the ground plane. A flip takes the nose through the vertical and round to the back (a backflip: the nose is behind the
        // car for a while), so the nose alone says "the car turned round" when it did not. The nose and its opposite are the two candidates, the one that is closest to where the
        // car faced a moment ago (the memory, not the camera) is the answer: a car that flips over keeps its heading, a car that turns turns it (a frame is a few degrees, the
        // candidates are 180 apart). With the nose (almost) straight up or down the memory stays.
        const V3 memory = first ? nose : Norm(OnPlane(FromRl(state.facing), up), nose);
        V3 facing = memory;
        if (in.onSurface)
        {
            facing = nose;   // on a surface the nose is the yaw
        }
        else if (Len(noseHeading) > 0.08f)
        {
            const V3 candidate = Norm(noseHeading, memory);
            facing = Dot(candidate, memory) >= 0.0f ? candidate : candidate * -1.0f;
        }
        state.facing = ToRl(facing);

        // in the air the camera follows where the car faces; "airVelocityFollow" lets the direction of the motion count as well (the camera before G4h3: it jerked in a dodge)
        const V3 airHeading = turned(facing, motionDirection, t.airVelocityFollow * motionWeight, 10.0f);
        const V3 carTarget = turned(airHeading, groundHeading, state.groundBlend, 10.0f);

        // the swing of a heading towards its target: exponential, and never faster than the limit (a flip, the car upside down, a wall)
        const float turnLimit = std::max(t.maxTurnRate, 30.0f) * kDegToRad * dt;
        const auto swing = [&](V3 from, V3 target, float rate) { return turned(from, target, 1.0f - std::exp(-dt * rate), turnLimit); };
        // the camera lags behind the car by the angle only: 11 per second at the swivel speed 4.7 and the stiffness 0.45 (a time constant of 0.09 s); in the air slower
        const float carRate = t.swivelSpeed * (1.0f + 3.0f * t.stiffness) * (state.groundBlend + (1.0f - state.groundBlend) * t.airSwivelScale);
        V3 carHeading = first ? carTarget : swing(lastHeading, carTarget, carRate);

        // the heading of the ball cam: from the car towards the ball, followed closely (the ball changes its direction in a flash)
        const V3 toBall = OnPlane(ball - carPosition, up);
        V3 ballHeading = Norm(OnPlane(FromRl(state.ballHeading), up), carHeading);
        if (Len(toBall) > 0.5f)
        {
            ballHeading = first ? Norm(toBall, carHeading) : swing(ballHeading, Norm(toBall, ballHeading), 16.0f);
        }
        else if (first)
        {
            ballHeading = carHeading;
        }
        carHeading = Norm(OnPlane(carHeading, up), nose);
        ballHeading = Norm(OnPlane(ballHeading, up), carHeading);
        state.ready = true;
        state.heading = ToRl(carHeading);
        state.ballHeading = ToRl(ballHeading);
        state.up = ToRl(up);

        // on a wall with the horizon level the ball cam is as stable as on the floor; with the camera leaning it fades (the ball is "up" from there)
        const float ballWeight = state.ballBlend * std::clamp((up.y - 0.2f) / 0.5f, 0.0f, 1.0f);
        const V3 heading = ballWeight > 0.001f ? turned(carHeading, ballHeading, ballWeight, 10.0f) : carHeading;

        // ---- the place: behind the car and above it at the distance of the settings. The camera lags by the angle (above) and is stretched a little on speed (never closer).
        // (the pivot sits on the side of the surface that is inside the arena: under a car on the ceiling, so the ray to the camera starts inside)
        const float speedShare = std::clamp(motionSpeed / 23.0f, 0.0f, 1.0f);
        state.stretch += (speedShare - state.stretch) * (1.0f - std::exp(-dt * 4.0f));
        const float distance = t.distance * (1.0f + 0.12f * state.stretch);
        const V3 inside = Norm(Lerp(up, surfaceUp, state.groundBlend), up);
        const V3 pivot = carPosition + inside * 0.35f;
        // The place of the camera on the way from the full distance (u = 0) to 0.6 of it (u = 1). The way "up": the nearer, the higher (the car stays in the picture when
        // the camera comes close to it). The way "down": the nearer, the lower, to the level of the car (when the roof or the ceiling is above).
        constexpr float kLeastShare = 0.6f;
        const auto placeAt = [&](float u, bool lowering)
        {
            const float share = 1.0f - (1.0f - kLeastShare) * u;
            const float height = lowering ? t.height * (1.0f - u) : t.height + (1.0f - share) * distance * 0.9f;
            return carPosition - heading * (distance * share) + up * height;
        };
        const auto clear = [&](V3 place)
        {
            const V3 beyond = place + Norm(place - pivot, up) * t.wallMargin;
            return sim.ArenaSegmentFree(ToRl(pivot), ToRl(beyond)) >= 0.999f;
        };
        // what the arena allows: the whole distance, or the way to 0.6 of it as far as the camera must go for the way to the car to be free
        float needed = 0.0f;
        bool lowering = false;
        bool squeezed = false;
        if (!clear(placeAt(0.0f, false)))
        {
            if (clear(placeAt(1.0f, false)) || (lowering = clear(placeAt(1.0f, true))))
            {
                float low = 0.0f;
                float high = 1.0f;
                for (int i = 0; i < 7; ++i)
                {
                    const float middle = 0.5f * (low + high);
                    (clear(placeAt(middle, lowering)) ? high : low) = middle;
                }
                needed = high;
            }
            else
            {
                needed = 1.0f;
                squeezed = true;
            }
        }
        // closer at once, farther again in a moment (a corner that goes by does not make the camera swing)
        state.pull = (first || lowering != state.lowering || needed > state.pull) ? needed : std::max(needed, state.pull + (needed - state.pull) * (1.0f - std::exp(-dt * 6.0f)));
        state.lowering = lowering;
        V3 eye = placeAt(state.pull, lowering);
        if (squeezed)
        {
            // squeezed even at 0.6: the arena in the way pulls the camera along its ray, not below what keeps the car readable
            const V3 ray = eye - pivot;
            const float rayLength = Len(ray);
            const float free = sim.ArenaSegmentFree(ToRl(pivot), ToRl(eye));
            if (free < 1.0f)
            {
                const float keep = std::min(std::max(rayLength * free - t.wallMargin, 0.3f), rayLength);
                eye = pivot + Norm(ray, up) * keep;
            }
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
        state.pitchOffset += (wantedOffset - state.pitchOffset) * (1.0f - std::exp(-dt * 8.0f));
        float pitch = t.pitchDegrees * kDegToRad + state.pitchOffset;
        if (ballWeight > 0.001f)
        {
            // the direction along the ground is the heading (it was mixed above); the angle up or down is mixed here, the same way, so that the two halves of the
            // transition do not add up to a faster one
            const V3 toBallFromEye = Norm(ball - eye, heading);
            // not steeper than the limit, and not so steep that the car leaves the picture (a ball high above the car lifts the camera only as far as the car
            // allows, a ball far below it - when the car is on the ceiling - lowers the camera only as far as the car allows)
            const float height = Dot(toBallFromEye, up);
            const float halfVertical = std::atan(std::tan(t.fov * 0.5f * kDegToRad) * (9.0f / 16.0f));
            const float highest = std::sin(std::min(t.ballCamMaxPitch * kDegToRad, std::max(elevation + halfVertical * 0.9f, 5.0f * kDegToRad)));
            const float lowest = std::sin(std::max(std::min(elevation - halfVertical * 0.9f, -5.0f * kDegToRad), -80.0f * kDegToRad));
            const float clamped0 = std::clamp(height, lowest, highest);
            // the angle follows smoothly: the ball flies over the car, the car climbs, the limit moves
            state.ballPitch = ballWeight < 0.01f ? clamped0 : state.ballPitch + (clamped0 - state.ballPitch) * (1.0f - std::exp(-dt * 12.0f));
            const float clamped = std::clamp(state.ballPitch, lowest, highest);
            pitch += ballWeight * (std::asin(clamped) - pitch);
        }
        const V3 forward = Norm(heading * std::cos(pitch) + up * std::sin(pitch), heading);
        const V3 right = Norm(Cross(up, forward), Cross(up, heading));
        const V3 cameraUp = Cross(forward, right);
        return {ToRl(eye), ToRl(forward), ToRl(cameraUp), ToRl(right)};
    }
}
