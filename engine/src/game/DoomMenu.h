// DoomMenu.h
//
// The menu of FlyDoom, "the laboratory" (docs/game/doom.md): the main screen, the choice of how to play (mode, game data, level, skill, neurons, where the brain runs), the
// settings and the credits, drawn with the UI kit of the game (Exo 2, the bevelled plates) over the brain that turns behind it. DoomGameFlow owns one and asks it each frame
// while the game has not started; the menu changes the settings it is given and says when the player wants to start or to leave.

#pragma once

#include <myengine/game/DoomController.h>
#include <myengine/game/DoomFrontEnd.h>
#include <myengine/game/DoomLevels.h>
#include <myengine/ui/rl/RlUiFrame.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace myengine::game
{
    // One WAD the player can choose
    struct DoomWadEntry
    {
        std::filesystem::path path;
        std::string label;                    // "Freedoom: Phase 1", "DOOM (shareware)", "DOOM 2"
        std::vector<DoomLevelEntry> levels;   // the first is the title screen
    };

    // What the flow tells the menu
    struct DoomMenuModel
    {
        struct Mode
        {
            std::string id;                 // "human" ...
            std::string title;              // "HUMAN"
            std::string caption;            // "Classic DOOM"
            std::string description;        // a line for the selected one
            bool available = false;
            std::string reason;             // why not: "soon"
            int modeIndex = -1;             // in DoomGameServices::modes (for "fly": the one of the chosen neurons)
        };
        std::vector<Mode> modes;            // always the five of FlyDoom in this order: human, fly, human-vs-fly, fly-vs-fly, swarm
        std::vector<DoomWadEntry> wads;     // empty: none found
        std::string wadProblem;             // the message when there is no WAD
        std::string brainStatus;            // "GPU (NVIDIA ...)": the chip at the top
        std::string brainProblem;           // the connectome is not there ...
        bool flyAvailable = false;          // the connectome file is there
    };

    // What the player asked for when the menu says Start
    struct DoomStartRequest
    {
        std::string modeId;
        DoomLaunchChoice choice;
        std::filesystem::path iwad;
        std::vector<std::string> doomArgs;  // the warp and the skill
    };

    class DoomMenu
    {
    public:
        enum class Result : std::uint8_t
        {
            None,
            Start,
            Quit,
        };

        // Draws the screen of this frame. Result::Start: `request` is filled and the settings have the choices. `settingsChanged` is set when a value was changed that the
        // game should apply (the volume, the window mode, the scale of the UI): the flow applies and saves.
        Result Draw(rlui::Frame& frame, DoomSettings& settings, const DoomMenuModel& model, double timeSeconds, DoomStartRequest& request, bool& settingsChanged);

        // The screen that is shown (the flow resets it when the game is left to the menu)
        void Reset();

        // The captures of the menu itself (MYENGINE_DOOM_MENU_SCREEN): opens "play", "settings" or "about" at once; anything else leaves the main screen
        void ShowScreen(std::string_view name);

    private:
        enum class Screen : std::uint8_t
        {
            Main,
            Play,
            Settings,
            About,
        };

        void DrawMain(rlui::Frame& f, const DoomSettings& settings, const DoomMenuModel& model, Result& result);
        void DrawPlay(rlui::Frame& f, DoomSettings& settings, const DoomMenuModel& model, Result& result, DoomStartRequest& request);
        void DrawSettings(rlui::Frame& f, DoomSettings& settings, bool& changed);
        void DrawAbout(rlui::Frame& f);
        void Go(rlui::Frame& f, Screen next);
        void SideShade(rlui::Frame& f, float width);

        Screen screen_ = Screen::Main;
        bool quitAsked_ = false;
        int modeIndex_ = 0;       // the card that is chosen in the Play screen (index in DoomMenuModel::modes)
        int wadIndex_ = 0;
        int levelIndex_ = 0;
        bool initialised_ = false;
        std::string status_;      // a line under the title ("the game data is not found ...")
    };
}
