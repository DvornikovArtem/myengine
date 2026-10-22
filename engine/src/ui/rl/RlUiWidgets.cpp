#include <myengine/ui/rl/RlUiWidgets.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <imgui/imgui.h>

#include <myengine/ui/rl/RlUiPaint.h>

#include "RlUiInternal.h"

namespace myengine::rlui
{
    using namespace paint;

    namespace
    {
        // The light plates inside a focused (white) row, as in the mock-ups
        constexpr Color kPlateLight = Color::Hex(0xdbe4f7);
        constexpr Color kPlateLightText = Color::Hex(0x4a5d85);
        constexpr Color kSegSelectedOnWhite = Color::Hex(0x0b3f94);
        constexpr Color kSegDark = Color::Hex(0x0a1630);
        constexpr Color kTrackOnWhite = Color::Hex(0xc9d4ec);

        Color Enabled(const Color c, const bool enabled) { return enabled ? c : c.Faded(0.4f); }

        // The focus of a bevelled item: the glow, the white plate, the stripe of the accent colour on its left edge
        void FocusPlate(Frame& f, const Rect& rect, const float glow)
        {
            Glow(f, rect, style::kAccent.Faded(glow), 16.0f, 4);
            Parallelogram(f, rect, style::kFocus);
            const float b = Bevel(rect.h);
            Parallelogram(f, {rect.x, rect.y, b + 7.0f, rect.h}, style::kAccent);
        }

        // Where the left edge of a bevelled shape of rect is at the vertical middle of a line of text
        float EdgeAt(const Rect& rect, const float y)
        {
            const float t = std::clamp((y - rect.y) / rect.h, 0.0f, 1.0f);
            return rect.x + Bevel(rect.h) * (1.0f - t);
        }

        bool ShiftHeld(const Frame& f) { return f.input().Raw().keys[0x10]; }

        // The step of a left / right press: 1, or 5 with Shift
        float StepFactor(const Frame& f) { return ShiftHeld(f) ? 5.0f : 1.0f; }

        // A row with a stripe: not bevelled, the focused one is white with the stripe of the accent colour
        void RowPlate(Frame& f, const Rect& rect, const ItemState& state, const bool enabled)
        {
            if (state.focused)
            {
                Glow(f, rect, style::kAccent.Faded(0.5f * state.glow), 8.0f, 3, 0.0f);
                FillRect(f, rect, style::kFocus);
                FillRect(f, {rect.x, rect.y, 6.0f, rect.h}, style::kAccent);
            }
            else
            {
                FillRect(f, rect, Enabled(state.hovered ? style::kNavy3.Faded(0.6f) : style::kNavy2, enabled));
            }
        }

        // The segments (Rookie | Pro | All-star): returns the index under the pointer when it is pressed, else -1. Draws them.
        int DrawSegments(Frame& f, const ItemState& row, const Rect& content, Span<std::string_view> options, const int selected, const bool enabled)
        {
            const float height = 34.0f;
            float x = content.x;
            const float y = content.y + (content.h - height) * 0.5f;
            int clicked = -1;
            for (int i = 0; i < static_cast<int>(options.size()); ++i)
            {
                const float width = TextWidth(f, FontStyle::Bold, 20.0f, options[static_cast<std::size_t>(i)]) + 34.0f;
                const Rect seg{x, y, width, height};
                const bool on = i == selected;
                Color fill;
                Color text;
                if (row.focused)
                {
                    fill = on ? kSegSelectedOnWhite : kPlateLight;
                    text = on ? style::kText : kPlateLightText;
                }
                else
                {
                    fill = on ? style::kBlue : kSegDark;
                    text = on ? style::kFocus : style::kTextDim;
                }
                FillRect(f, seg, Enabled(fill, enabled));
                TextIn(f, FontStyle::Bold, 20.0f, seg, Enabled(text, enabled), options[static_cast<std::size_t>(i)], Align::Center);
                if (enabled && f.input().MousePressed() && f.ToScreen(seg).Contains(f.input().MousePosition()))
                {
                    clicked = i;
                }
                x += width + 4.0f;
            }
            return clicked;
        }
    }

    // ---------------------------------------------------------------- buttons

    namespace
    {
        bool BevelButton(Frame& f, const Id id, const Rect& rect, const std::string_view label, const Icon icon, const bool enabled, const bool centered, const float fontSize, const bool primary = false)
        {
            const ItemState state = f.Item(id, rect, enabled);
            const float b = Bevel(rect.h);
            if (state.focused)
            {
                FocusPlate(f, rect, state.glow);
            }
            else if (primary)
            {
                // the main button of a screen is blue even without the focus (it must not get lost in the background)
                Parallelogram(f, rect, Enabled(state.hovered ? style::kBlueHi : style::kBlue, enabled));
            }
            else
            {
                Parallelogram(f, rect, Enabled(state.hovered ? style::kNavy3.Faded(0.85f) : style::kNavy, enabled));
            }
            const Color textColor = Enabled(state.focused ? style::kFocusText : (primary ? style::kFocusText : style::kText), enabled);
            const float iconSize = fontSize * 0.8f;
            const float textWidth = TextWidth(f, FontStyle::ExtraBoldItalic, fontSize, label);
            const bool hasIcon = icon != Icon::None;
            const float gap = fontSize * 0.5f;
            float left;
            if (centered)
            {
                const float total = textWidth + (hasIcon ? iconSize + gap : 0.0f);
                left = rect.x + (rect.w - total) * 0.5f + b * 0.5f;
            }
            else
            {
                left = rect.x + 40.0f + b * 0.5f;
            }
            if (hasIcon)
            {
                IconGlyph(f, icon, iconSize, {left + iconSize * 0.5f, rect.y + rect.h * 0.5f}, textColor);
                left += iconSize + gap;
            }
            TextIn(f, FontStyle::ExtraBoldItalic, fontSize, {left, rect.y, rect.w, rect.h}, textColor, label);
            return state.activated;
        }
    }

    bool MenuButton(Frame& f, const Id id, const Rect& rect, const std::string_view label, const Icon icon, const bool enabled)
    {
        // the menu buttons keep their text on the left: the icon at x + 62 (the centre), the text at x + 98 (§1.1, the mock-up)
        const ItemState state = f.Item(id, rect, enabled);
        if (state.focused)
        {
            FocusPlate(f, rect, state.glow);
        }
        else
        {
            Parallelogram(f, rect, Enabled(state.hovered ? style::kNavy3.Faded(0.85f) : style::kNavy, enabled));
        }
        const Color textColor = Enabled(state.focused ? style::kFocusText : style::kText, enabled);
        const float middleY = rect.y + rect.h * 0.5f;
        const float edge = EdgeAt(rect, middleY);
        if (icon != Icon::None)
        {
            IconGlyph(f, icon, 30.0f, {edge + 48.0f, middleY}, textColor);
        }
        TextIn(f, FontStyle::ExtraBoldItalic, 38.0f, {edge + 84.0f, rect.y, rect.w, rect.h}, textColor, label);
        return state.activated;
    }

    bool ActionButton(Frame& f, const Id id, const Rect& rect, const std::string_view label, const Icon icon, const bool enabled, const bool primary)
    {
        const float size = std::clamp(rect.h * 0.5f, 22.0f, 38.0f);
        return BevelButton(f, id, rect, label, icon, enabled, true, size, primary);
    }

    bool DialogButton(Frame& f, const Id id, const Rect& rect, const std::string_view label, const bool enabled)
    {
        return BevelButton(f, id, rect, label, Icon::None, enabled, true, 26.0f);
    }

    // ---------------------------------------------------------------- tabs, cards

    int Tabs(Frame& f, const Id id, const Rect& rect, Span<std::string_view> labels, int active)
    {
        const int count = static_cast<int>(labels.size());
        if (count == 0)
        {
            return active;
        }
        active = std::clamp(active, 0, count - 1);
        int result = active;
        if (f.ScopeLive())
        {
            if (f.TakeNav(Nav::TabPrev))
            {
                result = std::max(active - 1, 0);
            }
            else if (f.TakeNav(Nav::TabNext))
            {
                result = std::min(active + 1, count - 1);
            }
        }
        const float b = Bevel(rect.h);
        float x = rect.x;
        for (int i = 0; i < count; ++i)
        {
            const float width = TextWidth(f, FontStyle::ExtraBoldItalic, 26.0f, labels[static_cast<std::size_t>(i)]) + 72.0f;
            const Rect tab{x, rect.y, width, rect.h};
            const bool hovered = f.ScopeLive() && f.ToScreen(tab).Contains(f.input().MousePosition()) && f.input().Raw().windowActive;
            if (hovered && f.input().MousePressed())
            {
                result = i;
            }
            const bool on = i == result;
            Parallelogram(f, tab, on ? style::kBlue : (hovered ? style::kNavy3.Faded(0.9f) : style::kNavy.Faded(0.8f)));
            TextIn(f, FontStyle::ExtraBoldItalic, 26.0f, tab, on ? style::kFocus : Color::Hex(0xa9bce8), labels[static_cast<std::size_t>(i)], Align::Center, 0.0f);
            x += width - b * 0.6f + 6.0f;
        }
        (void)id;
        return result;
    }

    bool ModeCard(Frame& f, const Id id, const Rect& rect, const ModeCardDesc& desc, const bool selected)
    {
        const ItemState state = f.Item(id, rect, true);
        const float b = Bevel(rect.h);
        if (state.focused)
        {
            FocusPlate(f, rect, state.glow);
        }
        else
        {
            Parallelogram(f, rect, state.hovered ? style::kNavy3.Faded(0.9f) : style::kNavy);
            ParallelogramOutline(f, rect, selected ? style::kBlue : style::kLine, selected ? 2.5f : 1.0f);
        }
        const Color ink = state.focused ? style::kFocusText : style::kText;
        const float left = rect.x + b * 0.55f + 28.0f;
        const auto leftAt = [&](const float y) { return EdgeAt(rect, y) + 30.0f; };
        (void)left;
        // "2v2": the digits big, the "v" small
        const std::string_view big = desc.big;
        const std::size_t v = big.find('v');
        const float digitsY = rect.y + 24.0f;
        float x = leftAt(rect.y + 80.0f);
        if (v != std::string_view::npos)
        {
            const std::string_view first = big.substr(0, v);
            const std::string_view second = big.substr(v + 1);
            Text(f, FontStyle::BlackItalic, 96.0f, {x, digitsY}, ink, first);
            x += TextWidth(f, FontStyle::BlackItalic, 96.0f, first) + 2.0f;
            Text(f, FontStyle::BlackItalic, 34.0f, {x, digitsY + 52.0f}, ink, "v");
            x += TextWidth(f, FontStyle::BlackItalic, 34.0f, "v") + 2.0f;
            Text(f, FontStyle::BlackItalic, 96.0f, {x, digitsY}, ink, second);
        }
        else
        {
            Text(f, FontStyle::BlackItalic, 96.0f, {x, digitsY}, ink, big);
        }
        Text(f, FontStyle::ExtraBoldItalic, 26.0f, {leftAt(rect.y + 140.0f), rect.y + 124.0f}, ink, desc.name);
        // the squares of the teams: two rows (blue above, orange below)
        const int count = std::clamp(desc.playersPerTeam, 1, 4);
        for (int team = 0; team < 2; ++team)
        {
            const float rowY = rect.y + 168.0f + static_cast<float>(team) * 34.0f;
            const Color tint = state.focused ? style::TeamDark(team) : style::TeamColor(team);
            for (int i = 0; i < count; ++i)
            {
                FillRect(f, {leftAt(rowY + 13.0f) + static_cast<float>(i) * 32.0f, rowY, 26.0f, 26.0f}, tint);
            }
        }
        Text(f, FontStyle::SemiBold, 20.0f, {leftAt(rect.y + 276.0f), rect.y + 262.0f}, state.focused ? Color::Hex(0x4a5d85) : style::kTextDim, desc.caption);
        return state.activated;
    }

    // ---------------------------------------------------------------- the garage

    bool CarCard(Frame& f, const Id id, const Rect& rect, const CarCardDesc& desc)
    {
        const ItemState state = f.Item(id, rect, true);
        if (state.focused)
        {
            FocusPlate(f, rect, state.glow);
        }
        else
        {
            Parallelogram(f, rect, state.hovered ? style::kNavy3.Faded(0.9f) : style::kNavy);
            ParallelogramOutline(f, rect, style::kLine, 1.0f);
        }
        const Color ink = state.focused ? style::kFocusText : style::kText;
        const float x = EdgeAt(rect, rect.y + 36.0f) + 26.0f;
        Text(f, FontStyle::BlackItalic, 34.0f, {x, rect.y + 12.0f}, ink, desc.name);
        Text(f, FontStyle::SemiBold, 16.0f, {EdgeAt(rect, rect.y + 62.0f) + 26.0f, rect.y + 54.0f}, state.focused ? Color::Hex(0x4a5d85) : style::kTextDim, desc.klass, Align::Left, 0.1f);
        // the profile at the lower right: in the focus the blue body goes dark so that it reads on the white plate
        const Rect picture{rect.x + rect.w - 188.0f, rect.y + rect.h - 98.0f, 168.0f, 84.0f};
        Image(f, desc.image, picture, state.focused ? Color::Hex(0x3f6fd0) : Color::Hex(0xffffff));
        if (desc.equipped)
        {
            const Color good = state.focused ? Color::Hex(0x12803c) : style::kGood;
            const float y = rect.y + rect.h - 30.0f;
            IconGlyph(f, Icon::Check, 16.0f, {EdgeAt(rect, y) + 34.0f, y + 9.0f}, good);
            Text(f, FontStyle::Bold, 16.0f, {EdgeAt(rect, y) + 48.0f, y}, good, "EQUIPPED", Align::Left, 0.08f);
        }
        return state.activated;
    }

    void StatBar(Frame& f, const Vec2 pos, const StatBarDesc& desc)
    {
        constexpr int kSegments = 10;
        constexpr float kWidth = 30.0f;
        constexpr float kHeight = 16.0f;
        constexpr float kGap = 5.0f;
        const int value = std::clamp(desc.value, 0, kSegments);
        const bool compare = desc.compare >= 0;
        const int other = std::clamp(desc.compare, 0, kSegments);
        const Color gain = desc.betterIsMore ? style::kGood : style::kAccent;
        const Color loss = desc.betterIsMore ? style::kBad : style::kAccent;
        for (int i = 0; i < kSegments; ++i)
        {
            const Rect segment{pos.x + static_cast<float>(i) * (kWidth + kGap), pos.y, kWidth, kHeight};
            if (!compare)
            {
                Parallelogram(f, segment, i < value ? style::kBlueHi : style::kNavy3);
                continue;
            }
            if (i < value && i < other)
            {
                Parallelogram(f, segment, style::kBlueHi);
            }
            else if (i < value)
            {
                Parallelogram(f, segment, gain);
            }
            else if (i < other)
            {
                Parallelogram(f, segment, style::kNavy3.Faded(0.5f));
                ParallelogramOutline(f, segment, loss, 1.5f);
            }
            else
            {
                Parallelogram(f, segment, style::kNavy3);
            }
        }
        if (compare && value != other)
        {
            const bool up = value > other;
            char text[16];
            std::snprintf(text, sizeof(text), "%s%d", up ? "+" : "\xE2\x88\x92", std::abs(value - other));
            Text(f, FontStyle::BlackItalic, 26.0f, {pos.x + static_cast<float>(kSegments) * (kWidth + kGap) + 36.0f, pos.y - 6.0f}, up ? gain : loss, text);
        }
    }

    // ---------------------------------------------------------------- rows

    RowFrame Row(Frame& f, const Id id, const Rect& rect, const std::string_view label, const bool enabled, const float contentLeft)
    {
        RowFrame row;
        row.state = f.Item(id, rect, enabled);
        RowPlate(f, rect, row.state, enabled);
        TextIn(f, FontStyle::SemiBold, 26.0f, rect, Enabled(row.state.focused ? style::kFocusText : style::kText, enabled), label, Align::Left, 26.0f);
        row.content = {rect.x + contentLeft, rect.y, rect.w - contentLeft - 24.0f, rect.h};
        return row;
    }

    int OptionRow(Frame& f, const Id id, const Rect& rect, const std::string_view label, Span<std::string_view> options, int selected, const bool enabled)
    {
        const RowFrame row = Row(f, id, rect, label, enabled, rect.w < 800.0f ? 266.0f : 346.0f);
        const int count = static_cast<int>(options.size());
        if (count == 0)
        {
            return selected;
        }
        selected = std::clamp(selected, 0, count - 1);
        if (row.state.focused && enabled)
        {
            if (f.TakeNav(Nav::Left))
            {
                selected = std::max(selected - 1, 0);
            }
            else if (f.TakeNav(Nav::Right))
            {
                selected = std::min(selected + 1, count - 1);
            }
        }
        const int clicked = DrawSegments(f, row.state, row.content, options, selected, enabled);
        return clicked >= 0 ? clicked : selected;
    }

    bool ToggleRow(Frame& f, const Id id, const Rect& rect, const std::string_view label, const bool value, const bool enabled, const std::string_view offText,
                   const std::string_view onText)
    {
        const std::string_view options[2] = {offText, onText};
        return OptionRow(f, id, rect, label, Span<std::string_view>(options), value ? 1 : 0, enabled) == 1;
    }

    float SliderRow(Frame& f, const Id id, const Rect& rect, const std::string_view label, float value, const float minimum, const float maximum, const float step,
                    const std::string_view valueText, const bool enabled)
    {
        const RowFrame row = Row(f, id, rect, label, enabled, 395.0f);
        const float span = std::max(maximum - minimum, 1e-6f);
        value = std::clamp(value, minimum, maximum);
        const float trackWidth = std::min(row.content.w - 130.0f, 430.0f);
        const Rect track{row.content.x, rect.y + (rect.h - 10.0f) * 0.5f, trackWidth, 10.0f};

        if (row.state.focused && enabled)
        {
            const float amount = step * StepFactor(f);
            if (f.TakeNav(Nav::Left))
            {
                value = std::max(value - amount, minimum);
            }
            else if (f.TakeNav(Nav::Right))
            {
                value = std::min(value + amount, maximum);
            }
        }
        // the pointer drags the thumb from a press on the track (or on the row beside it) until the button is up
        int& dragging = f.IntState(id);
        if (enabled && f.input().MousePressed() && row.state.hovered)
        {
            const Rect hit = f.ToScreen(Rect{track.x - 12.0f, rect.y, trackWidth + 24.0f, rect.h});
            dragging = hit.Contains(f.input().MousePosition()) ? 1 : 0;
        }
        if (!f.input().MouseDown())
        {
            dragging = 0;
        }
        if (dragging != 0 && enabled)
        {
            const Vec2 mouse = f.ToDesign(f.input().MousePosition());
            const float t = std::clamp((mouse.x - track.x) / track.w, 0.0f, 1.0f);
            value = minimum + t * span;
        }
        if (step > 0.0f)
        {
            value = std::clamp(minimum + std::round((value - minimum) / step) * step, minimum, maximum);
        }

        const float fraction = (value - minimum) / span;
        const bool lit = row.state.focused;
        FillRect(f, track, Enabled(lit ? kTrackOnWhite : Color::Hex(0x0c1830), enabled));
        FillRect(f, {track.x, track.y, track.w * fraction, track.h}, Enabled(lit ? kSegSelectedOnWhite : style::kBlue, enabled));
        TextIn(f, FontStyle::BlackItalic, 26.0f, rect, Enabled(lit ? style::kFocusText : style::kText, enabled), valueText, Align::Right, 26.0f);
        return value;
    }

    int StepperRow(Frame& f, const Id id, const Rect& rect, const std::string_view label, const std::string_view valueText, const std::string_view note, const bool enabled)
    {
        const RowFrame row = Row(f, id, rect, label, enabled, rect.w < 800.0f ? 266.0f : 346.0f);
        int step = 0;
        if (row.state.focused && enabled)
        {
            if (f.TakeNav(Nav::Left))
            {
                step = -1;
            }
            else if (f.TakeNav(Nav::Right))
            {
                step = 1;
            }
        }
        const Color ink = Enabled(row.state.focused ? style::kFocusText : style::kText, enabled);
        const float middleY = rect.y + rect.h * 0.5f;
        const float valueWidth = TextWidth(f, FontStyle::ExtraBoldItalic, 34.0f, valueText);
        const Vec2 leftChevron{row.content.x + 12.0f, middleY};
        const float valueX = row.content.x + 40.0f;
        const Vec2 rightChevron{valueX + valueWidth + 28.0f, middleY};
        IconGlyph(f, Icon::ChevronLeft, 22.0f, leftChevron, ink);
        TextIn(f, FontStyle::ExtraBoldItalic, 34.0f, {valueX, rect.y, valueWidth, rect.h}, ink, valueText);
        IconGlyph(f, Icon::ChevronRight, 22.0f, rightChevron, ink);
        if (!note.empty())
        {
            TextIn(f, FontStyle::SemiBold, 26.0f, {rightChevron.x + 30.0f, rect.y, 200.0f, rect.h}, Enabled(row.state.focused ? Color::Hex(0x4a5d85) : style::kTextDim, enabled), note);
        }
        if (enabled && f.input().MousePressed())
        {
            const Vec2 mouse = f.input().MousePosition();
            if (f.ToScreen(Rect{leftChevron.x - 18.0f, rect.y, 36.0f, rect.h}).Contains(mouse))
            {
                step = -1;
            }
            else if (f.ToScreen(Rect{rightChevron.x - 18.0f, rect.y, 36.0f, rect.h}).Contains(mouse))
            {
                step = 1;
            }
        }
        return step;
    }

    bool TextRow(Frame& f, const Id id, const Rect& rect, const std::string_view label, std::string& text, const std::size_t maxLength, const std::string_view allowed)
    {
        const RowFrame row = Row(f, id, rect, label, true, 266.0f);
        bool changed = false;
        if (row.state.focused)
        {
            f.SetTextEditing();
            for (const char c : f.input().Typed())
            {
                const bool printable = static_cast<unsigned char>(c) >= 0x20 && c != 0x7F;
                const bool permitted = allowed.empty() || allowed.find(c) != std::string_view::npos;
                if (printable && permitted && text.size() < maxLength)
                {
                    text.push_back(c);
                    changed = true;
                }
            }
            if (f.input().KeyPressed(0x08) && !text.empty())
            {
                // erase one UTF-8 character
                std::size_t cut = text.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                {
                    --cut;
                }
                text.erase(cut);
                changed = true;
            }
        }
        const Rect field{row.content.x, rect.y + 8.0f, std::min(row.content.w, 520.0f), rect.h - 16.0f};
        FillRect(f, field, style::kNavySolid);
        TextIn(f, FontStyle::SemiBold, 26.0f, field, style::kFocus, text, Align::Left, 14.0f);
        if (row.state.focused && std::fmod(f.Time(), 1.0) < 0.6)
        {
            const float x = field.x + 14.0f + TextWidth(f, FontStyle::SemiBold, 26.0f, text) + 2.0f;
            FillRect(f, {x, field.y + 7.0f, 2.0f, field.h - 14.0f}, style::kAccent);
        }
        return changed;
    }

    ItemState ValueRow(Frame& f, const Id id, const Rect& rect, const std::string_view label, const std::string_view value, const bool enabled, const bool emphasis)
    {
        const RowFrame row = Row(f, id, rect, label, enabled, 266.0f);
        if (emphasis)
        {
            TextIn(f, FontStyle::BlackItalic, 26.0f, {row.content.x, rect.y, row.content.w, rect.h}, Enabled(row.state.focused ? style::kFocusText : style::kText, enabled), value);
        }
        else
        {
            TextIn(f, FontStyle::SemiBold, 26.0f, {row.content.x, rect.y, row.content.w, rect.h}, Enabled(row.state.focused ? Color::Hex(0x4a5d85) : style::kTextDim, enabled), value);
        }
        return row.state;
    }

    BindRowResult BindRow(Frame& f, const Id id, const Rect& rect, const std::string_view action, const std::string_view keyboardText, const std::string_view padText,
                          const bool enabled)
    {
        BindRowResult result;
        const RowFrame row = Row(f, id, rect, action, enabled, 442.0f);
        int& plate = f.IntState(id);
        plate = std::clamp(plate, 0, 1);
        result.focused = row.state.focused;
        const Rect plates[2] = {{rect.x + 442.0f, rect.y + (rect.h - style::kBindPlateHeight) * 0.5f, style::kBindPlateWidth, style::kBindPlateHeight},
                                {rect.x + 672.0f, rect.y + (rect.h - style::kBindPlateHeight) * 0.5f, style::kBindPlateWidth, style::kBindPlateHeight}};
        if (row.state.focused && enabled)
        {
            if (f.TakeNav(Nav::Left))
            {
                plate = 0;
            }
            else if (f.TakeNav(Nav::Right))
            {
                plate = 1;
            }
            if (row.state.activated)
            {
                result.rebind = true;
            }
            if (f.TakeNav(Nav::Clear))
            {
                result.clear = true;
            }
            if (f.TakeNav(Nav::ResetRow))
            {
                result.resetRow = true;
            }
        }
        if (enabled && f.input().MousePressed())
        {
            for (int i = 0; i < 2; ++i)
            {
                if (f.ToScreen(plates[i]).Contains(f.input().MousePosition()))
                {
                    plate = i;
                    result.rebind = true;
                }
            }
        }
        result.plate = plate;
        const std::string_view texts[2] = {keyboardText, padText};
        for (int i = 0; i < 2; ++i)
        {
            const bool chosen = row.state.focused && plate == i;
            Color fill = row.state.focused ? kPlateLight : style::kNavySolid;
            Color ink = row.state.focused ? Color::Hex(0x1c2c52) : style::kText;
            if (chosen)
            {
                fill = Color::Hex(0xe3ecff);
            }
            Parallelogram(f, plates[i], Enabled(fill, enabled));
            if (chosen)
            {
                ParallelogramOutline(f, plates[i], style::kAccent, 2.0f);
            }
            TextIn(f, FontStyle::Bold, 22.0f, plates[i], Enabled(ink, enabled), texts[i].empty() ? std::string_view("-") : texts[i], Align::Center, 0.0f);
        }
        return result;
    }

    void ColumnCaption(Frame& f, const Vec2 pos, const std::string_view text, const Align align)
    {
        Text(f, FontStyle::Bold, 16.0f, pos, style::kTextDim, text, align, 0.1f);
    }

    // ---------------------------------------------------------------- panels, chips, bars

    void Panel(Frame& f, const Rect& rect, const bool stripe, const bool solid)
    {
        Parallelogram(f, rect, solid ? style::kNavySolid : style::kNavy);
        ParallelogramOutline(f, rect, style::kLine, 1.0f);
        if (stripe)
        {
            Parallelogram(f, {rect.x, rect.y, Bevel(rect.h) + 6.0f, rect.h}, style::kAccent);
        }
    }

    void ScreenTitle(Frame& f, const std::string_view small, const std::string_view big, const Vec2 pos, const bool brand)
    {
        if (brand)
        {
            Text(f, FontStyle::ExtraBoldItalic, 34.0f, {pos.x, pos.y - 6.0f}, style::kAccent, small, Align::Left, 0.1f);
        }
        else
        {
            Text(f, FontStyle::SemiBold, 24.0f, {pos.x, pos.y - 2.0f}, style::kTextDim, small);
        }
        float y = brand ? pos.y + 28.0f : pos.y + 26.0f;
        std::size_t start = 0;
        while (start <= big.size())
        {
            const std::size_t end = big.find('\n', start);
            const std::string_view line = big.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            // the name of the game is 96 px (the main menu), the title of a screen 64 px (§4.3)
            Text(f, FontStyle::BlackItalic, brand ? 96.0f : 64.0f, {pos.x, y}, style::kText, line);
            y += brand ? 86.0f : 60.0f;
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }
    }

    float Chip(Frame& f, const Vec2 rightTop, const std::string_view text, const Icon icon)
    {
        const float textWidth = TextWidth(f, FontStyle::Bold, 18.0f, text, 0.08f);
        const float iconWidth = icon != Icon::None ? 30.0f : 0.0f;
        const float width = textWidth + iconWidth + 30.0f;
        const Rect box{rightTop.x - width, rightTop.y, width, 32.0f};
        FillRect(f, box, style::kNavy);
        OutlineRect(f, box, style::kLine, 1.0f);
        float x = box.x + 15.0f;
        if (icon != Icon::None)
        {
            IconGlyph(f, icon, 18.0f, {x + 9.0f, box.y + box.h * 0.5f}, style::kText);
            x += iconWidth;
        }
        TextIn(f, FontStyle::Bold, 18.0f, {x, box.y, textWidth, box.h}, style::kText, text, Align::Left, 0.0f, 0.08f);
        return width;
    }

    void ProgressBar(Frame& f, const Rect& rect, const float fraction)
    {
        Parallelogram(f, rect, Color::Hex(0x070f22, 0.9f));
        const float t = std::clamp(fraction, 0.0f, 1.0f);
        if (t > 0.0f)
        {
            ParallelogramGradient(f, {rect.x, rect.y, std::max(rect.w * t, Bevel(rect.h) + 2.0f), rect.h}, style::kBlue, style::kAccent);
        }
    }

    // ---------------------------------------------------------------- hints

    float KeyCap(Frame& f, const Vec2 pos, const std::string_view text, const bool dim)
    {
        const float width = std::max(TextWidth(f, FontStyle::Bold, 20.0f, text) + 24.0f, 38.0f);
        const Rect box{pos.x, pos.y, width, 38.0f};
        const float alpha = dim ? 0.45f : 1.0f;
        const Rect s = f.ToScreen(box);
        f.DrawList()->AddRectFilled(ImVec2(s.x, s.y), ImVec2(s.x + s.w, s.y + s.h), ToU32(style::kNavySolid.Faded(0.6f * alpha)), 4.0f * f.metrics().scale);
        f.DrawList()->AddRect(ImVec2(s.x, s.y), ImVec2(s.x + s.w, s.y + s.h), ToU32(style::kText.Faded(0.75f * alpha)), 4.0f * f.metrics().scale, 0, std::max(1.5f * f.metrics().scale, 1.0f));
        TextIn(f, FontStyle::Bold, 20.0f, box, style::kText.Faded(alpha), text, Align::Center);
        return width;
    }

    namespace
    {
        // One glyph of a hint; returns its width. `pos` is its left top.
        float DrawGlyph(Frame& f, const Glyph glyph, const Vec2 pos, const bool dim)
        {
            const float alpha = dim ? 0.62f : 1.0f;
            const auto cap = [&](const std::string_view text) { return KeyCap(f, pos, text, dim); };
            const auto iconCap = [&](const Icon icon)
            {
                const float width = 42.0f;
                const Rect box{pos.x, pos.y, width, 38.0f};
                const Rect s = f.ToScreen(box);
                f.DrawList()->AddRectFilled(ImVec2(s.x, s.y), ImVec2(s.x + s.w, s.y + s.h), ToU32(style::kNavySolid.Faded(0.6f * alpha)), 4.0f * f.metrics().scale);
                f.DrawList()->AddRect(ImVec2(s.x, s.y), ImVec2(s.x + s.w, s.y + s.h), ToU32(style::kText.Faded(0.75f * alpha)), 4.0f * f.metrics().scale, 0,
                                      std::max(1.5f * f.metrics().scale, 1.0f));
                IconGlyph(f, icon, 22.0f, {pos.x + width * 0.5f, pos.y + 19.0f}, style::kText.Faded(alpha));
                return width;
            };
            const auto padCircle = [&](const Color color, const std::string_view letter)
            {
                const Vec2 centre{pos.x + 19.0f, pos.y + 19.0f};
                Circle(f, centre, 19.0f, color.Faded(alpha));
                TextIn(f, FontStyle::ExtraBoldItalic, 18.0f, {pos.x, pos.y, 38.0f, 38.0f}, Color::Hex(0x08142c, alpha), letter, Align::Center);
                return 38.0f;
            };
            const auto padPill = [&](const std::string_view text)
            {
                const float width = std::max(TextWidth(f, FontStyle::Bold, 18.0f, text) + 22.0f, 38.0f);
                const Rect box{pos.x, pos.y, width, 38.0f};
                const Rect s = f.ToScreen(box);
                f.DrawList()->AddRectFilled(ImVec2(s.x, s.y), ImVec2(s.x + s.w, s.y + s.h), ToU32(Color::Hex(0xc9d4ec, alpha)), 8.0f * f.metrics().scale);
                TextIn(f, FontStyle::Bold, 18.0f, box, Color::Hex(0x08142c, alpha), text, Align::Center);
                return width;
            };
            switch (glyph)
            {
            case Glyph::None: return 0.0f;
            case Glyph::UpDown: return iconCap(Icon::ArrowsVertical);
            case Glyph::LeftRight: return iconCap(Icon::ArrowsHorizontal);
            case Glyph::AllArrows: return iconCap(Icon::ArrowsHorizontal) + 4.0f + 0.0f;
            case Glyph::Enter: return cap("Enter");
            case Glyph::Esc: return cap("Esc");
            case Glyph::Del: return cap("Del");
            case Glyph::R: return cap("R");
            case Glyph::G: return cap("G");
            case Glyph::Tab: return cap("Tab");
            case Glyph::Space: return cap("Space");
            case Glyph::QE: return cap("Q E");
            case Glyph::PadA: return padCircle(style::kPadA, "A");
            case Glyph::PadB: return padCircle(style::kPadB, "B");
            case Glyph::PadX: return padCircle(style::kPadX, "X");
            case Glyph::PadY: return padCircle(style::kPadY, "Y");
            case Glyph::PadLBRB: return padPill("LB");
            case Glyph::PadBack: return padPill("Back");
            case Glyph::PadStart: return padPill("Start");
            case Glyph::PadDpad: return padPill("D-pad");
            }
            return 0.0f;
        }
    }

    void HintBar(Frame& f, Span<Hint> hints)
    {
        const Space saved = f.space();
        f.SetSpace(Space::Menu);
        const bool pad = f.LastDevice() == Device::Gamepad;
        float x = 80.0f;
        const float y = 1006.0f;
        for (const Hint& hint : hints)
        {
            const bool both = hint.keyboard != Glyph::None && hint.pad != Glyph::None;
            if (hint.keyboard != Glyph::None)
            {
                x += DrawGlyph(f, hint.keyboard, {x, y}, both && pad) + 10.0f;
            }
            if (hint.pad != Glyph::None)
            {
                x += DrawGlyph(f, hint.pad, {x, y}, both && !pad) + 12.0f;
            }
            Text(f, FontStyle::SemiBold, 22.0f, {x, y + 4.0f}, style::kTextDim, hint.text);
            x += TextWidth(f, FontStyle::SemiBold, 22.0f, hint.text) + 36.0f;
        }
        f.SetSpace(saved);
    }

    // ---------------------------------------------------------------- dialogs

    DialogResult ConfirmDialog(Frame& f, const Id id, const ConfirmDesc& desc)
    {
        f.PushScope(id);
        const Space saved = f.space();
        f.SetSpace(Space::Menu);
        Dim(f, style::kDim);
        const Rect panel{580.0f, 380.0f, 760.0f, 300.0f};
        Panel(f, panel, true, true);
        Text(f, FontStyle::BlackItalic, 52.0f, {panel.x + 90.0f, panel.y + 34.0f}, style::kText, desc.title);
        float y = panel.y + 118.0f;
        std::size_t start = 0;
        while (start <= desc.text.size())
        {
            const std::size_t end = desc.text.find('\n', start);
            Text(f, FontStyle::SemiBold, 24.0f, {panel.x + 90.0f, y}, style::kTextDim, desc.text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            y += 34.0f;
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        const Id cancelId = HashId("cancel", id);
        const Id confirmId = HashId("confirm", id);
        f.SetDefaultFocus(cancelId);
        DialogResult result = DialogResult::None;
        const float top = panel.y + panel.h - 88.0f;
        if (DialogButton(f, cancelId, {panel.x + 90.0f, top, 280.0f, 60.0f}, desc.cancelLabel))
        {
            result = DialogResult::Cancel;
        }
        if (DialogButton(f, confirmId, {panel.x + 390.0f, top, 280.0f, 60.0f}, desc.confirmLabel))
        {
            result = DialogResult::Confirm;
        }
        if (f.TakeNav(Nav::Back))
        {
            result = DialogResult::Cancel;
        }
        f.SetSpace(saved);
        f.PopScope();
        return result;
    }

    void RebindDialog(Frame& f, const RebindDesc& desc)
    {
        f.PushScope(HashId("rebind"));
        const Space saved = f.space();
        f.SetSpace(Space::Menu);
        Dim(f, style::kDim);
        const Rect panel{530.0f, 380.0f, 860.0f, 300.0f};
        Panel(f, panel, true, true);
        const float centre = panel.x + panel.w * 0.5f + 20.0f;
        Text(f, FontStyle::SemiBold, 24.0f, {centre, panel.y + 40.0f}, style::kText, desc.header, Align::Center);
        Text(f, FontStyle::BlackItalic, 64.0f, {centre, panel.y + 74.0f}, style::kText, desc.action, Align::Center);
        Text(f, FontStyle::ExtraBoldItalic, 34.0f, {centre, panel.y + 160.0f}, style::kAccent, desc.prompt, Align::Center);
        const float noteWidth = TextWidth(f, FontStyle::SemiBold, 20.0f, desc.note) + (desc.conflict.empty() ? 0.0f : TextWidth(f, FontStyle::Bold, 20.0f, desc.conflict) + 40.0f);
        float x = centre - noteWidth * 0.5f;
        Text(f, FontStyle::SemiBold, 20.0f, {x, panel.y + 218.0f}, style::kTextDim, desc.note);
        x += TextWidth(f, FontStyle::SemiBold, 20.0f, desc.note);
        if (!desc.conflict.empty())
        {
            Text(f, FontStyle::Bold, 20.0f, {x, panel.y + 218.0f}, style::kMvp, desc.conflict);
            x += TextWidth(f, FontStyle::Bold, 20.0f, desc.conflict);
            Text(f, FontStyle::SemiBold, 20.0f, {x, panel.y + 218.0f}, style::kTextDim, "? It swaps.");
        }
        const float barWidth = 290.0f;
        FillRect(f, {centre - barWidth * 0.5f, panel.y + 262.0f, barWidth, 4.0f}, style::kNavy3.Faded(0.6f));
        FillRect(f, {centre - barWidth * 0.5f, panel.y + 262.0f, barWidth * std::clamp(desc.remaining, 0.0f, 1.0f), 4.0f}, style::kAccent);
        f.SetSpace(saved);
        f.PopScope();
    }

    // ---------------------------------------------------------------- lists

    ScrollView::ScrollView(Frame& f, const Id id, const Rect& view, const float contentHeight) : frame_(f)
    {
        float& stored = f.FloatState(id);
        const float maxOffset = std::max(contentHeight - view.h, 0.0f);
        if (f.input().Raw().windowActive && f.ToScreen(view).Contains(f.input().MousePosition()) && f.input().Wheel() != 0.0f)
        {
            stored -= f.input().Wheel() * 64.0f;
        }
        stored = std::clamp(stored, 0.0f, maxOffset);
        PushClip(f, view);
        clipped_ = true;
        offset_ = stored;
        f.scrollOffset_ = offset_;
        f.scrollView_ = view;
        f.scrollId_ = id;
        f.activeScroll_ = this;
    }

    ScrollView::~ScrollView()
    {
        if (clipped_)
        {
            PopClip(frame_);
        }
        frame_.scrollOffset_ = 0.0f;
        frame_.activeScroll_ = nullptr;
    }
}
