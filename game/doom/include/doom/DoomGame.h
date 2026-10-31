// DoomGame.h
//
// The classic DOOM (1993, the source of id Software through doomgeneric) as a library: the host feeds it time and input and takes a
// frame, sound effects and music out. No window, DX12 or engine type in here: the engine glue lives in engine/src/game/doom and the
// tests drive the same class headless (docs/game/doom.md).
//
// DOOM keeps all of its state in globals, so there is one DoomGame per process, and it can be started once: after Quit (the menu, or
// the end of the game) or an error the object only reports it, a new game needs a new process.
//
// One thread: everything is called from the thread that created it.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <doom/DoomAudio.h>
#include <doom/DoomKeys.h>

namespace doom
{
    // The frame of the game: DOOM's own 320x200, 32 bit, the colours as 0x00RRGGBB (the high byte is 0)
    constexpr int kScreenWidth = 320;
    constexpr int kScreenHeight = 200;

    struct DoomConfig
    {
        std::filesystem::path iwad;             // the game data (a WAD); see FindIwad()
        std::filesystem::path dataDirectory;    // saves and the config of DOOM; created when missing; empty: next to the process
        std::vector<std::string> extraArgs;     // the command line of DOOM as it is: "-warp", "1", "1", "-skill", "3", "-timedemo" ...
        IDoomAudioHost* audio = nullptr;        // nullptr: DOOM starts with no sound at all (the headless tests)
        bool music = true;                      // false: the sound effects only (no OPL rendering)
    };

    // The state of one player (DOOM's players[]): what a brain or a test reads
    struct PlayerState
    {
        bool inGame = false;
        int health = 0;
        int armor = 0;
        double x = 0.0;                         // map units
        double y = 0.0;
        double z = 0.0;
        double angleDegrees = 0.0;              // 0 = east, counter-clockwise
        int readyWeapon = 0;
        int kills = 0;
        int items = 0;
        int secrets = 0;
        int frags = 0;                          // deathmatch: kills of the others minus suicides
        int deaths = 0;                         // deathmatch: how many times the others killed this player
        bool hasAttacker = false;               // the monster (or player) that hurt this player last, while it exists (the turn away from it, N3b)
        double attackerX = 0.0;
        double attackerY = 0.0;
        // N5, measurement only: the attacks of this player since the game started (bullets and pellets, punches, saw tics, missiles), the ones that hurt
        // a living hostile (a monster, a Lost Soul, the other player of a deathmatch) and their damage
        int shots = 0;
        int hits = 0;
        int damageDealt = 0;
        int doorsOpened = 0;                    // N5, measurement only: the doors the player opened with "use"
        int sector = -1;                        // the sector the player is in
        bool inDoorSector = false;              // ... a door sector (a line of it is a manual door)
    };

    // A copy of the result of DOOM's native aim trace in a bounded +/-22.5 degree
    // combat-assist cone around the player's current view.
    // It never exposes an mobj pointer: the fly may use this only as a visible-target
    // signal for its explicitly authored combat-assist layer.
    struct VisibleTarget
    {
        bool found = false;
        double angleDegrees = 0.0;                 // signed ray offset from the view; positive is DOOM's left
    };

    enum class GameState : std::uint8_t
    {
        Level = 0,
        Intermission = 1,
        Finale = 2,
        DemoScreen = 3,
    };

    struct GameInfo
    {
        int gameTic = 0;                        // the tics DOOM has run (35 a second)
        GameState state = GameState::DemoScreen;
        int episode = 0;
        int map = 0;
        bool menuActive = false;
        bool paused = false;
        bool demoPlayback = false;
        int consolePlayer = 0;                  // the player the keyboard, the mouse and the screen belong to
        int skill = 0;
        int deathmatch = 0;
        bool netGame = false;
    };

    // The command of one player for one tic: what DOOM builds from the keys and the mouse, and what a brain or a bot gives instead.
    // forwardMove and sideMove are in the units of DOOM (walking: 25, running: 50), angleTurn is the change of the angle in the units of
    // 1/65536 of a circle (a full turn is 65536; the original keys turn by 640 a tic, 1280 when running).
    struct TicCmd
    {
        std::int8_t forwardMove = 0;
        std::int8_t sideMove = 0;
        std::int16_t angleTurn = 0;
        std::uint8_t buttons = 0;               // kButtonAttack, kButtonUse, ...
    };
    constexpr std::uint8_t kButtonAttack = 1;
    constexpr std::uint8_t kButtonUse = 2;

    // The extension point for a player that is not the keyboard (a bot, a brain, a second player; tasks N1, D1): before the tic runs, DOOM
    // asks the source for the command of a player and the source may replace it. Set it with DoomGame::SetTicCmdSource.
    class ITicCmdSource
    {
    public:
        virtual ~ITicCmdSource() = default;
        // `cmd` holds the command DOOM has built (for the console player: from the keys and the mouse). Change it to take over.
        virtual void BuildTicCmd(int player, TicCmd& cmd) = 0;
    };

    // One frame of one player's view, to draw into a buffer of the host (D1: every player of a game gets its own view)
    struct ViewBuffer
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint32_t> pixels;      // width * height, 0x00RRGGBB
    };

    class DoomGame
    {
    public:
        ~DoomGame();
        DoomGame(const DoomGame&) = delete;
        DoomGame& operator=(const DoomGame&) = delete;

        // Starts DOOM: reads the WAD and runs the start up of the original (the title screen or the level of extraArgs). nullptr and
        // `error` when the WAD is missing or bad, or when DOOM has been started in this process already.
        static std::unique_ptr<DoomGame> Create(const DoomConfig& config, std::string& error);

        // ---- time ----
        // Moves the clock of DOOM by `deltaSeconds` and runs a tic of the game when 1/35 s has passed (or more tics at once after a long
        // frame, as DOOM does). true: a new frame is ready. The clock is virtual, so a test that calls Advance(1.0 / 35) runs
        // exactly one tic each time.
        bool Advance(double deltaSeconds);
        // Runs exactly one tic of the game, whatever the clock says (a brain that plays DOOM steps the game tic by tic, when it has decided).
        // true: a new frame is ready.
        bool StepTic();

        // ---- the frame ----
        // 320x200 pixels, 0x00RRGGBB; valid until the next Advance. FrameCount() grows with every frame DOOM drew.
        const std::uint32_t* Pixels() const;
        std::uint64_t FrameCount() const { return frameCount_; }
        // A hash of the pixels of the last frame (the tests compare it)
        std::uint64_t FrameHash() const;
        // The title DOOM wants for the window ("DOOM Shareware", "Freedoom: Phase 1")
        const std::string& WindowTitle() const { return windowTitle_; }
        // The view of `player` in a buffer of the host: the console player is what DOOM drew (with the status bar), any other player of the game
        // ("-players N") is rendered through his eyes by an extra render pass (3D view only, the status bar is black). false: no such player or no level.
        // Call it on the game's thread between tics.
        bool RenderView(int player, ViewBuffer& out);
        // The 3D view of any player of the game (the console player too) drawn by the extra render pass: no status bar (those rows are black), the same
        // picture for each player, so that two brains see the same way. false: no such player or no level.
        bool RenderEyes(int player, ViewBuffer& out);
        // Loads the level again at the next tic: all the players are born again, the frags are zero (a deathmatch round is over)
        void RestartLevel();

        // ---- input ----
        // A key by the codes of DoomKeys.h (or the lower case ASCII of a letter, a digit, a symbol)
        void PostKey(int key, bool pressed);
        // The mouse: the buttons held (bit 0 left, 1 right, 2 middle) and the movement since the last call. The movement of all the calls between two tics adds up
        // and reaches DOOM as one event at the next tic (the host may call at the frame rate).
        void PostMouse(int buttons, int deltaX, int deltaY);
        // nullptr: DOOM builds the commands of all players itself
        void SetTicCmdSource(ITicCmdSource* source);
        // The command the player had in the last tic (after the source)
        bool GetTicCmd(int player, TicCmd& out) const;

        // ---- the state ----
        GameInfo Info() const;
        PlayerState Player(int player) const;
        // Probes only living hostile things that a native DOOM aim ray can hit in a
        // narrow cone around the player's current view. The game thread owns DOOM.
        // `angleOffsetDegrees` probes a hypothetical final yaw without
        // changing game state; the normal value is zero.
        VisibleTarget FindVisibleTarget(int player, double angleOffsetDegrees = 0.0) const;
        // N5: the whiskers, an authored local sensor outside the connectome (FlyPlayer, "WHISKERS"): for each angle from the view (degrees, positive is
        // DOOM's left) the distance to the first line the player cannot pass, up to `range` map units. Lines only, no things; the world is not changed
        std::vector<double> Whiskers(int player, const std::vector<double>& anglesDegrees, double range) const;
        // The game data it plays (DoomConfig::iwad); N4: the trained fly keeps a memory per WAD and map
        const std::filesystem::path& Iwad() const;

        // ---- the end ----
        // The player chose Quit in the menu (or a demo ended with -timedemo ...): the host closes the game
        bool IsFinished() const { return finished_; }
        // DOOM stopped with an error (I_Error): the text; empty when it did not
        const std::string& ErrorMessage() const { return error_; }

        // The clock, the queue of keys and the frame: the callbacks of doomgeneric reach them (DoomGame.cpp)
        struct Impl;
        void OnWindowTitle(const char* title) { windowTitle_ = title; }

    private:
        DoomGame();
        void Finish(const char* message, int code);

        std::unique_ptr<Impl> impl_;
        std::uint64_t frameCount_ = 0;
        std::string windowTitle_;
        std::string error_;
        bool finished_ = false;
    };

    // ---- the game data ----
    enum class IwadSource : std::uint8_t
    {
        None,
        Argument,       // --iwad
        NextToExe,      // assets/doom/, wads/ (the shareware WAD of the package) or doom/ next to the exe (Freedoom from tools/doom/fetch_freedoom.ps1, or a WAD the player put there)
        UserFolder,     // %LOCALAPPDATA%\FlyDoom\wads (the WAD files of the player, outside the repository and the package)
        Steam,          // the DOOM of the Steam library of the player
        Environment,    // DOOMWADDIR / DOOMWADPATH of DOOM
    };

    struct IwadSearchResult
    {
        std::filesystem::path path;
        IwadSource source = IwadSource::None;
        bool isFreedoom = false;
        std::vector<std::filesystem::path> alternatives;   // the other WAD files that were found (a menu can offer them)
        std::vector<std::string> tried;        // for the message "no WAD found": where it was looked for
    };

    // The places: the argument; assets/doom, wads or doom next to the exe; the folders of the user (userDirectories: %LOCALAPPDATA%\FlyDoom\wads ...); the DOOM
    // of the Steam library; DOOMWADDIR. Among the WAD files found the full game (DOOM.WAD, DOOM2.WAD, plutonia, tnt) comes before the shareware doom1.wad,
    // and that before Freedoom; with the same rank the order of the places says. What --iwad names is taken whatever it is. exeDirectory is where
    // FlyDoom.exe lies. An empty path in the result: nothing found.
    IwadSearchResult FindIwad(const std::filesystem::path& argument, const std::filesystem::path& exeDirectory,
                              const std::vector<std::filesystem::path>& userDirectories = {});
    // For the log: where the WAD came from
    const char* IwadSourceName(IwadSource source);

    // The text for the player when FindIwad found nothing
    std::string NoIwadMessage(const IwadSearchResult& result);
}
