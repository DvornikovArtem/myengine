#include <brainview/BrainRunner.h>

#include <algorithm>
#include <chrono>

namespace brainview
{
    namespace
    {
        constexpr double kSliceSeconds = 0.008;       // the simulation goes forward by this much between two looks at commands and spikes
        constexpr double kMaxDebtSeconds = 0.25;      // a slow machine does not catch up for ever
        constexpr std::size_t kMailboxLimit = 4'000'000;
        constexpr uint32_t kSpikeRing = 1u << 19;
        constexpr double kEyeSwitchSeconds = 1.5;
        constexpr float kVisionRateHz = 60.0f;
        constexpr float kTouchRateHz = 60.0f;
        constexpr float kTonicMillivolts = 12.0f;
        constexpr float kSugarRateHz = 150.0f;
        constexpr float kPainRateHz = 120.0f;
        constexpr double kPainPeriodSeconds = 2.0;
        constexpr float kPainDurationMs = 400.0f;
    }

    const char* ToString(const StimulusPreset preset)
    {
        switch (preset)
        {
        case StimulusPreset::Silence: return "Silence";
        case StimulusPreset::Rest: return "Rest";
        case StimulusPreset::Vision: return "Vision";
        case StimulusPreset::Taste: return "Taste";
        case StimulusPreset::Pain: return "Pain";
        case StimulusPreset::Everything: return "Everything";
        }
        return "?";
    }

    BrainRunner::BrainRunner(std::shared_ptr<const flybrain::Connectome> connectome, Options options) : connectome_(std::move(connectome)), options_(std::move(options)) {}

    BrainRunner::~BrainRunner()
    {
        Stop();
    }

    void BrainRunner::Start()
    {
        if (running_.exchange(true))
        {
            return;
        }
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.error.clear();
        }
        quit_.store(false);
        try
        {
            thread_ = std::thread([this]() {
                try { Loop(); }
                catch (const std::exception& error)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stats_.error = error.what();
                    backendText_ = "Brain stopped: " + stats_.error;
                    gpuFlash_ = {};
                    onGpu_.store(false);
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stats_.error = "unknown brain error";
                    backendText_ = "Brain stopped: " + stats_.error;
                    gpuFlash_ = {};
                    onGpu_.store(false);
                }
                running_.store(false);
            });
        }
        catch (...) { running_.store(false); throw; }
    }

    void BrainRunner::Stop()
    {
        if (!running_.load() && !thread_.joinable())
        {
            return;
        }
        quit_.store(true);
        if (thread_.joinable())
        {
            thread_.join();
        }
        running_.store(false);
    }

    void BrainRunner::SetPreset(const StimulusPreset preset)
    {
        preset_.store(preset);
    }

    void BrainRunner::Post(std::function<void(flybrain::FlyBrain&)> command)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(std::move(command));
    }

    void BrainRunner::Drain(std::vector<Spike>& out, double& simSecondsNow)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(mailbox_);
        mailbox_.clear();
        groupCounts_.clear();
        simSecondsNow = mailboxSimSeconds_;
    }

    void BrainRunner::Drain(std::vector<Spike>& out, std::vector<GroupCount>& counts, double& simSecondsNow)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(mailbox_);
        mailbox_.clear();
        counts.swap(groupCounts_);
        groupCounts_.clear();
        simSecondsNow = mailboxSimSeconds_;
    }

    std::string BrainRunner::BackendText() const
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        return backendText_;
    }

    bool BrainRunner::GpuFlash(myengine::render::GpuFlashSource& out) const
    {
        if (!onGpu_.load())
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        if (gpuFlash_.buffer == nullptr)
        {
            return false;
        }
        out = gpuFlash_;
        // the step was known `since` seconds ago: the flashes go on fading while the brain is between two slices
        const double since = std::min(std::chrono::duration<double>(std::chrono::steady_clock::now() - gpuFlashTaken_).count(), 0.25);
        out.stepFraction = static_cast<float>(since / static_cast<double>(out.stepSeconds));
        return true;
    }

    BrainRunner::Stats BrainRunner::GetStats() const
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        return stats_;
    }

    void BrainRunner::ApplyPreset(flybrain::FlyBrain& brain, const StimulusPreset preset)
    {
        for (const flybrain::StimulusId id : activeStimuli_)
        {
            brain.StopStimulus(id);
        }
        activeStimuli_.clear();
        appliedPreset_ = preset;
        leftEye_ = false;   // the first switch makes it the left eye
        if (preset != StimulusPreset::Silence && !lamina_.Empty())
        {
            activeStimuli_.push_back(brain.StimulateCurrent(lamina_, kTonicMillivolts));
        }
        // Vision and Pain are driven by UpdatePreset (the eyes in turn, the pulses)
        if ((preset == StimulusPreset::Taste || preset == StimulusPreset::Everything) && !sugar_.Empty())
        {
            activeStimuli_.push_back(brain.StimulateRate(sugar_, kSugarRateHz));
        }
        if (preset == StimulusPreset::Everything && !touch_.Empty())
        {
            activeStimuli_.push_back(brain.StimulateRate(touch_, kTouchRateHz));
        }
    }

    void BrainRunner::UpdatePreset(flybrain::FlyBrain& brain, const double simSeconds)
    {
        const StimulusPreset wanted = preset_.load();
        if (wanted != appliedPreset_)
        {
            ApplyPreset(brain, wanted);
            // the first step of the new preset right away
            eyeSwitchedAt_ = -1.0e9;
            painAt_ = -1.0e9;
        }
        const bool vision = appliedPreset_ == StimulusPreset::Vision || appliedPreset_ == StimulusPreset::Everything;
        const bool pain = appliedPreset_ == StimulusPreset::Pain || appliedPreset_ == StimulusPreset::Everything;
        if (vision && simSeconds - eyeSwitchedAt_ >= kEyeSwitchSeconds)
        {
            // the other eye sees it now: the stimulus of the old eye stops, the new one starts
            if (eyeStimulus_ != kNoStimulus)
            {
                brain.StopStimulus(eyeStimulus_);
            }
            leftEye_ = !leftEye_;
            const flybrain::Group& eye = leftEye_ ? eyesLeft_ : eyesRight_;
            eyeStimulus_ = eye.Empty() ? kNoStimulus : brain.StimulateRate(eye, kVisionRateHz);
            eyeSwitchedAt_ = simSeconds;
        }
        else if (!vision && eyeStimulus_ != kNoStimulus)
        {
            brain.StopStimulus(eyeStimulus_);
            eyeStimulus_ = kNoStimulus;
        }
        if (pain && !dopamine_.Empty() && simSeconds - painAt_ >= kPainPeriodSeconds)
        {
            brain.StimulateRate(dopamine_, kPainRateHz, kPainDurationMs);   // ends by itself
            painAt_ = simSeconds;
        }
    }

    void BrainRunner::Loop()
    {
        flybrain::FlyBrain brain(connectome_);
        brain.SetParallel(true);
        brain.SetSpikeRingCapacity(kSpikeRing);
        std::vector<flybrain::WatchId> watches;
        std::vector<uint64_t> watchTotals;
        if (options_.backend != flybrain::BackendChoice::Cpu)
        {
            flybrain::GpuOptions gpu;
            gpu.device = static_cast<ID3D12Device*>(options_.renderDevice);
            std::string error;
            const flybrain::Backend backend = brain.SetBackend(options_.backend, gpu, &error);
            onGpu_.store(backend == flybrain::Backend::Gpu);
            std::lock_guard<std::mutex> lock(mutex_);
            backendText_ = backend == flybrain::Backend::Gpu ? "GPU" + (brain.GpuAdapterName().empty() ? std::string() : " (" + brain.GpuAdapterName() + ")")
                                                              : "CPU" + (error.empty() ? std::string() : " (" + error + ")");
        }
        else
        {
            std::lock_guard<std::mutex> lock(mutex_);
            backendText_ = "CPU";
        }
        const bool onGpu = onGpu_.load();
        if (onGpu)
        {
            for (const flybrain::Group& group : options_.watchGroups)
            {
                watches.push_back(brain.Watch(group, 2000.0f));
            }
            watchTotals.assign(watches.size(), 0);
        }
        const flybrain::Connectome& connectome = *connectome_;
        eyesLeft_ = connectome.Find("R1-R6", flybrain::Side::Left);
        eyesRight_ = connectome.Find("R1-R6", flybrain::Side::Right);
        touch_ = connectome.FindPrefix("SNta");
        sugar_ = connectome.Find("LB3c");
        for (const char* type : {"L1", "L2", "L3", "L5"})
        {
            const flybrain::Group part = connectome.Find(type);
            lamina_.neurons.insert(lamina_.neurons.end(), part.neurons.begin(), part.neurons.end());
        }
        dopamine_ = connectome.FindPrefix("PPL1");
        appliedPreset_ = StimulusPreset::Silence;
        eyeStimulus_ = kNoStimulus;
        eyeSwitchedAt_ = -1.0e9;
        painAt_ = -1.0e9;

        using Clock = std::chrono::steady_clock;
        auto last = Clock::now();
        auto statsStart = last;
        double statsSim0 = 0.0;
        uint64_t statsSpikes0 = 0;
        double debt = 0.0;   // simulated seconds still to do
        uint64_t lastTotal = 0;
        std::vector<flybrain::SpikeEvent> events;
        const double dtSeconds = brain.Params().dtMs / 1000.0;

        while (!quit_.load())
        {
            std::vector<std::function<void(flybrain::FlyBrain&)>> commands;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                commands.swap(commands_);
            }
            for (auto& command : commands)
            {
                command(brain);
            }

            const auto now = Clock::now();
            const double elapsed = std::min(std::chrono::duration<double>(now - last).count(), 0.1);
            last = now;
            if (paused_.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            debt = std::min(debt + elapsed * speed_.load(), kMaxDebtSeconds);
            if (debt < kSliceSeconds * 0.5)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const double slice = std::min(debt, kSliceSeconds);
            const double simBefore = brain.TimeMs() / 1000.0;
            UpdatePreset(brain, simBefore);
            brain.AdvanceMs(slice * 1000.0);
            debt -= slice;
            const double simNow = brain.TimeMs() / 1000.0;

            const uint64_t total = brain.TotalSpikes();
            const uint64_t fresh = total - lastTotal;
            lastTotal = total;
            if (fresh > 0 && !onGpu)
            {
                brain.RecentSpikes(events, static_cast<uint32_t>(std::min<uint64_t>(fresh, kSpikeRing)));
            }
            else
            {
                events.clear();
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (mailbox_.size() + events.size() > kMailboxLimit)
                {
                    mailbox_.clear();   // nobody is looking: start again rather than grow
                }
                for (const flybrain::SpikeEvent& event : events)
                {
                    mailbox_.push_back({event.neuron, static_cast<double>(event.step) * dtSeconds});
                }
                mailboxSimSeconds_ = simNow;
                if (onGpu)
                {
                    // the spikes of this slice, counted per group by the GPU (the counts of a submission arrive a little later: they are what is known now)
                    const double completed = static_cast<double>(brain.CompletedStep()) * dtSeconds;
                    for (std::size_t g = 0; g < watches.size(); ++g)
                    {
                        const uint64_t totalNow = brain.TotalSpikes(watches[g]);
                        const uint64_t spikes = totalNow - watchTotals[g];
                        watchTotals[g] = totalNow;
                        if (spikes > 0)
                        {
                            groupCounts_.push_back({static_cast<uint32_t>(g), static_cast<uint32_t>(std::min<uint64_t>(spikes, 0xFFFFFFFFull)), completed});
                        }
                    }
                    const flybrain::GpuSpikeRing ring = brain.GetGpuSpikeRing();
                    gpuFlash_.buffer = ring.lastSpikeStep;
                    gpuFlash_.bufferLifetime = ring.lastSpikeOwner;
                    gpuFlash_.stepWhole = static_cast<uint32_t>(brain.Step());
                    gpuFlash_.stepSeconds = static_cast<float>(dtSeconds);
                    gpuFlashTaken_ = std::chrono::steady_clock::now();
                }
                stats_.simSeconds = simNow;
                stats_.totalSpikes = total;
                const double wall = std::chrono::duration<double>(now - statsStart).count();
                if (wall >= 1.0)
                {
                    stats_.realtimeRatio = (simNow - statsSim0) / wall;
                    stats_.spikesPerSecond = static_cast<double>(total - statsSpikes0) / wall;
                    stats_.awakeNeurons = brain.AwakeCount();
                    statsStart = now;
                    statsSim0 = simNow;
                    statsSpikes0 = total;
                }
            }
        }
    }
}
