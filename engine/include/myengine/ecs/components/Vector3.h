// Vector3.h

#pragma once

#include <algorithm>
#include <cmath>

namespace myengine::ecs::components
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    inline Vec3 operator+(const Vec3& lhs, const Vec3& rhs)
    {
        return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
    }

    inline Vec3 operator-(const Vec3& lhs, const Vec3& rhs)
    {
        return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
    }

    inline Vec3 operator-(const Vec3& value)
    {
        return {-value.x, -value.y, -value.z};
    }

    inline Vec3 operator*(const Vec3& value, const float scalar)
    {
        return {value.x * scalar, value.y * scalar, value.z * scalar};
    }

    inline Vec3 operator*(const float scalar, const Vec3& value)
    {
        return value * scalar;
    }

    inline Vec3 operator/(const Vec3& value, const float scalar)
    {
        if (std::abs(scalar) <= 1e-6f)
        {
            return {};
        }

        return {value.x / scalar, value.y / scalar, value.z / scalar};
    }

    inline Vec3& operator+=(Vec3& lhs, const Vec3& rhs)
    {
        lhs = lhs + rhs;
        return lhs;
    }

    inline Vec3& operator-=(Vec3& lhs, const Vec3& rhs)
    {
        lhs = lhs - rhs;
        return lhs;
    }

    inline Vec3& operator*=(Vec3& value, const float scalar)
    {
        value = value * scalar;
        return value;
    }

    inline Vec3& operator/=(Vec3& value, const float scalar)
    {
        value = value / scalar;
        return value;
    }

    inline float Dot(const Vec3& lhs, const Vec3& rhs)
    {
        return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    }

    inline Vec3 Cross(const Vec3& lhs, const Vec3& rhs)
    {
        return {
            lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x,
        };
    }

    inline float LengthSquared(const Vec3& value)
    {
        return Dot(value, value);
    }

    inline float Length(const Vec3& value)
    {
        return std::sqrt(LengthSquared(value));
    }

    inline Vec3 Normalize(const Vec3& value)
    {
        const float length = Length(value);
        if (length <= 1e-6f)
        {
            return {};
        }

        return value / length;
    }

    inline Vec3 HadamardMul(const Vec3& lhs, const Vec3& rhs)
    {
        return {lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z};
    }

    inline Vec3 Abs(const Vec3& value)
    {
        return {std::abs(value.x), std::abs(value.y), std::abs(value.z)};
    }

    inline Vec3 Max(const Vec3& lhs, const Vec3& rhs)
    {
        return {
            std::max(lhs.x, rhs.x),
            std::max(lhs.y, rhs.y),
            std::max(lhs.z, rhs.z),
        };
    }

    inline Vec3 Min(const Vec3& lhs, const Vec3& rhs)
    {
        return {
            std::min(lhs.x, rhs.x),
            std::min(lhs.y, rhs.y),
            std::min(lhs.z, rhs.z),
        };
    }

    inline Vec3 Clamp(const Vec3& value, const Vec3& minValue, const Vec3& maxValue)
    {
        return {
            std::clamp(value.x, minValue.x, maxValue.x),
            std::clamp(value.y, minValue.y, maxValue.y),
            std::clamp(value.z, minValue.z, maxValue.z),
        };
    }
}