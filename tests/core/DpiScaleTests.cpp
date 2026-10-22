// The scale of Windows (100 %, 125 % ...): the arithmetic of core/DpiScale.h (DPI -> scale, sizes in real and in logical pixels, WM_DPICHANGED), the size
// class and the scale of the game UI in real pixels (a window of a screen of 125 % is 1920 x 1080, not the virtual 1536 x 864) and, when the machine has
// a desktop, a hidden window: it is made in real pixels, grows with the scale of its screen and takes the rectangle of WM_DPICHANGED.

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <myengine/core/DpiScale.h>
#include <myengine/core/Window.h>
#include <myengine/ui/rl/RlUiHost.h>

namespace
{
    namespace dpi = myengine::core::dpi;
    int g_failures = 0;

    void Check(const bool condition, const char* text)
    {
        if (!condition)
        {
            ++g_failures;
            std::printf("  FAIL %s\n", text);
        }
    }

    bool Near(const float a, const float b, const float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

    void TestScaleFromDpi()
    {
        Check(Near(dpi::ScaleFromDpi(96), 1.0f), "96 DPI is 100 %");
        Check(Near(dpi::ScaleFromDpi(120), 1.25f), "120 DPI is 125 %");
        Check(Near(dpi::ScaleFromDpi(144), 1.5f), "144 DPI is 150 %");
        Check(Near(dpi::ScaleFromDpi(168), 1.75f), "168 DPI is 175 %");
        Check(Near(dpi::ScaleFromDpi(192), 2.0f), "192 DPI is 200 %");
        Check(Near(dpi::ScaleFromDpi(384), 4.0f), "384 DPI is 400 %");
        Check(Near(dpi::ScaleFromDpi(0), 1.0f), "an unknown DPI (0) is 100 %");
        Check(Near(dpi::ScaleFromDpi(48), 1.0f), "a DPI below 96 does not make the interface smaller than the design");
        Check(Near(dpi::ScaleFromDpi(4000), dpi::kMaxScale), "a absurd DPI is clamped");
    }

    void TestLengths()
    {
        Check(dpi::ScaleLength(1280, 1.25f) == 1600 && dpi::ScaleLength(720, 1.25f) == 900, "the default window 1280 x 720 is 1600 x 900 at 125 %");
        Check(dpi::ScaleLength(1280, 1.0f) == 1280, "at 100 % a length stays");
        Check(dpi::ScaleLength(1280, 1.5f) == 1920 && dpi::ScaleLength(720, 1.5f) == 1080, "1280 x 720 is 1920 x 1080 at 150 %");
        Check(dpi::ScaleLength(1366, 1.25f) == 1708, "a length is rounded (1707.5)");
        Check(dpi::ScaleLength(0, 1.25f) == 0, "zero stays zero");
        Check(dpi::ScaleLength(1, 0.1f) == 1, "a positive length never becomes zero");

        Check(Near(dpi::ToPhysical(100.0f, 1.25f), 125.0f), "logical -> real");
        Check(Near(dpi::ToLogical(1920.0f, 1.25f), 1536.0f) && Near(dpi::ToLogical(1080.0f, 1.25f), 864.0f),
              "the screen of 1920 x 1080 at 125 % is 1536 x 864 logical pixels (what the editor was laid out in)");
        Check(Near(dpi::ToLogical(dpi::ToPhysical(333.0f, 1.75f), 1.75f), 333.0f, 1e-3f), "real -> logical is the inverse");
        Check(Near(dpi::ToLogical(50.0f, 0.0f), 50.0f), "a zero scale does not divide");
        Check(Near(dpi::ToPhysical(7.0f, 1.0f), 7.0f) && Near(dpi::ToLogical(7.0f, 1.0f), 7.0f), "at 100 % nothing changes");
    }

    void TestDpiChangedMessage()
    {
        // WM_DPICHANGED: the new DPI is in the low word of wParam (X) and the high word (Y), both equal
        const std::uintptr_t wparam = (static_cast<std::uintptr_t>(144) << 16) | 144u;
        Check(dpi::DpiFromWParam(wparam) == 144, "the DPI is read from the low word of wParam");
        Check(dpi::DpiFromWParam(0x00000078u) == 120, "the DPI of 125 %");
        Check(dpi::DpiFromWParam(0xFFFF0060u) == 96, "the high word is not part of the DPI");

        Check(dpi::FollowsSuggestedRect(true), "a windowed window takes the rectangle that Windows suggests");
        Check(!dpi::FollowsSuggestedRect(false), "a borderless or a fullscreen window covers its monitor and keeps its rectangle");

        Check(dpi::SameScale(1.25f, dpi::ScaleFromDpi(120)), "the same DPI is the same scale");
        Check(!dpi::SameScale(1.25f, 1.5f), "125 % and 150 % are not the same (the fonts are baked again)");
        Check(dpi::SameScale(1.0f, 1.0004f), "a rounding noise is the same scale");

        // moving a window between screens keeps its physical size: 1600 x 900 at 125 % is 1280 x 720 logical, at 150 % the suggested size is 1920 x 1080
        const float logicalWidth = dpi::ToLogical(1600.0f, dpi::ScaleFromDpi(120));
        Check(Near(logicalWidth, 1280.0f), "the logical width before the move");
        Check(dpi::ScaleLength(static_cast<std::uint32_t>(logicalWidth), dpi::ScaleFromDpi(144)) == 1920, "the width after the move to 150 %");
    }

    void TestGameUiScale()
    {
        using myengine::rlui::FontBucketFor;
        using myengine::rlui::UiScaleForWindow;

        Check(Near(UiScaleForWindow(1920, 1080, 1.0f), 1.0f), "the design size is the scale 1");
        Check(Near(UiScaleForWindow(3840, 2160, 1.0f), 2.0f), "4K is the scale 2");
        Check(Near(UiScaleForWindow(1280, 720, 1.0f), 2.0f / 3.0f), "720p is two thirds");
        Check(Near(UiScaleForWindow(1920, 1080, 1.25f), 1.25f), "the setting of the UI scale multiplies");
        Check(Near(UiScaleForWindow(2560, 1080, 1.0f), 1.0f), "21:9 fits the height");
        Check(Near(UiScaleForWindow(1080, 1080, 1.0f), 1080.0f / 1920.0f), "a narrow window fits the width");

        // The point of the DPI awareness: a window of a screen of 125 % is 1920 x 1080 in real pixels; the virtual 1536 x 864 was baked for 0.8 and stretched
        const float real = UiScaleForWindow(1920, 1080, 1.0f);
        const float virtualized = UiScaleForWindow(1536, 864, 1.0f);
        Check(Near(FontBucketFor(real), 1.0f), "125 %: the fonts are baked for 1.0 at the real size");
        Check(Near(FontBucketFor(virtualized), 0.8f), "(the virtual size was baked for 0.8 and then enlarged by Windows)");

        Check(Near(FontBucketFor(0.5f), 0.5f) && Near(FontBucketFor(0.67f), 0.67f) && Near(FontBucketFor(0.8f), 0.8f), "the buckets map to themselves (small)");
        Check(Near(FontBucketFor(1.25f), 1.25f) && Near(FontBucketFor(1.5f), 1.5f) && Near(FontBucketFor(2.0f), 2.0f) && Near(FontBucketFor(3.0f), 3.0f),
              "the buckets map to themselves (large)");
        Check(Near(FontBucketFor(2560.0f / 1920.0f), 1.25f), "1440p is the class 1.25 (ratio)");
        Check(Near(FontBucketFor(0.0f), 0.5f) && Near(FontBucketFor(-1.0f), 0.5f), "a zero scale is the smallest class");
        Check(Near(FontBucketFor(50.0f), 3.0f), "a huge scale is the largest class");
        Check(Near(FontBucketFor(1.02f), 1.0f) && Near(FontBucketFor(0.98f), 1.0f), "a few pixels of a drag stay in the class (no new bake)");
    }

    // A window that is not shown. false: there is no desktop to make it on (a service session); the checks are skipped.
    bool TestWindow()
    {
        // the exe of this test has no manifest: the process takes the awareness that the game has
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        myengine::core::WindowDesc desc;
        desc.id = 1;
        desc.title = L"dpi";
        desc.width = 1280;
        desc.height = 720;
        myengine::core::Window window(desc);
        if (!window.Create(nullptr))
        {
            std::printf("  (no desktop: the window checks are skipped)\n");
            return false;
        }

        const std::uint32_t windowDpi = window.Dpi();
        const float scale = window.DpiScale();
        std::printf("  window DPI %u, scale %.2f, client %u x %u\n", windowDpi, scale, window.Width(), window.Height());
        Check(windowDpi >= 96, "the DPI of the window is known");
        Check(dpi::SameScale(scale, dpi::ScaleFromDpi(windowDpi)), "the scale of the window is its DPI / 96");
        Check(window.Width() == dpi::ScaleLength(1280, scale) && window.Height() == dpi::ScaleLength(720, scale),
              "the client area is the default size at the scale of the screen (real pixels)");

        RECT client{};
        GetClientRect(window.Handle(), &client);
        Check(static_cast<std::uint32_t>(client.right - client.left) == window.Width() && static_cast<std::uint32_t>(client.bottom - client.top) == window.Height(),
              "the size of the window is the real client area (the swap chain is made with it)");

        // WM_DPICHANGED to another scale: the window takes the suggested rectangle and keeps the new DPI
        RECT suggested{100, 100, 100 + 1920, 100 + 1080};
        Check(window.OnDpiChanged(144, suggested), "a windowed window takes the suggested rectangle");
        Check(window.Dpi() == 144 && dpi::SameScale(window.DpiScale(), 1.5f), "the new DPI is kept");
        RECT outer{};
        GetWindowRect(window.Handle(), &outer);
        Check(outer.left == 100 && outer.top == 100 && outer.right - outer.left == 1920 && outer.bottom - outer.top == 1080,
              "the window is at the suggested rectangle");

        // the sizes in real pixels also when the caller asks for them
        myengine::core::WindowDesc exact = desc;
        exact.id = 2;
        exact.scaleToDpi = false;
        exact.width = 800;
        exact.height = 600;
        myengine::core::Window exactWindow(exact);
        if (exactWindow.Create(nullptr))
        {
            Check(exactWindow.Width() == 800 && exactWindow.Height() == 600, "scaleToDpi = false: the size is in real pixels");
        }
        return true;
    }
}

int main()
{
    TestScaleFromDpi();
    TestLengths();
    TestDpiChangedMessage();
    TestGameUiScale();
    TestWindow();

    std::printf(g_failures == 0 ? "dpi_scale: all passed\n" : "dpi_scale: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
