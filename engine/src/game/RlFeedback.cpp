#include <myengine/game/RlFeedback.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <rl/RlMatch.h>

#include <myengine/audio/AudioSystem.h>

#include <tracy/Tracy.hpp>

namespace myengine::game
{
    namespace
    {
        using rl::RlVec3;

        // The windows of a kind, in ticks (120 per second): inside it the same event of the same car / pad is the same event
        constexpr std::uint32_t kHitWindow = 12;
        constexpr std::uint32_t kBounceWindow = 14;
        constexpr std::uint32_t kJumpWindow = 30;
        constexpr std::uint32_t kLandWindow = 36;
        constexpr std::uint32_t kDemolishWindow = 360;
        constexpr std::uint32_t kBumpWindow = 60;
        constexpr std::uint32_t kPadWindow = 120;
        constexpr std::uint32_t kGaspWindow = 720;
        constexpr std::uint32_t kBoostTail = 8;  // ticks the boost keeps sounding after the amount stopped falling

        // What counts as an event (m/s of the change of a speed inside one tick)
        constexpr float kJumpImpulse = 1.5f;
        constexpr float kHitMinChange = 1.0f;
        constexpr float kHitFullChange = 25.0f;
        constexpr float kBounceMinChange = 3.0f;
        constexpr float kLandMinSpeed = 1.2f;
        constexpr float kLandFullSpeed = 9.0f;
        constexpr float kBumpMinChange = 3.0f;
        constexpr float kBumpDistance = 3.0f;

        constexpr float kTeamTint[2][4] = {{0.25f, 0.55f, 1.0f, 1.0f}, {1.0f, 0.55f, 0.15f, 1.0f}};

        RlVec3 Add(const RlVec3& a, const RlVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        RlVec3 Sub(const RlVec3& a, const RlVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        RlVec3 Scale(const RlVec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float Dot(const RlVec3& a, const RlVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        float Length(const RlVec3& a) { return std::sqrt(Dot(a, a)); }
        float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

        RlVec3 Normalized(const RlVec3& a, const RlVec3& fallback = {0.0f, 1.0f, 0.0f})
        {
            const float length = Length(a);
            return length > 1e-5f ? Scale(a, 1.0f / length) : fallback;
        }

        RlVec3 Lerp(const RlVec3& a, const RlVec3& b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }

        // v rotated by the unit quaternion q
        RlVec3 Rotate(const rl::RlQuat& q, const RlVec3& v)
        {
            const RlVec3 u{q.x, q.y, q.z};
            const RlVec3 t = Scale({u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x}, 2.0f);
            const RlVec3 cross{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
            return Add(Add(v, Scale(t, q.w)), cross);
        }

        audio::Vec3 ToAudio(const RlVec3& v) { return {v.x, v.y, v.z}; }
        render::Float3 ToFloat3(const RlVec3& v) { return {v.x, v.y, v.z}; }

        std::uint32_t KeyOf(RlFeedbackKind kind, std::uint32_t key) { return (static_cast<std::uint32_t>(kind) << 24) | (key & 0xFFFFFFu); }
    }

    const char* RlFeedbackKindName(const RlFeedbackKind kind)
    {
        static const char* const names[] = {"CountdownBeep", "KickoffGo", "Goal", "OvertimeStart", "MatchEnd", "BallHit", "BallBounce", "Jump",
                                            "DoubleJump", "Dodge", "Land", "PadPickup", "Demolish", "Bump", "CrowdGasp"};
        const int index = static_cast<int>(kind);
        return index >= 0 && index < static_cast<int>(RlFeedbackKind::Count) ? names[index] : "?";
    }

    std::string FormatFeedbackEvent(const RlFeedbackEvent& event)
    {
        char text[256];
        std::snprintf(text, sizeof(text), "tick %6u (%7.2f s)  %-13s slot %2d  team %2d  number %3d  strength %.2f  at (%.1f, %.1f, %.1f)", event.tick,
                      static_cast<double>(event.tick) * static_cast<double>(rl::kTickDt), RlFeedbackKindName(event.kind), event.slot, event.team, event.number,
                      static_cast<double>(event.strength), static_cast<double>(event.position.x), static_cast<double>(event.position.y),
                      static_cast<double>(event.position.z));
        return text;
    }

    RlFeedback::RlFeedback(const rl::RlTuning& tuning, audio::AudioSystem* audio, fx::ParticleSystem* particles)
        : tuning_(tuning), audio_(audio), particles_(particles)
    {
    }

    RlFeedback::~RlFeedback()
    {
        StopLoops(0.0f);
        StopMenuMusic(0.0f);
    }

    void RlFeedback::SetOwnSlot(const int ownSlot) { ownSlot_ = ownSlot; }
    void RlFeedback::SetTuning(const rl::RlTuning& tuning) { tuning_ = tuning; }

    void RlFeedback::StartMatch(const int ownSlot)
    {
        StopLoops(0.0f);
        StopMenuMusic(0.5f);
        ownSlot_ = ownSlot;
        haveTick_ = false;
        lastTick_ = 0;
        lastByKey_.clear();
        announced_.clear();
        stats_ = {};
        journal_.clear();
        for (CarLoops& loops : loops_)
        {
            loops = {};
        }
        if (audio_ != nullptr)
        {
            crowd_ = audio_->PlayEvent("crowd_ambience");
        }
    }

    void RlFeedback::EndMatch() { StopLoops(0.4f); }

    void RlFeedback::StopLoops(const float fadeSeconds)
    {
        for (CarLoops& loops : loops_)
        {
            if (audio_ != nullptr)
            {
                audio_->Stop(loops.engine, fadeSeconds);
                audio_->Stop(loops.boost, fadeSeconds);
                audio_->Stop(loops.skid, fadeSeconds);
            }
            if (particles_ != nullptr)
            {
                particles_->Stop(loops.boostEffect);
                particles_->Stop(loops.supersonicEffect);
            }
            loops = {};
        }
        if (audio_ != nullptr)
        {
            audio_->Stop(crowd_, fadeSeconds);
        }
        if (particles_ != nullptr)
        {
            particles_->Stop(ballTrail_);
        }
        crowd_ = {};
        ballTrail_ = {};
    }

    void RlFeedback::SetVolumes(const float master, const float sfx, const float music, const float ui)
    {
        if (audio_ == nullptr)
        {
            return;
        }
        audio_->SetBusVolume(audio::Bus::Master, std::clamp(master, 0.0f, 1.0f));
        audio_->SetBusVolume(audio::Bus::Sfx, std::clamp(sfx, 0.0f, 1.0f));
        audio_->SetBusVolume(audio::Bus::Music, std::clamp(music, 0.0f, 1.0f));
        audio_->SetBusVolume(audio::Bus::Ui, std::clamp(ui, 0.0f, 1.0f));
    }

    void RlFeedback::PlayMenuMusic()
    {
        if (audio_ == nullptr || audio_->IsPlaying(menuMusic_))
        {
            return;
        }
        menuMusic_ = audio_->PlayEvent("menu_music");
    }

    void RlFeedback::StopMenuMusic(const float fadeSeconds)
    {
        if (audio_ != nullptr)
        {
            audio_->Stop(menuMusic_, fadeSeconds);
        }
        menuMusic_ = {};
    }

    bool RlFeedback::MenuMusicPlaying() const { return audio_ != nullptr && audio_->IsPlaying(menuMusic_); }

    int RlFeedback::TeamOf(const rl::RlWorldState& world, const int slot) const
    {
        const int mode = world.match.mode;
        return mode <= 0 ? 0 : (slot < mode ? 0 : 1);
    }

    bool RlFeedback::Fresh(const RlFeedbackKind kind, const std::uint32_t key, const std::uint32_t tick, const std::uint32_t window)
    {
        const std::uint32_t id = KeyOf(kind, key);
        const auto found = lastByKey_.find(id);
        if (found != lastByKey_.end() && (tick <= found->second || tick - found->second < window))
        {
            ++stats_.suppressed;
            return false;
        }
        lastByKey_[id] = tick;
        return true;
    }

    bool RlFeedback::FreshId(const RlFeedbackKind kind, const std::uint32_t id)
    {
        const std::uint64_t key = (static_cast<std::uint64_t>(kind) << 40) | id;
        if (!announced_.insert(key).second)
        {
            ++stats_.suppressed;
            return false;
        }
        return true;
    }

    void RlFeedback::Emit(const RlFeedbackEvent& event)
    {
        ++stats_.events[static_cast<int>(event.kind)];
        if (journalOn_)
        {
            journal_.push_back(event);
        }
        Play(event);
    }

    void RlFeedback::OnTick(const rl::RlWorldState& previous, const rl::RlWorldState& current)
    {
        ZoneScopedN("RlFeedback::OnTick");
        ++stats_.ticks;
        if (haveTick_ && current.tick <= lastTick_)
        {
            ++stats_.rollbacks;
        }
        haveTick_ = true;
        lastTick_ = std::max(lastTick_, current.tick);

        // After a restore of an old tick `previous` may still be the state from before it: nothing can be read from such a pair
        if (previous.tick >= current.tick)
        {
            return;
        }

        DetectMatch(previous, current);
        DetectCars(previous, current);
        DetectBall(previous, current);
        DetectPads(previous, current);
    }

    void RlFeedback::DetectMatch(const rl::RlWorldState& previous, const rl::RlWorldState& current)
    {
        const rl::RlMatchState& a = previous.match;
        const rl::RlMatchState& b = current.match;
        RlFeedbackEvent event;
        event.tick = current.tick;

        // the countdown: 3, 2, 1 (the number the clock is in, once per kick-off), then the start
        if (b.phase == rl::RlMatchPhase::Countdown)
        {
            const int number = static_cast<int>(std::ceil(b.phaseTimer - 1e-4f));
            if (number >= 1 && number <= 3 && FreshId(RlFeedbackKind::CountdownBeep, b.kickoffs * 8u + static_cast<std::uint32_t>(number)))
            {
                event.kind = RlFeedbackKind::CountdownBeep;
                event.number = number;
                Emit(event);
            }
        }
        if (a.phase == rl::RlMatchPhase::Countdown && b.phase == rl::RlMatchPhase::Playing && FreshId(RlFeedbackKind::KickoffGo, b.kickoffs))
        {
            event = {};
            event.tick = current.tick;
            event.kind = RlFeedbackKind::KickoffGo;
            Emit(event);
        }

        // a goal: the sum of the scores went up (the id is the sum, so a replayed tick finds it announced)
        const int scoreBefore = a.score[0] + a.score[1];
        const int scoreNow = b.score[0] + b.score[1];
        if (scoreNow > scoreBefore && FreshId(RlFeedbackKind::Goal, static_cast<std::uint32_t>(scoreNow)))
        {
            const float side = b.lastGoalTeam == 0 ? 1.0f : -1.0f;  // blue attacks +Z
            event = {};
            event.tick = current.tick;
            event.kind = RlFeedbackKind::Goal;
            event.team = b.lastGoalTeam;
            event.slot = b.lastGoalScorer;
            event.number = scoreNow;
            event.position = {std::clamp(current.ball.position.x, -tuning_.arena.goalHalfWidth, tuning_.arena.goalHalfWidth),
                              std::clamp(current.ball.position.y, 1.0f, tuning_.arena.goalHeight),
                              side * (tuning_.arena.halfLength + 1.0f)};
            event.direction = {0.0f, 1.0f, 0.0f};
            event.strength = 1.0f;
            Emit(event);
        }

        if (a.overtime == 0 && b.overtime != 0 && FreshId(RlFeedbackKind::OvertimeStart, 1))
        {
            event = {};
            event.tick = current.tick;
            event.kind = RlFeedbackKind::OvertimeStart;
            Emit(event);
        }
        if (a.phase != rl::RlMatchPhase::Finished && b.phase == rl::RlMatchPhase::Finished && FreshId(RlFeedbackKind::MatchEnd, 1))
        {
            event = {};
            event.tick = current.tick;
            event.kind = RlFeedbackKind::MatchEnd;
            event.team = b.winner;
            Emit(event);
        }

        // the stands: the ball runs fast at a goal that is near (once in six seconds)
        if (b.phase == rl::RlMatchPhase::Playing)
        {
            const RlVec3& position = current.ball.position;
            const RlVec3& velocity = current.ball.linearVelocity;
            const float toGoal = (position.z >= 0.0f ? 1.0f : -1.0f) * velocity.z;
            const float distance = tuning_.arena.halfLength - std::fabs(position.z);
            if (toGoal > 14.0f && distance < 24.0f && std::fabs(position.x) < tuning_.arena.goalHalfWidth + 8.0f && position.y < tuning_.arena.goalHeight + 6.0f &&
                Fresh(RlFeedbackKind::CrowdGasp, 0, current.tick, kGaspWindow))
            {
                event = {};
                event.tick = current.tick;
                event.kind = RlFeedbackKind::CrowdGasp;
                event.position = position;
                event.strength = Clamp01(toGoal / 40.0f);
                Emit(event);
            }
        }
    }

    void RlFeedback::DetectCars(const rl::RlWorldState& previous, const rl::RlWorldState& current)
    {
        const int count = std::min(current.carCount, rl::kMaxCars);
        for (int i = 0; i < count; ++i)
        {
            const rl::RlCarState& a = previous.cars[i];
            const rl::RlCarState& b = current.cars[i];
            RlFeedbackEvent event;
            event.tick = current.tick;
            event.slot = i;
            event.team = TeamOf(current, i);

            if (!a.demolished && b.demolished && Fresh(RlFeedbackKind::Demolish, static_cast<std::uint32_t>(i), current.tick, kDemolishWindow))
            {
                event.kind = RlFeedbackKind::Demolish;
                event.position = a.position;  // the demolished car is parked away at once
                event.strength = 1.0f;
                Emit(event);
            }
            if (b.demolished || a.demolished)
            {
                continue;
            }

            const RlVec3 change = Sub(b.linearVelocity, a.linearVelocity);
            const float changeLength = Length(change);
            event.position = b.position;

            // the first jump: the car leaves with a push of its own
            if (!a.jumped && b.jumped && changeLength >= kJumpImpulse && Fresh(RlFeedbackKind::Jump, static_cast<std::uint32_t>(i), current.tick, kJumpWindow))
            {
                event.kind = RlFeedbackKind::Jump;
                Emit(event);
            }
            // the second one: a dodge (the flip spin starts) or a double jump (a second push); a timed out window changes neither
            else if (a.canDodge && !b.canDodge)
            {
                if (b.dodging && !a.dodging && Fresh(RlFeedbackKind::Dodge, static_cast<std::uint32_t>(i), current.tick, kJumpWindow))
                {
                    event.kind = RlFeedbackKind::Dodge;
                    event.direction = Normalized(Rotate(b.rotation, {0.0f, 0.0f, 1.0f}));
                    Emit(event);
                }
                else if (!b.dodging && changeLength >= kJumpImpulse && Fresh(RlFeedbackKind::DoubleJump, static_cast<std::uint32_t>(i), current.tick, kJumpWindow))
                {
                    event.kind = RlFeedbackKind::DoubleJump;
                    Emit(event);
                }
            }

            // touchdown: the speed that went into the surface
            if (!a.onGround && b.onGround)
            {
                const float into = std::max(0.0f, -Dot(a.linearVelocity, Normalized(b.surfaceNormal)));
                if (into >= kLandMinSpeed && Fresh(RlFeedbackKind::Land, static_cast<std::uint32_t>(i), current.tick, kLandWindow))
                {
                    event.kind = RlFeedbackKind::Land;
                    event.strength = Clamp01((into - kLandMinSpeed) / (kLandFullSpeed - kLandMinSpeed));
                    event.direction = Normalized(b.surfaceNormal);
                    Emit(event);
                }
            }

            // a bump: a shove of the speed with another car close by (the ball and the floor are other events)
            if (changeLength >= kBumpMinChange && a.onGround == b.onGround)
            {
                for (int j = 0; j < count; ++j)
                {
                    if (j == i || current.cars[j].demolished)
                    {
                        continue;
                    }
                    if (Length(Sub(current.cars[j].position, b.position)) < kBumpDistance)
                    {
                        const std::uint32_t pair = static_cast<std::uint32_t>(std::min(i, j) * 16 + std::max(i, j));
                        if (Fresh(RlFeedbackKind::Bump, pair, current.tick, kBumpWindow))
                        {
                            event.kind = RlFeedbackKind::Bump;
                            event.strength = Clamp01(changeLength / 15.0f);
                            event.position = Lerp(b.position, current.cars[j].position, 0.5f);
                            Emit(event);
                        }
                        break;
                    }
                }
            }

            // the boost burns while the amount falls (the loop reads it in Frame)
            if (b.boost < a.boost - 0.005f)
            {
                loops_[i].boostUntilTick = current.tick + kBoostTail;
            }
        }
    }

    void RlFeedback::DetectBall(const rl::RlWorldState& previous, const rl::RlWorldState& current)
    {
        const RlVec3 change = Sub(current.ball.linearVelocity, previous.ball.linearVelocity);
        const float changeLength = Length(change);
        bool touched = false;

        for (int slot = 0; slot < std::min(current.carCount, rl::kMaxCars); ++slot)
        {
            if (current.match.touches[slot] <= previous.match.touches[slot])
            {
                continue;
            }
            touched = true;
            if (changeLength < kHitMinChange || !Fresh(RlFeedbackKind::BallHit, static_cast<std::uint32_t>(slot), current.tick, kHitWindow))
            {
                continue;
            }
            const RlVec3 normal = Normalized(Sub(current.ball.position, current.cars[slot].position));
            RlFeedbackEvent event;
            event.tick = current.tick;
            event.kind = RlFeedbackKind::BallHit;
            event.slot = slot;
            event.team = TeamOf(current, slot);
            event.strength = Clamp01(changeLength / kHitFullChange);
            event.position = Sub(current.ball.position, Scale(normal, tuning_.ball.radius));
            event.direction = normal;
            Emit(event);
        }

        // off the floor, a wall or the ceiling: a change of the velocity that no car made
        if (!touched && changeLength >= kBounceMinChange && Fresh(RlFeedbackKind::BallBounce, 0, current.tick, kBounceWindow))
        {
            const RlVec3 normal = Normalized(change);
            RlFeedbackEvent event;
            event.tick = current.tick;
            event.kind = RlFeedbackKind::BallBounce;
            event.team = -1;
            event.strength = Clamp01(changeLength / 30.0f);
            event.position = Sub(current.ball.position, Scale(normal, tuning_.ball.radius));
            event.direction = normal;
            Emit(event);
        }
    }

    void RlFeedback::DetectPads(const rl::RlWorldState& previous, const rl::RlWorldState& current)
    {
        for (int i = 0; i < rl::kPadCount; ++i)
        {
            if (!(previous.match.pads[i].respawnLeft <= 0.0f && current.match.pads[i].respawnLeft > 0.0f))
            {
                continue;
            }
            if (!Fresh(RlFeedbackKind::PadPickup, static_cast<std::uint32_t>(i), current.tick, kPadWindow))
            {
                continue;
            }
            float x = 0.0f;
            float z = 0.0f;
            const bool big = rl::RlPadSpot(i, x, z);
            RlFeedbackEvent event;
            event.tick = current.tick;
            event.kind = RlFeedbackKind::PadPickup;
            event.number = i;
            event.strength = big ? 1.0f : 0.0f;
            event.position = {x, 0.3f, z};
            // who took it: the nearest car
            float best = 1e9f;
            for (int slot = 0; slot < std::min(current.carCount, rl::kMaxCars); ++slot)
            {
                const float dx = current.cars[slot].position.x - x;
                const float dz = current.cars[slot].position.z - z;
                const float distance = dx * dx + dz * dz;
                if (distance < best)
                {
                    best = distance;
                    event.slot = slot;
                }
            }
            event.team = event.slot >= 0 ? TeamOf(current, event.slot) : -1;
            Emit(event);
        }
    }

    void RlFeedback::Play(const RlFeedbackEvent& event)
    {
        const bool own = event.slot >= 0 && event.slot == ownSlot_;
        audio::EventParams sound;
        sound.hasPosition = !own;
        sound.position = ToAudio(event.position);

        fx::SpawnParams effect;
        effect.position = ToFloat3(event.position);
        effect.direction = ToFloat3(event.direction);
        const float* tint = event.team >= 0 && event.team <= 1 ? kTeamTint[event.team] : nullptr;

        const auto playSound = [&](const char* name, const audio::EventParams& params)
        {
            if (audio_ != nullptr)
            {
                audio_->PlayEvent(name, params);
            }
        };
        const auto spawn = [&](const char* name, const fx::SpawnParams& params)
        {
            if (particles_ != nullptr)
            {
                particles_->Spawn(name, params);
            }
        };

        switch (event.kind)
        {
        case RlFeedbackKind::CountdownBeep:
            playSound("countdown_beep", audio::EventParams{});
            break;
        case RlFeedbackKind::KickoffGo:
            playSound("countdown_go", audio::EventParams{});
            break;
        case RlFeedbackKind::Goal:
        {
            playSound("goal_horn", audio::EventParams{});
            playSound("crowd_roar", audio::EventParams{});
            if (tint != nullptr)
            {
                std::copy(tint, tint + 4, effect.tint);
            }
            spawn("goal_explosion", effect);
            break;
        }
        case RlFeedbackKind::OvertimeStart:
            playSound("whistle", audio::EventParams{});
            break;
        case RlFeedbackKind::MatchEnd:
        {
            playSound("whistle", audio::EventParams{});
            audio::EventParams roar;
            roar.volumeScale = 0.8f;
            playSound("crowd_roar", roar);
            break;
        }
        case RlFeedbackKind::BallHit:
        {
            sound.hasPosition = true;  // the ball is not "own": everybody hears where it was hit
            sound.volumeScale = 0.35f + 0.65f * event.strength;
            playSound("ball_hit", sound);
            effect.intensity = 0.5f + event.strength;
            effect.scale = 0.7f + 0.5f * event.strength;
            spawn("sparks_hit", effect);
            break;
        }
        case RlFeedbackKind::BallBounce:
        {
            sound.hasPosition = true;
            sound.volumeScale = 0.3f + 0.7f * event.strength;
            playSound("ball_bounce", sound);
            if (event.strength > 0.5f)
            {
                effect.intensity = event.strength;
                effect.scale = 0.6f;
                spawn("sparks_hit", effect);
            }
            break;
        }
        case RlFeedbackKind::Jump:
            playSound("jump", sound);
            break;
        case RlFeedbackKind::DoubleJump:
            sound.pitchScale = 1.25f;
            sound.volumeScale = 0.8f;
            playSound("jump", sound);
            break;
        case RlFeedbackKind::Dodge:
            playSound("dodge", sound);
            break;
        case RlFeedbackKind::Land:
        {
            sound.volumeScale = 0.25f + 0.75f * event.strength;
            playSound("land", sound);
            effect.position.y -= 0.25f;
            effect.intensity = 0.4f + event.strength;
            spawn("landing", effect);
            break;
        }
        case RlFeedbackKind::PadPickup:
        {
            const bool big = event.strength > 0.5f;
            playSound(big ? "pad_big" : "pad_small", sound);
            effect.direction = {0.0f, 1.0f, 0.0f};
            effect.scale = big ? 1.5f : 1.0f;
            spawn("pad_pickup", effect);
            break;
        }
        case RlFeedbackKind::Demolish:
            sound.hasPosition = true;
            playSound("demolish", sound);
            spawn("demolish", effect);
            break;
        case RlFeedbackKind::Bump:
            sound.hasPosition = true;
            sound.volumeScale = 0.3f + 0.7f * event.strength;
            playSound("bump", sound);
            break;
        case RlFeedbackKind::CrowdGasp:
        {
            audio::EventParams gasp;
            gasp.volumeScale = 0.3f + 0.3f * event.strength;
            gasp.pitchScale = 1.1f;
            playSound("crowd_roar", gasp);
            break;
        }
        case RlFeedbackKind::Count:
            break;
        }
    }

    void RlFeedback::Frame(const float deltaTime, const rl::RlWorldState& previous, const rl::RlWorldState& current, const float alpha,
                           const audio::ListenerState* listener)
    {
        ZoneScopedN("RlFeedback::Frame");
        (void)deltaTime;
        if (listener != nullptr && audio_ != nullptr)
        {
            audio_->SetListener(*listener);
        }

        const int count = std::min(current.carCount, rl::kMaxCars);
        const bool sameKickoff = previous.match.kickoffs == current.match.kickoffs;
        for (int i = 0; i < count; ++i)
        {
            CarLoops& loops = loops_[i];
            const rl::RlCarState& a = previous.cars[i];
            const rl::RlCarState& b = current.cars[i];
            const bool own = i == ownSlot_;
            const int team = TeamOf(current, i);

            if (b.demolished)
            {
                if (audio_ != nullptr)
                {
                    audio_->Stop(loops.engine, 0.1f);
                    audio_->Stop(loops.boost, 0.05f);
                    audio_->Stop(loops.skid, 0.05f);
                }
                if (particles_ != nullptr)
                {
                    particles_->Stop(loops.boostEffect);
                    particles_->Stop(loops.supersonicEffect);
                }
                loops.engine = loops.boost = loops.skid = {};
                loops.boostEffect = loops.supersonicEffect = {};
                continue;
            }

            const float t = (a.demolished != b.demolished || !sameKickoff) ? 1.0f : std::clamp(alpha, 0.0f, 1.0f);
            const RlVec3 position = Lerp(a.position, b.position, t);
            const RlVec3 velocity = b.linearVelocity;
            const RlVec3 forward = Normalized(Rotate(b.rotation, {0.0f, 0.0f, 1.0f}), {0.0f, 0.0f, 1.0f});
            const RlVec3 right = Normalized(Rotate(b.rotation, {1.0f, 0.0f, 0.0f}), {1.0f, 0.0f, 0.0f});
            const RlVec3 up = Normalized(Rotate(b.rotation, {0.0f, 1.0f, 0.0f}));
            const float speed = Length(velocity);
            const float speed01 = Clamp01(speed / tuning_.car.maxSpeed);
            const bool boosting = current.tick <= loops.boostUntilTick;

            if (audio_ != nullptr)
            {
                // the engine: pitch from the speed (a little more with the boost), the car of the player a bit softer and in the middle
                const float pitch = 0.7f + 1.1f * speed01 + (boosting ? 0.12f : 0.0f) - (b.onGround ? 0.0f : 0.12f);
                if (!audio_->IsPlaying(loops.engine))
                {
                    audio::EventParams params;
                    params.hasPosition = !own;
                    params.position = ToAudio(position);
                    params.velocity = ToAudio(velocity);
                    params.pitchScale = pitch;
                    loops.engine = audio_->PlayEvent("engine", params);
                }
                audio_->SetVoicePitch(loops.engine, pitch);
                audio_->SetVoiceVolume(loops.engine, own ? 0.7f : 1.0f);
                if (!own)
                {
                    audio_->SetVoicePosition(loops.engine, ToAudio(position), ToAudio(velocity));
                }

                // the burn of the boost
                if (boosting && !audio_->IsPlaying(loops.boost))
                {
                    audio::EventParams params;
                    params.hasPosition = !own;
                    params.position = ToAudio(position);
                    loops.boost = audio_->PlayEvent("boost", params);
                }
                else if (!boosting && audio_->IsPlaying(loops.boost))
                {
                    audio_->Stop(loops.boost, 0.12f);
                    loops.boost = {};
                }
                if (boosting && !own)
                {
                    audio_->SetVoicePosition(loops.boost, ToAudio(position), ToAudio(velocity));
                }
                if (boosting)
                {
                    audio_->SetVoiceVolume(loops.boost, own ? 0.8f : 1.0f);
                }

                // the tyres: sliding sideways on the ground
                const float sideways = b.onGround ? std::fabs(Dot(velocity, right)) : 0.0f;
                const bool skidding = b.onGround && sideways > 4.0f && speed > 6.0f;
                if (skidding)
                {
                    if (!audio_->IsPlaying(loops.skid))
                    {
                        audio::EventParams params;
                        params.hasPosition = !own;
                        params.position = ToAudio(position);
                        loops.skid = audio_->PlayEvent("skid", params);
                    }
                    audio_->SetVoiceVolume(loops.skid, Clamp01((sideways - 3.0f) / 9.0f) * (own ? 0.8f : 1.0f));
                    audio_->SetVoicePitch(loops.skid, 0.85f + 0.35f * speed01);
                    if (!own)
                    {
                        audio_->SetVoicePosition(loops.skid, ToAudio(position), ToAudio(velocity));
                    }
                }
                else if (audio_->IsPlaying(loops.skid))
                {
                    audio_->Stop(loops.skid, 0.08f);
                    loops.skid = {};
                }
            }

            if (particles_ != nullptr)
            {
                const RlVec3 back = Scale(forward, -1.0f);
                const RlVec3 exhaust = Add(Add(position, Scale(forward, -0.75f)), Scale(up, -0.1f));
                if (boosting && !particles_->IsAlive(loops.boostEffect))
                {
                    fx::SpawnParams params;
                    params.position = ToFloat3(exhaust);
                    params.direction = ToFloat3(back);
                    params.velocity = ToFloat3(velocity);
                    loops.boostEffect = particles_->Spawn("boost", params);
                }
                else if (!boosting && loops.boostEffect.IsValid())
                {
                    particles_->Stop(loops.boostEffect);
                    loops.boostEffect = {};
                }
                if (particles_->IsAlive(loops.boostEffect))
                {
                    particles_->SetTransform(loops.boostEffect, ToFloat3(exhaust), ToFloat3(back), ToFloat3(velocity));
                }

                if (b.supersonic && !particles_->IsAlive(loops.supersonicEffect))
                {
                    fx::SpawnParams params;
                    params.position = ToFloat3(position);
                    params.direction = ToFloat3(back);
                    params.velocity = ToFloat3(velocity);
                    std::copy(kTeamTint[team], kTeamTint[team] + 4, params.tint);
                    loops.supersonicEffect = particles_->Spawn("supersonic", params);
                }
                else if (!b.supersonic && loops.supersonicEffect.IsValid())
                {
                    particles_->Stop(loops.supersonicEffect);
                    loops.supersonicEffect = {};
                }
                if (particles_->IsAlive(loops.supersonicEffect))
                {
                    particles_->SetTransform(loops.supersonicEffect, ToFloat3(Add(position, Scale(forward, -0.4f))), ToFloat3(back), ToFloat3(velocity));
                }
            }
        }

        // the trail of the ball on speed: thicker the faster it goes, in the colour of the team that touched it last
        if (particles_ != nullptr)
        {
            const rl::RlBallState& ball = current.ball;
            const float ballSpeed = Length(ball.linearVelocity);
            const float trailFrom = 18.0f;
            if (ballSpeed > trailFrom)
            {
                const RlVec3 position = Lerp(previous.ball.position, ball.position, sameKickoff ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f);
                if (!particles_->IsAlive(ballTrail_))
                {
                    fx::SpawnParams params;
                    params.position = ToFloat3(position);
                    ballTrail_ = particles_->Spawn("ball_trail", params);
                }
                particles_->SetTransform(ballTrail_, ToFloat3(position), ToFloat3(Normalized(Scale(ball.linearVelocity, -1.0f))), ToFloat3(ball.linearVelocity));
                particles_->SetRateScale(ballTrail_, Clamp01((ballSpeed - trailFrom) / (tuning_.ball.maxSpeed - trailFrom)) * 0.8f + 0.2f);
                const int toucher = current.lastBallToucher;
                const float white[4] = {0.9f, 0.95f, 1.0f, 1.0f};
                particles_->SetTint(ballTrail_, toucher >= 0 ? kTeamTint[TeamOf(current, toucher)] : white);
            }
            else if (ballTrail_.IsValid())
            {
                particles_->Stop(ballTrail_);
                ballTrail_ = {};
            }
        }
    }
}
