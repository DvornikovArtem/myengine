// FlySwarm.h
//
// The swarm of enemy flies in DOOM (docs/game/flydoom-fly.md, "Swarm"): the human plays at the keyboard, and 8..32 flies attack in waves. Every fly has its own
// small brain (flybrain::FlySwarmBrain, the circuit cut out of the connectome for the path "looming -> turn, walk, escape jump"; all flies are one batch). The
// game gives each fly what it sees as the input of the brain and drives a Lost Soul of DOOM (game/doom DoomSwarm.h) with what the brain says.
//
// What is the brain and what is not (the panel and the doc say the same): the brain turns the fly away from looming (DNa02, DNa01), makes it walk faster or
// slower (DNp09, MDN) and makes it jump away (the giant fibre DNp01). The hunt itself, the flight to the player round the walls, is a reflex of the game, as is
// the bite on contact and the height of the flight.
//
//   what a fly sees: the player rushing at it (the speed of the player towards the fly, the nearer the stronger) and the muzzle of the weapon pointing at it
//   (a shot): that is the looming on the left or the right of the fly (LPLC2, LC4), by the side the player is on. A fly that attacks does not see the player
//   growing: the looming of its own approach is not given to the brain (the fly knows its own movement).

#pragma once

#include <doom/DoomGame.h>
#include <doom/DoomSwarm.h>
#include <flybrain/FlySwarmBrain.h>
#include <myengine/game/DoomController.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace doomfly
{
    struct FlyModeOptions;

    struct SwarmConfig
    {
        std::shared_ptr<const flybrain::Connectome> circuit;   // swarm_circuit.fbrain
        int maxFlies = 16;                                     // the size of the biggest wave, 8..32
        int startWave = 1;                                     // the first wave: waves have 8 + 4 (n - 1) flies, up to maxFlies, and are faster by the number
        std::uint64_t seed = 1;
        flybrain::BackendChoice backend = flybrain::BackendChoice::Auto;
        bool parallel = true;                                  // the neurons of a window are jobs (jobs::Initialize has been called)
        std::function<void(const std::string&)> log;
    };

    // What happens in the swarm, for the HUD and the tests
    struct SwarmStats
    {
        int wave = 0;                  // 0: the first wave has not started
        int flies = 0;                 // alive
        int waveSize = 0;
        int killed = 0;                // all waves
        int biteDamage = 0;            // what the flies took from the player
        int bites = 0;
        int leaps = 0;                 // jumps of the giant fibre
        int tics = 0;
        int ticsToNextWave = 0;        // while the field is clear
        bool playerDead = false;
        double brainMsPerTic = 0.0;    // wall time of the brain for one tic (average of the last tics); the tic is 28.6 ms
        flybrain::Backend backend = flybrain::Backend::Cpu;
        float meanTurn = 0.0f;         // the brain, the average of the live flies
        float meanEscape = 0.0f;
        float meanLoom = 0.0f;         // what the flies see
    };

    class FlySwarm final : public myengine::game::IDoomController
    {
    public:
        // nullptr and `error` when the circuit lacks the inputs or the outputs
        static std::unique_ptr<FlySwarm> Create(const SwarmConfig& config, std::string& error);
        ~FlySwarm() override;

        // IDoomController: the game runs by tics here (the flies are driven before every one), the human keeps the keyboard
        bool Advance(doom::DoomGame& game, double deltaSeconds) override;
        void DrawPanel(ImDrawList* list, float x, float y, float width, float height) override;
        void DrawOverlay(ImDrawList* list, float width, float height) override;
        bool HumanPlays() const override { return true; }
        std::string Status() const override;

        // One tic: the brain, the flies, one tic of DOOM. true: a new frame (the tests use this one and the ITicCmdSource of the game for the player)
        bool StepTic(doom::DoomGame& game);

        const SwarmStats& Stats() const;
        // For the tests: where the flies are now (the alive ones) and the last input and output of the brain of a fly
        struct FlyView
        {
            int slot = 0;
            bool alive = false;
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            double heading = 0.0;
            double distance = 0.0;
            bool sees = false;             // a line of sight to the player
            flybrain::SwarmInput input;
            flybrain::SwarmOutput output;
        };
        std::vector<FlyView> Flies() const;

    private:
        struct Impl;
        explicit FlySwarm(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

    // The mode of the menu: the file of the circuit (next to the exe, then in the sources), the arguments of DOOM for the swarm
    std::filesystem::path FindSwarmCircuit(const FlyModeOptions& options);
    bool SwarmAvailable(const FlyModeOptions& options, std::string& reason);
    const std::vector<std::string>& SwarmDoomArgs();
    // `flies`: the size of the biggest wave (8..32); `backend`: "auto", "cpu" or "gpu"; the menu of FlyDoom gives them (DoomLaunchChoice)
    std::unique_ptr<myengine::game::IDoomController> MakeSwarmController(const FlyModeOptions& options, int flies, const std::string& backend, std::uint64_t seed, std::string& error);
}
