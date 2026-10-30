// DoomController.h
//
// Who plays DOOM in FlyDoom (docs/game/doom.md, docs/game/flydoom-fly.md). The default player is the human at the keyboard; a controller replaces
// the keyboard and the mouse: the fly player (game/doomfly) is one. DoomGameFlow owns the window, the frame and the sound and asks the controller
// to run the game, so a controller can pace the game by what it needs (the fly brain makes DOOM wait for it).
//
// The interface names no engine type except ImDrawList, so a controller lives in a library that does not link the engine (game/doomfly).

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImDrawList;

namespace doom
{
    class DoomGame;
}

namespace myengine::game
{
    class IDoomController
    {
    public:
        virtual ~IDoomController() = default;

        // Called once per frame of the window instead of DoomGame::Advance. `deltaSeconds` is the real time of the frame; the controller advances the
        // game as it sees fit (a tick of DOOM is 1/35 s of game time) and returns true when DOOM drew a new frame.
        virtual bool Advance(doom::DoomGame& game, double deltaSeconds) = 0;

        // The panel of the controller (right of the picture): window pixels. A controller with nothing to show draws nothing.
        virtual void DrawPanel(ImDrawList* list, float x, float y, float width, float height) = 0;

        // The short text for the title of the window and the log ("Fly plays (Bio), brain speed 0.92x")
        virtual std::string Status() const { return {}; }
    };

    // One entry of the menu in front of the game: "Human plays", "Fly plays (Bio controls)". `make` is empty for the human
    struct DoomMode
    {
        std::string name;
        std::function<std::unique_ptr<IDoomController>(std::string& error)> make;
    };
}
