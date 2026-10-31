// FlyPlayer.cpp

#include <doomfly/FlyPlayer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace doomfly
{
    namespace
    {
        constexpr double kTicSeconds = 1.0 / 35.0;
        constexpr double kTicMs = 1000.0 / 35.0;
        constexpr float kPi = 3.14159265f;

        std::string GroupName(const ControlTerm& term)
        {
            return term.type + (term.side == flybrain::Side::Left ? " L" : term.side == flybrain::Side::Right ? " R" : "");
        }

        float Shaped(const float sum, const AnalogChannel& channel)
        {
            float v = sum - channel.offsetHz;
            if (std::fabs(v) <= channel.deadZoneHz)
            {
                return 0.0f;
            }
            v -= v > 0.0f ? channel.deadZoneHz : -channel.deadZoneHz;
            v *= channel.gain;
            return std::clamp(v, -channel.limit, channel.limit);
        }
    }

    FlyPlayer::FlyPlayer(const FlyPlayerConfig& config) : config_(config) {}

    std::unique_ptr<FlyPlayer> FlyPlayer::Create(const FlyPlayerConfig& config, std::string& error)
    {
        std::unique_ptr<FlyPlayer> player(new FlyPlayer(config));
        if (!player->Initialize(error))
        {
            return nullptr;
        }
        return player;
    }

    bool FlyPlayer::Initialize(std::string& error)
    {
        if (!config_.connectome)
        {
            error = "no connectome";
            return false;
        }
        const flybrain::Connectome& connectome = *config_.connectome;
        if (!eyes_.Build(connectome, config_.eyes, error))
        {
            return false;
        }

        auto resolve = [&](const std::vector<ControlTerm>& terms, std::vector<Group>& out, const char* channel) -> bool
        {
            for (const ControlTerm& term : terms)
            {
                Group group;
                group.name = GroupName(term);
                group.neurons = connectome.Find(term.type, term.side);
                group.weight = term.weight;
                if (group.neurons.Empty())
                {
                    error = std::string("the controls (") + channel + ") name " + group.name + ", the connectome has no such neurons";
                    return false;
                }
                out.push_back(std::move(group));
            }
            return true;
        };
        if (!resolve(config_.controls.turn.terms, turnGroups_, "turn") || !resolve(config_.controls.forward.terms, forwardGroups_, "forward") ||
            !resolve(config_.controls.fire.terms, fireGroups_, "fire") || !resolve(config_.controls.fire.spikeTerms, spikeGroups_, "fire (spikes)") ||
            !resolve(config_.controls.show, showGroups_, "show"))
        {
            return false;
        }
        central_ = connectome.FindSuperclass("cb_intrinsic");
        dopamine_ = connectome.Find("PPL101");
        if (dopamine_.Empty())
        {
            error = "the connectome has no PPL101 (the dopamine of the pain)";
            return false;
        }
        rewardGroup_ = connectome.FindPrefix("PAM");
        if (config_.learning.enabled)
        {
            std::string visionError;
            if (!kcVision_.Build(connectome, config_.learning.vision, &visionError))
            {
                error = "learning: " + visionError;
                return false;
            }
            // the columns of the KC inputs are the columns of the eyes (the same assignedOlHex1/2 of one side)
            const std::vector<Column>& columns = eyes_.Columns();
            kcVisionColumn_.assign(kcVision_.Columns().size(), -1);
            for (size_t k = 0; k < kcVisionColumn_.size(); ++k)
            {
                const flybrain::KcVisionColumn& c = kcVision_.Columns()[k];
                const int eye = c.side == flybrain::Side::Left ? 0 : c.side == flybrain::Side::Right ? 1 : -1;
                for (size_t i = 0; i < columns.size() && eye >= 0; ++i)
                {
                    if (columns[i].eye == eye && columns[i].hex1 == c.hex1 && columns[i].hex2 == c.hex2)
                    {
                        kcVisionColumn_[k] = static_cast<int>(i);
                        break;
                    }
                }
            }
            kcFeature_.assign(kcVisionColumn_.size(), 0.0f);
            kcs_ = connectome.FindPrefix("KC");
        }

        if (config_.wallSense.enabled)
        {
            std::vector<float> columnX, columnY;
            for (const Column& column : eyes_.Columns())
            {
                columnX.push_back(column.x);
                columnY.push_back(column.y);
            }
            wallSense_.Build(config_.wallSense, columnX, columnY);
            std::string loadError;
            for (const std::string& path : config_.wallSenseLoad)
            {
                if (!wallSense_.Load(path, &loadError, true))
                {
                    error = loadError;
                    return false;
                }
                if (config_.log)
                {
                    config_.log("fly: our wall layer starts trained (" + path + ", " + std::to_string(wallSense_.Places()) + " places)");
                }
            }
        }

        MakeBrain();
        history_.reserve(64);
        return true;
    }

    FlyPlayer::~FlyPlayer()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable())
        {
            worker_.join();
        }
        if (wallSense_.Enabled() && !config_.wallSenseSave.empty())
        {
            std::string error;
            if (!wallSense_.Save(config_.wallSenseSave, &error) && config_.log)
            {
                config_.log("fly: our wall layer was not saved: " + error);
            }
        }
        if (!workerError_ && brain_ && config_.learning.enabled && config_.learning.saveMemory && !config_.learning.memoryPath.empty())
        {
            try
            {
                std::string error;
                if (!brain_->SaveMemory(config_.learning.memoryPath, &error) && config_.log)
                    config_.log("fly: the memory was not saved: " + error);
            }
            catch (const std::exception& error)
            {
                if (config_.log) config_.log(std::string("fly: the memory was not saved: ") + error.what());
            }
        }
    }

    void FlyPlayer::MakeBrain()
    {
        if (brain_ && config_.learning.enabled)
        {
            learned_ = brain_->GetPlasticScales();
        }
        if (brain_)
        {
            // not destroyed on this thread: a view may be drawing the ring of its spikes (see BrainFeedSnapshot)
            std::lock_guard<std::mutex> lock(mutex_);
            retired_.push_back(std::move(brain_));
        }
        flybrain::ModelParams params;
        params.seed = config_.seed;
        if (config_.learning.enabled)
        {
            // dopamine as a modulator (fast, it starts the self-sustained activity of the central brain), the antennal lobe silent (with the visual input
            // into the KCs its local neurons start it too)
            params.fastModulatorySynapses = false;
            params.silentTypePrefixes = {"lLN"};
        }
        brain_ = std::make_unique<flybrain::FlyBrain>(config_.connectome, params);
        brain_->SetParallel(config_.parallel);
        if (config_.learning.enabled)
        {
            flybrain::PlasticityRule rule;
            rule.learningRate = config_.learning.learningRate;
            rule.learnEveryMs = kTicMs - 0.5;   // once a tic, not at every substep: the GPU is not stopped 4 times a tic (N4)
            brain_->EnablePlasticity(rule);   // before the GPU takes it
            std::string error;
            if (!learned_.empty())
            {
                brain_->SetPlasticScales(learned_);
            }
            else if (!config_.learning.memoryPath.empty() && !brain_->LoadMemory(config_.learning.memoryPath, &error) && config_.log)
            {
                config_.log("fly: no memory loaded (" + error + "), the fly starts with the weights of the connectome");
            }
            brain_->SetLearning(config_.learning.learning);
        }
        {
            flybrain::GpuOptions options;
            options.device = static_cast<ID3D12Device*>(config_.renderDevice);
            std::string error;
            const flybrain::Backend backend = brain_->SetBackend(config_.backend, options, &error);
            const std::string text = backend == flybrain::Backend::Gpu ? (config_.renderDevice != nullptr ? std::string("GPU (render device)") : "GPU (" + brain_->GpuAdapterName() + ")") : "CPU (job system)";
            if (text != backendText_)
            {
                backendText_ = text;
                if (config_.log)
                {
                    config_.log("fly: the brain runs on the " + text + (backend == flybrain::Backend::Cpu && config_.backend != flybrain::BackendChoice::Cpu ? "; the GPU was not taken: " + error : std::string()));
                }
            }
        }
        ++generation_;
        brain_->StimulateCurrent(eyes_.Lamina(), config_.eyes.tonicMv);
        auto watch = [&](std::vector<Group>& groups)
        {
            for (Group& group : groups)
            {
                group.watch = brain_->Watch(group.neurons, 2000.0f);
            }
        };
        watch(turnGroups_);
        watch(forwardGroups_);
        watch(fireGroups_);
        watch(spikeGroups_);
        watch(showGroups_);
        centralWatch_ = central_.Empty() ? 0 : brain_->Watch(central_, 500.0f);
        kcWatch_ = kcs_.Empty() ? 0 : brain_->Watch(kcs_, 500.0f);
        slowColumn_.clear();
        lastMotionDrive_.assign(eyes_.MotionNeurons().size(), -1.0f);
        filtered_.clear();
        slow_.clear();
        lastDrive_.assign(eyes_.Receptors().size(), -1.0f);
        lastSpikes_ = 0;
        overSeizureMs_ = 0.0;
        {
            std::lock_guard<std::mutex> lock(injectMutex_);
            injected_.clear();
        }
    }

    void FlyPlayer::InjectCurrentForTest(const std::string& type, const float currentMv)
    {
        std::lock_guard<std::mutex> lock(injectMutex_);
        injected_.emplace_back(type, currentMv);
    }

    std::vector<float> FlyPlayer::LastLuminance() const
    {
        return lastLuminance_;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------------
    // the brain thread

    FlyPlayer::TickResult FlyPlayer::RunTick(const TickInput& input)
    {
        const auto begin = std::chrono::steady_clock::now();
        TickResult result;
        if (input.reset || !brain_)
        {
            MakeBrain();
            result.reset = true;
        }
        {
            std::vector<std::pair<std::string, float>> injected;
            {
                std::lock_guard<std::mutex> lock(injectMutex_);
                injected.swap(injected_);
            }
            for (const auto& item : injected)
            {
                brain_->StimulateCurrent(config_.connectome->Find(item.first), item.second);
            }
        }
        if (input.damage || input.wall)
        {
            if (config_.learning.enabled)
            {
                brain_->StimulateCurrent(brain_->PunishmentNeurons(), config_.learning.punishmentMv, config_.damageMs);
            }
            else
            {
                brain_->StimulateCurrent(dopamine_, config_.damageMv, config_.damageMs);
            }
            dopamineUntilMs_ = brain_->TimeMs() + config_.damageMs;
        }
        if (input.reward && !rewardGroup_.Empty())
        {
            brain_->StimulateCurrent(rewardGroup_, config_.rewardMv, config_.rewardMs);
        }

        const std::vector<Photoreceptor>& receptors = eyes_.Receptors();
        const size_t count = receptors.size();
        if (filtered_.size() != count)
        {
            filtered_ = input.luminance;   // the first picture: no transient from darkness
            slow_ = input.luminance;
        }
        const bool motionChannel = config_.eyes.motionOnGainMv > 0.0f || config_.eyes.motionOffGainMv > 0.0f;
        // learning: the KCs see the change of the light of their columns against the slow average of the motion channel (KcVision, Poisson input)
        if (config_.learning.enabled && !input.columnLuminance.empty())
        {
            if (slowColumn_.size() != input.columnLuminance.size())
            {
                slowColumn_ = input.columnLuminance;
            }
            for (size_t k = 0; k < kcVisionColumn_.size(); ++k)
            {
                const int c = kcVisionColumn_[k];
                kcFeature_[k] = c < 0 ? 0.0f : std::min(1.0f, std::fabs(input.columnLuminance[c] - slowColumn_[c]) / (slowColumn_[c] + 0.05f));
            }
            kcVision_.Apply(*brain_, kcFeature_);
            if (!motionChannel)
            {
                const float slowAlphaTic = 1.0f - std::exp(-static_cast<float>(kTicMs) / std::max(config_.eyes.motionAverageMs, 1.0f));
                for (size_t c = 0; c < slowColumn_.size(); ++c)
                {
                    slowColumn_[c] += (input.columnLuminance[c] - slowColumn_[c]) * slowAlphaTic;
                }
            }
        }
        // the motion channel: the columns' changes of the light drive the cells that carry them (once a tic: the picture is the same for the whole tic)
        if (!input.columnLuminance.empty() && motionChannel)
        {
            if (slowColumn_.size() != input.columnLuminance.size())
            {
                slowColumn_ = input.columnLuminance;
            }
            const std::vector<FlyEyes::MotionNeuron>& motion = eyes_.MotionNeurons();
            for (size_t i = 0; i < motion.size(); ++i)
            {
                const size_t c = static_cast<size_t>(motion[i].column);
                const float drive = eyes_.MotionDriveMv(input.columnLuminance[c], slowColumn_[c], motion[i].on);
                if (std::fabs(drive - lastMotionDrive_[i]) > 0.05f)
                {
                    lastMotionDrive_[i] = drive;
                    brain_->SetDrive(motion[i].neuron, drive);
                }
            }
            const float slowAlphaTic = 1.0f - std::exp(-static_cast<float>(kTicMs) / std::max(config_.eyes.motionAverageMs, 1.0f));
            for (size_t c = 0; c < slowColumn_.size(); ++c)
            {
                slowColumn_[c] += (input.columnLuminance[c] - slowColumn_[c]) * slowAlphaTic;
            }
        }
        const int substeps = std::max(1, config_.substeps);
        const double subMs = kTicMs / substeps;
        const float alpha = 1.0f - std::exp(-static_cast<float>(subMs) / std::max(config_.eyes.lowPassMs, 0.1f));
        const float slowAlpha = 1.0f - std::exp(-static_cast<float>(subMs) / std::max(config_.eyes.motionAverageMs, 1.0f));
        for (int step = 0; step < substeps; ++step)
        {
            for (size_t i = 0; i < count; ++i)
            {
                filtered_[i] += (input.luminance[i] - filtered_[i]) * alpha;
                slow_[i] += (input.luminance[i] - slow_[i]) * slowAlpha;
                const float drive = eyes_.ResponseMv(filtered_[i]) + config_.eyes.motionGainMv * (filtered_[i] - slow_[i]);
                if (std::fabs(drive - lastDrive_[i]) > 0.02f)
                {
                    lastDrive_[i] = drive;
                    brain_->SetDrive(receptors[i].neuron, drive);
                }
            }
            brain_->AdvanceMs(subMs);
        }
        // On the GPU the windows are only sent: wait for them, so that what the controls read is the end of this tic and not "whatever has come back" (the same game every time)
        brain_->Synchronize();

        // what the controls read
        const float window = config_.controls.smoothingMs;
        auto sum = [&](const std::vector<Group>& groups)
        {
            float total = 0.0f;
            for (const Group& group : groups)
            {
                total += group.weight * brain_->RateHz(group.watch, window);
            }
            return total;
        };
        result.turnHz = sum(turnGroups_);
        result.forwardHz = sum(forwardGroups_);
        result.fireHz = sum(fireGroups_);
        for (const Group& group : spikeGroups_)
        {
            if (config_.controls.fire.minSpikes > 0 && brain_->SpikeCount(group.watch, config_.controls.fire.windowMs) >= config_.controls.fire.minSpikes)
            {
                result.fireSpikes = true;
            }
        }
        for (const std::vector<Group>* groups : {&turnGroups_, &forwardGroups_, &fireGroups_, &spikeGroups_, &showGroups_})
        {
            for (const Group& group : *groups)
            {
                const bool known = std::any_of(result.rates.begin(), result.rates.end(), [&](const RateReading& r) { return r.name == group.name; });
                if (!known)
                {
                    result.rates.push_back({group.name, brain_->RateHz(group.watch, window)});
                }
            }
        }
        result.dopamineMv = brain_->TimeMs() < dopamineUntilMs_ ? (config_.learning.enabled ? config_.learning.punishmentMv : config_.damageMv) : 0.0f;
        if (config_.learning.enabled)
        {
            result.kcSpikesPerSecond = kcWatch_ != 0 ? static_cast<float>(static_cast<double>(brain_->SpikeCount(kcWatch_, static_cast<float>(kTicMs))) / (kTicMs / 1000.0)) : 0.0f;
            if (learnedTics_++ % 35 == 0)
            {
                // N4: the mean over the synapses of the KCs that see (the others never learn: their mean said 0.98 when the seeing ones were at 0.6)
                const std::vector<double> scales = brain_->GetPlasticScales();
                if (visualEdges_.empty())
                {
                    std::vector<uint8_t> seeing(config_.connectome->NeuronCount(), 0);
                    const flybrain::Connectome& cn = *config_.connectome;
                    for (const flybrain::KcVisionInput& in : kcVision_.Inputs())
                    {
                        for (uint32_t e = cn.EdgeBegin(in.neuron); e < cn.EdgeEnd(in.neuron); ++e)
                        {
                            seeing[cn.Targets()[e]] = cn.Weights()[e] > 0 ? 1 : seeing[cn.Targets()[e]];
                        }
                    }
                    for (uint32_t k = 0; k < brain_->PlasticEdgeCount(); ++k)
                    {
                        if (seeing[brain_->PlasticPre(k)] != 0)
                        {
                            visualEdges_.push_back(k);
                        }
                    }
                }
                double scaleSum = 0.0;
                for (const uint32_t k : visualEdges_)
                {
                    scaleSum += k < scales.size() ? scales[k] : 1.0;
                }
                result.learnedScale = visualEdges_.empty() ? 1.0 : scaleSum / static_cast<double>(visualEdges_.size());
            }
        }

        const uint64_t total = brain_->TotalSpikes();
        result.spikesPerSecond = static_cast<float>(static_cast<double>(total - lastSpikes_) / (kTicMs / 1000.0));
        lastSpikes_ = total;
        result.centralSpikesPerSecond = centralWatch_ != 0 ? static_cast<float>(static_cast<double>(brain_->SpikeCount(centralWatch_, static_cast<float>(kTicMs))) / (kTicMs / 1000.0)) : 0.0f;
        if (result.centralSpikesPerSecond > config_.seizureSpikesPerSecond)
        {
            overSeizureMs_ += kTicMs;
        }
        else
        {
            overSeizureMs_ = 0.0;
        }
        result.snapshot.step = brain_->Step();
        brain_->RecentSpikes(result.snapshot.spikes, 2048);
        if (overSeizureMs_ >= config_.seizureMs)
        {
            // the brain ran away: it starts again from rest (the neurons and the eyes are put back at the next tick)
            result.seizure = true;
            result.reset = true;
            MakeBrain();
        }
        result.feed.connectome = config_.connectome;
        result.feed.spikes = result.snapshot.spikes;
        result.feed.step = result.snapshot.step;
        result.feed.backend = brain_->GetBackend();
        result.feed.ring = brain_->GetGpuSpikeRing();
        result.feed.generation = generation_;
        result.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        result.valid = true;
        return result;
    }

    void FlyPlayer::WorkerLoop()
    {
#ifndef MYENGINE_SHIPPING
        char* inject = nullptr;
        std::size_t injectSize = 0;
        const bool injectFailure = _dupenv_s(&inject, &injectSize, "MYENGINE_FLY_TEST_WORKER_ERROR") == 0 && inject && inject[0] == '1';
        std::free(inject);
#endif
        for (;;)
        {
            TickInput input;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return quit_ || job_.valid; });
                if (quit_)
                {
                    return;
                }
                input = std::move(job_);
                job_ = {};
                brainBusy_ = true;
            }
            TickResult result;
            try
            {
#ifndef MYENGINE_SHIPPING
                if (injectFailure) throw std::runtime_error("fly worker injected failure");
#endif
                result = RunTick(input);
            }
            catch (...)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    workerError_ = std::current_exception();
                    brainBusy_ = false;
                }
                done_.notify_all();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                result_ = std::move(result);
                brainBusy_ = false;
            }
            done_.notify_all();
        }
    }

    void FlyPlayer::Submit(const TickInput& input)
    {
        if (!worker_.joinable())
        {
            worker_ = std::thread([this] { WorkerLoop(); });
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = input;
            job_.valid = true;
            result_ = {};
            pending_ = true;
        }
        wake_.notify_all();
    }

    bool FlyPlayer::WaitResult(TickResult& out, const bool block)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (block)
        {
            done_.wait(lock, [this] { return result_.valid || workerError_; });
        }
        if (workerError_) std::rethrow_exception(workerError_);
        if (!result_.valid)
        {
            return false;
        }
        out = std::move(result_);
        result_ = {};
        pending_ = false;
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------------
    // the game's side

    FlyPlayer::TickInput FlyPlayer::SampleInput(doom::DoomGame& game)
    {
        TickInput input;
        const doom::GameInfo info = game.Info();
        const int index = PlayerIndex(game);
        const doom::PlayerState player = game.Player(index);
        const std::uint32_t* picture = game.Pixels();
        if ((index != info.consolePlayer || config_.ownView) && game.RenderEyes(index, eyesBuffer_))
        {
            picture = eyesBuffer_.pixels.data();
            eyesPicture_ = eyesBuffer_.pixels;
        }
        else if (picture != nullptr)
        {
            eyesPicture_.assign(picture, picture + doom::kScreenWidth * doom::kScreenHeight);
        }
        eyes_.SampleLuminance(picture, doom::kScreenWidth, doom::kScreenHeight, input.luminance);
        eyes_.SampleColumns(picture, doom::kScreenWidth, doom::kScreenHeight, input.columnLuminance);
        lastLuminance_ = input.luminance;
        lastColumns_ = input.columnLuminance;

        if (player.inGame)
        {
            input.damage = player.health < prevHealth_;
            if (input.damage)
            {
                ++damages_;
            }
            input.wall = wallPunish_ && config_.learning.enabled && config_.learning.wallPunishment;
            input.reward = config_.reward && (player.kills > prevKills_ || player.items > prevItems_);
            const bool died = prevHealth_ <= 0 && player.health > 0;
            const bool newLevel = prevMap_ >= 0 && (info.map != prevMap_ || info.episode != prevEpisode_);
            if (died || newLevel)
            {
                needReset_ = true;
                wallSense_.ResetTransient();
                input.wall = false;
                atWall_ = false;
                wallTicsNow_ = 0;
                stuckCount_ = 0;
                history_.clear();
                commandColumns_.clear();
                whiskerActive_ = false;
                lastCommandWantedForward_ = 0.0f;
                lastCommandForward_ = 0.0f;
                lastCommandReflex_ = false;
            }
            prevHealth_ = player.health;
            prevKills_ = player.kills;
            prevItems_ = player.items;
            prevMap_ = info.map;
            prevEpisode_ = info.episode;
        }
        wallPunish_ = false;
        if (needReset_)
        {
            input.reset = true;
            needReset_ = false;
            ++resets_;
        }
        return input;
    }

    void FlyPlayer::ApplyResult(doom::DoomGame& game, const TickResult& result)
    {
        const doom::GameInfo info = game.Info();
        const doom::PlayerState player = game.Player(PlayerIndex(game));
        if (result.seizure)
        {
            ++seizures_;
            ++resets_;
        }

        const FlyControls& controls = config_.controls;
        doom::TicCmd cmd;
        const bool inLevel = info.state == doom::GameState::Level && !info.menuActive && player.inGame;
        float turnDegrees = 0.0f;
        float forwardUnits = 0.0f;
        bool fire = false;
        bool use = false;
        if (inLevel)
        {
            Shape(result, turnDegrees, forwardUnits, fire);
            const bool brainFire = fire;
            status_.brainFire = brainFire;
            status_.targetVisible = false;
            status_.authoredCombatAssist = config_.combatAssist.enabled;
            status_.targetAngleDegrees = 0.0f;
            if (config_.combatAssist.enabled && player.health > 0)
            {
                // Authored helper only: the native game thread returns a copied
                // target angle from wall/vertical-aware P_AimLineAttack probes.
                const doom::VisibleTarget target = game.FindVisibleTarget(PlayerIndex(game));
                status_.targetVisible = target.found;
                status_.targetAngleDegrees = static_cast<float>(target.angleDegrees);
                if (target.found)
                {
                    const float correction = std::clamp(-static_cast<float>(target.angleDegrees), -config_.combatAssist.steerDegreesPerTic,
                                                        config_.combatAssist.steerDegreesPerTic);
                    // Once a real target is in the native ray fan, let this
                    // explicitly-authored assist own the small turn command;
                    // adding it to an opposite brain turn can cancel the aid.
                    turnDegrees = std::clamp(correction, -controls.turn.limit, controls.turn.limit);
                }
            }
            const float wantedForwardUnits = forwardUnits;

            // Measure the command that actually ran during the previous game tic before
            // deciding what this result should do.  The old code compared the current
            // brain command with the current position, so a WallSense brake/reflex could
            // hide the push that caused the stall and feed the picture label one tic late.
            history_.push_back({player.x, player.y});
            while (history_.size() > controls.use.stuckTics + 1)
            {
                history_.erase(history_.begin());
            }
            const bool notMoving = history_.size() > controls.use.stuckTics &&
                                   std::hypot(history_.back().x - history_.front().x, history_.back().y - history_.front().y) < controls.use.stuckUnits;
            const bool previousPushing = lastCommandWantedForward_ > 3.0f && !lastCommandReflex_;
            const bool previousWalking = lastCommandForward_ > 3.0f && !lastCommandReflex_;
            // Keep the public wall metric/learning label comparable to the old
            // meaning: the command actually sent forward while not moving.
            // `previousPushing` remains the raw intent used only to trigger
            // escape when WallSense had already braked the command.
            const bool previousAtWall = previousWalking && notMoving && player.health > 0;
            const bool previousBlocked = notMoving && (previousPushing || previousWalking) && player.health > 0;

            // N4: our layer (not the connectome): label the picture that produced the
            // previous command with its measured collision outcome, then steer the new
            // command from the current picture.  Braking does not erase the original
            // forward intent used by the escape reflex.
            if (wallSense_.Enabled() && player.health > 0)
            {
                if (info.map != placeMap_ || info.episode != placeEpisode_)
                {
                    // a memory per WAD and map ("doom1 E1M1"): the layer learns the places of a map
                    placeMap_ = info.map;
                    placeEpisode_ = info.episode;
                    std::string wad = game.Iwad().stem().u8string();
                    std::transform(wad.begin(), wad.end(), wad.begin(), [](const char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
                    char place[96];
                    std::snprintf(place, sizeof(place), "%s E%dM%d", wad.c_str(), info.episode, info.map);
                    wallSense_.SetPlace(place);
                }
                const std::vector<float>& picture = commandColumns_.empty() ? lastColumns_ : commandColumns_;
                wallSense_.Tick(picture, previousAtWall, previousWalking);
                if (wallSense_.Brake() > 0.0f)
                {
                    forwardUnits = forwardUnits > 0.0f ? forwardUnits * (1.0f - wallSense_.Brake()) : forwardUnits;
                    turnDegrees = std::clamp(turnDegrees + wallSense_.TurnDegrees(), -controls.turn.limit, controls.turn.limit);
                }
            }

            // N5: the whiskers (authored local sensor, not the connectome): the walls of now around the fly, before it touches them
            status_.whiskersOn = config_.whiskers.enabled;
            if (config_.whiskers.enabled && player.health > 0)
            {
                const FlyPlayerConfig::Whiskers& w = config_.whiskers;
                static const std::vector<double> kAngles = {0.0, 30.0, -30.0, 60.0, -60.0};   // degrees from the view, DOOM's positive is left
                const std::vector<double> d = game.Whiskers(PlayerIndex(game), kAngles, w.range);
                const float ahead = static_cast<float>(d[0]);
                const float leftOpen = static_cast<float>(d[1] + d[3]);
                const float rightOpen = static_cast<float>(d[2] + d[4]);
                if (!whiskerActive_ && ahead < w.stopUnits)
                {
                    whiskerActive_ = true;
                    whiskerTics_ = 0;
                    whiskerSign_ = leftOpen > rightOpen ? -1.0f : 1.0f;   // to the more open side (turnDegrees: + is right)
                }
                else if (whiskerActive_ && (ahead >= w.clearUnits || whiskerTics_ >= w.maxTics))
                {
                    whiskerActive_ = false;
                }
                if (whiskerActive_)
                {
                    ++whiskerTics_;
                    forwardUnits = std::min(forwardUnits, 0.0f);
                    turnDegrees = whiskerSign_ * w.turnDegrees;
                }
                else
                {
                    if (ahead < w.slowUnits && forwardUnits > 0.0f)
                    {
                        forwardUnits *= std::clamp((ahead - w.stopUnits) / std::max(w.slowUnits - w.stopUnits, 1.0f), 0.25f, 1.0f);
                    }
                    if (d[1] < w.sideUnits)
                    {
                        turnDegrees += w.sideTurnDegrees;   // a wall close on the left: a little to the right
                    }
                    if (d[2] < w.sideUnits)
                    {
                        turnDegrees -= w.sideTurnDegrees;
                    }
                    turnDegrees = std::clamp(turnDegrees, -controls.turn.limit, controls.turn.limit);
                }
                status_.whiskerTurn = whiskerActive_;
                status_.whiskerAhead = ahead;
            }

            if (useCooldown_ > 0)
            {
                --useCooldown_;
            }
            if (previousBlocked && useCooldown_ == 0)
            {
                use = true;
                useCooldown_ = controls.use.everyTics;
            }
            // dead in a deathmatch: the use button brings the player back (the fly has nothing in the brain for that)
            if (player.health <= 0)
            {
                use = true;
                fire = false;
            }

            // N4: the metric of pushing against a wall, and its punishment (with learning)
            if (player.health > 0)
            {
                ++levelTics_;
            }
            if (previousAtWall)
            {
                ++wallTics_;
                if (!atWall_)
                {
                    ++wallHits_;
                    wallTicsNow_ = 0;
                }
                if (wallTicsNow_++ % std::max(1u, config_.learning.wallEveryTics) == 0)
                {
                    wallPunish_ = true;
                }
            }
            atWall_ = previousAtWall;

            // the reflex of the legs: pushing against a wall for a long time
            if (previousBlocked)
            {
                ++stuckCount_;
            }
            else if (reflexLeft_ == 0)
            {
                stuckCount_ = 0;
            }
            if (reflexLeft_ == 0 && sinceReflex_ < 1000000)
            {
                ++sinceReflex_;
            }
            if (controls.unstick.afterTics > 0 && reflexLeft_ == 0 && stuckCount_ >= controls.unstick.afterTics)
            {
                // N5: a new wall turns the other way than the last one; the same wall again soon after (the brain walked straight back into it)
                // keeps the side and turns further, so that the fly does not swing between the same two directions
                if (sinceReflex_ > kReflexRepeatTics)
                {
                    reflexSign_ = -reflexSign_;
                    reflexRepeat_ = 0;
                }
                else
                {
                    reflexRepeat_ = std::min(reflexRepeat_ + 1, 2u);
                }
                reflexLeft_ = controls.unstick.forTics + controls.unstick.forTics * reflexRepeat_ / 2;
                stuckCount_ = 0;
                sinceReflex_ = 0;
            }
            bool reflexCommand = false;
            if (reflexLeft_ > 0)
            {
                --reflexLeft_;
                turnDegrees = reflexSign_ * controls.unstick.turnDegrees;
                forwardUnits = -controls.unstick.backUnits;
                fire = false;
                reflexCommand = true;
            }
            if (config_.combatAssist.enabled && player.health > 0)
            {
                // Validate the shot along the yaw that DOOM will apply for this
                // command, after WallSense and leg escape have had their say.
                // A visible target alone is not enough: the final native ray
                // must still be aligned with the actual weapon direction.
                const doom::VisibleTarget firingTarget = game.FindVisibleTarget(PlayerIndex(game), -turnDegrees);
                status_.targetVisible = firingTarget.found;
                status_.targetAngleDegrees = static_cast<float>(firingTarget.angleDegrees);
                // The fan is useful for steering, but a side-ray hit is not a
                // proof that P_GunShot will hit. Actual fire requires the
                // center ray of the final weapon yaw exactly.
                fire = !reflexCommand && firingTarget.found && std::fabs(static_cast<float>(firingTarget.angleDegrees)) < 0.001f;
            }
            lastCommandWantedForward_ = wantedForwardUnits;
            lastCommandForward_ = forwardUnits;
            lastCommandReflex_ = reflexCommand;
            commandColumns_ = lastColumns_;
        }
        else
        {
            history_.clear();
            useCooldown_ = 0;
            commandColumns_.clear();
            whiskerActive_ = false;
            lastCommandWantedForward_ = 0.0f;
            lastCommandForward_ = 0.0f;
            lastCommandReflex_ = false;
            status_.brainFire = false;
            status_.targetVisible = false;
            status_.authoredCombatAssist = false;
            status_.targetAngleDegrees = 0.0f;
        }

        // the screens that wait for a key: the title, the intermission, the finale (the player of DOOM has no keyboard)
        if (!config_.drivesScreens)
        {
        }
        else if (!inLevel && info.state != doom::GameState::Level)
        {
            if (screenCooldown_ > 0)
            {
                --screenCooldown_;
            }
            else
            {
                if (info.state == doom::GameState::DemoScreen && !info.menuActive)
                {
                    game.PostKey(doom::key::kEscape, true);
                    game.PostKey(doom::key::kEscape, false);
                }
                else if ((info.state == doom::GameState::Intermission || info.state == doom::GameState::Finale) && !info.menuActive)
                {
                    // WI/F_Ticker read BT_USE/BT_ATTACK from the tic command; PostKey(Enter) is not visible to a FlyPlayer source.
                    use = true;
                }
                else
                {
                    game.PostKey(doom::key::kEnter, true);
                    game.PostKey(doom::key::kEnter, false);
                }
                screenCooldown_ = 14;
            }
        }
        else if (info.menuActive)
        {
            // the menu of DOOM opened by itself (a key of the screens above): New Game and the defaults of the episode and the skill
            if (screenCooldown_ > 0)
            {
                --screenCooldown_;
            }
            else
            {
                game.PostKey(doom::key::kEnter, true);
                game.PostKey(doom::key::kEnter, false);
                screenCooldown_ = 14;
            }
        }

        const float unitsPerDegree = 65536.0f / 360.0f;
        cmd.forwardMove = static_cast<std::int8_t>(std::lround(std::clamp(forwardUnits, -50.0f, 50.0f)));
        cmd.angleTurn = static_cast<std::int16_t>(std::lround(std::clamp(-turnDegrees * unitsPerDegree, -32000.0f, 32000.0f)));   // DOOM turns left with a positive angle
        cmd.buttons = static_cast<std::uint8_t>((fire ? doom::kButtonAttack : 0) | (use ? doom::kButtonUse : 0));
        command_ = cmd;
        commandReady_ = true;

        status_.turnHz = result.turnHz;
        status_.forwardHz = result.forwardHz;
        status_.fireHz = result.fireHz;
        status_.turnDegrees = turnDegrees;
        status_.forwardUnits = forwardUnits;
        status_.fire = fire;
        status_.use = use;
        status_.reflex = lastCommandReflex_ || reflexLeft_ > 0 || use;
        status_.spikesPerSecond = result.spikesPerSecond;
        status_.centralSpikesPerSecond = result.centralSpikesPerSecond;
        status_.dopamineMv = result.dopamineMv;
        status_.kcSpikesPerSecond = result.kcSpikesPerSecond;
        status_.wallSense = wallSense_.Enabled();
        status_.wallChance = wallSense_.Chance();
        status_.wallBrake = wallSense_.Brake();
        status_.wallPositives = wallSense_.Positives();
        if (result.learnedScale >= 0.0)
        {
            status_.learnedScale = result.learnedScale;
        }
        status_.rates = result.rates;
        snapshot_ = result.snapshot;
        feed_ = result.feed;
        const double speed = result.wallSeconds > 1e-6 ? kTicSeconds / result.wallSeconds : 0.0;
        speedSmoothed_ = speedSmoothed_ == 0.0 ? speed : speedSmoothed_ * 0.9 + speed * 0.1;
    }

    void FlyPlayer::Shape(const TickResult& result, float& turnDegrees, float& forwardUnits, bool& fire) const
    {
        const FlyControls& controls = config_.controls;
        turnDegrees = Shaped(result.turnHz, controls.turn);
        forwardUnits = Shaped(result.forwardHz, controls.forward);

        fire = result.fireSpikes || (controls.fire.thresholdHz > 0.0f && result.fireHz >= controls.fire.thresholdHz);
    }

    FlyStatus FlyPlayer::EvaluateSequenceForTest(const std::function<void(int, std::vector<uint32_t>&)>& makeFrame, const int tics, std::vector<FlyStatus>* perTic)
    {
        FlyStatus last;
        std::vector<uint32_t> frame(static_cast<size_t>(doom::kScreenWidth) * doom::kScreenHeight);
        for (int tic = 0; tic < tics; ++tic)
        {
            makeFrame(tic, frame);
            last = EvaluateFrameForTest(frame.data(), 1, false);
            if (perTic != nullptr)
            {
                perTic->push_back(last);
            }
        }
        return last;
    }

    FlyStatus FlyPlayer::EvaluateFrameForTest(const uint32_t* frame, const int tics, const bool damageAtFirst)
    {
        FlyStatus last;
        for (int tic = 0; tic < tics; ++tic)
        {
            TickInput input;
            eyes_.SampleLuminance(frame, doom::kScreenWidth, doom::kScreenHeight, input.luminance);
            eyes_.SampleColumns(frame, doom::kScreenWidth, doom::kScreenHeight, input.columnLuminance);
            lastLuminance_ = input.luminance;
            input.damage = damageAtFirst && tic == 0;
            input.reset = needReset_;
            needReset_ = false;
            Submit(input);
            TickResult result;
            WaitResult(result, true);
            if (result.seizure)
            {
                ++seizures_;
                ++resets_;
            }
            if (input.damage)
            {
                ++damages_;
            }
            bool fire = false;
            Shape(result, status_.turnDegrees, status_.forwardUnits, fire);
            status_.fire = fire;
            status_.turnHz = result.turnHz;
            status_.forwardHz = result.forwardHz;
            status_.fireHz = result.fireHz;
            status_.spikesPerSecond = result.spikesPerSecond;
            status_.centralSpikesPerSecond = result.centralSpikesPerSecond;
            status_.dopamineMv = result.dopamineMv;
            status_.kcSpikesPerSecond = result.kcSpikesPerSecond;
            if (result.learnedScale >= 0.0)
            {
                status_.learnedScale = result.learnedScale;
            }
            status_.rates = result.rates;
            snapshot_ = result.snapshot;
            feed_ = result.feed;
            ++tics_;
            last = Status();
        }
        return last;
    }

    void FlyPlayer::Attach(doom::DoomGame& game)
    {
        game.SetTicCmdSource(this);
    }

    void FlyPlayer::Detach(doom::DoomGame& game)
    {
        game.SetTicCmdSource(nullptr);
    }

    void FlyPlayer::BuildTicCmd(const int player, doom::TicCmd& cmd)
    {
        if (player == (config_.playerIndex >= 0 ? config_.playerIndex : 0))
        {
            cmd = command_;
        }
    }

    bool FlyPlayer::StepBlocking(doom::DoomGame& game)
    {
        Submit(SampleInput(game));
        TickResult result;
        WaitResult(result, true);
        ApplyResult(game, result);
        const bool frame = game.StepTic();
        commandReady_ = false;
        ++tics_;
        // the speed of the game when a host steps it as fast as the brain goes (Fly learns, MAX): the tics of every second of the clock (the tics come in bursts,
        // one burst a frame, so the interval between two of them says nothing)
        const auto now = std::chrono::steady_clock::now();
        if (stepWindowTics_++ == 0)
        {
            stepWindowStart_ = now;
        }
        const double window = std::chrono::duration<double>(now - stepWindowStart_).count();
        if (window >= 1.0)
        {
            ticIntervalSmoothed_ = window / static_cast<double>(stepWindowTics_ - 1);
            stepWindowTics_ = 0;
        }
        return frame;
    }

    void FlyPlayer::Begin(doom::DoomGame& game, const double wallSeconds)
    {
        sinceSubmit_ = std::min(sinceSubmit_ + wallSeconds, 4.0 * kTicSeconds);
        wallSinceTic_ += wallSeconds;
        {
            // the brains that were made again: the thread of the game has taken the feed of the new one by now
            std::vector<std::unique_ptr<flybrain::FlyBrain>> retired;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                retired.swap(retired_);
            }
        }
        if (!pending_ && !commandReady_ && (!config_.pace || sinceSubmit_ >= kTicSeconds))
        {
            Submit(SampleInput(game));
            sinceSubmit_ = std::max(0.0, sinceSubmit_ - kTicSeconds);
        }
    }

    bool FlyPlayer::Poll(doom::DoomGame& game)
    {
        TickResult result;
        if (pending_ && WaitResult(result, false))
        {
            ApplyResult(game, result);
        }
        waiting_ = pending_;
        return commandReady_;
    }

    void FlyPlayer::EndTic(doom::DoomGame& game)
    {
        commandReady_ = false;
        ++tics_;
        ticIntervalSmoothed_ = ticIntervalSmoothed_ == 0.0 ? wallSinceTic_ : ticIntervalSmoothed_ * 0.9 + wallSinceTic_ * 0.1;
        wallSinceTic_ = 0.0;
        if (!pending_ && (!config_.pace || sinceSubmit_ >= kTicSeconds))
        {
            Submit(SampleInput(game));
            sinceSubmit_ = std::max(0.0, sinceSubmit_ - kTicSeconds);
        }
    }

    bool FlyPlayer::Pump(doom::DoomGame& game, const double wallSeconds)
    {
        Begin(game, wallSeconds);
        bool frame = false;
        if (Poll(game))
        {
            frame = game.StepTic();
            EndTic(game);
        }
        return frame;
    }

    FlyStatus FlyPlayer::Status() const
    {
        FlyStatus status = status_;
        status.tics = tics_;
        status.brainSpeed = speedSmoothed_;
        status.gameSpeed = ticIntervalSmoothed_ > 0.0 ? kTicSeconds / ticIntervalSmoothed_ : 0.0;
        status.waiting = waiting_;
        status.resets = resets_;
        status.seizures = seizures_;
        status.damages = damages_;
        status.wallHits = wallHits_;
        status.wallTics = wallTics_;
        status.levelTics = levelTics_;
        status.controlsName = config_.controls.name;
        status.backend = backendText_;
        return status;
    }

    BrainSnapshot FlyPlayer::Snapshot() const
    {
        return snapshot_;
    }
}
