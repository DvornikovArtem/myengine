// RlUiHud.h
//
// The HUD of a match (§3 of the spec): the scoreboard with the clock, the countdown 3-2-1-GO, the boost ring, the banners (GOAL!, OWN GOAL,
// OVERTIME, DEMOLITION), the feed of events, the table of points on Tab, the ball cam hint, the ping. It is drawn in Space::Screen on the
// real edges of the window (the scoreboard in the middle on top, the boost at the bottom right), so on 21:9 it sticks to the corners.
//
// The data comes in one plain structure (HudState) that the game fills every frame from its world state: the HUD finds the changes itself
// (the phase turns to GoalScored -> the banner; the overtime flag turns on -> OVERTIME!; the boost jumps up -> the flash of a pad). The
// events of the feed (goal, assist, save, shot, demolition) are pushed one by one (PushEvent), as they come out of the ring of events of the
// match. The enums mirror rl::RlMatchPhase and the match state (the game library is not included here).
//
//     rlui::Hud hud;                                  // one per match; hud.Reset() at a new match
//     rlui::HudState s; ... fill ...                  // every frame
//     hud.Update(dt, s);                              // the animations and the changes
//     hud.Draw(frame, scoreboardHeld);                // Tab / Back held -> the table
//
// The training (rl::RlTraining, docs/game/rl-training.md) uses the same class: fill HudState::training (active = true) and Draw shows the
// training pieces in place of the scoreboard.
//
// Draw() draws nothing it should not: in the pause only the scoreboard, in the results nothing (the game decides what to call).

#pragma once

#include <string>
#include <vector>

#include <myengine/ui/rl/RlUiFrame.h>

namespace myengine::rlui
{
    // = rl::RlMatchPhase (the same numbers)
    enum class HudPhase : int
    {
        Countdown = 0,
        Playing = 1,
        GoalScored = 2,
        Finished = 3,
    };

    struct HudPlayer
    {
        std::string name;
        int team = 0;               // 0 blue, 1 orange
        bool bot = false;
        int score = 0;              // the points (the big number of the table)
        int goals = 0;
        int assists = 0;
        int saves = 0;
        int shots = 0;
        int demos = 0;
        int pingMs = -1;            // online; -1 none
    };

    // The replay of a goal (RlReplay): while `active` the HUD shows the REPLAY label, the author and the speed of the goal, the letterbox bars, the
    // progress of the clip and the hint "Skip replay" instead of the usual pieces (the scoreboard stays)
    struct HudReplay
    {
        bool active = false;
        std::string author;          // the scorer, "" when nobody touched the ball
        int team = -1;               // the team the goal counts for (the colour of the label)
        float goalSpeedKmh = 0.0f;
        float progress = 0.0f;       // 0..1 of the clip
        float speed = 1.0f;          // the playback speed now (1, 0.5): a tag "0.5x" is shown in the slow motion
    };

    // = rl::RlTrainingPhase (the same numbers) plus the free play
    enum class HudTrainingPhase : int
    {
        Ready = 0,      // the exercise waits: GET READY
        Running = 1,
        Success = 2,    // the banner SUCCESS
        Miss = 3,       // the banner MISS
        PackDone = 4,   // the summary of the pack
        FreePlay = 5,   // no exercise: the counters of the hits and the keys of the free play
    };

    // One key of the row of hints of the training: the key of the keyboard, the button of the pad ("" = none) and what it does
    struct HudKeyHint
    {
        std::string key;
        std::string pad;
        std::string label;
    };

    // The training (RlTraining): the plate with the pack, the number of the exercise, its name, the hint and the attempts, the timer, the banner
    // SUCCESS / MISS, the counters of the hits, the row of keys. While `active` the HUD shows these instead of the scoreboard (there is no match).
    struct HudTraining
    {
        bool active = false;
        HudTrainingPhase phase = HudTrainingPhase::Running;
        std::string pack;                 // "Striker", "Free play"
        std::string exercise;             // "Rolling ball"
        std::string hint;                 // one line: what to do
        int index = 0;                    // 0-based; the HUD shows "3 / 8"
        int count = 0;
        int attempt = 1;                  // the attempt now, 1-based
        int attempts = 3;
        float timeLeft = 0.0f;            // seconds of play left; with timeLimit = 0 (free play) the session time `elapsed` is shown instead
        float timeLimit = 0.0f;
        float elapsed = 0.0f;
        float bannerTime = 0.0f;          // seconds since the banner appeared (Success / Miss / PackDone)
        float bannerDuration = 1.8f;
        std::string missReason;           // "TIME UP", "OWN GOAL", "LOST KICK-OFF"
        bool outOfAttempts = false;       // Miss: the exercise is left
        bool newBest = false;             // Success: a record
        int hits = 0;                     // free play and the exercise: the hits of the car on the ball
        float lastHitKmh = 0.0f;
        float bestHitKmh = 0.0f;
        float maxTouchHeight = 0.0f;      // metres
        bool infiniteBoost = false;       // free play: the endless boost is on
        float recordTime = 0.0f;          // the best time of the exercise, 0 = none yet
        float recordHitKmh = 0.0f;
        int done = 0;                     // PackDone: exercises with a success
        std::vector<HudKeyHint> hints;    // empty: the usual keys of the phase (R / N / P in a pack, R / T / B in free play)
    };

    struct HudState
    {
        HudPhase phase = HudPhase::Playing;
        float phaseTimer = 0.0f;    // seconds left of the countdown / of the celebration
        float clock = 300.0f;       // seconds left; in the overtime the seconds played in it
        bool overtime = false;
        int mode = 2;               // players per team; 0 = practice: no score, only the time of the session
        int score[2] = {0, 0};

        // the car of the player
        float boost = 33.0f;        // 0..100
        bool boosting = false;      // the boost is being burnt (the ring pulses)
        bool demolished = false;
        float respawnTimer = 0.0f;  // seconds until the demolished car is back

        // the last goal (for the GOAL! banner)
        int lastGoalTeam = -1;
        int lastGoalScorer = -1;    // index in `players`, -1 nobody
        bool lastGoalOwn = false;
        float lastGoalSpeedKmh = 0.0f;

        std::vector<HudPlayer> players;   // in the order of the slots
        int localPlayer = 0;              // the index of the player of this machine in `players`, -1 a spectator
        int pingMs = -1;                  // the ping of this machine (online); -1 hides it (offline)
        bool ballCam = true;              // the state of the ball cam (the hint "Ball cam ON / OFF")
        std::string ballCamKey = "Space"; // what is shown on the key cap of the hint (the binding of the player)
        bool showFps = false;
        float fps = 0.0f;
        HudReplay replay;
        HudTraining training;
    };

    enum class HudEventType : int
    {
        Goal = 0,
        OwnGoal,
        Assist,
        Save,
        Shot,
        Demo,
    };

    struct HudEvent
    {
        HudEventType type = HudEventType::Goal;
        int player = -1;       // the index in HudState::players: who scored / assisted / saved / shot / demolished
        int other = -1;        // the victim of a demolition
    };

    class Hud
    {
    public:
        void Reset();                                   // a new match: banners, feed and timers are cleared
        void Update(float deltaTime, const HudState& state);
        // An event of the feed (and, for a demolition, the banner of the player who is demolished or who demolished). Names are taken from the
        // state of the last Update.
        void PushEvent(const HudEvent& event);
        // The OVERTIME! banner is made by Update from the flag; this is for tests and the gallery
        void ShowOvertime();

        // Everything of a match in Space::Screen. `scoreboardHeld`: the Tab / Back of the player is down (the table, the dimmed match).
        // `paused`: only the scoreboard of the clock is drawn.
        void Draw(Frame& f, bool scoreboardHeld, bool paused = false);

        // ---- what is on the HUD right now (the tests and the logs read it; the drawing uses the same data)
        std::string BannerTitle() const { return banner_.title; }          // "GOAL!", "OWN GOAL", "OVERTIME!", "" when there is no banner
        std::string BannerSubtitle() const { return banner_.subtitle; }    // "ARTEM Â· 98 km/h"
        std::string DemolitionTitle() const { return demolition_.title; }
        int FeedSize() const { return static_cast<int>(feed_.size()); }
        std::string CountdownText() const;                                // "3", "2", "1", "GO!" or ""
        std::string ClockText() const;                                    // "4:32", "+0:42"

        // ---- the components (the gallery shows them one by one; the results screen reuses the table)
        void DrawScorebar(Frame& f) const;
        void DrawCountdown(Frame& f) const;
        void DrawBoostRing(Frame& f) const;
        void DrawBanners(Frame& f) const;
        void DrawFeed(Frame& f) const;
        void DrawBallCamHint(Frame& f) const;
        void DrawPing(Frame& f) const;
        // The REPLAY overlay (the letterbox bars, the label, the author, the progress, the hint): drawn by Draw while HudState::replay.active
        void DrawReplay(Frame& f) const;
        // The training: Draw calls it while HudState::training.active (the plate, the timer, the banner and the keys together)
        void DrawTraining(Frame& f) const;
        void DrawTrainingPlate(Frame& f) const;     // top left: pack, "3 / 8", the name, the hint, the attempts; in free play the counters
        void DrawTrainingTimer(Frame& f) const;     // top middle: the time left (a pack) or the session time (free play)
        void DrawTrainingBanner(Frame& f) const;    // SUCCESS / MISS / PACK COMPLETE and GET READY
        void DrawTrainingHints(Frame& f) const;     // the row of keys at the bottom left
        // The table of the players (the scoreboard, the results): the header of the columns, a bar per team, a row per player, the row of the
        // local player lighter. `rect` is in Space::Screen / Menu, the width 1200 of the spec at x = rect.x. Returns the bottom of the table.
        // `teamBars`: a bar per team with its score (the scoreboard); without them the rows follow each other as the players are listed (the results).
        // `footer`: the line with the pings and the hint "Hold Tab / Back".
        static float DrawScoreTable(Frame& f, const Rect& rect, const HudState& state, bool pings, bool teamBars = true, bool footer = true);

    private:
        struct FeedLine
        {
            HudEvent event;
            std::string text[4];   // name, verb, name, "": the pieces drawn in the colours of the teams
            int team[2] = {0, 0};
            float age = 0.0f;
        };
        struct Banner
        {
            enum class Kind { None, Goal, OwnGoal, Overtime, Demolition } kind = Kind::None;
            std::string title;
            std::string subtitle;
            int team = -1;
            float age = 0.0f;
            float duration = 0.0f;
        };

        HudState state_;
        HudState previous_;
        bool haveState_ = false;
        std::vector<FeedLine> feed_;
        Banner banner_;
        Banner demolition_;
        float ballCamHint_ = 0.0f;
        float firstMinutesLeft_ = 30.0f;   // the hint stays on for the first 30 s of the first match
        float boostFlash_ = 0.0f;
        float pulse_ = 0.0f;
        float goAge_ = 1e9f;               // seconds since the countdown ended (the GO!)
        float replayAge_ = 0.0f;           // seconds since the replay began (the bars slide in, the dot pulses)
    };
}
