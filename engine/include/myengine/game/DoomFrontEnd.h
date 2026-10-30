// DoomFrontEnd.h
//
// What the menu of FlyDoom needs besides the game itself (docs/game/doom.md, "The laboratory"): the settings of the player (kept in %LOCALAPPDATA%\\FlyDoom\\settings.json),
// and the picture of the brain that turns behind the menu and sits in the panel of the game. The brain view is a library of its own (game/brainview) that the engine does
// not link, so the engine knows it only as this interface and the application hands in a factory.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

struct ImDrawList;

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
    // The picture of a brain in the window. Draw is called every frame while it is wanted (the menu, the panel, the big brain); it moves the clock of the picture and
    // puts it (a texture of the live-view service, a frame behind) into the rect.
    class IDoomBrainView
    {
    public:
        virtual ~IDoomBrainView() = default;
        // `alpha` 0..1: how strongly the picture is drawn (the menu fades it in and out); `quiet`: the brain is at rest, not running a game (the menu: it breathes)
        virtual void Draw(ImDrawList* list, float x, float y, float width, float height, double deltaSeconds, float alpha) = 0;
        // 0: the main screen (the brain is the hero, right of the buttons), 1: a screen under it (the brain is dimmer and a little farther right, behind the content)
        virtual void SetNested(float mix) { (void)mix; }
        // Where the brain runs and how it does ("GPU (NVIDIA ...)", "CPU, 0.8x real time"); empty while it loads
        virtual std::string Status() const { return {}; }
        // The connectome is not here, the cloud could not be made ...: a line for the menu; empty when all is well
        virtual std::string Problem() const { return {}; }
    };

    struct DoomBrainViewContext
    {
        render::IRenderAdapter* render = nullptr;
        editor::ThumbnailService* thumbnails = nullptr;
        std::function<void(const std::string&)> log;
    };
    using DoomBrainViewFactory = std::function<std::unique_ptr<IDoomBrainView>(const DoomBrainViewContext&)>;

    // The settings of FlyDoom. The menu changes them, the game applies them, the file keeps them between runs.
    struct DoomSettings
    {
        // sound
        float sfxVolume = 1.0f;          // 0 .. 1
        float musicVolume = 1.0f;
        bool musicOn = true;
        // controls
        float mouseSensitivity = 1.0f;   // 0.25 .. 3, a factor on the turn of the mouse
        // window
        int windowMode = 0;              // 0 windowed, 1 borderless, 2 fullscreen
        int uiScalePercent = 100;        // 75 .. 125
        bool brainBackdrop = true;       // the brain behind the menu
        // the last choices of the menu
        std::string lastMode = "human";  // the id of the mode
        std::string controlSet = "doomfly";
        std::string backend = "auto";
        std::string iwad;                // the path of the WAD that was chosen, empty: the best found
        int episode = 0;                 // 0: the title screen of DOOM
        int map = 1;
        int skill = 3;

        void Clamp();
        // false: no file or not readable (the defaults stay); Save false: not written
        bool Load(const std::filesystem::path& file);
        bool Save(const std::filesystem::path& file) const;
    };
}
