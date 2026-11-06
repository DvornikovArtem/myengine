// PrankExplorer.h
//
// The "Explorer" of the easter egg tab (docs/editor/prank-tab.md): a light Windows 11 explorer, the ten folders inside one another, the banner that
// grows louder with every level, the shaking, the files that open in a "Notepad" or in a little viewer. Everything is drawn with the draw list of the
// tab window. The gags between the levels (captcha, paywall, ...) are the business of PrankGags; the explorer only asks for them.
// Internal to engine/src/ui/editor.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <imgui/imgui.h>

#include "PrankStrings.h"

namespace myengine::ui::prank
{
    enum class GagKind : int
    {
        None,
        Captcha,   // level 4 -> 5
        Paywall,   // 5 -> 6
        Ivan,      // 6 -> 7
        Age,       // 7 -> 8
        Load99,    // 8 -> 9
        Uac,       // the generator file, level 10
        Count,
    };

    // What the explorer wants from the tab after a frame
    struct ExplorerRequest
    {
        GagKind gag = GagKind::None;
        int targetLevel = -1;   // the level the gag leads to when it is passed (-1: stay)
        bool video = false;     // the video file: the 99 % gag again, without a new level
    };

    class PrankExplorer
    {
    public:
        // Draws the explorer in [origin, origin + size] (the whole tab). `interactive` false: a gag or a dialog is on, nothing reacts
        void Draw(ImDrawList* list, ImVec2 origin, ImVec2 size, double time, float dt, bool interactive, ExplorerRequest& request);

        // Goes to a level: the palette turns into its colours, the window shakes
        void GoTo(int level);
        int Level() const { return level_; }
        int Furthest() const { return furthest_; }

        void Toast(const std::string& text);
        // A shake of the whole content, for `seconds`
        void Shake(float seconds, float amplitude);
        // The address bar fills with green (0..1); a negative value: it does not
        void SetAddressFill(float fraction) { addressFill_ = fraction; }
        // The "Working on it" text in place of the files while a video "loads"
        void SetWorking(bool working) { working_ = working; }

        bool Passed(GagKind gag) const { return passed_[static_cast<int>(gag)]; }
        void SetPassed(GagKind gag) { passed_[static_cast<int>(gag)] = true; }

        // The explorer comes back to the start (a new run of the tab)
        void Reset();

        // For the tests and the screenshots: opens a file viewer, closes it
        bool ViewerOpen() const { return viewer_ >= 0; }
        void OpenItem(int index);
        void CloseViewer() { viewer_ = -1; }

        // What the last frame drew (for the tab: the hint to the keyboard handling)
        bool SearchActive() const { return searchActive_; }

    private:
        struct Palette
        {
            ImU32 bg = 0, chrome = 0, title = 0, side = 0, text = 0, dim = 0, line = 0, field = 0, sel = 0, selb = 0, hover = 0;
        };

        static Palette PaletteFor(int level);
        ImU32 OnField() const;
        void UpdateMotion(double time, float dt);
        ImVec2 ShakeOffset(double time) const;
        void DrawTitleBar(ImDrawList* list, ImVec2 min, float width, bool interactive);
        void DrawNavBar(ImDrawList* list, ImVec2 min, float width, bool interactive);
        void DrawCommandBar(ImDrawList* list, ImVec2 min, float width, bool interactive);
        void DrawSidebar(ImDrawList* list, ImVec2 min, float height, bool interactive);
        void DrawContent(ImDrawList* list, ImVec2 min, ImVec2 size, double time, bool interactive, ExplorerRequest& request);
        void DrawStatusBar(ImDrawList* list, ImVec2 min, float width);
        void DrawBanner(ImDrawList* list, ImVec2 min, float width, double time, float& heightOut);
        void DrawToast(ImDrawList* list, ImVec2 areaMin, ImVec2 areaMax);
        void DrawMenus(ImDrawList* list, bool interactive);
        void DrawViewer(ImDrawList* list, ImVec2 areaMin, ImVec2 areaMax, double time);
        void HandleKeys(bool interactive, ExplorerRequest& request);
        void Activate(int index, ExplorerRequest& request);
        void GoUp();
        void GoForward();
        std::string StatusText() const;

        int level_ = 0;
        int furthest_ = 0;
        int selected_ = -1;
        bool passed_[static_cast<int>(GagKind::Count)]{};
        Palette palette_;
        bool paletteReady_ = false;
        float enterAge_ = 10.0f;        // seconds since the last level change
        float enterAmplitude_ = 0.0f;
        float bumpAge_ = 10.0f;         // seconds since the last short bump
        float nextBump_ = 3.0f;
        float shakeLeft_ = 0.0f;        // a shake that was asked for (seconds), and its amplitude
        float shakeAmplitude_ = 0.0f;
        float squeeze_ = 0.0f;          // the "minimize" animation, 0..1
        float squeezeAge_ = 10.0f;
        std::uint32_t random_ = 12345u;
        ImVec2 offset_{};
        ImVec2 origin_{};
        ImVec2 size_{};
        double time_ = 0.0;
        float toastAge_ = 100.0f;
        std::string toast_;
        float addressFill_ = -1.0f;
        bool working_ = false;
        int viewer_ = -1;               // the item of the level whose file is open
        float viewerAge_ = 0.0f;
        int openMenu_ = -1;             // 0 create, 1 sort, 2 view, 3 more
        ImVec2 menuAnchor_{};
        char search_[64]{};
        bool searchActive_ = false;
        float searchMessageAge_ = 100.0f;
        std::vector<ImVec2> tileMin_;
    };
}
