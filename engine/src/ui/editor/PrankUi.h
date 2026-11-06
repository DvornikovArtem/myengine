// PrankUi.h
//
// What the pieces of the easter egg tab share: colours, text in the baked sizes of the prank fonts (with the symbols the font cannot draw left out),
// the rectangles with glow, the buttons, the "18+" sign. Internal to engine/src/ui/editor.

#pragma once

#include <cstdint>
#include <string>

#include <imgui/imgui.h>

namespace myengine::ui::prank
{
    // ---- fonts: the sizes (em) and the code points baked into the atlas (UiManager.cpp), pairs first..last, ends with 0 ----
    inline constexpr float kFontSizes[] = {12.0f, 13.0f, 14.0f, 16.0f, 18.0f, 20.0f, 22.0f, 26.0f, 30.0f, 40.0f, 56.0f};
    inline constexpr ImWchar kBakedRanges[] = {0x0020, 0x007E, 0x00A0, 0x00FF, 0x0400, 0x045F, 0x2010, 0x2027, 0x2030, 0x203A, 0x20BD, 0x20BD,
                                               0x2190, 0x2193, 0x2212, 0x2212, 0x221E, 0x221E, 0x2713, 0x2713, 0x2717, 0x2717, 0};

    // ---- colours ----
    inline constexpr ImU32 Rgb(const unsigned rgb, const unsigned alpha = 255)
    {
        return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
    }

    inline ImU32 WithAlpha(const ImU32 color, const float alpha)
    {
        const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * (alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha);
        return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
    }

    ImU32 Lerp(ImU32 a, ImU32 b, float t);
    ImU32 Scaled(ImU32 color, float factor);   // the brightness times `factor` (the alpha stays)

    inline constexpr ImU32 kHot = Rgb(0xFF2D7A);
    inline constexpr ImU32 kHot2 = Rgb(0xFF5A3C);
    inline constexpr ImU32 kRed = Rgb(0xFF1F3D);
    inline constexpr ImU32 kGold = Rgb(0xFFCC4D);
    inline constexpr ImU32 kHotHover = Rgb(0xFF4F8F);
    inline constexpr ImU32 kHotPressed = Rgb(0xD81B60);
    inline constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);

    // A value that goes 0..1..0 with the given period (seconds): the pulse of the tab and of the menu button
    float Pulse(double time, float period);
    // A smooth step towards a target, for a value that moves in time: v += (target - v) * min(1, dt / seconds)
    float Approach(float value, float target, float dt, float seconds);

    // ---- text ----
    // A text for the current font: the symbols the prank fonts cannot draw (the emoji, the signs of a wide Unicode) are left out, and the spaces they
    // leave are trimmed. The result is cached.
    const std::string& Clean(const char* text);

    // The prank font nearest to `em` (the editor font of the Strong role while they are not baked) and the size to draw it at
    ImFont* FontFor(float em, float& size);
    float TextWidth(float em, const char* text);
    float TextHeight(float em);
    // Draws `text` (cleaned) with its top left corner at `position`
    void Text(ImDrawList* list, ImVec2 position, float em, ImU32 color, const char* text);
    // The same, centred on `center` (both ways)
    void TextCentered(ImDrawList* list, ImVec2 center, float em, ImU32 color, const char* text);
    // The same, with the right edge at `right`
    void TextRight(ImDrawList* list, float right, float top, float em, ImU32 color, const char* text);
    // Words wrapped to `width`; returns the height. `center`: every line is centred in the width. `maxLines` > 0 cuts the rest
    float TextWrapped(ImDrawList* list, ImVec2 position, float width, float em, ImU32 color, const char* text, bool center, float lineHeight, int maxLines = 0);
    float WrappedHeight(float width, float em, const char* text, float lineHeight);
    // The same text as Text, shrunk to fit `width` (never grows)
    void TextFit(ImDrawList* list, ImVec2 position, float width, float em, ImU32 color, const char* text);

    // ---- shapes ----
    // A glow around a rectangle: `layers` rectangles, wider and fainter outwards
    void Glow(ImDrawList* list, ImVec2 min, ImVec2 max, ImU32 color, float radius, int layers, float rounding);
    // The sign "18": a ring with the number inside. `ring` is the width of the ring
    void Sign18(ImDrawList* list, ImVec2 center, float diameter, ImU32 ringColor, ImU32 fill, ImU32 textColor, float ring, bool plus = false);
    // A vertical gradient rectangle
    void GradientV(ImDrawList* list, ImVec2 min, ImVec2 max, ImU32 top, ImU32 bottom, float rounding = 0.0f);
    // A circle spinner (a sector of a ring that turns)
    void Spinner(ImDrawList* list, ImVec2 center, float radius, float width, ImU32 color, double time, float period);

    // The vertices drawn since `start` (VtxBuffer.Size taken before): scaled and rotated about `center`, and their alpha multiplied. The appearing of a dialog
    // (alpha 0..1, scale 0.96..1), the squeeze and the tilt of the explorer
    void ScaleRotate(ImDrawList* list, int start, ImVec2 center, float scale, float degrees);
    void FadeVertices(ImDrawList* list, int start, float alpha);

    // ---- buttons ----
    enum class ButtonKind
    {
        Primary,   // kHot fill, white text
        Ghost,     // a 1.5 px ring, kHot text
        Plain,     // for dialogs of the explorer: the fill is given by the caller
    };

    // The rectangles of the buttons drawn in the last frames, by id: the scripted input of the tests clicks them (`editor prank click @pcapok`). Hit() records
    // every button; the tab clears the list at the start of its frame
    void ClearHitRecords();
    bool FindHitRecord(const std::string& id, ImVec2& center);

    // An invisible button over [min, max]: returns true when clicked. Hover and held are out values (optional)
    bool Hit(const char* id, ImVec2 min, ImVec2 max, bool* hovered = nullptr, bool* held = nullptr);
    // A button of the easter egg tab with the text centred. `enabled` false: it is drawn dimmed and does nothing
    bool Button(ImDrawList* list, const char* id, ImVec2 min, ImVec2 max, const char* label, ButtonKind kind, float em, bool enabled = true);
    // A button with free colours (the dialogs): fill, hover fill, text, optional ring
    bool ColorButton(ImDrawList* list, const char* id, ImVec2 min, ImVec2 max, const char* label, float em, ImU32 fill, ImU32 hoverFill, ImU32 text, ImU32 ring,
                     float rounding);

    inline ImVec2 Add(const ImVec2 a, const ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
}
