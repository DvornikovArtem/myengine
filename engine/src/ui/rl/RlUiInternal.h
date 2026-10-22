// RlUiInternal.h: what the files of the kit share and the public headers do not show (ImGui types, helpers).

#pragma once

#include <imgui/imgui.h>

#include <myengine/ui/rl/RlUiCommon.h>

namespace myengine::rlui
{
    // The codepoint of an icon in lucide.ttf (0: none)
    unsigned int IconCodepoint(Icon icon);

    inline ImU32 ToU32(const Color& c)
    {
        const auto byte = [](float v) { return static_cast<int>(v <= 0.0f ? 0.0f : (v >= 1.0f ? 255.0f : v * 255.0f + 0.5f)); };
        return IM_COL32(byte(c.r), byte(c.g), byte(c.b), byte(c.a));
    }
    inline ImVec2 ToIm(const Vec2& v) { return ImVec2(v.x, v.y); }
}
