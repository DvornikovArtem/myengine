// DoomController.h
//
// Who plays DOOM in FlyDoom (docs/game/doom.md, docs/game/flydoom-fly.md). The default player is the human at the keyboard; a controller replaces
// the keyboard and the mouse: the fly player (game/doomfly) is one. DoomGameFlow owns the window, the frame and the sound and asks the controller
// to run the game, so a controller can pace the game by what it needs (the fly brain makes DOOM wait for it).
//
// The interface names no engine type except ImDrawList, so a controller lives in a library that does not link the engine (game/doomfly).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImDrawList;

namespace doom
{
    class DoomGame;
}

namespace myengine::render
{
    class IRenderAdapter;
}

namespace myengine::editor
{
    class ThumbnailService;
}

namespace myengine::game
{
    // The panel of a controller as data: the flow draws it with the UI kit of the game (Exo 2, the plates of the menu), so the controller does not need the fonts
    struct DoomPanelBar
    {
        std::string label;                      // "DNp20 R"
        std::string text;                       // the value as it is shown: "30 Hz", "+0.84 deg/tic"
        float value = 0.0f;                     // the bar: value / limit (centered: -limit .. limit around the middle)
        float limit = 1.0f;
        bool centered = false;
        std::uint32_t rgb = 0x5090f0;           // 0xRRGGBB: the colour of the neurons in the cloud
    };

    struct DoomPanelGroup
    {
        std::string title;                      // "MOTOR"
        std::vector<DoomPanelBar> bars;
    };

    struct DoomPanelData
    {
        std::string title;                      // "FLY PLAYS"
        std::string subtitle;                   // "DOOMFLY neurons"
        std::string chip;                       // "1.7x real time"
        std::string brainChip;                  // "BRAIN GPU": on the picture of the cloud
        std::string brainLine;                  // the big brain: "164 789 neurons, 0.34 M spikes/s, 1.56x real time"
        std::vector<DoomPanelGroup> groups;
        std::vector<std::string> debug;         // dim lines at the foot: the speed, the tics, the resets
    };

    class IDoomController
    {
    public:
        virtual ~IDoomController() = default;

        // Called once per frame of the window instead of DoomGame::Advance. `deltaSeconds` is the real time of the frame; the controller advances the
        // game as it sees fit (a tick of DOOM is 1/35 s of game time) and returns true when DOOM drew a new frame.
        virtual bool Advance(doom::DoomGame& game, double deltaSeconds) = 0;

        // The panel of the controller (right of the picture): window pixels. A controller with nothing to show draws nothing.
        virtual void DrawPanel(ImDrawList* list, float x, float y, float width, float height) = 0;

        // Tab in the modes of a controller (the keyboard does not play): the big view of the brain over the whole window. DoomGameFlow calls
        // ToggleBigBrain on the edge of the key and DrawOverlay every frame after the picture and the panel (window pixels); both do nothing by default.
        virtual void ToggleBigBrain() {}
        virtual void DrawOverlay(ImDrawList* list, float width, float height)
        {
            (void)list;
            (void)width;
            (void)height;
        }

        // The panel as data (see DoomPanelData); false: the controller draws the panel itself in DrawPanel (no renderer, the old way). With true the flow draws the panel and
        // asks DrawBrain for the picture of the brain inside the rect it leaves for it.
        virtual bool DescribePanel(DoomPanelData& out) const
        {
            (void)out;
            return false;
        }
        virtual void DrawBrain(ImDrawList* list, float x, float y, float width, float height)
        {
            (void)list;
            (void)x;
            (void)y;
            (void)width;
            (void)height;
        }
        // The big brain is on: the flow hides the panel and draws the legend over the picture of the brain
        virtual bool BigBrainOpen() const { return false; }

        // The short text for the title of the window and the log ("Fly plays (Bio), brain speed 0.92x")
        virtual std::string Status() const { return {}; }
    };

    // What the player chose in the menu for a mode: the neurons, the backend of the brain, where to play. The WAD and the -warp of DOOM are the flow's own.
    struct DoomLaunchChoice
    {
        std::string controlSet = "doomfly";     // "doomfly" | "bio"
        std::string backend = "auto";           // "auto" | "gpu" | "cpu" (the brain of the fly)
        int episode = 1;
        int map = 1;
        int skill = 3;
        int flies = 16;                         // the swarm: 8..32
        std::uint64_t seed = 1;
        // The services of the window for a controller that draws a 3D brain (V1): set by DoomGameFlow, null in the tests
        myengine::render::IRenderAdapter* render = nullptr;
        myengine::editor::ThumbnailService* thumbnails = nullptr;
    };

    // One entry of the menu in front of the game: "Human plays", "Fly plays", "Human vs Fly", ... The first mode of the list is the human (no controller)
    struct DoomMode
    {
        std::string id;                         // "human", "fly", "human-vs-fly", "fly-vs-fly", "swarm"
        std::string title;
        std::string description;
        bool available = true;
        std::string unavailableReason;          // "soon", "no connectome: tools\\flybrain\\fetch_malecns.ps1"
        std::vector<std::string> doomArgs;      // added to the command line of DOOM for this mode ("-deathmatch" ...)
        // Makes the controller of the mode; empty for the human. nullptr and `error` when it cannot start.
        std::function<std::unique_ptr<IDoomController>(const DoomLaunchChoice& choice, std::string& error)> make;
    };
}
