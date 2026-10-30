// BrainViewPanel.h
// The picture of a BrainView in an ImGui window with the HUD over it: the title and numbers, a bar for the rate of every group (Turn L / Turn R / Forward / Fire /
// Dopamine ...) and below them the raster of the last two seconds. The mouse over the picture orbits (left button) and zooms (wheel). Used by the editor window
// (Window > Fly Brain) and by anything else that draws ImGui: the HUD is ImGui draw commands over the texture, not part of it.

#pragma once

#include <brainview/BrainView.h>

#include <cstdint>
#include <string>

struct ImDrawList;

namespace brainview
{
    struct BrainPanelInfo
    {
        std::string title = "FLY BRAIN";
        std::string subtitle;            // e.g. the source of the connectome
        double simSeconds = 0.0;
        double realtimeRatio = 0.0;      // 0: not shown
        double spikesPerSecond = 0.0;
        bool showHud = true;
        bool showRaster = true;
        bool allowMouse = true;
    };

    // Draws the texture (an ImGui texture id, 0: a dark box with the text only) at width x height pixels at the cursor of the current ImGui window, moves the cursor
    // past it and draws the HUD over it.
    void DrawBrainViewPanel(BrainView& view, std::uint64_t texture, float width, float height, const BrainPanelInfo& info);

    // Only the HUD (the title, the bars of the groups, the raster) as draw commands in the rect: for a caller that has no ImGui window (the panel of the game draws on
    // the background list)
    void DrawBrainHud(ImDrawList* list, float left, float top, float right, float bottom, BrainView& view, const BrainPanelInfo& info);

    // The size of the bars and the raster, for the callers that lay out around them
    constexpr float kHudBarWidth = 150.0f;
}
