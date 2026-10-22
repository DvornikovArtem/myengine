// RlFeedback.h
//
// The sounds and effects of a Rocket League match, made from the state of the simulation (docs/game/rl-feedback.md). The module reads
// the world of two ticks (rl::RlWorldState before and after a step), finds what happened (a jump, a hit of the ball, a goal, a pad
// taken, a demolition ...) and orders the sound (audio::AudioSystem) and the particles (fx::ParticleSystem). It does not step, draw
// or own anything of the match: RlGameSystem calls it in three places (OnTick after each simulated tick, Frame once per frame,
// StartMatch / EndMatch around a match) and the module does the rest.
//
// Events come from the difference of two ticks, never from the wall clock, so they are the same on every machine, and they are
// remembered by tick: the client of the netcode simulates a tick again after a rollback and must not play the goal twice. Every
// kind of one-shot has a window of ticks (and the goals, the countdown and the end of the match have an id) inside which the same
// event of the same car / pad is not played again.
//
// The module needs no sound device and no renderer: with null pointers (or the null audio backend) it only records what it would
// have played in the journal, which is what the tests and the bot-match log read.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <myengine/audio/AudioTypes.h>
#include <myengine/fx/ParticleSystem.h>

namespace myengine::audio
{
    class AudioSystem;
}

namespace myengine::game
{
    enum class RlFeedbackKind : std::uint8_t
    {
        CountdownBeep = 0,  // number = 3, 2, 1
        KickoffGo,
        Goal,               // team = the team that scored, number = the goals so far, slot = who scored (-1 nobody), position in the goal
        OvertimeStart,
        MatchEnd,           // team = the winner
        BallHit,            // slot = the car, strength 0..1 from the change of the speed of the ball, position = the contact point
        BallBounce,         // the ball off the arena
        Jump,
        DoubleJump,
        Dodge,
        Land,               // strength from the speed into the surface
        PadPickup,          // number = the pad, strength 1 for a big one, slot = the nearest car
        Demolish,           // slot = the car that was destroyed, position where it was
        Bump,               // two cars touching hard
        CrowdGasp,          // the ball runs at a goal
        Count
    };

    const char* RlFeedbackKindName(RlFeedbackKind kind);

    struct RlFeedbackEvent
    {
        RlFeedbackKind kind = RlFeedbackKind::Count;
        std::uint32_t tick = 0;
        int slot = -1;
        int team = -1;
        int number = 0;
        float strength = 0.0f;
        rl::RlVec3 position{};
        rl::RlVec3 direction{0.0f, 1.0f, 0.0f};
        bool replay = false;   // found in the replay of a goal (RlFeedback::BeginReplay), not in the live match
    };

    // One line of the journal: "tick 1481 (12.34 s) BallHit slot 1 team 0 strength 0.62 at (x, y, z)"
    std::string FormatFeedbackEvent(const RlFeedbackEvent& event);

    struct RlFeedbackStats
    {
        std::uint64_t events[static_cast<int>(RlFeedbackKind::Count)] = {};  // played
        std::uint64_t suppressed = 0;   // found again inside the window of the same event: a replayed tick or a doubled contact
        std::uint64_t rollbacks = 0;    // ticks that were not newer than the last one seen
        std::uint64_t ticks = 0;
    };

    class RlFeedback
    {
    public:
        // `audio` and `particles` may be null: the module then only decides and records. In the game both are the global ones:
        //   RlFeedback feedback(tuning, &audio::GetAudioSystem(), &fx::GetParticleSystem());
        RlFeedback(const rl::RlTuning& tuning, audio::AudioSystem* audio, fx::ParticleSystem* particles);
        ~RlFeedback();
        RlFeedback(const RlFeedback&) = delete;
        RlFeedback& operator=(const RlFeedback&) = delete;

        // A new match (or the first tick of a replay of the same one from the start): forgets what was played, stops the loops, starts
        // the hum of the stands. `ownSlot` is the car of this player (its sounds are not 3D and louder; -1 for a spectator).
        void StartMatch(int ownSlot);
        // The match is over or abandoned: the loops fade out, the effects stop.
        void EndMatch();
        void SetOwnSlot(int ownSlot);
        void SetTuning(const rl::RlTuning& tuning);

        // After EVERY simulated tick, replayed ticks of a rollback included: `previous` and `current` are the worlds before and after the
        // tick (RlMatch::PreviousWorld() / World(); the net observers give the match). Finds the events of the tick and plays them.
        void OnTick(const rl::RlWorldState& previous, const rl::RlWorldState& current);

        // Once per frame, after the ticks of the frame: the loops (engine, boost, tyres, supersonic trail, the trail of the ball) follow
        // the cars. `alpha` is the share (0..1) of the way from `previous` to `current` the frame has gone (what the renderer
        // interpolates by); a client that already has an interpolated world passes it as both and 0. `listener` (the camera) is put
        // to the audio system when given; FxUpdateSystem already does it from the primary camera, then pass nullptr.
        void Frame(float deltaTime, const rl::RlWorldState& previous, const rl::RlWorldState& current, float alpha, const audio::ListenerState* listener = nullptr);

        // The replay of a goal (docs/game/rl-replay.md): between BeginReplay and EndReplay the module plays the events of the recorded ticks that the
        // game passes to OnTick, and Frame follows the replayed cars. The events of the live match are not touched and do not suppress the replayed ones
        // (the memory of what was played is kept apart and comes back at EndReplay). The sounds are quieter (`gain`), the announcer ones (countdown, the
        // horn and the roar of a goal, the overtime, the end of the match, the gasp of the stands) are not repeated, the stands are muted.
        void BeginReplay(float gain = 0.7f);
        void EndReplay();
        bool Replaying() const { return replaying_; }
        // The speed of the replay (1, 0.5 in the slow motion): the loops and the sounds drop their pitch a little with it
        void SetReplaySpeed(float speed) { replaySpeed_ = speed; }

        // The settings: the buses 0..1 (G1 gives the sliders)
        void SetVolumes(float master, float sfx, float music, float ui = 1.0f);

        // The music of the menu: a loop on the music bus; StartMatch stops it by itself
        void PlayMenuMusic();
        void StopMenuMusic(float fadeSeconds = 0.5f);
        bool MenuMusicPlaying() const;

        // The journal of what was played (off by default; the tests and the logs of a bot match turn it on)
        void SetJournal(bool enabled) { journalOn_ = enabled; }
        const std::vector<RlFeedbackEvent>& Journal() const { return journal_; }
        void ClearJournal() { journal_.clear(); }
        const RlFeedbackStats& Stats() const { return stats_; }

    private:
        struct CarLoops
        {
            audio::VoiceHandle engine{};
            audio::VoiceHandle boost{};
            audio::VoiceHandle skid{};
            fx::EffectId boostEffect{};
            fx::EffectId supersonicEffect{};
            std::uint32_t boostUntilTick = 0;   // the boost was burning at this tick (the amount fell)
            bool boosting = false;
        };

        // The same event again: inside the window of the last one of its kind and key, or not newer than it
        bool Fresh(RlFeedbackKind kind, std::uint32_t key, std::uint32_t tick, std::uint32_t window);
        bool FreshId(RlFeedbackKind kind, std::uint32_t id);
        void Emit(const RlFeedbackEvent& event);
        void Play(const RlFeedbackEvent& event);
        int TeamOf(const rl::RlWorldState& world, int slot) const;

        void DetectMatch(const rl::RlWorldState& previous, const rl::RlWorldState& current);
        void DetectCars(const rl::RlWorldState& previous, const rl::RlWorldState& current);
        void DetectBall(const rl::RlWorldState& previous, const rl::RlWorldState& current);
        void DetectPads(const rl::RlWorldState& previous, const rl::RlWorldState& current);

        void StopLoops(float fadeSeconds);

        rl::RlTuning tuning_;
        audio::AudioSystem* audio_ = nullptr;
        fx::ParticleSystem* particles_ = nullptr;
        int ownSlot_ = 0;

        std::uint32_t lastTick_ = 0;
        bool haveTick_ = false;
        std::unordered_map<std::uint32_t, std::uint32_t> lastByKey_;   // (kind, key) -> the tick it was played at
        std::unordered_set<std::uint64_t> announced_;                  // (kind, id): the goals, the countdown, the end
        CarLoops loops_[rl::kMaxCars];
        audio::VoiceHandle crowd_{};
        audio::VoiceHandle menuMusic_{};
        fx::EffectId ballTrail_{};

        bool replaying_ = false;
        float replayGain_ = 0.7f;
        float replaySpeed_ = 1.0f;
        std::unordered_map<std::uint32_t, std::uint32_t> liveLastByKey_;   // the memory of the live match while a replay runs
        std::unordered_set<std::uint64_t> liveAnnounced_;
        std::uint32_t liveLastTick_ = 0;
        bool liveHaveTick_ = false;

        bool journalOn_ = false;
        std::vector<RlFeedbackEvent> journal_;
        RlFeedbackStats stats_;
    };
}
