// DoomMenu.h
//
// The menu of FlyDoom, "the laboratory" (docs/game/doom.md, design: .team/design/fd-menu-spec.md): the main screen, the choice of who plays (mode, game data, level, skill, neurons,
// where the brain runs), the settings and the credits, drawn with its own look (DoomMenuWidgets: Big Shoulders Display and JetBrains Mono, a red DOOM and a cyan fly brain, thin lines)
// over the brain that turns behind it. DoomGameFlow owns one and asks it each frame while the game has not started; the menu changes the settings it is given and says when the
// player wants to start or to leave.

#pragma once

#include "DoomMenuWidgets.h"

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
    class DoomMenuSounds;

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
        // The rows of the setup of the fly modes that the application plays (N4): a row is drawn when its flag is set
        struct LearnOptions
        {
            bool speed = false;             // "Speed": x1 x2 x4 x8 (Fly learns)
            bool startFrom = false;         // "Start from": SCRATCH | TRAINED (Fly learns)
            bool flyBrain = false;          // "Fly brain": TRAINED | FROM SCRATCH (Fly plays, Human vs Fly, Fly vs Fly)
        };
        std::vector<Mode> modes;            // the six of FlyDoom in this order: human, fly, fly-learn, human-vs-fly, fly-vs-fly, swarm
        std::vector<DoomWadEntry> wads;     // empty: none found
        std::string wadProblem;             // the message when there is no WAD
        std::string brainStatus;            // "GPU": the chip at the top
        std::string brainProblem;           // the connectome is not there ...
        bool flyAvailable = false;          // the connectome file is there
        LearnOptions learnOptions;
        // the numbers of the brain behind the menu for the top line (0: not known yet)
        std::uint32_t neuronCount = 0;
        float spikesPerSecond = 0.0f;       // the mean over the brain
        std::vector<DoomBrainAnchor> anchors;   // where the optic lobes, the mushroom bodies and the descending neurons are in the window now
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
        DoomMenu();
        ~DoomMenu();

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
        // A screen under the main one (Play, Settings, About): the brain behind it steps back
        bool Nested() const;
        // How bright the brain behind the screen should be (the main screen 1, Play 0.32, Settings and About about a quarter): the target, the flow smooths it
        float BackdropBrightness() const;
        // The sounds of the menu (the sounds of the game data); null: silence
        void SetSounds(DoomMenuSounds* sounds) { sounds_ = sounds; }

        // The pause over the game (Esc): what the player chose this frame. `canGoBack` false: no "to the laboratory" (the application cannot start itself again);
        // `allowBack` false: Esc does not resume yet (it is still down from the press that opened the pause).
        enum class PauseResult : std::uint8_t
        {
            None,
            Resume,
            DoomMenu,      // the menu of DOOM itself (save, load, options)
            Laboratory,
            Quit,
        };
        struct PauseInfo
        {
            std::string line;       // "FLY PLAYS · E1M2 · HURT ME PLENTY": the left of the top line; empty: none
            std::string backend;    // "GPU" / "CPU" in the modes of the fly; empty: none
            std::string speed;      // "1.7x real time": the speed of the brain; empty: none
            bool reduceMotion = false;
            float rightEdge = 1824.0f;   // where the right of the top line ends (a panel of the game stands at the right: the line stops before it)
            float frameRight = 0.0f;     // > 0: the right marks of the corners stand at this x of the window (design px), not at its edge
        };
        PauseResult DrawPause(rlui::Frame& frame, bool canGoBack, bool allowBack, const PauseInfo& info = {});

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

        void DrawMain(rlui::Frame& f, DoomSettings& settings, const DoomMenuModel& model, Result& result);
        void DrawPlay(rlui::Frame& f, DoomSettings& settings, const DoomMenuModel& model, Result& result, DoomStartRequest& request);
        void DrawSettings(rlui::Frame& f, DoomSettings& settings, bool& changed);
        void DrawAbout(rlui::Frame& f);
        void DrawCallouts(rlui::Frame& f, const DoomMenuModel& model, float alpha);
        void Go(rlui::Frame& f, Screen next);
        void Sound(fd::Sfx sfx);

        Screen screen_ = Screen::Main;
        bool quitAsked_ = false;
        float quitAppear_ = 0.0f;
        int modeIndex_ = 0;       // the mode of the dossier (index in DoomMenuModel::modes)
        int wadIndex_ = 0;
        int levelIndex_ = 0;
        bool initialised_ = false;
        DoomMenuSounds* sounds_ = nullptr;
        fd::Fx fx_;
        double time_ = 0.0;            // the time of the flow
        double enterTime_ = 0.0;       // when the screen came
        double dossierTime_ = -10.0;   // when the dossier of Play changed its mode
        rlui::Id lastFocus_ = rlui::kNoId;
        rlui::Id mainFocus_ = rlui::kNoId;   // the item of the main screen that was left (the focus comes back to it)
        // the entry of the first show (§5.2)
        float introTime_ = 0.0f;
        bool introDone_ = false;
        // the numbers of the top line
        float shownNeurons_ = 0.0f;
        // settings: the flash of the check mark, the test dial
        double savedTime_ = -10.0;
        float dialAngle_ = 0.0f;
        float dialSweep_ = 0.0f;
        double dialRest_ = 0.0;
        rlui::Vec2 lastMouse_{};
        // about
        std::vector<float> sectionTops_;
        // the pause
        double pauseDrawn_ = -10.0;    // the time of the last frame of the pause
        double pauseOpened_ = 0.0;
        bool pauseEscDown_ = false;
    };
}
