// AudioSpatial.cpp

#include <myengine/audio/AudioSpatial.h>

#include <algorithm>
#include <cmath>

namespace myengine::audio
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;

        float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

        Vec3 Normalized(const Vec3& a)
        {
            const float length = Length(a);
            return length > 1e-6f ? Vec3{a.x / length, a.y / length, a.z / length} : Vec3{};
        }
    }

    SpatialResult ComputeSpatial(const ListenerState& listener, const PlayParams& source, const float speedOfSound, const float globalDoppler)
    {
        SpatialResult result;
        const Vec3 toSource = Sub(source.position, listener.position);
        const float distance = Length(toSource);
        result.distance = distance;

        // distance attenuation
        const float minDistance = std::max(source.minDistance, 0.01f);
        const float maxDistance = std::max(source.maxDistance, minDistance + 0.01f);
        if (distance <= minDistance)
        {
            result.gain = 1.0f;
        }
        else if (distance >= maxDistance)
        {
            result.gain = 0.0f;
        }
        else
        {
            float gain = std::pow(minDistance / distance, std::max(source.rolloff, 0.0f));
            const float fadeStart = maxDistance - (maxDistance - minDistance) * 0.2f;
            if (distance > fadeStart)
            {
                const float t = (distance - fadeStart) / (maxDistance - fadeStart);
                gain *= 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep to 0
            }
            result.gain = std::clamp(gain, 0.0f, 1.0f);
        }

        // pan: the side the source is on, in the frame of the listener
        const Vec3 forward = Normalized(listener.forward);
        const Vec3 right = Normalized(Cross(listener.up, forward)); // left handed world: up x forward = right
        if (distance > 1e-4f)
        {
            const Vec3 direction = {toSource.x / distance, toSource.y / distance, toSource.z / distance};
            // closer than minDistance the sound is inside the head: it pans less
            const float width = std::clamp(distance / minDistance, 0.0f, 1.0f);
            result.pan = std::clamp(Dot(direction, right), -1.0f, 1.0f) * width;

            const float factor = source.dopplerFactor * globalDoppler;
            if (factor > 0.0f && speedOfSound > 1.0f)
            {
                // dir points from the listener to the source: the listener moving along it comes closer (+), the source moving along it goes away (+)
                const float listenerToward = Dot(listener.velocity, direction) * factor;
                const float sourceAway = Dot(source.velocity, direction) * factor;
                const float c = speedOfSound;
                const float ratio = (c + std::clamp(listenerToward, -c * 0.9f, c * 0.9f)) / std::max(c + std::clamp(sourceAway, -c * 0.9f, c * 0.9f), c * 0.1f);
                result.doppler = std::clamp(ratio, 0.5f, 2.0f);
            }
        }
        return result;
    }

    void PanGains(const float pan, float& left, float& right)
    {
        const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * kPi * 0.25f; // 0 .. pi/2
        left = std::cos(angle);
        right = std::sin(angle);
    }
}
