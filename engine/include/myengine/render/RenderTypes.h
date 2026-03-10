// RenderTypes.h

#pragma once

#include <array>
#include <cstdint>

#include <myengine/core/Types.h>

namespace myengine::render
{
    enum class PrimitiveType : std::uint8_t
    {
        Line = 0,
        Triangle = 1,
        Quad = 2,
        Cube = 3,
    };

    struct RenderSurfaceHandle
    {
        std::uint32_t value = 0;

        bool IsValid() const
        {
            return value != 0;
        }
    };

    struct Matrix4
    {
        std::array<float, 16> data{};

        static Matrix4 Identity()
        {
            Matrix4 matrix{};
            matrix.data[0] = 1.0f;
            matrix.data[5] = 1.0f;
            matrix.data[10] = 1.0f;
            matrix.data[15] = 1.0f;
            return matrix;
        }
    };
}