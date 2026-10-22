// RlGameSystem.h
// The Rocket League game: in Play mode it starts an rl::RlMatch (physics, rules, bots) and runs it at 120 Hz next to the frame loop, moving the
// scene's entities (RlArena, RlBall, RlBlue0.., RlOrange0.., RlPad0.., RlCamera) to the interpolated state. Keyboard and XInput drive the first
// blue car.
//
// Two ways to use it. In the editor (Play) it shows the old text menu and overlay. In the game mode (--game, docs/game/rl-ui.md) there is no text:
// the screens of the game (game/RlGameFlow) give it commands (StartMatch, SetPaused, LeaveMatch ...) and read what it does (Status): the stage of the
// match, the HudState for the HUD, the events of the feed, the lobby of a private match. The system owns the match and the scene; the flow owns the
// screens and the settings.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <myengine/ecs/System.h>
#include <myengine/ui/rl/RlUiHud.h>

namespace myengine::input
{
    class InputManager;
}

namespace rl
{
    struct RlTuning;
    struct RlCameraTuning;
    class RlControls;
}

namespace myengine::ecs::systems
{
    // The online game (G5): --rl-host [port] and --rl-join ip[:port]. With a role the game skips the menu: the host waits for its guests and starts
    // the match, a client joins the host and plays its slot with prediction and rollback (game/rl RlNet).
    struct RlOnlineOptions
    {
        enum class Role
        {
            None,
            Host,
            Client,
        };

        Role role = Role::None;
        std::string address;       // client: "ip" or "ip:port" of the host
        std::uint16_t port = 47777;  // host: the port to listen on
        int expectedClients = 1;   // host: the match starts when this many guests have joined (Enter starts it earlier)
        int playersPerTeam = 2;    // host: 1..4
        std::string netSim;        // "latency=100,jitter=20,loss=5": delay and losses added to everything this window sends
    };

    // ---- the game mode: what the screens ask for and what they see ---------------------------------------------------------------------

    // What the match is doing right now (the system sets it; the flow chooses the screen from it)
    enum class RlGameStage
    {
        Menu,        // no match: the podium scene behind the menu
        Connecting,  // joining a host (Join by IP)
        Lobby,       // hosting and waiting for guests, or joined and waiting for the host
        Playing,     // a match is running (countdown, play, goal, the end of time): the HUD
        Paused,      // an offline match stands still
        Finished,    // the match ended: the results
        Failed,      // the network failed (error says why)
    };

    struct RlGameMatchSetup
    {
        enum class Kind
        {
            Offline,   // against bots (mode 0: the practice)
            Host,      // private match, this window is the host
            Join,      // private match, this window joins `address`
            Training,  // free play or a pack of exercises (rl::RlTraining, docs/game/rl-training.md): `trainingPack`
        };

        Kind kind = Kind::Offline;
        int mode = 2;                   // players per team 1..4, 0 practice
        int botLevel = 1;               // 0 Rookie, 1 Pro, 2 All-star
        bool fillWithBots = true;       // Host: empty slots play as bots (otherwise the host starts when the guests are in)
        std::string address;            // Join: "ip" or "ip:port"
        std::uint16_t port = 27015;     // Host: the UDP port
        std::string playerName;         // the name in the table
        std::string trainingPack;       // Training: the id of the pack ("striker"), empty = free play
        int trainingExercise = 0;       // Training: the exercise to begin with
    };

    struct RlLobbySlot
    {
        std::string name;
        int team = 0;
        bool bot = false;
        bool you = false;
        bool host = false;
        int pingMs = -1;
    };

    struct RlGameStatus
    {
        RlGameStage stage = RlGameStage::Menu;
        RlGameMatchSetup setup;          // what is being played
        rlui::HudState hud;              // the HUD of the match, filled every frame while a match exists
        int localSlot = 0;
        bool online = false;
        bool isHost = true;              // false: a client of someone else's match
        bool ballCam = true;
        bool scoreboardHeld = false;     // the button of the table (Tab / Back) is down
        std::vector<RlLobbySlot> lobby;  // Lobby: the slots of the two teams
        std::string statusText;          // "Connecting to 192.168.1.5:27015 ..."
        std::string error;               // Failed
        int winner = -1;                 // Finished: 0 blue, 1 orange
        int mvp = -1;                    // Finished: the slot of the MVP (the best player of the winning team), -1 none
        bool overtimePlayed = false;
        double matchSeconds = 0.0;       // the time of the match so far
        float loading = 1.0f;            // 0..1: the assets of the scene are in (the loading screen)
        bool padConnected = false;
        std::string ballCamKey = "Space";
        std::string pauseKey = "Esc";
    };

    class RlGameSystem final : public IUpdateSystem
    {
    public:
        explicit RlGameSystem(input::InputManager& input);
        ~RlGameSystem() override;

        // Before the first Play: the role of this window in an online match
        void SetOnlineOptions(const RlOnlineOptions& options);

        void Update(World& world, float deltaTime) override;

        // The lines of the debug overlay (empty when the scene is not an RL sandbox or the editor is not in Play)
        std::vector<std::string> HudLines() const;

        // The numbers of the running game for the RL Tuning window (null when no match runs). After changing them call TuningEdited();
        // the game reads assets/data/rl_tuning.json again whenever it changes on disk.
        rl::RlTuning* EditableTuning();
        void TuningEdited();
        std::string SaveTuning();    // writes the json; the result is a message for the window
        std::string ReloadTuning();  // reads it again

        // ---- the game mode (RlGameFlow) -----------------------------------------------------------------------------------------------
        // On: no text menu and overlay, the podium scene when there is no match, the cursor and the mouse buttons belong to the menus outside a match.
        void SetGameMode(bool enabled);
        bool GameMode() const;
        const RlGameStatus& Status() const;
        // Starts (or joins) a match on the next frame. The previous one, if any, ends. When it cannot start, Status().stage turns Failed with an error.
        void StartMatch(const RlGameMatchSetup& setup);
        // The host of a private match starts it (the lobby has the players it is going to have)
        void StartHostedMatch();
        // Back to the menu: the match, the lobby or the connection ends
        void LeaveMatch();
        // Offline: stands still / goes on. Online the match does not stop; the pause screen only covers it.
        void SetPaused(bool paused);
        void RestartMatch();  // offline: the same setup again
        // The events that came out of the ring of the match since the last call (for the feed of the HUD)
        std::vector<rlui::HudEvent> TakeHudEvents();
        // The pause button of the controls was pressed in a match (the flow turns it into the pause screen)
        bool TakePausePressed();
        // The controls of the player: the settings screen edits them and writes the file (SaveControls)
        rl::RlControls& Controls();
        std::string SaveControls();
        std::string ResetControls();   // the shipped bindings again (the user file is removed)
        // The camera as set in the menu (replaces the camera numbers of rl_tuning.json for what the player sees)
        void SetCameraSettings(const rl::RlCameraTuning& camera, bool ballCamHold);
        // The tuning of the numbers the camera has no menu row for, and the one of the preview
        const rl::RlCameraTuning& CameraSettings() const;
        void SetInvertPitch(bool invert);
        // The audio buses: 0..1 (the sounds of the match and the music of the menu)
        void SetVolumes(float master, float effects, float music);
        // The sound of the menu (a click, a confirmation, a refusal)
        void PlayUiSound(const char* eventName);
        // The name of the player in the table
        void SetPlayerName(const std::string& name);
        // Commands of the tests (UiScript: `game <command>`): "goal-blue" / "goal-orange" put the ball into that goal, "demolish" demolishes the car of the player,
        // "boost" fills its boost, "time <seconds>" sets the clock. They work in a running match only. In a training "autopilot" / "autopilot-off" hands
        // the car to the bot of the training (RlTrainingAutopilot), "next" / "previous" / "retry" press the keys of the exercise.
        void DebugCommand(const std::string& command);
        // The aspect ratio of the window (RL's field of view is the horizontal one)
        void SetViewAspect(float aspect);
        // The preview of the camera settings: a car and a ball on the field, the camera computed with the given numbers; for the Settings > Camera screen
        void SetCameraPreview(bool enabled);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
