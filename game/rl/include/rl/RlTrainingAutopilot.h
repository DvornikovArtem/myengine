// RlTrainingAutopilot.h
// A player for the training that is a bot of the game (RlBots v2): it drives the blue car of RlTraining through an exercise the way the bots of
// a match drive theirs. Two uses: the tests prove that the exercises of assets/data/training can be done (success rates per exercise), and the
// game mode shows what an exercise looks like (the debug command "autopilot" of RlGameSystem). It reads the world only, no state of the
// simulation is touched. Deterministic: the same world sequence and seed give the same inputs.

#pragma once

#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <cstdint>
#include <memory>

namespace rl
{
    struct RlTrainingExercise;

    // What the autopilot plays: the bot of a match (strikers, kick-offs), the same bot kept in defence (saves), or a controller of aerials (the
    // bots of the match fly only to balls of 3 to 7.5 m): it drives under the ball, jumps and boosts with the nose along the acceleration that
    // brings the car to the ball, and wants to touch it at least `minHeight` metres up
    enum class RlAutopilotStyle : int
    {
        Bot = 0,
        Goalie,
        Aerial,
    };

    class RlTrainingAutopilot
    {
    public:
        // `level` 0..2 (the bot levels of the match), `seed` for the random inexactness of the lower levels
        explicit RlTrainingAutopilot(const RlTuning& tuning, int level = 2, uint64_t seed = 1);
        ~RlTrainingAutopilot();
        RlTrainingAutopilot(const RlTrainingAutopilot&) = delete;
        RlTrainingAutopilot& operator=(const RlTrainingAutopilot&) = delete;

        void SetStyle(RlAutopilotStyle style, float minHeight = 4.0f);
        // The style an exercise needs: a save is played by the goalie, a touch above N metres by the aerial controller, the rest by the bot
        void SetStyleFor(const RlTrainingExercise& exercise);

        // The input of the car in slot `slot` (blue, team 0) for this tick. Call once per tick with the world that was just simulated.
        RlCarInput Think(const RlWorldState& world, int slot = 0);
        // A new attempt: the memory of the bot is cleared
        void Reset();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
