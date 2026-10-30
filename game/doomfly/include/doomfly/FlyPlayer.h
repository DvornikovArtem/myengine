// FlyPlayer.h
//
// The fly plays DOOM (docs/game/flydoom-fly.md): the picture goes through the eyes into the brain, the brain runs for the time of one tic of the
// game, the rates of the descending neurons become the command of the player, the game runs one tic. Then again.
//
//   frame -> FlyEyes (luminance of the photoreceptors) -> FlyBrain (28.57 ms of the brain per tic) -> rates of the descending neurons
//         -> FlyControls (turn, forward, fire, use) -> doom::TicCmd -> one tic of DOOM
//
// The game waits for the brain: a tic starts when the brain has finished the 28.57 ms of its time. The brain runs on its own thread (and on the job system
// for the neurons), so the window keeps drawing while it thinks; when the brain is slower than real time the game is slower, never the brain faster than
// real time (Pump does not start a tic before 1/35 s of the wall clock has passed since the last one). StepBlocking does the whole chain on the calling thread
// without any pace (the tests, and a brain that learns faster than real time).
//
// Reinforcement as in DOOMFLY: damage puts a current into the dopamine neurons PPL101 (4 mV for 200 ms). Optionally a kill or an item puts a current into PAM
// neurons (reward, off by default). The brain is made again (rest state, the stimuli of the eyes put back) when the player dies, when the level changes,
// and when the brain runs away: the activity of the whole brain above `seizureSpikesPerSecond` for `seizureMs` (the tonic current of the lamina above
// ~15 mV does it, and so does the taste input, docs/flybrain/flybrain.md).
// The weights of the connectome do not change, unless `learning` is on (N3b, docs/flybrain/flybrain.md, "Муха учится в игре"): then the KC -> MBON synapses
// learn, the change of the light of the columns goes straight into the visual inputs of the Kenyon cells (flybrain::KcVision, our bridge in place of the optic
// lobe), damage goes into every PPL1 above their threshold, dopamine is a modulator, the local neurons of the antennal lobe are silent, and what was learned
// stays across the brains of the lives (and in a memory file).

#pragma once

#include <doom/DoomGame.h>
#include <doomfly/FlyControls.h>
#include <doomfly/FlyEyes.h>
#include <flybrain/FlyBrain.h>
#include <flybrain/KcVision.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace doomfly
{
    struct FlyPlayerConfig
    {
        std::shared_ptr<const flybrain::Connectome> connectome;
        FlyControls controls;
        EyesConfig eyes;
        uint64_t seed = 1;
        flybrain::BackendChoice backend = flybrain::BackendChoice::Auto;   // the brain on the GPU when there is one that gives the same spikes (--flybrain)
        void* renderDevice = nullptr;           // the ID3D12Device* of the renderer: the brain lives on it, so that a view reads the ring of the spikes without a copy
        std::function<void(const std::string&)> log;
        bool parallel = true;                   // the neurons of a window run as jobs (jobs::Initialize must have been called)
        int substeps = 4;                       // the low pass of the photoreceptors is applied this many times per tic
        float damageMv = 4.0f;                  // PPL101: current for damageMs when the player is hurt
        float damageMs = 200.0f;
        bool reward = false;                    // PAM: current when the player kills a monster or takes an item
        float rewardMv = 4.0f;
        float rewardMs = 200.0f;
        // The brain ran away when the central brain (the cb_intrinsic neurons, 32 160 of them) fires more than this many spikes a second for seizureMs: at rest and under a
        // bright picture it is silent (0), a looming object lights it up to 0.1 - 1 million, the runaway of a lamina current above ~15 mV is several million
        float seizureSpikesPerSecond = 6.0e5f;
        float seizureMs = 300.0f;
        bool pace = true;                       // Pump does not run the game faster than 35 tics a second
        // Several players in one game (Human vs Fly, Fly vs Fly: DOOM's "-players N"): the player this brain is and where its eyes look. The picture of a player that is
        // not the console player always comes from the extra render pass; `ownView` makes the console player's brain use it too (two brains see the same way).
        int playerIndex = -1;                   // -1: the console player of the game
        bool ownView = false;
        bool drivesScreens = true;              // presses the keys of the title, the intermission and the menu (one brain of a game does)
        // The fly learns (N3b). Off: the brain of D1a, as it was (fast dopamine, the antennal lobe on: the looming answer of D1a needs neither change)
        struct Learning
        {
            bool enabled = false;
            bool learning = true;               // false: the weights stay (the control arm of a measurement), everything else the same
            float punishmentMv = 15.0f;         // damage into every PPL1 for damageMs (their threshold is 7 mV), instead of damageMv into PPL101
            std::string memoryPath;             // what was learned, between the runs (flybrain::FlyBrain::DefaultMemoryPath()); empty: no file
            flybrain::KcVisionConfig vision;    // the KCs see the change of the light of the columns: |L - slow| / (slow + 0.05), 0..1
        } learning;
    };

    struct RateReading
    {
        std::string name;                       // "DNp20 R"
        float hz = 0.0f;
    };

    struct FlyStatus
    {
        std::string backend;                    // "GPU (NVIDIA GeForce RTX 3050 Laptop GPU)" or "CPU (8 threads)"
        uint64_t tics = 0;                      // tics of the game the fly has played
        double brainSpeed = 0.0;                // brain time / wall time of the last tics; 1.0 is real time, above 1 the brain waits for the clock
        double gameSpeed = 0.0;                 // tics per second / 35
        bool waiting = false;                   // the game stands: the brain has not finished
        float turnHz = 0.0f;                    // the sums of the channels (Hz)
        float forwardHz = 0.0f;
        float fireHz = 0.0f;
        float turnDegrees = 0.0f;               // what the player does now
        float forwardUnits = 0.0f;
        bool fire = false;
        bool use = false;
        bool reflex = false;                    // the command is a reflex outside the brain now (turning away from a wall, the use button at a door), not what the neurons say
        float spikesPerSecond = 0.0f;           // the whole brain
        float centralSpikesPerSecond = 0.0f;    // the central brain (what the guard against the runaway watches)
        float dopamineMv = 0.0f;                // the current PPL101 (with learning: every PPL1) gets now
        float kcSpikesPerSecond = 0.0f;         // learning: the Kenyon cells
        double learnedScale = 1.0;              // learning: the mean scale of the KC -> MBON synapses (1: nothing learned), once a second
        uint32_t resets = 0;                    // the brain was made again (all reasons)
        uint32_t seizures = 0;                  // ... because it ran away
        uint32_t damages = 0;
        std::vector<RateReading> rates;         // the groups of the controls
        std::string controlsName;
    };

    // The spikes of the last tic and where the brain is (for a view of the brain, task V0)
    struct BrainSnapshot
    {
        uint64_t step = 0;
        std::vector<flybrain::SpikeEvent> spikes;
    };

    // What a view of the brain needs (V1): the connectome, the spikes of the last tic, and on the GPU the ring of the spikes of the brain itself. The ring points into
    // the resources of the brain that made it; the brain is made again when the player dies, the level changes or the brain runs away (`generation` grows), and the old
    // one is kept until the next Pump of the game's thread: the ring of a feed is good until the end of the frame in which the feed was taken. Do not keep it longer.
    struct BrainFeedSnapshot
    {
        std::shared_ptr<const flybrain::Connectome> connectome;
        std::vector<flybrain::SpikeEvent> spikes;
        flybrain::GpuSpikeRing ring;
        uint64_t step = 0;
        flybrain::Backend backend = flybrain::Backend::Cpu;
        uint32_t generation = 0;
    };

    class FlyPlayer final : public doom::ITicCmdSource
    {
    public:
        // nullptr and `error` when the connectome lacks what the controls or the eyes need
        static std::unique_ptr<FlyPlayer> Create(const FlyPlayerConfig& config, std::string& error);
        ~FlyPlayer() override;

        // Makes the player the source of the commands of the console player of `game`
        void Attach(doom::DoomGame& game);
        void Detach(doom::DoomGame& game);

        // One frame of the window: starts a tic of the brain when it is time, runs the game tic when the brain has finished. true: a new frame of DOOM
        bool Pump(doom::DoomGame& game, double wallSeconds);
        // The whole chain for one tic, here and now. true: a new frame
        bool StepBlocking(doom::DoomGame& game);

        FlyStatus Status() const;
        BrainSnapshot Snapshot() const;
        BrainFeedSnapshot BrainFeed() const { return feed_; }
        const FlyEyes& Eyes() const { return eyes_; }
        const FlyControls& Controls() const { return config_.controls; }
        const flybrain::Connectome& GetConnectome() const { return *config_.connectome; }
        bool Learns() const { return config_.learning.enabled; }

        // For the tests: a current (mV) into every neuron of a type until the next reset, to make the brain run away
        void InjectCurrentForTest(const std::string& type, float currentMv);
        // For the tests: the chain without a game: the picture `frame` (320x200, 0x00RRGGBB) shown for `tics` tics of the brain, `damageAtFirst`: the player is hurt
        // at the first one. Returns the status after the last tic (the command that the brain gives; the game is not touched). Not together with Pump / StepBlocking.
        FlyStatus EvaluateFrameForTest(const uint32_t* frame, int tics, bool damageAtFirst = false);
        // The same for a picture that changes: `makeFrame(tic, frame)` fills the frame of each tic (320x200, 0x00RRGGBB); `perTic` gets the status after every tic
        FlyStatus EvaluateSequenceForTest(const std::function<void(int, std::vector<uint32_t>&)>& makeFrame, int tics, std::vector<FlyStatus>* perTic = nullptr);
        // For the tests: the luminance per photoreceptor of the last tick
        std::vector<float> LastLuminance() const;
        // doom::ITicCmdSource: the command of the player is the last one the brain gave
        void BuildTicCmd(int player, doom::TicCmd& cmd) override;

        // The pieces of Pump, for several brains that play one game (FlyDuel): Begin counts the time and sends the picture to the brain when it is time, Poll takes the
        // answer of the brain (true: the command is ready), the host runs ONE tic of the game when all the brains are ready, then EndTic of each.
        void Begin(doom::DoomGame& game, double wallSeconds);
        bool Poll(doom::DoomGame& game);
        void EndTic(doom::DoomGame& game);
        const doom::TicCmd& Command() const { return command_; }
        int PlayerIndex(const doom::DoomGame& game) const { return config_.playerIndex >= 0 ? config_.playerIndex : game.Info().consolePlayer; }
        // The picture the brain sees now (320x200, 0x00RRGGBB); for a second view next to the game
        const std::vector<std::uint32_t>& EyesPicture() const { return eyesPicture_; }

    private:
        explicit FlyPlayer(const FlyPlayerConfig& config);
        struct Group
        {
            std::string name;
            flybrain::Group neurons;
            float weight = 1.0f;
            flybrain::WatchId watch = 0;
        };
        struct TickInput
        {
            std::vector<float> luminance;       // per photoreceptor
            bool damage = false;
            bool reward = false;
            bool reset = false;
            std::vector<float> columnLuminance; // per column of the eyes (the motion channel)
            bool valid = false;
        };
        struct TickResult
        {
            float turnHz = 0, forwardHz = 0, fireHz = 0;
            bool fireSpikes = false;
            float spikesPerSecond = 0;
            float centralSpikesPerSecond = 0;
            bool seizure = false;
            bool reset = false;
            float dopamineMv = 0;
            float kcSpikesPerSecond = 0;
            double learnedScale = -1.0;         // -1: not measured this tic
            std::vector<RateReading> rates;
            BrainSnapshot snapshot;
            BrainFeedSnapshot feed;
            double wallSeconds = 0;
            bool valid = false;
        };

        bool Initialize(std::string& error);
        void MakeBrain();                       // worker thread (and the constructor)
        TickResult RunTick(const TickInput& input);
        void WorkerLoop();
        TickInput SampleInput(doom::DoomGame& game);
        void ApplyResult(doom::DoomGame& game, const TickResult& result);
        void Shape(const TickResult& result, float& turnDegrees, float& forwardUnits, bool& fire) const;
        void Submit(const TickInput& input);
        bool WaitResult(TickResult& out, bool block);

        FlyPlayerConfig config_;
        FlyEyes eyes_;
        flybrain::Group dopamine_;
        flybrain::Group rewardGroup_;
        std::vector<Group> turnGroups_, forwardGroups_, fireGroups_, spikeGroups_, showGroups_;
        flybrain::Group central_;
        flybrain::WatchId centralWatch_ = 0;
        std::vector<float> slowColumn_;         // the slow average of the luminance of each column (the motion channel)
        std::vector<float> lastMotionDrive_;
        // learning (N3b)
        flybrain::KcVision kcVision_;
        std::vector<int> kcVisionColumn_;       // a column of kcVision_ -> the column of the eyes (-1: the eyes do not have it)
        std::vector<float> kcFeature_;
        flybrain::Group kcs_;
        flybrain::WatchId kcWatch_ = 0;
        std::vector<double> learned_;           // the scales of the last brain, for the next one
        uint64_t learnedTics_ = 0;

        // the brain: touched by the worker thread only (and by the constructor before it runs)
        std::unique_ptr<flybrain::FlyBrain> brain_;
        std::vector<std::unique_ptr<flybrain::FlyBrain>> retired_;   // made again: kept until the game's thread has drawn (under mutex_)
        std::string backendText_;
        uint32_t generation_ = 0;
        std::vector<float> filtered_;           // the low pass of the photoreceptors
        std::vector<float> slow_;               // the slow average of the motion channel
        std::vector<float> lastDrive_;
        uint64_t lastSpikes_ = 0;
        double overSeizureMs_ = 0.0;
        std::vector<std::pair<std::string, float>> injected_;
        std::mutex injectMutex_;
        double dopamineUntilMs_ = 0.0;

        // the thread
        std::thread worker_;
        mutable std::mutex mutex_;
        std::condition_variable wake_;
        std::condition_variable done_;
        TickInput job_;
        TickResult result_;
        bool quit_ = false;
        bool pending_ = false;                  // a tick was submitted and its result is not taken yet
        bool brainBusy_ = false;

        // the player's side (the thread of the game)
        doom::TicCmd command_;
        int prevHealth_ = 100;
        int prevMap_ = -1;
        int prevEpisode_ = -1;
        int prevKills_ = 0;
        int prevItems_ = 0;
        bool needReset_ = true;
        double sinceSubmit_ = 0.0;
        uint32_t resets_ = 0, seizures_ = 0, damages_ = 0;
        uint64_t tics_ = 0;
        double speedSmoothed_ = 0.0;            // brain time / wall time of the brain
        double ticIntervalSmoothed_ = 0.0;      // wall seconds between two tics of the game
        double wallSinceTic_ = 0.0;
        bool waiting_ = false;
        struct Position
        {
            double x = 0, y = 0;
        };
        std::vector<Position> history_;
        uint32_t useCooldown_ = 0;
        uint32_t stuckCount_ = 0;
        uint32_t reflexLeft_ = 0;
        float reflexSign_ = 1.0f;
        uint32_t screenCooldown_ = 0;
        FlyStatus status_;
        BrainSnapshot snapshot_;
        BrainFeedSnapshot feed_;
        std::vector<float> lastLuminance_;
        std::vector<std::uint32_t> eyesPicture_;
        doom::ViewBuffer eyesBuffer_;
        bool commandReady_ = false;
    };
}
