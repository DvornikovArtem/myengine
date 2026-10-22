// RlUiGallery.h
//
// The gallery of the kit: every screen and every HUD state of the mock-ups (.team/design/rl-ui/png) built from the widgets with fake data. It is
// how the kit is checked by eye and by the designer, and an example of how a screen is made from the kit (the source is the reference for
// coder-2: RlUiGallery.cpp).
//
// Run it:  set MYENGINE_UI_GALLERY=<name>   then   myengine.exe --screenshot out.tga --screenshot-frame 90 --exit-after-frames 100
// (`<name>`: one of GalleryScreenName(...), "all" cycles them). The gallery is interactive (the focus, the tabs, the sliders work on fake data).

#pragma once

#include <string_view>

#include <myengine/ui/rl/RlUiFrame.h>

namespace myengine::rlui
{
    enum class GalleryScreen : int
    {
        Main = 0,        // rl-main
        Play,            // rl-play
        Private,         // rl-private
        Loading,         // rl-loading
        Pause,           // rl-pause
        Results,         // rl-results
        Controls,        // rl-set-controls
        Rebind,          // rl-set-rebind
        Camera,          // rl-set-camera
        Video,           // rl-set-video
        HudKickoff,      // rl-hud-kickoff
        HudGoal,         // rl-hud-goal
        HudScoreboard,   // rl-hud-scoreboard
        HudReplay,       // no mock-up: the replay of a goal (G7e)
        Count
    };

    const char* GalleryScreenName(GalleryScreen screen);                  // "main", "play", "private", ..., "hud-scoreboard"
    bool ParseGalleryScreen(std::string_view name, GalleryScreen& out);   // "all" is not a screen: the caller cycles

    // Draws one screen of the gallery for this frame (the backdrop included; the HUD screens add a fake game view)
    void DrawGalleryScreen(Frame& f, GalleryScreen screen);
}
