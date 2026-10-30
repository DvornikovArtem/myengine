// DoomMenuWidgets.cpp

#include "DoomMenuWidgets.h"

#include <myengine/ui/rl/RlUiInput.h>

#include <algorithm>
#include <cmath>

namespace myengine::game::fd
{
    using namespace rlui;

    namespace
    {
        constexpr float kPi = 3.14159265f;

        // Where the capitals sit in the line box of the faces (ascent and cap height in em): Big Shoulders 0.984 and 0.8, JetBrains Mono 1.02 and 0.73
        struct Face
        {
            float ascent;
            float cap;
        };
        Face FaceOf(const FontStyle style)
        {
            return style == kDisplay || style == kItem ? Face{0.984f, 0.80f} : Face{1.02f, 0.73f};
        }

        float Lerp(const float a, const float b, const float t) { return a + (b - a) * t; }
        float Clamp01(const float v) { return std::clamp(v, 0.0f, 1.0f); }

        std::string GlyphRange(const char first, const char last)
        {
            std::string text;
            for (int c = first; c <= last; ++c)
            {
                text.push_back(static_cast<char>(c));
            }
            return text;
        }

        // an arrowhead of a triangle: `dir` +1 right, -1 left; `size` the half height
        void Triangle(Frame& f, const Vec2 center, const int dir, const float size, const Color color)
        {
            const float d = static_cast<float>(dir);
            paint::Polygon(f, {{center.x - d * size * 0.7f, center.y - size}, {center.x - d * size * 0.7f, center.y + size}, {center.x + d * size * 0.9f, center.y}}, color);
        }

        // a corner mark: two lines of `length` from the corner
        void Corner(Frame& f, const Vec2 corner, const float dx, const float dy, const float length, const Color color)
        {
            paint::Line(f, corner, {corner.x + dx * length, corner.y}, color, 2.0f);
            paint::Line(f, corner, {corner.x, corner.y + dy * length}, color, 2.0f);
        }

        bool PointerIn(const Frame& f, const Rect& designRect)
        {
            return f.input().Raw().windowActive && f.ToScreen(designRect).Contains(f.input().MousePosition());
        }

        bool Pressed(const Frame& f, const Rect& designRect) { return f.input().MousePressed() && PointerIn(f, designRect); }

        // the cap of a key: returns its width; draws it when `draw`
        float KeyCap(Frame& f, const Vec2 pos, const std::string& key, const bool draw, const bool pad, const float alpha)
        {
            const bool arrow = key == "UP" || key == "DOWN" || key == "LEFT" || key == "RIGHT";
            std::string text = key;
            if (pad)
            {
                text = key == "ENTER" ? "A" : (key == "ESC" ? "B" : (arrow ? "" : key));
            }
            const float size = 26.0f;
            float width = size;
            if (!pad && !arrow)
            {
                width = std::max(size, paint::TextWidth(f, kMonoBold, 13.0f, text) + 16.0f);
            }
            if (!draw)
            {
                return width;
            }
            const Rect box{pos.x, pos.y - size * 0.5f, width, size};
            const Vec2 c = box.Center();
            if (pad)
            {
                paint::CircleOutline(f, {c.x, c.y}, 13.0f, tone::kLine2.Faded(alpha), 1.5f);
            }
            else
            {
                paint::OutlineRect(f, box, tone::kLine2.Faded(alpha), 1.0f);
                paint::FillRect(f, {box.x, box.y + box.h - 3.0f, box.w, 3.0f}, tone::kLine2.Faded(alpha));
            }
            if (arrow)
            {
                const Color ink = tone::kBone.Faded(alpha);
                const float dxs = key == "LEFT" ? -1.0f : (key == "RIGHT" ? 1.0f : 0.0f);
                const float dys = key == "UP" ? -1.0f : (key == "DOWN" ? 1.0f : 0.0f);
                const Vec2 tip{c.x + dxs * 5.0f, c.y + dys * 5.0f - (pad ? 0.0f : 1.0f)};
                const Vec2 tail{c.x - dxs * 5.0f, c.y - dys * 5.0f - (pad ? 0.0f : 1.0f)};
                paint::Line(f, tail, tip, ink, 1.6f);
                const Vec2 side{-dys, dxs};
                paint::Line(f, tip, {tip.x - dxs * 4.0f + side.x * 4.0f, tip.y - dys * 4.0f + side.y * 4.0f}, ink, 1.6f);
                paint::Line(f, tip, {tip.x - dxs * 4.0f - side.x * 4.0f, tip.y - dys * 4.0f - side.y * 4.0f}, ink, 1.6f);
            }
            else
            {
                TextMid(f, kMonoBold, 13.0f, {c.x, c.y - (pad ? 0.0f : 1.0f)}, tone::kBone.Faded(alpha), text, Align::Center);
            }
            return width;
        }
    }

    const rlui::FontSet& FontSet()
    {
        static const rlui::FontSet set = []
        {
            rlui::FontSet s;
            s.files[static_cast<int>(kDisplay)] = "bigshoulders/BigShouldersDisplay-Black.ttf";
            s.files[static_cast<int>(kItem)] = "bigshoulders/BigShouldersDisplay-ExtraBold.ttf";
            s.files[static_cast<int>(kMonoBold)] = "jetbrainsmono/JetBrainsMono-Bold.ttf";
            s.files[static_cast<int>(kMono)] = "JetBrainsMono-Regular.ttf";
            s.requiredFile = s.files[static_cast<int>(kDisplay)];
            // the headlines: the big ones carry only the letters they are written with (FLYDOOM, PAUSED)
            const std::string capitals = GlyphRange(' ', 'Z');
            for (const float size : {26.0f, 30.0f, 34.0f, 96.0f, 104.0f})
            {
                s.sizes[static_cast<int>(kDisplay)].push_back({size, capitals});
            }
            for (const float size : {200.0f, 220.0f})
            {
                s.sizes[static_cast<int>(kDisplay)].push_back({size, " ADEFLMOPSUY"});
            }
            const std::string latin = GlyphRange(' ', '~') + "\xB7\xD7";
            for (const float size : {16.0f, 18.0f, 22.0f, 26.0f, 30.0f, 34.0f, 38.0f, 44.0f, 54.0f, 56.0f})
            {
                s.sizes[static_cast<int>(kItem)].push_back({size, {}});
            }
            // the Latin of the spec, Cyrillic (the names of the files), the arrows and the signs
            const std::vector<std::uint32_t> mono = {0x0020, 0x007E, 0x00A0, 0x017F, 0x0400, 0x04FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x2212, 0x2212, 0x25B6, 0x25B6, 0x25B8, 0x25B8, 0x25C2, 0x25C2};
            s.ranges[static_cast<int>(kItem)] = {0x0020, 0x007E, 0x00B7, 0x00B7, 0x00D7, 0x00D7};
            (void)latin;
            for (const FontStyle style : {kMonoBold, kMono})
            {
                for (const float size : {12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f, 18.0f, 20.0f, 22.0f, 24.0f})
                {
                    s.sizes[static_cast<int>(style)].push_back({size, {}});
                }
                s.ranges[static_cast<int>(style)] = mono;
            }
            return s;
        }();
        return set;
    }

    // ---- text

    std::string Upper(const std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
        {
            if (c >= 'a' && c <= 'z')
            {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        return out;
    }

    void Caps(Frame& f, const FontStyle style, const float size, const Vec2 pos, const Color color, const std::string_view text, const Align align, const float spacing)
    {
        paint::Text(f, style, size, pos, color, Upper(text), align, spacing);
    }

    float CapsWidth(Frame& f, const FontStyle style, const float size, const std::string_view text, const float spacing)
    {
        return paint::TextWidth(f, style, size, Upper(text), spacing);
    }

    void TextTop(Frame& f, const FontStyle style, const float size, const Vec2 pos, const Color color, const std::string_view text, const Align align, const float spacing)
    {
        const Face face = FaceOf(style);
        paint::Text(f, style, size, {pos.x, pos.y - (face.ascent - face.cap) * size}, color, text, align, spacing);
    }

    void TextMid(Frame& f, const FontStyle style, const float size, const Vec2 pos, const Color color, const std::string_view text, const Align align, const float spacing)
    {
        const Face face = FaceOf(style);
        paint::Text(f, style, size, {pos.x, pos.y - (face.ascent - face.cap * 0.5f) * size}, color, text, align, spacing);
    }

    void CapsMid(Frame& f, const FontStyle style, const float size, const Vec2 pos, const Color color, const std::string_view text, const Align align, const float spacing)
    {
        TextMid(f, style, size, pos, color, Upper(text), align, spacing);
    }

    std::vector<std::string> Wrap(Frame& f, const FontStyle style, const float size, const std::string_view text, const float width)
    {
        std::vector<std::string> lines;
        std::string current;
        std::string_view rest = text;
        while (!rest.empty())
        {
            const std::size_t space = rest.find(' ');
            const std::string word(rest.substr(0, space));
            rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
            const std::string trial = current.empty() ? word : current + " " + word;
            if (!current.empty() && paint::TextWidth(f, style, size, trial) > width)
            {
                lines.push_back(current);
                current = word;
            }
            else
            {
                current = trial;
            }
        }
        if (!current.empty())
        {
            lines.push_back(current);
        }
        return lines;
    }

    float RunsWidth(Frame& f, const std::vector<Run>& runs, const float size, const float spacing)
    {
        float width = 0.0f;
        for (const Run& run : runs)
        {
            width += (run.dot ? 16.0f - spacing * size : (run.check ? 24.0f - spacing * size : CapsWidth(f, run.bold ? kMonoBold : kMono, size, run.text, spacing))) + spacing * size;
        }
        return std::max(width - spacing * size, 0.0f);
    }

    void DrawRuns(Frame& f, const std::vector<Run>& runs, const float size, const Vec2 pos, const Align align, const float spacing)
    {
        float x = pos.x;
        const float width = RunsWidth(f, runs, size, spacing);
        if (align == Align::Right)
        {
            x -= width;
        }
        else if (align == Align::Center)
        {
            x -= width * 0.5f;
        }
        for (const Run& run : runs)
        {
            const FontStyle style = run.bold ? kMonoBold : kMono;
            if (run.dot)
            {
                paint::Circle(f, {x + 5.0f, pos.y}, 4.5f, run.color);
                x += 16.0f;
                continue;
            }
            if (run.check)
            {
                paint::Line(f, {x + 1.0f, pos.y + 1.0f}, {x + 5.0f, pos.y + 5.0f}, run.color, 2.0f);
                paint::Line(f, {x + 5.0f, pos.y + 5.0f}, {x + 13.0f, pos.y - 5.0f}, run.color, 2.0f);
                x += 24.0f;
                continue;
            }
            CapsMid(f, style, size, {x, pos.y}, run.color, run.text, Align::Left, spacing);
            x += CapsWidth(f, style, size, run.text, spacing) + spacing * size;
        }
    }

    // ---- the page

    void PaintGround(Frame& f)
    {
        f.SetSpace(Space::Screen);
        paint::FillRect(f, {0.0f, 0.0f, f.metrics().designWindow.x, f.metrics().designWindow.y}, tone::kInk);
        f.SetSpace(Space::Menu);
    }

    void PaintFrame(Frame& f, const float grow, const bool withGrid, const float rightLimit)
    {
        const Space saved = f.space();
        f.SetSpace(Space::Screen);
        const Vec2 window = f.metrics().designWindow;
        if (withGrid)
        {
            // the grid, 48 px, hardly there
            const Color grid = Color::Hex(0xFFFFFF, 0.025f);
            for (float x = 0.0f; x < window.x; x += 48.0f)
            {
                paint::FillRect(f, {x, 0.0f, 1.0f, window.y}, grid);
            }
            for (float y = 0.0f; y < window.y; y += 48.0f)
            {
                paint::FillRect(f, {0.0f, y, window.x, 1.0f}, grid);
            }
            // the vignette: the edges go dark
            const Color dark = Color::Hex(0x000000, 0.45f);
            const Color clear = Color::Hex(0x000000, 0.0f);
            const float edgeX = window.x * 0.18f;
            const float edgeY = window.y * 0.22f;
            paint::FillRectGradient(f, {0.0f, 0.0f, edgeX, window.y}, dark, clear);
            paint::FillRectGradient(f, {window.x - edgeX, 0.0f, edgeX, window.y}, clear, dark);
            paint::FillRectGradientV(f, {0.0f, 0.0f, window.x, edgeY}, dark, clear);
            paint::FillRectGradientV(f, {0.0f, window.y - edgeY, window.x, edgeY}, clear, dark);
        }
        // the marks of the corners
        const float length = 22.0f * Clamp01(grow);
        if (length > 0.5f)
        {
            const float m = 32.0f;
            Corner(f, {m, m}, 1.0f, 1.0f, length, tone::kLine2);
            const float right = rightLimit > 0.0f ? std::min(rightLimit, window.x) : window.x;
            Corner(f, {right - m, m}, -1.0f, 1.0f, length, tone::kLine2);
            Corner(f, {m, window.y - m}, 1.0f, -1.0f, length, tone::kLine2);
            Corner(f, {right - m, window.y - m}, -1.0f, -1.0f, length, tone::kLine2);
        }
        f.SetSpace(saved);
    }

    void TopLeft(Frame& f, const std::vector<Run>& runs) { DrawRuns(f, runs, 15.0f, {96.0f, 55.0f}, Align::Left); }

    void DrawFitted(Frame& f, const FontStyle style, const float size, const Vec2 pos, const Color color, const std::string_view text, const float maxWidth, const Align align, const float spacing)
    {
        float use = size;
        float width = paint::TextWidth(f, style, use, text, spacing);
        if (width > maxWidth && width > 0.0f)
        {
            use = std::max(12.0f, std::floor(size * maxWidth / width));
            width = paint::TextWidth(f, style, use, text, spacing);
        }
        std::string shown(text);
        while (width > maxWidth && shown.size() > 4)
        {
            // cut one symbol (not a byte) from the end and put the dots
            std::size_t cut = shown.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(shown[cut]) & 0xC0) == 0x80)
            {
                --cut;
            }
            shown.erase(cut);
            while (!shown.empty() && shown.back() == '.')
            {
                shown.pop_back();
            }
            width = paint::TextWidth(f, style, use, shown + "...", spacing);
            if (width <= maxWidth)
            {
                shown += "...";
                break;
            }
        }
        paint::Text(f, style, use, pos, color, shown, align, spacing);
    }

    void TopRight(Frame& f, const std::vector<std::vector<Run>>& segments, const float rightEdge)
    {
        float right = rightEdge;
        for (std::size_t i = segments.size(); i-- > 0;)
        {
            const float width = RunsWidth(f, segments[i], 15.0f);
            DrawRuns(f, segments[i], 15.0f, {right, 55.0f}, Align::Right);
            right -= width;
            if (i > 0)
            {
                paint::Line(f, {right - 20.0f, 45.0f}, {right - 20.0f, 66.0f}, tone::kLine2, 1.0f);
                right -= 40.0f;
            }
        }
    }

    // ---- the bar of the focus

    void BarTarget(Fx& fx, const Rect& rect, const float plank)
    {
        fx.bar.target = rect;
        fx.bar.targetPlank = plank;
        fx.bar.wanted = true;
    }

    void DrawBar(Frame& f, Fx& fx)
    {
        Fx::Bar& bar = fx.bar;
        if (bar.wanted && !bar.placed)
        {
            bar.rect = bar.target;
            bar.plank = bar.targetPlank;
            bar.placed = true;
        }
        const float dt = f.DeltaTime();
        const float follow = fx.reduced ? 1.0f : 1.0f - std::exp(-dt / 0.035f);   // ~120 ms to settle
        if (bar.wanted)
        {
            bar.rect.x = Lerp(bar.rect.x, bar.target.x, follow);
            bar.rect.y = Lerp(bar.rect.y, bar.target.y, follow);
            bar.rect.w = Lerp(bar.rect.w, bar.target.w, follow);
            bar.rect.h = Lerp(bar.rect.h, bar.target.h, follow);
            bar.plank = Lerp(bar.plank, bar.targetPlank, follow);
            bar.alpha = std::min(1.0f, bar.alpha + dt / 0.12f);
        }
        else
        {
            bar.alpha = std::max(0.0f, bar.alpha - dt / 0.12f);
        }
        bar.wanted = false;
        if (bar.alpha <= 0.01f)
        {
            return;
        }
        const Rect r = bar.rect.Moved(bar.shift, 0.0f);
        const float a = bar.alpha * bar.gate;
        // the red gradient: 0.20 -> 0.06 at 55 % -> 0
        paint::FillRectGradient(f, {r.x, r.y, r.w * 0.55f, r.h}, tone::kRed.Faded(0.20f * a), tone::kRed.Faded(0.06f * a));
        paint::FillRectGradient(f, {r.x + r.w * 0.55f, r.y, r.w * 0.45f, r.h}, tone::kRed.Faded(0.06f * a), tone::kRed.Faded(0.0f));
        // the stripe and its soft light
        paint::FillRectGradient(f, {r.x - 14.0f, r.y, 14.0f, r.h}, tone::kRed.Faded(0.0f), tone::kRed.Faded(0.22f * a));
        paint::FillRect(f, {r.x, r.y, bar.plank, r.h}, tone::kRed.Faded(a));
    }

    // ---- widgets

    rlui::ItemState MenuItem(Frame& f, Fx& fx, const Id id, const float y, const MenuItemSpec& spec, const float alpha, const float dx)
    {
        const float height = spec.sub.empty() ? 76.0f : 104.0f;
        const Rect rect{72.0f, y, 640.0f, height};
        const rlui::ItemState state = f.Item(id, rect, spec.enabled);
        if (state.focused)
        {
            BarTarget(fx, rect, 6.0f);
        }
        const float a = alpha * (spec.enabled ? 1.0f : 0.55f);
        const Color word = state.focused ? tone::kBone : (spec.enabled ? tone::kDim : tone::kFaint);
        CapsMid(f, kMono, 15.0f, {96.0f + dx, y + 38.0f}, (state.focused ? tone::kRed : tone::kFaint).Faded(a), spec.index);
        const float wordWidth = CapsWidth(f, kItem, 54.0f, spec.word, 0.02f);
        CapsMid(f, kItem, 54.0f, {144.0f + dx, y + 38.0f}, word.Faded(a), spec.word, Align::Left, 0.02f);
        if (state.focused && spec.enabled)
        {
            Triangle(f, {144.0f + dx + wordWidth + 22.0f, y + 38.0f}, 1, 5.0f, tone::kRed.Faded(alpha));
        }
        if (!spec.sub.empty())
        {
            CapsMid(f, kMono, 14.0f, {144.0f + dx, y + 79.0f}, (state.focused ? tone::kDim : tone::kFaint).Faded(a), spec.sub);
        }
        return state;
    }

    float DrawTags(Frame& f, const float right, const float midY, const std::vector<Tag>& tags)
    {
        float x = right;
        for (std::size_t i = tags.size(); i-- > 0;)
        {
            const float width = CapsWidth(f, kMonoBold, 12.0f, tags[i].text, 0.08f) + 18.0f;
            const Rect box{x - width, midY - 11.0f, width, 22.0f};
            paint::OutlineRect(f, box, tags[i].color, 1.0f);
            CapsMid(f, kMonoBold, 12.0f, {box.x + 9.0f, midY}, tags[i].color, tags[i].text, Align::Left, 0.08f);
            x -= width + 6.0f;
        }
        return x + 6.0f;
    }

    rlui::ItemState ModeRow(Frame& f, Fx& fx, const Id id, const Rect& rect, const ModeRowSpec& spec)
    {
        const rlui::ItemState state = f.Item(id, rect, true);
        if (state.focused)
        {
            BarTarget(fx, rect, 6.0f);
        }
        paint::FillRect(f, {rect.x, rect.y + rect.h - 1.0f, rect.w, 1.0f}, tone::kLine);
        if (spec.chosen && !state.focused)
        {
            paint::FillRect(f, {rect.x, rect.y, 3.0f, rect.h}, tone::kRed);
        }
        const bool lit = state.focused || spec.chosen;
        const Color name = !spec.available ? tone::kFaint : (lit ? tone::kBone : tone::kDim);
        CapsMid(f, kMono, 15.0f, {rect.x + 24.0f, rect.y + 34.0f}, (lit ? tone::kRed : tone::kFaint), spec.index);
        CapsMid(f, kItem, 44.0f, {rect.x + 72.0f, rect.y + 41.0f}, name, spec.name, Align::Left, 0.02f);
        TextMid(f, kMono, 15.0f, {rect.x + 72.0f, rect.y + 78.0f}, spec.available ? (lit ? tone::kDim : tone::kFaint) : tone::kFaint, spec.caption);
        if (spec.available)
        {
            DrawTags(f, rect.x + rect.w - 20.0f, rect.y + 39.0f, spec.tags);
        }
        else
        {
            DrawTags(f, rect.x + rect.w - 20.0f, rect.y + 39.0f, {{spec.soon.empty() ? "SOON" : spec.soon, tone::kAmber}});
        }
        return state;
    }

    rlui::ItemState SetupRow(Frame& f, Fx& fx, const Id id, const Rect& rect, const std::string_view label, const bool enabled, const std::string_view tag)
    {
        const rlui::ItemState state = f.Item(id, rect, enabled);
        if (state.focused)
        {
            BarTarget(fx, rect, 4.0f);
        }
        paint::FillRect(f, {rect.x, rect.y + rect.h - 1.0f, rect.w, 1.0f}, tone::kLine);
        const Color ink = state.focused ? tone::kBone : tone::kDim;
        TextMid(f, kMono, 16.0f, {rect.x + 20.0f, rect.y + 26.0f}, ink, label);
        if (!tag.empty())
        {
            const float labelWidth = paint::TextWidth(f, kMono, 16.0f, label);
            DrawTags(f, rect.x + 20.0f + labelWidth + 14.0f + CapsWidth(f, kMonoBold, 12.0f, tag, 0.08f) + 18.0f, rect.y + 26.0f, {{std::string(tag), tone::kAmber}});
        }
        return state;
    }

    Edit Stepper(Frame& f, Fx& fx, const Id id, const Rect& rect, const std::string_view label, const std::string_view value, const std::string_view meta)
    {
        Edit edit;
        const rlui::ItemState state = SetupRow(f, fx, id, rect, label);
        edit.focused = state.focused;
        edit.hovered = state.hovered;
        const float mid = rect.y + 26.0f;
        const float right = rect.x + rect.w - 20.0f;
        const Color arrow = state.focused ? tone::kRed : tone::kFaint;
        const float metaWidth = meta.empty() ? 0.0f : paint::TextWidth(f, kMono, 14.0f, meta) + 12.0f;
        const float valueWidth = CapsWidth(f, kMonoBold, 17.0f, value, 0.06f);
        const Vec2 rightArrow{right - 5.0f, mid};
        const float valueRight = right - 24.0f;
        const Vec2 leftArrow{valueRight - metaWidth - valueWidth - 20.0f, mid};
        Triangle(f, rightArrow, 1, 4.5f, arrow);
        Triangle(f, leftArrow, -1, 4.5f, arrow);
        if (!meta.empty())
        {
            TextMid(f, kMono, 14.0f, {valueRight, mid}, tone::kFaint, meta, Align::Right);
        }
        CapsMid(f, kMonoBold, 17.0f, {valueRight - metaWidth, mid}, tone::kBone, value, Align::Right, 0.06f);
        const Rect leftHit{leftArrow.x - 14.0f, rect.y, 28.0f, rect.h};
        const Rect rightHit{rightArrow.x - 14.0f, rect.y, 28.0f, rect.h};
        bool clicked = false;
        if (Pressed(f, leftHit))
        {
            edit.delta = -1;
            clicked = true;
        }
        else if (Pressed(f, rightHit))
        {
            edit.delta = 1;
            clicked = true;
        }
        else if (state.focused)
        {
            if (f.TakeNav(Nav::Left))
            {
                edit.delta = -1;
            }
            else if (f.TakeNav(Nav::Right))
            {
                edit.delta = 1;
            }
        }
        edit.changed = edit.delta != 0;
        edit.activated = state.activated && !clicked;
        return edit;
    }

    Edit Segments(Frame& f, Fx& fx, const Id id, const Rect& rect, const std::string_view label, const std::vector<std::string>& labels, int& selected, const bool readOnly, const std::string_view tag)
    {
        Edit edit;
        const rlui::ItemState state = SetupRow(f, fx, id, rect, label, true, tag);
        edit.focused = state.focused;
        edit.hovered = state.hovered;
        const float mid = rect.y + 26.0f;
        std::vector<float> widths;
        float total = 0.0f;
        for (const std::string& text : labels)
        {
            widths.push_back(CapsWidth(f, kMonoBold, 14.0f, text, 0.1f) + 26.0f);
            total += widths.back();
        }
        float x = rect.x + rect.w - 16.0f - total;
        bool clicked = false;
        const int before = selected;
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            const Rect box{x, mid - 15.0f, widths[i], 30.0f};
            const bool on = static_cast<int>(i) == selected;
            const Color fill = state.focused ? tone::kRed : tone::kBone;
            if (on)
            {
                paint::FillRect(f, box, readOnly ? fill.Faded(0.55f) : fill);
            }
            paint::OutlineRect(f, box, on ? (readOnly ? fill.Faded(0.55f) : fill) : tone::kLine2, 1.0f);
            const Color ink = on ? (state.focused ? tone::kOnRed : tone::kInk) : (readOnly ? tone::kFaint : tone::kBone);
            CapsMid(f, kMonoBold, 14.0f, {box.x + box.w * 0.5f, mid}, ink, labels[i], Align::Center, 0.1f);
            if (!readOnly && Pressed(f, box))
            {
                selected = static_cast<int>(i);
                clicked = true;
            }
            x += widths[i];
        }
        if (!readOnly && state.focused && !clicked)
        {
            if (f.TakeNav(Nav::Left))
            {
                selected = std::max(selected - 1, 0);
            }
            else if (f.TakeNav(Nav::Right))
            {
                selected = std::min(selected + 1, static_cast<int>(labels.size()) - 1);
            }
        }
        edit.changed = selected != before;
        edit.activated = state.activated && !clicked;
        return edit;
    }

    Edit Slider(Frame& f, Fx& fx, const Id id, const Rect& rect, const std::string_view label, float& t, const std::function<float(float, int)>& keyStep, const std::string_view valueText, const int ticks, const bool midTick)
    {
        Edit edit;
        const rlui::ItemState state = SetupRow(f, fx, id, rect, label);
        edit.focused = state.focused;
        edit.hovered = state.hovered;
        const float mid = rect.y + 26.0f;
        const float valueRight = rect.x + rect.w - 16.0f;
        const float trackRight = valueRight - 104.0f;
        const Rect track{trackRight - 300.0f, mid - 1.0f, 300.0f, 2.0f};
        const float before = t;
        // the mouse: a press near the track grabs it, the drag follows until the button is up
        int& dragging = f.IntState(id);
        const Rect grab{track.x - 10.0f, rect.y, track.w + 20.0f, rect.h};
        if (Pressed(f, grab))
        {
            dragging = 1;
        }
        if (dragging != 0)
        {
            if (f.input().MouseDown())
            {
                const float x = f.ToDesign(f.input().MousePosition()).x;
                t = Clamp01((x - track.x) / track.w);
            }
            else
            {
                dragging = 0;
            }
        }
        else if (state.focused)
        {
            if (f.TakeNav(Nav::Left))
            {
                t = Clamp01(keyStep(t, -1));
            }
            else if (f.TakeNav(Nav::Right))
            {
                t = Clamp01(keyStep(t, 1));
            }
        }
        const bool lit = state.focused || dragging != 0;
        const Color fill = lit ? tone::kRed : tone::kBone;
        paint::FillRect(f, track, tone::kLine2);
        paint::FillRect(f, {track.x, track.y, track.w * t, track.h}, fill);
        for (int i = 0; i <= ticks; ++i)
        {
            const float x = track.x + track.w * static_cast<float>(i) / static_cast<float>(ticks);
            const bool tall = midTick && i * 2 == ticks;
            paint::FillRect(f, {x - 0.5f, mid + 6.0f, 1.0f, tall ? 10.0f : 5.0f}, tone::kLine2);
        }
        paint::FillRect(f, {track.x + track.w * t - 3.0f, mid - 10.0f, 6.0f, 20.0f}, fill);
        CapsMid(f, kMonoBold, 17.0f, {valueRight, mid}, tone::kBone, valueText, Align::Right, 0.02f);
        edit.changed = t != before;
        edit.activated = state.activated && dragging == 0 && !Pressed(f, grab);
        return edit;
    }

    rlui::ItemState Button(Frame& f, Fx& fx, const Id id, const Rect& rect, const std::string_view label, const ButtonKind kind, const bool canDo, const std::string_view keyHint, const bool arrow)
    {
        (void)fx;
        const rlui::ItemState state = f.Item(id, rect, true);
        const bool primary = kind == ButtonKind::Primary;
        const float glow = state.glow;
        Color ink = tone::kBone;
        if (primary && canDo)
        {
            paint::FillRect(f, rect, tone::kRed);
            ink = tone::kOnRed;
        }
        else if (primary)
        {
            paint::OutlineRect(f, rect, tone::kLine2, 1.0f);
            ink = tone::kFaint;
        }
        else
        {
            if (state.focused)
            {
                paint::FillRect(f, rect, tone::kBone.Faded(0.08f));
            }
            paint::OutlineRect(f, rect, state.focused ? tone::kBone : tone::kLine2, 1.0f);
        }
        if (state.focused)
        {
            // an outline of bone 5 px away, and the light of the red round it
            const Rect ring{rect.x - 5.0f, rect.y - 5.0f, rect.w + 10.0f, rect.h + 10.0f};
            for (int i = 0; i < 3; ++i)
            {
                const float spread = 4.0f * static_cast<float>(i + 1);
                paint::OutlineRect(f, {ring.x - spread, ring.y - spread, ring.w + 2.0f * spread, ring.h + 2.0f * spread}, tone::kRed.Faded(0.22f / static_cast<float>(i + 1) * glow), 4.0f);
            }
            paint::OutlineRect(f, ring, tone::kBone.Faded(glow), 2.0f);
        }
        CapsMid(f, kItem, 44.0f, {rect.x + 30.0f, rect.y + rect.h * 0.5f + 2.0f}, ink, label, Align::Left, 0.02f);
        if (arrow)
        {
            const float width = CapsWidth(f, kItem, 44.0f, label, 0.02f);
            Triangle(f, {rect.x + 30.0f + width + 28.0f, rect.y + rect.h * 0.5f + 1.0f}, 1, 8.0f, ink);
        }
        if (!keyHint.empty())
        {
            const float width = CapsWidth(f, kMonoBold, 12.0f, keyHint, 0.08f) + 18.0f;
            const Rect key{rect.x + rect.w - 28.0f - width, rect.y + rect.h * 0.5f - 11.0f, width, 22.0f};
            const Color keyInk = primary ? (canDo ? tone::kOnRed : tone::kFaint) : tone::kDim;
            paint::OutlineRect(f, key, keyInk, 1.0f);
            CapsMid(f, kMonoBold, 12.0f, {key.x + 9.0f, key.y + 11.0f}, keyInk, keyHint, Align::Left, 0.08f);
        }
        return state;
    }

    float KeyCapDraw(Frame& f, const Vec2 pos, const std::string& key, const float alpha)
    {
        return KeyCap(f, pos, key, true, f.LastDevice() == Device::Gamepad, alpha);
    }

    void Hints(Frame& f, const std::vector<Hint>& hints, const float alpha)
    {
        const bool pad = f.LastDevice() == Device::Gamepad;
        float x = 96.0f;
        const float mid = 1017.0f;
        for (const Hint& hint : hints)
        {
            std::vector<std::string> keys = hint.keys;
            if (pad)
            {
                // a pad has one D-pad: the arrows of a hint collapse into it
                std::vector<std::string> collapsed;
                bool arrows = false;
                for (const std::string& key : keys)
                {
                    const bool arrow = key == "UP" || key == "DOWN" || key == "LEFT" || key == "RIGHT";
                    if (arrow)
                    {
                        if (!arrows)
                        {
                            collapsed.push_back("UP");
                        }
                        arrows = true;
                    }
                    else
                    {
                        collapsed.push_back(key);
                    }
                }
                keys = collapsed;
            }
            for (const std::string& key : keys)
            {
                x += KeyCap(f, {x, mid}, key, true, pad, alpha) + 6.0f;
            }
            x += 6.0f;
            CapsMid(f, kMono, 15.0f, {x, mid}, tone::kDim.Faded(alpha), hint.text);
            x += CapsWidth(f, kMono, 15.0f, hint.text, 0.12f) + 34.0f;
        }
    }

    void Note(Frame& f, const Vec2 pos, const std::string_view text, const float width, const Color color, const float size)
    {
        paint::FillRect(f, {pos.x, pos.y - 5.0f, 10.0f, 10.0f}, color);
        const std::vector<std::string> lines = width > 0.0f ? Wrap(f, kMono, size, text, width - 24.0f) : std::vector<std::string>{std::string(text)};
        float y = pos.y;
        for (const std::string& line : lines)
        {
            TextMid(f, kMono, size, {pos.x + 22.0f, y}, color, line);
            y += size + 9.0f;
        }
    }

    void Section(Frame& f, const float x, const float y, const float width, const std::string_view index, const std::string_view name)
    {
        TextMid(f, kMonoBold, 13.0f, {x, y}, tone::kRed, index, Align::Left, 0.08f);
        const float nameX = x + 34.0f;
        CapsMid(f, kMono, 13.0f, {nameX, y}, tone::kDim, name, Align::Left, 0.14f);
        const float lineX = nameX + CapsWidth(f, kMono, 13.0f, name, 0.14f) + 18.0f;
        paint::FillRect(f, {lineX, y, std::max(x + width - lineX, 0.0f), 1.0f}, tone::kLine);
    }

    void Sheet(Frame& f, const Rect& rect, const float alpha)
    {
        paint::FillRect(f, rect, tone::kSheet.Faded(alpha));
        paint::OutlineRect(f, rect, tone::kLine.Faded(alpha), 1.0f);
        const Color tick = tone::kLine2.Faded(alpha);
        const float m = 3.0f;
        Corner(f, {rect.x - m, rect.y - m}, 1.0f, 1.0f, 14.0f, tick);
        Corner(f, {rect.x + rect.w + m, rect.y - m}, -1.0f, 1.0f, 14.0f, tick);
        Corner(f, {rect.x - m, rect.y + rect.h + m}, 1.0f, -1.0f, 14.0f, tick);
        Corner(f, {rect.x + rect.w + m, rect.y + rect.h + m}, -1.0f, -1.0f, 14.0f, tick);
    }

    void Chain(Frame& f, const float x, const float y, const std::vector<ChainNode>& nodes, const float grow, const float maxRight)
    {
        (void)maxRight;
        const float count = static_cast<float>(nodes.size());
        float cursor = x;
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const ChainNode& node = nodes[i];
            const float appear = Clamp01(grow * (count + 1.0f) - static_cast<float>(i));
            const float textWidth = CapsWidth(f, kMonoBold, 13.0f, node.text, 0.08f);
            const float metaWidth = node.meta.empty() ? 0.0f : CapsWidth(f, kMono, 11.0f, node.meta, 0.06f) + 8.0f;
            const float width = 28.0f + textWidth + metaWidth;
            const Rect box{cursor, y, width, 40.0f};
            Color line = tone::kRed;
            Color ink = tone::kRed;
            if (node.kind == NodeKind::Eye)
            {
                line = tone::kBrain.Faded(0.5f);
                ink = tone::kBone;
            }
            else if (node.kind == NodeKind::Brain)
            {
                line = tone::kBrain;
                ink = tone::kBrain;
                paint::FillRect(f, box, tone::kBrain.Faded(0.10f * appear));
            }
            paint::OutlineRect(f, box, line.Faded(appear), 1.0f);
            CapsMid(f, kMonoBold, 13.0f, {box.x + 14.0f, y + 20.0f}, ink.Faded(appear), node.text, Align::Left, 0.08f);
            if (!node.meta.empty())
            {
                CapsMid(f, kMono, 11.0f, {box.x + 14.0f + textWidth + 8.0f, y + 21.0f}, tone::kDim.Faded(appear), node.meta, Align::Left, 0.06f);
            }
            cursor += width;
            if (i + 1 < nodes.size())
            {
                // the arrow grows from the node to the next one
                const float length = 34.0f * Clamp01(grow * (count + 1.0f) - static_cast<float>(i) - 0.5f);
                const Color arrow = node.both || (node.kind == NodeKind::Red && nodes[i + 1].kind == NodeKind::Red) ? tone::kRed : tone::kBrain;
                const float mid = y + 20.0f;
                if (length > 2.0f)
                {
                    paint::FillRect(f, {cursor, mid, length, 1.0f}, arrow);
                    Triangle(f, {cursor + length - 3.0f, mid + 0.5f}, 1, 3.5f, arrow);
                    if (node.both)
                    {
                        Triangle(f, {cursor + 3.0f, mid + 0.5f}, -1, 3.5f, arrow);
                    }
                }
                cursor += 34.0f;
            }
        }
    }

    void Dial(Frame& f, const Vec2 center, const float radius, const float angleDegrees)
    {
        paint::CircleOutline(f, center, radius, tone::kLine2, 1.5f);
        for (int i = 0; i < 12; ++i)
        {
            const float a = static_cast<float>(i) * 30.0f * kPi / 180.0f;
            const Vec2 outer{center.x + std::sin(a) * radius, center.y - std::cos(a) * radius};
            const Vec2 inner{center.x + std::sin(a) * (radius - 6.0f), center.y - std::cos(a) * (radius - 6.0f)};
            paint::Line(f, inner, outer, tone::kLine2, 1.0f);
        }
        const float a = angleDegrees * kPi / 180.0f;
        paint::Line(f, center, {center.x + std::sin(a) * radius * 0.82f, center.y - std::cos(a) * radius * 0.82f}, tone::kRed, 2.0f);
        paint::Circle(f, center, 3.5f, tone::kBone);
    }

    int ConfirmQuit(Frame& f, Fx& fx, const std::string_view title, const std::string_view line, const float appear)
    {
        const Id dialog = HashId("fd/quit");
        f.PushScope(dialog);
        const Space saved = f.space();
        f.SetSpace(Space::Menu);
        const float a = Clamp01(appear);
        paint::Dim(f, Color::Hex(0x060607, 0.72f * a));
        const float lift = fx.reduced ? 0.0f : (1.0f - a) * 12.0f;
        const Rect sheet{600.0f, 390.0f + lift, 720.0f, 300.0f};
        paint::FillRect(f, sheet, Color::Hex(0x0F1012, a));
        paint::OutlineRect(f, sheet, tone::kLine2.Faded(a), 1.0f);
        paint::FillRect(f, {sheet.x, sheet.y, sheet.w, 4.0f}, tone::kRed.Faded(a));
        CapsMid(f, kItem, 56.0f, {sheet.x + 48.0f, sheet.y + 70.0f}, tone::kBone.Faded(a), title, Align::Left, 0.02f);
        TextMid(f, kMono, 17.0f, {sheet.x + 48.0f, sheet.y + 128.0f}, tone::kDim.Faded(a), line);
        const Id stayId = HashId("fd/quit-stay");
        const Id leaveId = HashId("fd/quit-leave");
        f.SetDefaultFocus(stayId);
        int result = 0;
        const rlui::ItemState stay = Button(f, fx, stayId, {sheet.x + 48.0f, sheet.y + 176.0f, 280.0f, 76.0f}, "STAY", ButtonKind::Secondary, true, "N", false);
        const rlui::ItemState leave = Button(f, fx, leaveId, {sheet.x + 392.0f, sheet.y + 176.0f, 280.0f, 76.0f}, "LEAVE", ButtonKind::Primary, true, "Y", false);
        if (leave.activated || f.input().KeyPressed('Y'))
        {
            result = 1;
        }
        else if (stay.activated || f.input().KeyPressed('N') || f.TakeNav(Nav::Back))
        {
            result = -1;
        }
        f.SetSpace(saved);
        f.PopScope();
        return result;
    }
}
