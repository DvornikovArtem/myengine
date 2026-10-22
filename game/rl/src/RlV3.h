// RlV3.h: small vector helpers of the camera code (RlReplay.cpp, RlCinematic.cpp). Not a public header.

#pragma once

#include <rl/RlTypes.h>

#include <algorithm>
#include <cmath>

namespace rl::detail
{
    struct V3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
    inline V3 Norm(V3 a, V3 fallback)
    {
        const float l = Len(a);
        return l > 1e-6f ? a * (1.0f / l) : fallback;
    }
    inline V3 Lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }
    inline V3 FromRl(const RlVec3& v) { return {v.x, v.y, v.z}; }
    inline RlVec3 ToRl(V3 v) { return {v.x, v.y, v.z}; }
    inline float Smooth(float t)
    {
        const float c = std::clamp(t, 0.0f, 1.0f);
        return c * c * (3.0f - 2.0f * c);
    }

    // A rotation that is the mix of two (normalized lerp: the ticks are close, it is enough)
    inline RlQuat Nlerp(const RlQuat& a, const RlQuat& b, float t)
    {
        const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        const float sign = dot < 0.0f ? -1.0f : 1.0f;
        RlQuat q{a.x + (b.x * sign - a.x) * t, a.y + (b.y * sign - a.y) * t, a.z + (b.z * sign - a.z) * t, a.w + (b.w * sign - a.w) * t};
        const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (l > 1e-8f)
        {
            q = {q.x / l, q.y / l, q.z / l, q.w / l};
        }
        return q;
    }
}
