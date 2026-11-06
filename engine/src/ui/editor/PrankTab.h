// PrankTab.h
//
// The easter egg tab of the editor (docs/editor/prank-tab.md): "ПОРНО", a very loud tab next to the Viewport with a folder inside a folder inside a folder,
// and at the bottom an "AI generator 18+" that turns any request into a harmless little game. The whole joke is in the contrast: the wrapping promises
// something terrible, what is inside is a game, a pun or a student. Nothing here is real adult content.
//
// The tab is on by default; PrankSettings.h switches it off (the "prankTab" setting, --no-prank). Switched off, nothing of it exists: no window, no menu
// button, no badge. Internal to engine/src/ui/editor.

#pragma once

#include <memory>

#include <imgui/imgui.h>

#include "PrankExplorer.h"
#include "PrankGags.h"
#include "PrankGenerator.h"

namespace myengine::ui::prank
{
    class PrankTab
    {
    public:
        PrankTab(PrankSceneServices scene, AgeCheckFace face);
        ~PrankTab();

        // The ImGui name of the window ("###Prank" is its id; the spaces leave room for the 18+ sign and the badge that are drawn on the tab)
        static const char* WindowName();

        // Every frame while the editor runs: the generator (the assistant) goes on even when the tab is hidden
        void Update(float dt);
        // The window. `dockId`: the node of the Viewport, where the window lands when the saved layout does not know it
        void DrawWindow(ImGuiID dockId);
        // Brings the tab to the front (the menu button, the first frames of the editor)
        void RequestFocus() { focusFrames_ = 3; }

        // The button in the main menu bar, at the screen position (it must be drawn inside the menu bar window)
        float MenuButtonWidth() const;
        void DrawMenuButton(ImVec2 position);

        void Shutdown();

        // For the tests and the screenshots
        PrankExplorer& Explorer() { return explorer_; }
        PrankGags& Gags() { return gags_; }
        PrankGenerator& Generator() { return generator_; }
        bool InGenerator() const { return mode_ == Mode::Generator; }
        void EnterGenerator() { SwitchTo(Mode::Generator); }
        // The badge ("3") is shown until the tab has been the front one for 2 seconds
        bool BadgeShown() const { return !badgeGone_; }

    private:
        enum class Mode
        {
            Explorer,
            Generator,
        };

        void SwitchTo(Mode mode);
        void DrawTabDecoration(ImVec2 tabMin, ImVec2 tabMax, bool active);
        void DrawContent(ImDrawList* list, ImVec2 min, ImVec2 size, double time, float dt);

        PrankExplorer explorer_;
        PrankGags gags_;
        PrankGenerator generator_;
        Mode mode_ = Mode::Explorer;
        float switchAge_ = 10.0f;         // seconds since the mode changed (the fade)
        int pendingTarget_ = -1;
        GagKind lastGag_ = GagKind::None;
        int focusFrames_ = 0;
        float activeSeconds_ = 0.0f;
        bool badgeGone_ = false;
        bool visible_ = false;
    };
}
