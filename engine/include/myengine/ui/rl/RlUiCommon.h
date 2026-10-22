// RlUiCommon.h
//
// The UI kit of the Rocket League game (G1-kit, docs/game/rl-ui.md; design: .team/design/rl-ui-spec.md). Everything is in the namespace
// myengine::rlui (not "ui::rl": inside it `rl::` would stop meaning the game library). The public headers do not include ImGui or the
// game library: the data goes in and out through the plain types below, so that a screen of the game (coder-2: RlGameSystem, G1-flow)
// only calls the kit.
//
// Coordinates: every Rect a screen passes to a widget is in DESIGN pixels, a canvas of 1920 x 1080 (the 1080p of the mock-ups). The
// kit scales it by Metrics::scale (= the height of the window / 1080 * the UI scale of the settings) and puts it into the "menu
// column" (a 16:9 area in the middle of the window, so on 21:9 the menu stays in the middle and the background fills the sides). The
// HUD (RlUiHud.h) is anchored to the real edges of the window instead.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace myengine::rlui
{
    using Id = std::uint32_t;
    constexpr Id kNoId = 0;

    // FNV-1a of the text; a widget id is a name: `rlui::HashId("main/play")`. The same text gives the same id on every frame.
    constexpr Id HashId(std::string_view text, Id seed = 2166136261u)
    {
        Id hash = seed;
        for (const char c : text)
        {
            hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
        }
        return hash == kNoId ? 1u : hash;
    }

    // A view of a run of items (C++17 has no std::span): `const std::string_view labels[] = {...}; Tabs(f, id, rect, labels, 0)` works, so does a
    // std::vector or a braced list passed straight to a function (it lives until the end of the call).
    template <class T>
    class Span
    {
    public:
        constexpr Span() = default;
        constexpr Span(const T* data, std::size_t size) : data_(data), size_(size) {}
        template <std::size_t N>
        constexpr Span(const T (&array)[N]) : data_(array), size_(N) {}
        Span(const std::vector<T>& vector) : data_(vector.data()), size_(vector.size()) {}
        Span(std::initializer_list<T> list) : data_(list.begin()), size_(list.size()) {}
        constexpr const T* begin() const { return data_; }
        constexpr const T* end() const { return data_ + size_; }
        constexpr std::size_t size() const { return size_; }
        constexpr bool empty() const { return size_ == 0; }
        constexpr const T& operator[](std::size_t i) const { return data_[i]; }

    private:
        const T* data_ = nullptr;
        std::size_t size_ = 0;
    };

    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Rect
    {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;

        constexpr float Right() const { return x + w; }
        constexpr float Bottom() const { return y + h; }
        constexpr Vec2 Center() const { return {x + w * 0.5f, y + h * 0.5f}; }
        constexpr bool Contains(const Vec2 p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
        constexpr Rect Inset(const float d) const { return {x + d, y + d, w - 2.0f * d, h - 2.0f * d}; }
        constexpr Rect Moved(const float dx, const float dy) const { return {x + dx, y + dy, w, h}; }
    };

    struct Color
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;

        static constexpr Color Hex(const std::uint32_t rgb, const float alpha = 1.0f)
        {
            return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                    static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
        }
        constexpr Color WithAlpha(const float alpha) const { return {r, g, b, alpha}; }
        constexpr Color Faded(const float k) const { return {r, g, b, a * k}; }  // alpha times k
    };

    enum class Align : std::uint8_t
    {
        Left,
        Center,
        Right,
    };

    // The device the player used last: the hints at the bottom of a screen show its buttons (§5.2 of the spec)
    enum class Device : std::uint8_t
    {
        KeyboardMouse,
        Gamepad,
    };

    // The icons of the kit (Lucide, ISC license, assets/fonts/lucide.ttf): menu buttons, chips, the feed, the table
    enum class Icon : std::uint8_t
    {
        None = 0,
        Play,        // Play, Start match, Resume
        Target,      // Training
        Settings,    // Settings
        Exit,        // Exit, Leave match
        Refresh,     // Restart, Play again, Reset to defaults
        Home,        // Main menu
        Star,        // MVP
        Person,      // a human in the table
        Robot,       // a bot
        Wifi,        // ping
        WifiOff,     // Offline
        Shield,      // a save in the feed
        Bolt,        // a demolition in the feed
        Ball,        // a goal in the feed
        Server,      // Host
        Join,        // Join by IP
        Eye,         // Live preview
        Info,        // the notes
        ChevronLeft,
        ChevronRight,
        ArrowsVertical,    // the key hint "up / down"
        ArrowsHorizontal,  // "left / right"
        Car,               // Garage, Change car
        Check,             // Equipped
        Ruler,             // the size of the hitbox
        Count
    };
}
