// DoomGameFlow.h
//
// FlyDoom in the engine (docs/game/doom.md): the classic DOOM of game/doom drawn as the whole window. Every frame it reads the keyboard and the
// mouse, runs the game at its own 35 tics a second, puts the 320x200 frame into a texture of the render adapter and draws it, centred, with
// an integer scale and black margins; the sound and the music of DOOM go to the AudioSystem.
//
//     game::DoomGameServices services;  ... fill ...
//     game::DoomGameFlow flow(services);
//     flow.Install();                      // after UiManager::Initialize and before RegisterWindow

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace myengine::ui
{
    class UiManager;
}

namespace myengine::render
{
    class IRenderAdapter;
}

namespace myengine::audio
{
    class AudioSystem;
}

namespace myengine::game
{
    struct DoomGameServices
    {
        ui::UiManager* ui = nullptr;
        render::IRenderAdapter* render = nullptr;
        audio::AudioSystem* audio = nullptr;                                                     // nullptr: no sound
        std::function<void()> quit;                                                              // the player left the game
        std::function<std::pair<int, int>()> takeMouseDelta;                                     // the raw movement of the mouse since the last call
        std::function<void(bool)> captureCursor;                                                 // true: hide the cursor and keep it in the window (the mouse turns the player)
        std::function<void(const std::string&)> setTitle;                                        // the title of the window ("Freedoom: Phase 1")
        std::function<void(const std::string&)> log;
        std::filesystem::path iwad;                                                              // --iwad; empty: look for it
        std::vector<std::string> doomArgs;                                                       // the command line of DOOM itself ("-warp", "1", "1" ...)
        std::filesystem::path exeDirectory;                                                      // where assets/doom is looked for
        std::vector<std::filesystem::path> wadDirectories;                                       // the folders of the WAD files of the user (%LOCALAPPDATA%\FlyDoom\wads)
        std::filesystem::path dataDirectory;                                                     // the saves and the config of DOOM
        bool squarePixels = false;                                                               // false: 4:3 as on the monitors of 1993 (320x200 stretched to 320x240)
        bool music = true;
    };

    class DoomGameFlow
    {
    public:
        explicit DoomGameFlow(const DoomGameServices& services);
        ~DoomGameFlow();
        DoomGameFlow(const DoomGameFlow&) = delete;
        DoomGameFlow& operator=(const DoomGameFlow&) = delete;

        // Hooks the game into the UiManager (it takes the place of the editor)
        void Install();

        // For the logs and the tests: "start" (nothing yet), "game", "error", "finished"
        const char* StateName() const;
        std::uint64_t FrameCount() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
