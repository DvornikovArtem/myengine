// FlyPlayer.cpp

#include <doomfly/FlyPlayer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

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
            !resolve(config_.controls.fire.terms, fireGroups_, "fire"))
        {
            return false;
        }
        dopamine_ = connectome.Find("PPL101");
        if (dopamine_.Empty())
        {
            error = "the connectome has no PPL101 (the dopamine of the pain)";
            return false;
        }
        rewardGroup_ = connectome.FindPrefix("PAM");

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
    }

    void FlyPlayer::MakeBrain()
    {
        flybrain::ModelParams params;
        params.seed = config_.seed;
        brain_ = std::make_unique<flybrain::FlyBrain>(config_.connectome, params);
        brain_->SetParallel(config_.parallel);
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
        if (input.damage)
        {
            brain_->StimulateCurrent(dopamine_, config_.damageMv, config_.damageMs);
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
        for (const Group& group : fireGroups_)
        {
            if (config_.controls.fire.minSpikes > 0 && brain_->SpikeCount(group.watch, config_.controls.fire.windowMs) >= config_.controls.fire.minSpikes)
            {
                result.fireSpikes = true;
            }
        }
        for (const std::vector<Group>* groups : {&turnGroups_, &forwardGroups_, &fireGroups_})
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
        result.dopamineMv = brain_->TimeMs() < dopamineUntilMs_ ? config_.damageMv : 0.0f;

        const uint64_t total = brain_->TotalSpikes();
        result.spikesPerSecond = static_cast<float>(static_cast<double>(total - lastSpikes_) / (kTicMs / 1000.0));
        lastSpikes_ = total;
        if (result.spikesPerSecond > config_.seizureSpikesPerSecond)
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
        result.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        result.valid = true;
        return result;
    }

    void FlyPlayer::WorkerLoop()
    {
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
            TickResult result = RunTick(input);
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
            done_.wait(lock, [this] { return result_.valid; });
        }
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
        const doom::PlayerState player = game.Player(info.consolePlayer);
        eyes_.SampleLuminance(game.Pixels(), doom::kScreenWidth, doom::kScreenHeight, input.luminance);
        lastLuminance_ = input.luminance;

        if (player.inGame)
        {
            input.damage = player.health < prevHealth_;
            if (input.damage)
            {
                ++damages_;
            }
            input.reward = config_.reward && (player.kills > prevKills_ || player.items > prevItems_);
            const bool died = prevHealth_ <= 0 && player.health > 0;
            const bool newLevel = prevMap_ >= 0 && (info.map != prevMap_ || info.episode != prevEpisode_);
            if (died || newLevel)
            {
                needReset_ = true;
            }
            prevHealth_ = player.health;
            prevKills_ = player.kills;
            prevItems_ = player.items;
        }
        prevMap_ = info.map;
        prevEpisode_ = info.episode;
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
        const doom::PlayerState player = game.Player(info.consolePlayer);
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

            // stuck at a door: pushing and not moving
            history_.push_back({player.x, player.y});
            while (history_.size() > controls.use.stuckTics + 1)
            {
                history_.erase(history_.begin());
            }
            const bool pushing = forwardUnits > 3.0f;
            const bool notMoving = history_.size() > controls.use.stuckTics &&
                                   std::hypot(history_.back().x - history_.front().x, history_.back().y - history_.front().y) < controls.use.stuckUnits;
            if (useCooldown_ > 0)
            {
                --useCooldown_;
            }
            if (pushing && notMoving && useCooldown_ == 0)
            {
                use = true;
                useCooldown_ = controls.use.everyTics;
            }

            // the reflex of the legs: pushing against a wall for a long time
            if (pushing && notMoving)
            {
                ++stuckCount_;
            }
            else if (reflexLeft_ == 0)
            {
                stuckCount_ = 0;
            }
            if (controls.unstick.afterTics > 0 && reflexLeft_ == 0 && stuckCount_ >= controls.unstick.afterTics)
            {
                reflexLeft_ = controls.unstick.forTics;
                reflexSign_ = -reflexSign_;
                stuckCount_ = 0;
            }
            if (reflexLeft_ > 0)
            {
                --reflexLeft_;
                turnDegrees = reflexSign_ * controls.unstick.turnDegrees;
                forwardUnits = -controls.unstick.backUnits;
                fire = false;
            }
        }
        else
        {
            history_.clear();
            useCooldown_ = 0;
        }

        // the screens that wait for a key: the title, the intermission, the finale (the player of DOOM has no keyboard)
        if (!inLevel && info.state != doom::GameState::Level)
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

        status_.turnHz = result.turnHz;
        status_.forwardHz = result.forwardHz;
        status_.fireHz = result.fireHz;
        status_.turnDegrees = turnDegrees;
        status_.forwardUnits = forwardUnits;
        status_.fire = fire;
        status_.use = use;
        status_.spikesPerSecond = result.spikesPerSecond;
        status_.dopamineMv = result.dopamineMv;
        status_.rates = result.rates;
        snapshot_ = result.snapshot;
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

    FlyStatus FlyPlayer::EvaluateFrameForTest(const uint32_t* frame, const int tics, const bool damageAtFirst)
    {
        FlyStatus last;
        for (int tic = 0; tic < tics; ++tic)
        {
            TickInput input;
            eyes_.SampleLuminance(frame, doom::kScreenWidth, doom::kScreenHeight, input.luminance);
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
            status_.dopamineMv = result.dopamineMv;
            status_.rates = result.rates;
            snapshot_ = result.snapshot;
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
        if (player == 0)
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
        ++tics_;
        return frame;
    }

    bool FlyPlayer::Pump(doom::DoomGame& game, const double wallSeconds)
    {
        sinceSubmit_ = std::min(sinceSubmit_ + wallSeconds, 4.0 * kTicSeconds);
        wallSinceTic_ += wallSeconds;
        bool frame = false;
        auto submitIfDue = [&]()
        {
            if (!pending_ && (!config_.pace || sinceSubmit_ >= kTicSeconds))
            {
                Submit(SampleInput(game));
                sinceSubmit_ = std::max(0.0, sinceSubmit_ - kTicSeconds);
            }
        };
        submitIfDue();
        TickResult result;
        if (pending_ && WaitResult(result, false))
        {
            ApplyResult(game, result);
            frame = game.StepTic();
            ++tics_;
            ticIntervalSmoothed_ = ticIntervalSmoothed_ == 0.0 ? wallSinceTic_ : ticIntervalSmoothed_ * 0.9 + wallSinceTic_ * 0.1;
            wallSinceTic_ = 0.0;
            submitIfDue();
        }
        waiting_ = pending_;
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
        status.controlsName = config_.controls.name;
        return status;
    }

    BrainSnapshot FlyPlayer::Snapshot() const
    {
        return snapshot_;
    }
}
