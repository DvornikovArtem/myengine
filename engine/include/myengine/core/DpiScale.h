// DpiScale.h
//
// The scale of Windows (100 %, 125 %, 150 % ...) as numbers. The process is Per-Monitor V2 DPI aware (app/resources/myengine.manifest): the
// windows, the mouse and the screen are in real pixels, and an application that wants the same physical size on every screen scales itself
// by DPI / 96. These helpers are that arithmetic without any window, so that it is tested without one:
//
//   - the editor lays itself out in "logical" pixels (what a 100 % screen shows) and the renderer multiplies them by the scale; the fonts
//     are baked at the scale (UiManager);
//   - the game UI (ui/rl) scales itself by the size of the window and needs no DPI at all;
//   - a window that moves to a screen of another scale gets WM_DPICHANGED with the size it should have (Window::OnDpiChanged).

#pragma once

#include <cstdint>

namespace myengine::core::dpi
{
    inline constexpr std::uint32_t kBaseDpi = 96;   // 100 %
    inline constexpr float kMaxScale = 4.0f;        // 400 %: the largest scale that Windows offers

    /// DPI / 96 (1.25 at 120 DPI). 1 for an unknown DPI (0), never below 1 (a screen smaller than 100 % does not exist in Windows) and never above kMaxScale.
    float ScaleFromDpi(std::uint32_t dpi);

    /// A length in pixels made for 100 % at this scale: rounded, at least 1 for a positive length
    std::uint32_t ScaleLength(std::uint32_t length, float scale);

    /// Logical pixels (the layout of the editor) -> real pixels of the window and back
    float ToPhysical(float logical, float scale);
    float ToLogical(float physical, float scale);

    /// The new DPI of WM_DPICHANGED (the X and the Y DPI are equal in Windows 10, the low word of wParam is the one)
    std::uint32_t DpiFromWParam(std::uintptr_t wparam);

    /// A window takes the rectangle that WM_DPICHANGED suggests only in the windowed mode: a borderless or a fullscreen window covers its monitor
    bool FollowsSuggestedRect(bool windowed);

    /// Two scales are the same scale (a rebuild of the fonts is needed only when they are not)
    bool SameScale(float a, float b);
}
