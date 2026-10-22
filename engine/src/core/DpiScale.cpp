// DpiScale.cpp

#include <myengine/core/DpiScale.h>

#include <algorithm>
#include <cmath>

namespace myengine::core::dpi
{
    float ScaleFromDpi(const std::uint32_t dpi)
    {
        if (dpi == 0)
        {
            return 1.0f;
        }
        const float scale = static_cast<float>(dpi) / static_cast<float>(kBaseDpi);
        return std::clamp(scale, 1.0f, kMaxScale);
    }

    std::uint32_t ScaleLength(const std::uint32_t length, const float scale)
    {
        if (length == 0)
        {
            return 0;
        }
        const float scaled = std::round(static_cast<float>(length) * std::max(scale, 0.0f));
        return static_cast<std::uint32_t>(std::max(scaled, 1.0f));
    }

    float ToPhysical(const float logical, const float scale)
    {
        return logical * scale;
    }

    float ToLogical(const float physical, const float scale)
    {
        return scale > 0.0f ? physical / scale : physical;
    }

    std::uint32_t DpiFromWParam(const std::uintptr_t wparam)
    {
        return static_cast<std::uint32_t>(wparam & 0xFFFFu);
    }

    bool FollowsSuggestedRect(const bool windowed)
    {
        return windowed;
    }

    bool SameScale(const float a, const float b)
    {
        return std::fabs(a - b) < 0.001f;
    }
}
