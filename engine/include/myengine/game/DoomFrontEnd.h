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
#include <vector>

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
    // Human mouse look baseline; the saved Turn speed slider remains a relative multiplier.
    inline constexpr float kDoomMouseBaselineGain = 1.5f;

    // The numbers of the brain that runs behind the menu (0: not known yet)
    struct DoomBrainStats
    {
        std::uint32_t neurons = 0;
        float spikesPerSecond = 0.0f;    // all the neurons together
    };

    // A part of the brain where it is in the window (pixels of the window): the menu puts a label on it
    struct DoomBrainAnchor
    {
        std::string id;                  // "optic" | "mushroom" | "descending"
        float x = 0.0f;
        float y = 0.0f;
    };

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
        // How bright the picture is: 1 on the main screen, less behind the other screens
        virtual void SetBrightness(float level) { (void)level; }
        virtual DoomBrainStats Stats() const { return {}; }
        // Where the optic lobes, the mushroom bodies and the descending neurons are in the window now (as of the last Draw); empty: not known
        virtual std::vector<DoomBrainAnchor> Anchors() const { return {}; }
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
        float mouseSensitivity = 1.0f;   // 0.25 .. 4, a factor on the turn of the mouse (the menu shows it on a log scale, 1.00x in the middle)
        // window
        int windowMode = 0;              // 0 windowed, 1 borderless, 2 fullscreen
        int uiScalePercent = 100;        // 75 .. 125
        bool brainBackdrop = true;       // the brain behind the menu
        int menuMotion = 0;              // 0 full (the menu comes in, the focus slides), 1 reduced (everything only fades in)
        // the last choices of the menu
        std::string lastMode = "human";  // the id of the mode
        std::string controlSet = "doomfly";
        std::string backend = "auto";
        std::string iwad;                // the path of the WAD that was chosen, in UTF-8; empty: the best found
        int episode = 0;                 // 0: the title screen of DOOM
        int map = 1;
        int skill = 3;
        // the choices of the fly modes that the application plays (N4: the fly that learns); the menu shows them when the model says so (DoomMenuModel::learnOptions)
        int flies = 16;                          // the swarm: 8 .. 32 flies
        int learnSpeed = 1;                      // 1: real time, 0: MAX (as fast as the brain goes) when the fly learns
        std::string learnStart = "scratch";      // "scratch" | "trained": the fly that learns starts untrained or from the brain it has learned
        std::string flyBrain = "trained";        // "trained" | "scratch": the brain of the fly in the modes where it only plays

        void Clamp();
        // false: no file or not readable (the defaults stay); Save false: not written
        bool Load(const std::filesystem::path& file);
        bool Save(const std::filesystem::path& file) const;
    };
}
