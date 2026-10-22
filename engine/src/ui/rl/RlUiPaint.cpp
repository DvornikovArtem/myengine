#include <myengine/ui/rl/RlUiPaint.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <imgui/imgui_internal.h>

#include "RlUiInternal.h"

namespace myengine::rlui::paint
{
    namespace
    {
        constexpr float kPi = 3.14159265f;

        // The colour of the screen fades with the transition (menu space only: the HUD has no transition)
        ImU32 Tint(const Frame& f, const Color& c)
        {
            return ToU32(c.Faded(f.Alpha() * (f.space() == Space::Menu ? f.TransitionAlpha() : 1.0f)));
        }

        struct Quad
        {
            ImVec2 p[4];  // TL, TR, BR, BL
        };

        Quad ParallelogramPoints(Frame& f, const Rect& rect, const float bevel)
        {
            const Rect r = f.ToScreen(rect);
            const float b = (bevel < 0.0f ? Bevel(rect.h) : bevel) * f.metrics().scale;
            return {{ImVec2(r.x + b, r.y), ImVec2(r.x + r.w, r.y), ImVec2(r.x + r.w - b, r.y + r.h), ImVec2(r.x, r.y + r.h)}};
        }

        void WriteVertex(ImDrawList* dl, const ImVec2 pos, const ImU32 color)
        {
            dl->PrimWriteVtx(pos, dl->_Data->TexUvWhitePixel, color);
        }

        float Lerp(const float a, const float b, const float t) { return a + (b - a) * t; }

        Color Mix(const Color& a, const Color& b, const float t)
        {
            return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t)};
        }

        // A fan of rings: the centre has `centre`, the rings fall off smoothly (about a quadratic curve) to a transparent rim
        void DrawRadial(ImDrawList* dl, const ImVec2 c, const float rx, const float ry, const Color& centre, const float alphaScale)
        {
            constexpr int kSegments = 40;
            constexpr int kRings = 5;
            const float radii[kRings] = {0.25f, 0.5f, 0.72f, 0.88f, 1.0f};
            const float falloff[kRings] = {0.72f, 0.40f, 0.18f, 0.06f, 0.0f};
            dl->PrimReserve(kSegments * 3 + (kRings - 1) * kSegments * 6, 1 + kRings * kSegments);
            const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
            const auto colorAt = [&](const float k) { return ToU32(Color{centre.r, centre.g, centre.b, centre.a * k * alphaScale}); };
            WriteVertex(dl, c, colorAt(1.0f));
            for (int ring = 0; ring < kRings; ++ring)
            {
                const ImU32 color = colorAt(falloff[ring]);
                for (int i = 0; i < kSegments; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(kSegments) * 2.0f * kPi;
                    WriteVertex(dl, ImVec2(c.x + std::cos(t) * rx * radii[ring], c.y + std::sin(t) * ry * radii[ring]), color);
                }
            }
            for (int i = 0; i < kSegments; ++i)
            {
                const int next = (i + 1) % kSegments;
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + next));
            }
            for (int ring = 0; ring < kRings - 1; ++ring)
            {
                for (int i = 0; i < kSegments; ++i)
                {
                    const int next = (i + 1) % kSegments;
                    const int a = 1 + ring * kSegments + i;
                    const int b = 1 + ring * kSegments + next;
                    const int c2 = 1 + (ring + 1) * kSegments + i;
                    const int d = 1 + (ring + 1) * kSegments + next;
                    for (const int index : {a, c2, b, b, c2, d})
                    {
                        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
                    }
                }
            }
        }

        const char* Utf8Icon(const unsigned int codepoint, char (&buffer)[8])
        {
            // the Private Use Area: three bytes
            buffer[0] = static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
            buffer[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            buffer[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
            buffer[3] = 0;
            return buffer;
        }
    }

    void Parallelogram(Frame& f, const Rect& rect, const Color color, const float bevel)
    {
        const Quad q = ParallelogramPoints(f, rect, bevel);
        f.DrawList()->AddConvexPolyFilled(q.p, 4, Tint(f, color));
    }

    void ParallelogramGradient(Frame& f, const Rect& rect, const Color left, const Color right, const float bevel)
    {
        const Quad q = ParallelogramPoints(f, rect, bevel);
        ImDrawList* dl = f.DrawList();
        const ImU32 l = Tint(f, left);
        const ImU32 r = Tint(f, right);
        dl->PrimReserve(6, 4);
        const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
        WriteVertex(dl, q.p[0], l);
        WriteVertex(dl, q.p[1], r);
        WriteVertex(dl, q.p[2], r);
        WriteVertex(dl, q.p[3], l);
        for (const int index : {0, 1, 2, 0, 2, 3})
        {
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
        }
    }

    void ParallelogramGradient3(Frame& f, const Rect& rect, const Color left, const Color middle, const Color right, const float bevel)
    {
        const Quad q = ParallelogramPoints(f, rect, bevel);
        ImDrawList* dl = f.DrawList();
        const ImU32 l = Tint(f, left);
        const ImU32 m = Tint(f, middle);
        const ImU32 r = Tint(f, right);
        const ImVec2 topMid((q.p[0].x + q.p[1].x) * 0.5f, q.p[0].y);
        const ImVec2 bottomMid((q.p[3].x + q.p[2].x) * 0.5f, q.p[3].y);
        dl->PrimReserve(12, 6);
        const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
        WriteVertex(dl, q.p[0], l);      // 0 TL
        WriteVertex(dl, topMid, m);      // 1 TM
        WriteVertex(dl, q.p[1], r);      // 2 TR
        WriteVertex(dl, q.p[3], l);      // 3 BL
        WriteVertex(dl, bottomMid, m);   // 4 BM
        WriteVertex(dl, q.p[2], r);      // 5 BR
        for (const int index : {0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4})
        {
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
        }
    }

    void ParallelogramOutline(Frame& f, const Rect& rect, const Color color, const float thickness, const float bevel)
    {
        const Quad q = ParallelogramPoints(f, rect, bevel);
        f.DrawList()->AddPolyline(q.p, 4, Tint(f, color), ImDrawFlags_Closed, std::max(thickness * f.metrics().scale, 1.0f));
    }

    void Glow(Frame& f, const Rect& rect, const Color color, const float spread, const int layers, const float bevel)
    {
        const int count = std::max(layers, 1);
        for (int i = count; i >= 1; --i)
        {
            const float d = spread * static_cast<float>(i) / static_cast<float>(count);
            const Rect grown{rect.x - d, rect.y - d, rect.w + 2.0f * d, rect.h + 2.0f * d};
            const float b = bevel < 0.0f ? Bevel(grown.h) : bevel;
            Parallelogram(f, grown, color.Faded(0.2f / static_cast<float>(count) * 2.0f), b);
        }
    }

    void Polygon(Frame& f, const Span<Vec2> points, const Color color)
    {
        if (points.size() < 3)
        {
            return;
        }
        std::vector<ImVec2> screen;
        screen.reserve(points.size());
        for (const Vec2& p : points)
        {
            screen.push_back(ToIm(f.ToScreen(p)));
        }
        f.DrawList()->AddConvexPolyFilled(screen.data(), static_cast<int>(screen.size()), Tint(f, color));
    }

    void RadialGlow(Frame& f, const Vec2 center, const float rx, const float ry, const Color color)
    {
        const float scale = f.metrics().scale;
        const Vec2 c = f.ToScreen(center);
        DrawRadial(f.DrawList(), ToIm(c), rx * scale, ry * scale, color, f.Alpha() * (f.space() == Space::Menu ? f.TransitionAlpha() : 1.0f));
    }

    void Image(Frame& f, const std::uint64_t texture, const Rect& rect, const Color color)
    {
        if (texture == 0)
        {
            return;
        }
        const Rect r = f.ToScreen(rect);
        f.DrawList()->AddImage(static_cast<ImTextureID>(texture), ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), Tint(f, color));
    }

    void FillRect(Frame& f, const Rect& rect, const Color color)
    {
        const Rect r = f.ToScreen(rect);
        f.DrawList()->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), Tint(f, color));
    }

    void FillRectGradient(Frame& f, const Rect& rect, const Color left, const Color right)
    {
        const Rect r = f.ToScreen(rect);
        const ImU32 l = Tint(f, left);
        const ImU32 rr = Tint(f, right);
        f.DrawList()->AddRectFilledMultiColor(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), l, rr, rr, l);
    }

    void FillRectGradientV(Frame& f, const Rect& rect, const Color top, const Color bottom)
    {
        const Rect r = f.ToScreen(rect);
        const ImU32 t = Tint(f, top);
        const ImU32 b = Tint(f, bottom);
        f.DrawList()->AddRectFilledMultiColor(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), t, t, b, b);
    }

    void OutlineRect(Frame& f, const Rect& rect, const Color color, const float thickness)
    {
        const Rect r = f.ToScreen(rect);
        f.DrawList()->AddRect(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), Tint(f, color), 0.0f, 0, std::max(thickness * f.metrics().scale, 1.0f));
    }

    void Line(Frame& f, const Vec2 a, const Vec2 b, const Color color, const float thickness)
    {
        const Vec2 sa = f.ToScreen(a);
        const Vec2 sb = f.ToScreen(b);
        f.DrawList()->AddLine(ToIm(sa), ToIm(sb), Tint(f, color), std::max(thickness * f.metrics().scale, 1.0f));
    }

    void Circle(Frame& f, const Vec2 center, const float radius, const Color color)
    {
        const Vec2 c = f.ToScreen(center);
        f.DrawList()->AddCircleFilled(ToIm(c), radius * f.metrics().scale, Tint(f, color), 48);
    }

    void CircleOutline(Frame& f, const Vec2 center, const float radius, const Color color, const float thickness)
    {
        const Vec2 c = f.ToScreen(center);
        f.DrawList()->AddCircle(ToIm(c), radius * f.metrics().scale, Tint(f, color), 48, std::max(thickness * f.metrics().scale, 1.0f));
    }

    void Arc(Frame& f, const Vec2 center, const float radius, const float thickness, const float a0, const float a1, const Color color)
    {
        const Vec2 c = f.ToScreen(center);
        ImDrawList* dl = f.DrawList();
        dl->PathArcTo(ToIm(c), radius * f.metrics().scale, a0, a1, std::max(8, static_cast<int>(std::fabs(a1 - a0) / (3.0f * kPi / 180.0f))));
        dl->PathStroke(Tint(f, color), 0, thickness * f.metrics().scale);
    }

    void ArcGradient(Frame& f, const Vec2 center, const float radius, const float thickness, const float a0, const float a1, const Color colorA, const Color colorB)
    {
        const Vec2 c = f.ToScreen(center);
        const float scale = f.metrics().scale;
        const float outer = (radius + thickness * 0.5f) * scale;
        const float inner = (radius - thickness * 0.5f) * scale;
        const int segments = std::max(2, static_cast<int>(std::ceil(std::fabs(a1 - a0) / (3.0f * kPi / 180.0f))));
        ImDrawList* dl = f.DrawList();
        dl->PrimReserve(segments * 6, (segments + 1) * 2);
        const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
        for (int i = 0; i <= segments; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            const float a = Lerp(a0, a1, t);
            const ImU32 color = Tint(f, Mix(colorA, colorB, t));
            WriteVertex(dl, ImVec2(c.x + std::cos(a) * outer, c.y + std::sin(a) * outer), color);
            WriteVertex(dl, ImVec2(c.x + std::cos(a) * inner, c.y + std::sin(a) * inner), color);
        }
        for (int i = 0; i < segments; ++i)
        {
            const int v = i * 2;
            for (const int index : {v, v + 1, v + 2, v + 1, v + 3, v + 2})
            {
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
            }
        }
    }

    namespace
    {
        // The italic Exo 2 draws the Cyrillic in its italic forms: "п" is an "n", "т" is an "m", the names of the players are unreadable.
        // A text with Cyrillic in an italic style is drawn upright (Bold) instead: the Latin headlines keep the slant.
        FontStyle StyleFor(const FontStyle style, const std::string_view text)
        {
            if (style != FontStyle::BlackItalic && style != FontStyle::ExtraBoldItalic)
            {
                return style;
            }
            for (const char c : text)
            {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u >= 0xD0 && u <= 0xD3)
                {
                    return FontStyle::Bold;
                }
            }
            return style;
        }
    }

    float TextWidth(Frame& f, const FontStyle style, const float designSize, const std::string_view text, const float letterSpacing)
    {
        if (text.empty())
        {
            return 0.0f;
        }
        const FontHandle handle = f.fonts().Ready() ? f.fonts().Get(StyleFor(style, text), designSize) : FontHandle{};
        const float scale = f.metrics().scale;
        ImFont* font = handle.Valid() ? handle.font : ImGui::GetFont();
        const float size = handle.Valid() ? handle.DrawSize(designSize, scale) : designSize * scale;
        const float width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
        int glyphs = 0;
        for (const char c : text)
        {
            glyphs += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
        }
        return width / scale + letterSpacing * designSize * static_cast<float>(std::max(glyphs - 1, 0));
    }

    float TextHeight(Frame& f, const FontStyle style, const float designSize)
    {
        const FontHandle handle = f.fonts().Ready() ? f.fonts().Get(style, designSize) : FontHandle{};
        return handle.Valid() ? designSize * handle.lineScale : designSize;
    }

    void Text(Frame& f, const FontStyle style, const float designSize, const Vec2 pos, const Color color, const std::string_view text, const Align align,
              const float letterSpacing)
    {
        if (text.empty())
        {
            return;
        }
        const FontHandle handle = f.fonts().Ready() ? f.fonts().Get(StyleFor(style, text), designSize) : FontHandle{};
        const float scale = f.metrics().scale;
        ImFont* font = handle.Valid() ? handle.font : ImGui::GetFont();
        const float size = handle.Valid() ? handle.DrawSize(designSize, scale) : designSize * scale;
        const float width = TextWidth(f, style, designSize, text, letterSpacing);
        float x = pos.x;
        if (align == Align::Center)
        {
            x -= width * 0.5f;
        }
        else if (align == Align::Right)
        {
            x -= width;
        }
        const Vec2 origin = f.ToScreen(Vec2{x, pos.y});
        const ImU32 col = Tint(f, color);
        ImDrawList* dl = f.DrawList();
        if (letterSpacing == 0.0f)
        {
            dl->AddText(font, size, ToIm(origin), col, text.data(), text.data() + text.size());
            return;
        }
        // with the letter spacing: glyph after glyph
        float cursor = origin.x;
        const float spacing = letterSpacing * designSize * scale;
        for (std::size_t i = 0; i < text.size();)
        {
            std::size_t length = 1;
            while (i + length < text.size() && (static_cast<unsigned char>(text[i + length]) & 0xC0) == 0x80)
            {
                ++length;
            }
            dl->AddText(font, size, ImVec2(cursor, origin.y), col, text.data() + i, text.data() + i + length);
            cursor += font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data() + i, text.data() + i + length).x + spacing;
            i += length;
        }
    }

    void TextIn(Frame& f, const FontStyle style, const float designSize, const Rect& rect, const Color color, const std::string_view text, const Align align,
                const float padding, const float letterSpacing)
    {
        const float line = TextHeight(f, style, designSize);
        const float y = rect.y + (rect.h - line) * 0.5f - designSize * 0.05f;
        float x = rect.x + padding;
        if (align == Align::Center)
        {
            x = rect.x + rect.w * 0.5f;
        }
        else if (align == Align::Right)
        {
            x = rect.x + rect.w - padding;
        }
        Text(f, style, designSize, {x, y}, color, text, align, letterSpacing);
    }

    void TextShadow(Frame& f, const FontStyle style, const float designSize, const Vec2 pos, const Color color, const std::string_view text, const Color shadow,
                    const float shadowOffset, const Align align)
    {
        Text(f, style, designSize, {pos.x, pos.y + shadowOffset}, shadow, text, align);
        Text(f, style, designSize, pos, color, text, align);
    }

    void IconGlyph(Frame& f, const Icon icon, const float designSize, const Vec2 center, const Color color)
    {
        const unsigned int codepoint = IconCodepoint(icon);
        if (codepoint == 0 || !f.fonts().Ready())
        {
            return;
        }
        const FontHandle handle = f.fonts().Icons(designSize);
        if (!handle.Valid())
        {
            return;
        }
        char buffer[8];
        const char* text = Utf8Icon(codepoint, buffer);
        const float scale = f.metrics().scale;
        const float size = handle.DrawSize(designSize, scale);
        const ImVec2 extent = handle.font->CalcTextSizeA(size, FLT_MAX, 0.0f, text, text + 3);
        const Vec2 c = f.ToScreen(center);
        f.DrawList()->AddText(handle.font, size, ImVec2(c.x - extent.x * 0.5f, c.y - size * 0.5f), Tint(f, color), text, text + 3);
    }

    void PushClip(Frame& f, const Rect& rect)
    {
        const Rect r = f.ToScreen(rect);
        f.DrawList()->PushClipRect(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), true);
    }

    void PopClip(Frame& f) { f.DrawList()->PopClipRect(); }

    void Dim(Frame& f, const Color color)
    {
        const Metrics& m = f.metrics();
        f.DrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(m.window.x, m.window.y), ToU32(color));
    }

    void MenuBackdrop(Frame& f)
    {
        const Metrics& m = f.metrics();
        ImDrawList* dl = f.DrawList();
        const float w = m.window.x;
        const float h = m.window.y;
        // the night: dark on top, the blue of the stadium in the middle, a warm dark at the bottom
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h * 0.45f), IM_COL32(4, 9, 24, 255), IM_COL32(4, 9, 24, 255), IM_COL32(11, 26, 60, 255), IM_COL32(11, 26, 60, 255));
        dl->AddRectFilledMultiColor(ImVec2(0, h * 0.45f), ImVec2(w, h), IM_COL32(11, 26, 60, 255), IM_COL32(11, 26, 60, 255), IM_COL32(24, 24, 38, 255), IM_COL32(24, 24, 38, 255));

        // the glow of the stage in the middle: a soft radial light, and a smaller brighter one in its heart
        DrawRadial(dl, ImVec2(w * 0.5f, h * 0.40f), w * 0.46f, h * 0.42f, Color::Hex(0x2c64b8, 0.55f), 1.0f);
        DrawRadial(dl, ImVec2(w * 0.5f, h * 0.38f), w * 0.20f, h * 0.20f, Color::Hex(0x4f95e8, 0.30f), 1.0f);

        // the beams of the floodlights: slanting strips from the top down to a third of the height
        const float scale = m.scale;
        const float beamHeight = 420.0f * scale;
        const float step = 162.0f * scale;
        const float beamWidth = 22.0f * scale;
        const float shift = beamHeight * 0.27f;   // the beams lean to the left going down
        for (float x = -shift; x < w + shift; x += step)
        {
            ImVec2 p[4] = {ImVec2(x + shift, 0.0f), ImVec2(x + shift + beamWidth, 0.0f), ImVec2(x + beamWidth, beamHeight), ImVec2(x, beamHeight)};
            dl->AddConvexPolyFilled(p, 4, IM_COL32(110, 160, 255, 12));
        }
        (void)h;
    }
}
