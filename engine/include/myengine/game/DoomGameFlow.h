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

#include <myengine/game/DoomController.h>
#include <myengine/ui/rl/RlUiInput.h>
#include <myengine/game/DoomFrontEnd.h>

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

namespace myengine::editor
{
    class ThumbnailService;
}

namespace myengine::game
{
    struct RlVideoSettings;

    struct DoomGameServices
    {
        ui::UiManager* ui = nullptr;
        render::IRenderAdapter* render = nullptr;
        audio::AudioSystem* audio = nullptr;                                                     // nullptr: no sound
        std::function<void()> quit;                                                              // the player left the game
        // "To the laboratory" of the pause menu: DOOM can live once in a process, so the application starts itself again (the menu on the PLAY screen) and quits. Empty: no such button.
        std::function<void()> backToMenu;
        std::function<std::pair<int, int>()> takeMouseDelta;                                     // the raw movement of the mouse since the last call
        std::function<void(bool)> captureCursor;                                                 // true: hide the cursor and keep it in the window (the mouse turns the player)
        std::function<void(const std::string&)> setTitle;                                        // the title of the window ("Freedoom: Phase 1")
        std::function<void(const std::string&)> log;
        std::filesystem::path iwad;                                                              // --iwad; empty: look for it
        std::vector<std::string> doomArgs;                                                       // the command line of DOOM itself ("-warp", "1", "1" ...)
        std::filesystem::path exeDirectory;                                                      // where assets/doom is looked for
        std::vector<std::filesystem::path> wadDirectories;                                       // the folders of the WAD files of the user (%LOCALAPPDATA%\FlyDoom\wads)
        std::filesystem::path dataDirectory;                                                     // the saves and the config of DOOM
        // Who plays (DoomController.h). The first mode is the human. More than one mode: a menu in front of the game asks, unless initialMode says.
        std::vector<DoomMode> modes;
        DoomLaunchChoice choice;                                                                 // what a mode is made with when there is no menu (--fly-bio: controlSet "bio")
        int initialMode = -1;                                                                    // the index of the mode to start with, -1: ask when there are several
        bool squarePixels = false;                                                               // false: 4:3 as on the monitors of 1993 (320x200 stretched to 320x240)
        bool music = true;
        // The laboratory (V1, docs/game/doom.md): the menu in front of the game. With showMenu false the game starts at once with what the command line says.
        bool showMenu = true;
        editor::ThumbnailService* thumbnails = nullptr;                                          // the live views: the brain behind the menu and in the panel of the game are textures
        DoomBrainViewFactory makeBrainView;                                                      // the brain behind the menu (the engine does not link game/brainview); empty: none
        std::function<void(const RlVideoSettings&)> applyVideo;                                  // the window mode of the settings
        std::function<std::pair<std::uint32_t, std::uint32_t>()> windowSize;
        std::filesystem::path settingsFile;                                                      // %LOCALAPPDATA%\FlyDoom\settings.json; empty: the settings are not kept
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

        // The scripted input of the tests: a hook on the devices that the menu sees (MYENGINE_UI_SCRIPT, docs/game/rl-ui.md)
        void SetRawInputHook(std::function<void(rlui::RawInput&)> hook);

        // For the logs and the tests: "start" (nothing yet), "game", "error", "finished"
        const char* StateName() const;
        // The status of the controller ("Fly plays, brain speed 0.92x"), empty for the human
        std::string ControllerStatus() const;
        std::uint64_t FrameCount() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
