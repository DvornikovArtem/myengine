// RlTraining.h
// The training of the Rocket League game (G4g): free play and the packs of exercises, on top of RlSim (and RlMatch for the kick-offs against a
// bot). No window, no renderer: the game steps it with the input of the player each 120 Hz tick and draws World() as it draws a match.
//
// Free play: one car and the ball; the boost can be endless, the ball goes back to the car or into the air in front of it (for aerials), and the
// speed and the height of the hits are counted. A pack (assets/data/training/*.json) is a list of exercises: where the car and the ball start,
// the time, the number of attempts and what counts as a success (a goal, a save, a touch above N metres, a won kick-off against the bot of level 2).
// The runner shows the banner SUCCESS / MISS, goes on by itself (a success -> the next exercise, a miss -> the next attempt) and the keys of the
// HUD (retry, next, previous) do the same by hand. The results of the exercises (successes, attempts, the best time and hit speed) are kept in a
// json file whose path the game gives (core::apppaths::UserDataDirectory() / "training.json", see myengine/game/RlTrainingStore.h).
// Everything is plain data and deterministic: the same inputs give the same run. docs/game/rl-training.md.

#pragma once

#include <rl/RlSim.h>
#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rl
{
    // What makes an exercise a success
    enum class RlTrainingCondition : int
    {
        Goal = 0,        // the ball is in the goal of the opponent (+Z)
        Saved,           // the ball was going into the own goal (-Z), the car touched it and it did not go in
        TouchAbove,      // the car touches the ball while the centre of the ball is at least `height` metres high
        KickoffWin,      // kick-off against a bot: the car touches the ball first and the ball is `distance` metres into the half of the opponent
    };

    struct RlTrainingExercise
    {
        std::string id;                 // the key in the results file; unique inside a pack
        std::string name;               // "Rolling ball"
        std::string hint;               // one line for the HUD: what to do

        // where the car starts (the car stands on the floor looking `carYaw` radians from +Z towards +X, at rest)
        float carX = 0.0f;
        float carZ = -20.0f;
        float carYaw = 0.0f;
        float carBoost = 33.3f;         // 0..100

        // where the ball starts
        RlVec3 ballPosition{0.0f, 0.9125f, 0.0f};
        RlVec3 ballVelocity{};
        RlVec3 ballSpin{};

        float timeLimit = 10.0f;        // seconds of play (a kick-off: after the countdown)
        int attempts = 3;               // the misses allowed before the exercise is left
        RlTrainingCondition condition = RlTrainingCondition::Goal;
        float height = 4.0f;            // TouchAbove: metres (the centre of the ball)
        float distance = 10.0f;         // KickoffWin: metres past the centre line

        // a kick-off against a bot (RlMatch, mode 1): the spot 0..4 of RlKickoffSpot is the same for the car and the bot, `botLevel` 0..2
        bool kickoff = false;
        int kickoffSpot = 0;
        int botLevel = 2;
    };

    struct RlTrainingPack
    {
        std::string id;                 // "striker"
        std::string name;               // "Striker"
        std::string description;
        int order = 0;                  // the place in the list of the Training screen
        std::vector<RlTrainingExercise> exercises;
    };

    // Reads a pack; false and a message in `error` for a file that does not parse or has no exercises.
    bool RlLoadTrainingPackFromText(const std::string& text, RlTrainingPack& out, std::string* error = nullptr);
    bool RlLoadTrainingPackFromFile(const std::string& path, RlTrainingPack& out, std::string* error = nullptr);
    // Every *.json of a folder (assets/data/training), the packs in the order of their "order" field then of the name; the files that do not
    // read are listed in `errors` and skipped.
    std::vector<RlTrainingPack> RlLoadTrainingPacks(const std::string& directory, std::vector<std::string>* errors = nullptr);

    // ---------------------------------------------------------------------------------------------------------------------------------

    struct RlTrainingExerciseResult
    {
        int successes = 0;
        int attempts = 0;               // every finished attempt, a success or a miss
        float bestTime = 0.0f;          // seconds of the fastest success, 0 = none yet
        float bestHitSpeed = 0.0f;      // m/s: the fastest hit of the ball (right after the touch) in a success, 0 = none yet
    };

    // The results of the player by pack and exercise, in a json file. An unknown or broken file is an empty set of results.
    class RlTrainingResults
    {
    public:
        bool LoadFromText(const std::string& text, std::string* error = nullptr);
        bool LoadFromFile(const std::string& path, std::string* error = nullptr);  // a missing file is not an error: no results yet
        std::string ToText() const;
        bool SaveToFile(const std::string& path, std::string* error = nullptr) const;  // makes the folder

        RlTrainingExerciseResult Get(const std::string& packId, const std::string& exerciseId) const;
        // One finished attempt (`time`, `hitSpeed` count only for a success)
        void Record(const std::string& packId, const std::string& exerciseId, bool success, float time, float hitSpeed);
        int PackSuccesses(const RlTrainingPack& pack) const;  // exercises of the pack that were done at least once
        void Clear() { entries_.clear(); }

    private:
        std::map<std::pair<std::string, std::string>, RlTrainingExerciseResult> entries_;  // by pack and exercise
    };

    // ---------------------------------------------------------------------------------------------------------------------------------

    enum class RlTrainingMode : int
    {
        FreePlay = 0,
        Pack = 1,
    };

    enum class RlTrainingPhase : int
    {
        Ready = 0,      // the exercise is set up and waits (the world is frozen for a moment; at a kick-off the countdown runs)
        Running = 1,
        Success = 2,    // the banner; the world keeps living
        Miss = 3,
        PackDone = 4,   // the last exercise is done: the summary
    };

    enum class RlTrainingMiss : int
    {
        None = 0,
        TimeUp,
        OwnGoal,        // the ball went into the own goal
        LostKickoff,    // the bot touched the ball first or sent it back
    };

    // The data of the HUD, filled by RlTraining::Info()
    struct RlTrainingInfo
    {
        RlTrainingMode mode = RlTrainingMode::FreePlay;
        RlTrainingPhase phase = RlTrainingPhase::Running;
        RlTrainingMiss missReason = RlTrainingMiss::None;
        std::string packId;
        std::string packName;
        std::string exerciseName;
        std::string hint;
        int exerciseIndex = 0;          // 0-based
        int exerciseCount = 0;
        int attempt = 1;                // the attempt now, 1-based
        int attempts = 0;               // the attempts of the exercise
        float elapsed = 0.0f;           // seconds of play of the attempt (frozen when it ends); in free play the time of the session
        float timeLeft = 0.0f;          // seconds of play left (a pack); 0 in free play
        float timeLimit = 0.0f;
        bool outOfAttempts = false;     // Miss: this was the last attempt, the exercise is left
        bool newBest = false;           // Success: a record of the time or of the hit speed
        float bannerTime = 0.0f;        // seconds since the banner appeared (Success / Miss), the HUD slides it in with this
        bool infiniteBoost = false;
        // free play and the exercise: the hits of the car on the ball since the last reset of the counters / since the exercise began
        int hits = 0;
        float lastHitSpeed = 0.0f;      // m/s, right after the touch
        float bestHitSpeed = 0.0f;
        float maxTouchHeight = 0.0f;    // m, the centre of the ball at the touch
        RlTrainingExerciseResult best;  // the results of this exercise (Pack)
        int packDoneSuccesses = 0;      // PackDone: exercises done / count
    };

    class RlTraining
    {
    public:
        explicit RlTraining(const RlTuning& tuning);
        ~RlTraining();
        RlTraining(const RlTraining&) = delete;
        RlTraining& operator=(const RlTraining&) = delete;

        // ---- the sessions
        void StartFreePlay();
        // The pack is copied. `exercise` is where to begin. false: no exercises.
        bool StartPack(const RlTrainingPack& pack, int exercise = 0);
        // The file of the results: read now, written after every finished attempt. "" = the results stay in memory.
        void SetResultsFile(const std::string& path);
        RlTrainingResults& Results();
        const RlTrainingResults& Results() const;

        // ---- every tick (120 Hz)
        void Step(const RlCarInput& input);

        // ---- the keys of the HUD (a pack)
        void Retry();      // the same exercise from the start, no attempt is spent
        void Next();       // the next exercise; after the last one the summary
        void Previous();
        void RestartPack();

        // ---- free play
        void SetInfiniteBoost(bool on);
        bool InfiniteBoost() const;
        // The ball on the floor `distance` metres in front of the car, at rest
        void ResetBallToCar(float distance = 6.0f);
        // The ball in the air in front of the car (for an aerial): `distance` metres ahead and `height` metres up, standing still for a moment.
        void BallInAir(float distance = 14.0f, float height = 7.0f);
        void ResetCar();           // the car on the spot of a kick-off with the starting boost
        void ResetCounters();

        // ---- what to draw
        const RlWorldState& World() const;
        const RlWorldState& PreviousWorld() const;
        const RlEvents& LastEvents() const;
        const RlTuning& Tuning() const;
        RlTrainingInfo Info() const;
        RlTrainingMode Mode() const;
        RlTrainingPhase Phase() const;
        const RlTrainingPack& Pack() const;
        const RlTrainingExercise* CurrentExercise() const;   // nullptr in free play and in the summary
        const RlSim& Sim() const;
        RlSim& MutableSim();       // the tests set up odd situations through it

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
