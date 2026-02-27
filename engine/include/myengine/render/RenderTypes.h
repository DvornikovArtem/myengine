// RenderTypes.h

#pragma once

#include <myengine/core/Types.h>

namespace myengine::render
{
    struct RenderSurfaceHandle
    {
        std::uint32_t value = 0;

        bool IsValid() const
        {
            return value != 0;
        }
    };

    struct Transform2D
    {
        float positionX = 0.0f;
        float positionY = 0.0f;
        float scale = 1.0f;
        float rotationDeg = 0.0f;
    };
}