// RlUiFonts.h
//
// Exo 2 (Natanael Gama, SIL OFL 1.1) in four styles, as in §4.3 of the spec, and the Lucide icons. The files are static instances of
// the variable font (assets/fonts/exo2/, made by tools/make_exo2.py). ImGui bakes an atlas once (the renderer uploads it a single time),
// so the kit bakes every size it needs for a given scale into the atlas when a window gets its ImGui context, and bakes them again when
// the scale changes (a new resolution, the UI scale of the settings). Screens do not touch this: the Host (RlUiHost.h) does it; a screen
// names a style and a design size (§4.3: 220, 150, 96, 84, 64, 60, 52, 38, 34, 26, 24, 22, 20, 18, 16) and the kit draws it.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <myengine/ui/rl/RlUiCommon.h>

struct ImFont;
struct ImFontAtlas;

namespace myengine::rlui
{
    enum class FontStyle : std::uint8_t
    {
        BlackItalic = 0,     // 900 italic: the digits, the headlines, the banners
        ExtraBoldItalic,     // 800 italic: the buttons, the tabs
        Bold,                // 700: the plates, the feed
        SemiBold,            // 600: the text of the rows
        Count
    };

    // A baked font of a style at a design size. `font` is what the draw list needs; `pixelSize` is the size it was baked at (it is the
    // size to pass to ImDrawList::AddText together with `font`, scaled sizes are computed by the kit).
    struct FontHandle
    {
        ImFont* font = nullptr;
        float pixelSize = 0.0f;
        float designSize = 0.0f;
        float lineScale = 1.0f;   // line height / em of the file: the size to draw at is design size * scale * lineScale (ImGui sizes a font by its line height)
        bool Valid() const { return font != nullptr; }
    };

    class Fonts
    {
    public:
        // The design sizes baked for a style (the sizes of the spec). A size that is not in the list uses the nearest one.
        static const std::vector<float>& DesignSizes(FontStyle style);

        // Adds the fonts to `atlas` (before the atlas is built) for `scale` = window height / 1080 * UI scale. `fontDirectory` holds
        // exo2/*.ttf and lucide.ttf (assets/fonts). Returns false and fills `error` when a file is missing; the handles stay empty and the kit
        // falls back to the default font of ImGui, so nothing crashes.
        bool Bake(ImFontAtlas& atlas, const std::filesystem::path& fontDirectory, float scale, std::string* error = nullptr);
        void Clear();  // the atlas is rebuilt or gone: forget the handles
        bool Ready() const { return ready_; }
        float Scale() const { return scale_; }

        FontHandle Get(FontStyle style, float designSize) const;
        FontHandle Icons(float designSize) const;  // the Lucide icons at (about) this size

    private:
        struct Slot
        {
            FontStyle style = FontStyle::SemiBold;
            bool icons = false;
            float designSize = 0.0f;
            FontHandle handle;
        };
        std::vector<Slot> slots_;
        float scale_ = 1.0f;
        bool ready_ = false;
    };

    // The files the kit needs, relative to the font directory (for the packager and the tests)
    const std::vector<std::string>& RequiredFontFiles();
}
