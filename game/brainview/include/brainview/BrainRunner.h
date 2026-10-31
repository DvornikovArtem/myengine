// BrainRunner.h
// Runs a FlyBrain on a thread of its own at (at most) the speed of real time and hands its spikes to the thread that draws, so that drawing never waits for the
// simulation and the simulation never waits for a frame (docs/flybrain/brain-view.md). It is for the demo window and for anything that wants to show a brain without
// DOOM: DOOM drives its own brain from the game tick and calls BrainView::Flash with what it reads from it.
//
// The FlyBrain belongs to the thread of the runner; the other thread talks to it with Post (a command that runs between two slices of the simulation).

#pragma once

#include <flybrain/Connectome.h>
#include <flybrain/FlyBrain.h>

#include <myengine/render/RenderTypes.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace brainview
{
    // What the brain is fed with (the demo window picks one; DOOM feeds it the screen instead)
    enum class StimulusPreset : uint8_t
    {
        Silence = 0,          // nothing: a LIF network without input does not spike
        Rest = 1,             // the tonic 12 mV into the lamina (L1, L2, L3, L5) that DOOMFLY gives a fly at rest: the optic lobes hum, the rest is quiet
        Vision = 2,           // rest plus Poisson input into the photoreceptors of the left and the right eye in turn (1.5 s each)
        Taste = 3,            // rest plus the sugar neurons LB3c at 150 Hz (a brain that sustains itself, as the model of Shiu et al. does)
        Pain = 4,             // rest plus a pulse into the dopamine neurons of the PPL1 cluster every 2 s (the hit of DOOM)
        Everything = 5,       // all of them together, and the touch neurons
    };
    const char* ToString(StimulusPreset preset);

    class BrainRunner
    {
    public:
        struct Spike
        {
            uint32_t neuron;
            double simSeconds;   // when it spiked, on the clock of the simulation
        };
        struct Stats
        {
            double simSeconds = 0.0;
            double realtimeRatio = 0.0;       // simulated seconds per wall second over the last second
            uint64_t totalSpikes = 0;
            double spikesPerSecond = 0.0;     // of the whole brain, over the last second
            uint32_t awakeNeurons = 0;
            std::string error;
        };

        struct Options
        {
            flybrain::BackendChoice backend = flybrain::BackendChoice::Cpu;
            void* renderDevice = nullptr;              // the renderer's ID3D12Device*: snapshots stay on this GPU (IRenderAdapter::GetNativeDevice)
            std::vector<flybrain::Group> watchGroups;  // the groups of the HUD: on the GPU their spikes are counted per slice (there are no spike events to read without waiting)
        };
        // The spikes of a group in one slice of the simulation (the GPU backend: RecentSpikes would wait for the GPU)
        struct GroupCount
        {
            uint32_t group;
            uint32_t count;
            double simSeconds;
        };

        explicit BrainRunner(std::shared_ptr<const flybrain::Connectome> connectome, Options options = {});
        ~BrainRunner();
        BrainRunner(const BrainRunner&) = delete;
        BrainRunner& operator=(const BrainRunner&) = delete;

        void Start();
        void Stop();
        bool Running() const { return running_.load(); }

        void SetPreset(StimulusPreset preset);
        StimulusPreset Preset() const { return preset_.load(); }
        void SetPaused(bool paused) { paused_.store(paused); }
        bool Paused() const { return paused_.load(); }
        // 1: real time. More than 1 only helps when the machine is faster than real time
        void SetSpeed(double speed) { speed_.store(speed); }
        double Speed() const { return speed_.load(); }

        // A command for the brain: runs on the thread of the runner between two slices
        void Post(std::function<void(flybrain::FlyBrain&)> command);

        // The spikes since the last Drain, oldest first, and the simulated time at the end of them. Stops growing at 4 million: the oldest are dropped.
        void Drain(std::vector<Spike>& out, double& simSecondsNow);
        // The same, and the counts of the watched groups since the last Drain (filled on the GPU backend only: there `out` stays empty)
        void Drain(std::vector<Spike>& out, std::vector<GroupCount>& counts, double& simSecondsNow);
        // Where the brain runs (known once the thread has made it) and why not on the GPU when it was asked for
        bool OnGpu() const { return onGpu_.load(); }
        std::string BackendText() const;
        // The buffer of the last spikes of a GPU brain for the 3D view, with the step now (and a part of a step for the wall time since it was taken); false on the CPU
        bool GpuFlash(myengine::render::GpuFlashSource& out) const;
        // Groups of the connectome the thread can talk about without a lock (read only)
        const flybrain::Connectome& GetConnectome() const { return *connectome_; }
        Stats GetStats() const;

    private:
        void Loop();
        void ApplyPreset(flybrain::FlyBrain& brain, StimulusPreset preset);
        void UpdatePreset(flybrain::FlyBrain& brain, double simSeconds);

        std::shared_ptr<const flybrain::Connectome> connectome_;
        Options options_;
        std::atomic<bool> onGpu_{false};
        std::string backendText_;                    // guarded by mutex_
        myengine::render::GpuFlashSource gpuFlash_;  // guarded by mutex_: the last slice
        std::chrono::steady_clock::time_point gpuFlashTaken_{};
        std::vector<GroupCount> groupCounts_;        // guarded by mutex_
        std::thread thread_;
        std::atomic<bool> running_{false};
        std::atomic<bool> quit_{false};
        std::atomic<bool> paused_{false};
        std::atomic<double> speed_{1.0};
        std::atomic<StimulusPreset> preset_{StimulusPreset::Vision};

        std::mutex mutex_;                                         // the commands, the mailbox, the numbers
        std::vector<std::function<void(flybrain::FlyBrain&)>> commands_;
        std::vector<Spike> mailbox_;
        double mailboxSimSeconds_ = 0.0;
        Stats stats_;

        // the state of the preset, the thread of the runner only
        StimulusPreset appliedPreset_ = StimulusPreset::Silence;
        std::vector<flybrain::StimulusId> activeStimuli_;
        static constexpr flybrain::StimulusId kNoStimulus = 0xFFFFFFFFu;
        flybrain::StimulusId eyeStimulus_ = kNoStimulus;
        double eyeSwitchedAt_ = -1.0e9;
        double painAt_ = -1.0e9;
        bool leftEye_ = true;
        flybrain::Group eyesLeft_;
        flybrain::Group eyesRight_;
        flybrain::Group touch_;
        flybrain::Group sugar_;
        flybrain::Group lamina_;
        flybrain::Group dopamine_;
    };
}
