// RlUiPaint.h
//
// The drawing primitives of the kit on ImDrawList (§4.2 and §6.2 of the spec): the bevelled shapes (a parallelogram with a 12 degree
// slope: AddConvexPolyFilled), horizontal gradients (vertex colours: AddRectFilledMultiColor has no slope), the glow (3-4 transparent
// polygons), arcs, text with the Exo 2 styles, icons. All coordinates and sizes are DESIGN px in the current space of the frame
// (Frame::SetSpace); the functions scale them. A screen rarely needs them: the widgets (RlUiWidgets.h) and the HUD (RlUiHud.h) are
// built from them, and the gallery shows what they look like.

#pragma once

#include <string_view>

#include <myengine/ui/rl/RlUiFrame.h>

namespace myengine::rlui::paint
{
    // The shift of the top edge of a parallelogram of height h to the right, design px (h * tan 12 deg)
    constexpr float Bevel(const float height) { return height * style::kBevelTan; }

    // A parallelogram inscribed in `rect`: the top edge runs from x + bevel to x + w, the bottom edge from x to x + w - bevel (the left and the right
    // edges slope by 12 degrees). `bevel < 0` takes the standard one, Bevel(h).
    void Parallelogram(Frame& f, const Rect& rect, Color color, float bevel = -1.0f);
    // The same with a gradient left -> right (also with alpha: banners that fade out to the edges)
    void ParallelogramGradient(Frame& f, const Rect& rect, Color left, Color right, float bevel = -1.0f);
    // A gradient in three stops (left, middle, right): the banner of the team that fades at both ends
    void ParallelogramGradient3(Frame& f, const Rect& rect, Color left, Color middle, Color right, float bevel = -1.0f);
    void ParallelogramOutline(Frame& f, const Rect& rect, Color color, float thickness = 1.0f, float bevel = -1.0f);
    // The soft light round a parallelogram: `layers` polygons growing by `spread` with a falling alpha (the glow of the focus)
    void Glow(Frame& f, const Rect& rect, Color color, float spread, int layers = 4, float bevel = -1.0f);

    // A convex polygon filled with one colour (the points in design px, clockwise or not)
    void Polygon(Frame& f, Span<Vec2> points, Color color);
    // A soft round light: an ellipse (`rx`, `ry` design px) that is `color` in the middle and falls smoothly to nothing at the rim (the glow of the stage
    // behind the menus, the halo of the countdown)
    void RadialGlow(Frame& f, Vec2 center, float rx, float ry, Color color);
    void FillRect(Frame& f, const Rect& rect, Color color);
    void FillRectGradient(Frame& f, const Rect& rect, Color left, Color right);
    void FillRectGradientV(Frame& f, const Rect& rect, Color top, Color bottom);
    void OutlineRect(Frame& f, const Rect& rect, Color color, float thickness = 1.0f);
    void Line(Frame& f, Vec2 a, Vec2 b, Color color, float thickness = 1.0f);
    void Circle(Frame& f, Vec2 center, float radius, Color color);
    void CircleOutline(Frame& f, Vec2 center, float radius, Color color, float thickness = 1.0f);

    // An arc of a ring from angle a0 to a1 (radians, 0 = the right, clockwise on the screen), filled with a gradient from colorA at a0 to colorB
    // at a1 in segments of about 3 degrees (the boost ring)
    void ArcGradient(Frame& f, Vec2 center, float radius, float thickness, float a0, float a1, Color colorA, Color colorB);
    void Arc(Frame& f, Vec2 center, float radius, float thickness, float a0, float a1, Color color);

    // Text. `pos` is the left / centre / right end of the baseline box (the top of the line is pos.y), `letterSpacing` in em (the captions of the
    // columns: 0.1). A size that is not baked uses the nearest baked one.
    void Text(Frame& f, FontStyle style, float designSize, Vec2 pos, Color color, std::string_view text, Align align = Align::Left, float letterSpacing = 0.0f);
    // The same with a shadow of the colour below the glyphs (the countdown: #0b3f94, 8 px down)
    void TextShadow(Frame& f, FontStyle style, float designSize, Vec2 pos, Color color, std::string_view text, Color shadow, float shadowOffset, Align align = Align::Left);
    float TextWidth(Frame& f, FontStyle style, float designSize, std::string_view text, float letterSpacing = 0.0f);  // design px
    float TextHeight(Frame& f, FontStyle style, float designSize);                                                     // design px: the height of a line
    // Text in a box: the caps are centred on the middle of `rect` (Exo 2 sits a little low in its line), `padding` from the left / right edge
    void TextIn(Frame& f, FontStyle style, float designSize, const Rect& rect, Color color, std::string_view text, Align align = Align::Left, float padding = 0.0f,
                float letterSpacing = 0.0f);

    void IconGlyph(Frame& f, Icon icon, float designSize, Vec2 center, Color color);

    // Clipping (the lists): everything between Push and Pop is cut to `rect`
    void PushClip(Frame& f, const Rect& rect);
    void PopClip(Frame& f);

    // The dimming of the whole window (the pause, a dialog): `color` over everything under it
    void Dim(Frame& f, Color color);
    // The background of the menus when there is no 3D scene behind (the gallery): the night gradient, the slanting beams, the glow in the middle
    void MenuBackdrop(Frame& f);
}
