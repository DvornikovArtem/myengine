// The sounds and effects of the match (G7d): events from pairs of states (synthetic), no doubles when a tick is simulated again after a
// rollback, the loops of the cars and the commands that reach the audio and particle systems (no device, no renderer), and a match of
// bots on the real simulation with the journal of what played and when. RL_FEEDBACK_JOURNAL=<file> writes the journal of the bot match.

#include <myengine/audio/AudioSystem.h>
#include <myengine/fx/ParticleSystem.h>
#include <myengine/game/RlFeedback.h>

#include <rl/RlMatch.h>
#include <rl/RlTuning.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using namespace myengine;
    using game::RlFeedback;
    using game::RlFeedbackEvent;
    using game::RlFeedbackKind;
    namespace fs = std::filesystem;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    int CountOf(const RlFeedback& feedback, const RlFeedbackKind kind)
    {
        return static_cast<int>(feedback.Stats().events[static_cast<int>(kind)]);
    }

    const RlFeedbackEvent* Find(const RlFeedback& feedback, const RlFeedbackKind kind, const int nth = 0)
    {
        int seen = 0;
        for (const RlFeedbackEvent& event : feedback.Journal())
        {
            if (event.kind == kind && seen++ == nth)
            {
                return &event;
            }
        }
        return nullptr;
    }

    // A world of two cars in the middle of a game (mode 1: slot 0 blue, slot 1 orange), both on the ground, at rest
    rl::RlWorldState MakeWorld(const std::uint32_t tick)
    {
        rl::RlWorldState world;
        world.tick = tick;
        world.carCount = 2;
        world.match.mode = 1;
        world.match.phase = rl::RlMatchPhase::Playing;
        world.match.kickoffs = 1;
        for (int i = 0; i < 2; ++i)
        {
            world.cars[i].position = {i == 0 ? -20.0f : 20.0f, 0.4f, 0.0f};
            world.cars[i].onGround = true;
            world.cars[i].wheelContacts = 4;
            world.cars[i].surfaceNormal = {0.0f, 1.0f, 0.0f};
        }
        world.ball.position = {0.0f, 1.0f, 0.0f};
        return world;
    }

    rl::RlTuning Tuning() { return rl::RlTuning(); }

    // ---------------------------------------------------------------- the events of one tick

    void TestCountdownAndGoal()
    {
        RlFeedback feedback(Tuning(), nullptr, nullptr);
        feedback.SetJournal(true);
        feedback.StartMatch(0);

        rl::RlWorldState before = MakeWorld(0);
        before.match.phase = rl::RlMatchPhase::Countdown;
        before.match.phaseTimer = 3.0f;
        for (int pass = 0; pass < 2; ++pass)  // the second pass is a replay of the same ticks
        {
            rl::RlWorldState a = before;
            const float timers[] = {3.0f, 2.9f, 2.0f, 1.99f, 1.0f, 0.99f, 0.01f};
            std::uint32_t tick = 0;
            for (const float timer : timers)
            {
                rl::RlWorldState b = MakeWorld(++tick);
                b.match.phase = rl::RlMatchPhase::Countdown;
                b.match.phaseTimer = timer;
                feedback.OnTick(a, b);
                a = b;
            }
            rl::RlWorldState go = MakeWorld(++tick);
            go.match.phase = rl::RlMatchPhase::Playing;
            feedback.OnTick(a, go);
        }
        Check(CountOf(feedback, RlFeedbackKind::CountdownBeep) == 3, "the countdown beeps three times (3, 2, 1)");
        Check(Find(feedback, RlFeedbackKind::CountdownBeep, 0)->number == 3 && Find(feedback, RlFeedbackKind::CountdownBeep, 2)->number == 1, "the numbers go 3, 2, 1");
        Check(CountOf(feedback, RlFeedbackKind::KickoffGo) == 1, "one start signal");

        // the second kick-off beeps again
        rl::RlWorldState next = MakeWorld(500);
        next.match.phase = rl::RlMatchPhase::Countdown;
        next.match.phaseTimer = 3.0f;
        next.match.kickoffs = 2;
        feedback.OnTick(MakeWorld(499), next);
        Check(CountOf(feedback, RlFeedbackKind::CountdownBeep) == 4, "a new kick-off counts down again");

        // a goal of blue: horn, roar, the explosion at the goal that was hit; the same tick again, and a later one, play nothing
        rl::RlWorldState scored = MakeWorld(900);
        scored.match.score[0] = 1;
        scored.match.lastGoalTeam = 0;
        scored.match.lastGoalScorer = 0;
        scored.match.phase = rl::RlMatchPhase::GoalScored;
        scored.ball.position = {1.0f, 2.0f, 52.5f};
        feedback.OnTick(MakeWorld(899), scored);
        feedback.OnTick(MakeWorld(899), scored);
        rl::RlWorldState later = scored;
        later.tick = 903;
        feedback.OnTick(MakeWorld(902), later);
        Check(CountOf(feedback, RlFeedbackKind::Goal) == 1, "a goal is played once, also when the tick comes again (even a few ticks later)");
        const RlFeedbackEvent* goal = Find(feedback, RlFeedbackKind::Goal);
        Check(goal->team == 0 && goal->slot == 0 && goal->number == 1 && goal->position.z > 51.0f, "the goal of blue is in the +Z goal");
        rl::RlWorldState second = scored;
        second.tick = 2000;
        second.match.score[1] = 1;
        second.match.lastGoalTeam = 1;
        second.ball.position = {0.0f, 2.0f, -52.5f};
        feedback.OnTick(MakeWorld(1999), second);
        Check(CountOf(feedback, RlFeedbackKind::Goal) == 2 && Find(feedback, RlFeedbackKind::Goal, 1)->position.z < -51.0f, "the second goal is a goal of its own, in the other goal");

        // overtime and the end
        rl::RlWorldState overtime = second;
        overtime.tick = 3000;
        overtime.match.overtime = 1;
        feedback.OnTick(second, overtime);
        feedback.OnTick(second, overtime);
        rl::RlWorldState finished = overtime;
        finished.tick = 4000;
        finished.match.phase = rl::RlMatchPhase::Finished;
        finished.match.winner = 1;
        feedback.OnTick(overtime, finished);
        feedback.OnTick(overtime, finished);
        Check(CountOf(feedback, RlFeedbackKind::OvertimeStart) == 1 && CountOf(feedback, RlFeedbackKind::MatchEnd) == 1, "the overtime and the end are played once");
        Check(Find(feedback, RlFeedbackKind::MatchEnd)->team == 1, "the end names the winner");
        Check(feedback.Stats().suppressed > 0, "the suppressed doubles are counted");
    }

    void TestCars()
    {
        RlFeedback feedback(Tuning(), nullptr, nullptr);
        feedback.SetJournal(true);
        feedback.StartMatch(0);

        // the jump: the car leaves with a push; a car that merely drives off a ramp has no push and makes no sound
        rl::RlWorldState a = MakeWorld(10);
        rl::RlWorldState b = MakeWorld(11);
        b.cars[0].onGround = false;
        b.cars[0].jumped = true;
        b.cars[0].canDodge = true;
        b.cars[0].linearVelocity.y = 2.9f;
        b.cars[1].onGround = false;
        b.cars[1].jumped = true;
        feedback.OnTick(a, b);
        Check(CountOf(feedback, RlFeedbackKind::Jump) == 1 && Find(feedback, RlFeedbackKind::Jump)->slot == 0, "the first jump of slot 0 is a jump, a drive-off is not");

        // the same tick again and the same jump a few ticks later (the rollback moved it): still one
        feedback.OnTick(a, b);
        rl::RlWorldState shifted = b;
        shifted.tick = 14;
        feedback.OnTick(a, shifted);
        Check(CountOf(feedback, RlFeedbackKind::Jump) == 1, "a replayed jump is not played again");

        // the double jump (a second push) and the dodge (the flip starts); the timeout of the window is silent
        rl::RlWorldState air = b;
        air.tick = 100;
        rl::RlWorldState air2 = air;
        air2.tick = 101;
        air2.cars[0].canDodge = false;
        air2.cars[0].linearVelocity.y += 2.9f;
        feedback.OnTick(air, air2);
        Check(CountOf(feedback, RlFeedbackKind::DoubleJump) == 1 && CountOf(feedback, RlFeedbackKind::Dodge) == 0, "a second push without a flip is a double jump");

        rl::RlWorldState air3 = air;
        air3.tick = 300;
        rl::RlWorldState air4 = air3;
        air4.tick = 301;
        air4.cars[0].canDodge = false;
        air4.cars[0].dodging = true;
        air4.cars[0].linearVelocity.z += 4.0f;
        feedback.OnTick(air3, air4);
        Check(CountOf(feedback, RlFeedbackKind::Dodge) == 1, "the start of the flip is a dodge");

        rl::RlWorldState air5 = air;
        air5.tick = 600;
        rl::RlWorldState air6 = air5;
        air6.tick = 601;
        air6.cars[0].canDodge = false;
        feedback.OnTick(air5, air6);
        Check(CountOf(feedback, RlFeedbackKind::DoubleJump) == 1 && CountOf(feedback, RlFeedbackKind::Dodge) == 1, "the window that ran out makes no sound");

        // the landing: loud for a fall, silent for a bump of the suspension
        rl::RlWorldState fall = MakeWorld(700);
        fall.cars[0].onGround = false;
        fall.cars[0].linearVelocity = {0.0f, -6.0f, 0.0f};
        rl::RlWorldState landed = MakeWorld(701);
        landed.cars[0].linearVelocity = {0.0f, -1.0f, 0.0f};
        feedback.OnTick(fall, landed);
        const RlFeedbackEvent* land = Find(feedback, RlFeedbackKind::Land);
        Check(land != nullptr && land->strength > 0.3f && land->strength < 0.9f, "a fall at 6 m/s lands with a middle strength");
        rl::RlWorldState soft = MakeWorld(800);
        soft.cars[0].onGround = false;
        soft.cars[0].linearVelocity = {0.0f, -0.4f, 0.0f};
        feedback.OnTick(soft, MakeWorld(801));
        Check(CountOf(feedback, RlFeedbackKind::Land) == 1, "a touchdown at 0.4 m/s is silent");

        // the demolition: at the place the car was, once, and a car that is down makes nothing else
        rl::RlWorldState alive = MakeWorld(900);
        alive.cars[1].position = {5.0f, 1.0f, 7.0f};
        rl::RlWorldState dead = MakeWorld(901);
        dead.cars[1].position = {0.0f, -50.0f, 0.0f};
        dead.cars[1].demolished = true;
        feedback.OnTick(alive, dead);
        feedback.OnTick(alive, dead);
        const RlFeedbackEvent* demolish = Find(feedback, RlFeedbackKind::Demolish);
        Check(CountOf(feedback, RlFeedbackKind::Demolish) == 1 && demolish->slot == 1 && demolish->team == 1 && std::fabs(demolish->position.x - 5.0f) < 1e-4f,
              "a demolition is played once at the old place of the car");

        // a bump: a shove with another car close by, not with one far away
        rl::RlWorldState bumpA = MakeWorld(1000);
        bumpA.cars[0].position = {0.0f, 0.4f, 0.0f};
        bumpA.cars[1].position = {2.0f, 0.4f, 0.0f};
        rl::RlWorldState bumpB = bumpA;
        bumpB.tick = 1001;
        bumpB.cars[0].linearVelocity = {-6.0f, 0.0f, 0.0f};
        feedback.OnTick(bumpA, bumpB);
        feedback.OnTick(bumpA, bumpB);
        Check(CountOf(feedback, RlFeedbackKind::Bump) == 1, "a shove next to another car is one bump");
        rl::RlWorldState farA = MakeWorld(2000);
        rl::RlWorldState farB = MakeWorld(2001);
        farB.cars[0].linearVelocity = {-6.0f, 0.0f, 0.0f};
        feedback.OnTick(farA, farB);
        Check(CountOf(feedback, RlFeedbackKind::Bump) == 1, "a shove with nobody near is not a bump");
    }

    void TestBallAndPads()
    {
        RlFeedback feedback(Tuning(), nullptr, nullptr);
        feedback.SetJournal(true);
        feedback.StartMatch(0);

        // a hit: a touch of slot 1 and a change of the speed of the ball; its strength follows the change
        rl::RlWorldState a = MakeWorld(10);
        rl::RlWorldState b = MakeWorld(11);
        b.match.touches[1] = 1;
        b.ball.linearVelocity = {0.0f, 3.0f, 12.0f};
        b.cars[1].position = {0.0f, 1.0f, -1.5f};
        feedback.OnTick(a, b);
        const RlFeedbackEvent* hit = Find(feedback, RlFeedbackKind::BallHit);
        Check(hit != nullptr && hit->slot == 1 && hit->team == 1 && hit->strength > 0.4f && hit->strength < 0.6f, "a touch with a change of 12 m/s is a hit of middle strength");
        feedback.OnTick(a, b);
        Check(CountOf(feedback, RlFeedbackKind::BallHit) == 1, "the same hit again is not played again");

        // a touch that does not push the ball (dribbling) is silent; a push with no touch is a bounce
        rl::RlWorldState c = MakeWorld(100);
        rl::RlWorldState d = MakeWorld(101);
        d.match.touches[0] = 1;
        d.ball.linearVelocity = {0.0f, 0.2f, 0.0f};
        feedback.OnTick(c, d);
        Check(CountOf(feedback, RlFeedbackKind::BallHit) == 1, "a touch that does not change the speed is silent");
        rl::RlWorldState e = MakeWorld(200);
        e.ball.linearVelocity = {0.0f, -12.0f, 0.0f};
        rl::RlWorldState f = MakeWorld(201);
        f.ball.linearVelocity = {0.0f, 7.0f, 0.0f};
        feedback.OnTick(e, f);
        Check(CountOf(feedback, RlFeedbackKind::BallBounce) == 1 && Find(feedback, RlFeedbackKind::BallBounce)->strength > 0.5f, "a change of the speed with no car is a bounce");

        // the pad: the timer goes up from zero; the big pad has strength 1
        rl::RlWorldState padA = MakeWorld(300);
        rl::RlWorldState padB = MakeWorld(301);
        float x = 0.0f;
        float z = 0.0f;
        int bigPad = -1;
        int smallPad = -1;
        for (int i = 0; i < rl::kPadCount; ++i)
        {
            (rl::RlPadSpot(i, x, z) ? bigPad : smallPad) = i;
        }
        padB.match.pads[bigPad].respawnLeft = 10.0f;
        padB.match.pads[smallPad].respawnLeft = 4.0f;
        feedback.OnTick(padA, padB);
        feedback.OnTick(padA, padB);
        Check(CountOf(feedback, RlFeedbackKind::PadPickup) == 2, "two pads taken are two events, the replay adds none");
        bool sawBig = false;
        bool sawSmall = false;
        for (const RlFeedbackEvent& event : feedback.Journal())
        {
            if (event.kind == RlFeedbackKind::PadPickup)
            {
                (event.number == bigPad ? sawBig : sawSmall) = event.strength > 0.5f ? event.number == bigPad : event.number == smallPad;
            }
        }
        Check(sawBig && sawSmall, "the big pad has strength 1, the small one 0");
        rl::RlWorldState padC = padB;
        padC.tick = 302;
        padC.match.pads[bigPad].respawnLeft = 9.99f;
        feedback.OnTick(padB, padC);
        Check(CountOf(feedback, RlFeedbackKind::PadPickup) == 2, "a pad that is only counting down plays nothing");

        // the stands: the ball runs at a goal that is near, once in six seconds
        rl::RlWorldState goalA = MakeWorld(400);
        goalA.ball.position = {0.0f, 3.0f, 40.0f};
        goalA.ball.linearVelocity = {0.0f, 0.0f, 25.0f};
        rl::RlWorldState goalB = goalA;
        goalB.tick = 401;
        feedback.OnTick(goalA, goalB);
        goalB.tick = 402;
        feedback.OnTick(goalA, goalB);
        Check(CountOf(feedback, RlFeedbackKind::CrowdGasp) == 1, "a fast ball at a near goal makes the stands gasp once");
        rl::RlWorldState slow = goalA;
        slow.tick = 2000;
        slow.ball.linearVelocity = {0.0f, 0.0f, 5.0f};
        feedback.OnTick(goalA, slow);
        Check(CountOf(feedback, RlFeedbackKind::CrowdGasp) == 1, "a slow ball does not");
    }

    // ---------------------------------------------------------------- the commands that reach the systems

    void TestSystems(const fs::path& source)
    {
        audio::AudioSystem audio;
        audio::AudioConfig config;
        config.useDevice = false;
        Check(audio.Initialize(config), "the audio system starts without a device");
        Check(audio.LoadBank(source / "assets/data/audio_bank.json"), "the sound bank loads");
        for (const char* name : {"crowd_ambience", "engine", "boost", "skid", "bump", "whistle", "goal_horn", "crowd_roar", "ball_hit", "jump", "land", "demolish",
                                 "pad_small", "pad_big", "countdown_beep", "countdown_go", "menu_music"})
        {
            Check(audio.HasEvent(name), std::string("the bank has the event ") + name);
        }
        fx::ParticleSystem particles;
        std::vector<std::string> messages;
        Check(particles.LoadEffects(source / "assets/data/fx", &messages) >= 8, "the effects load");
        for (const char* name : {"boost", "ball_trail", "sparks_hit", "goal_explosion", "demolish", "landing", "pad_pickup", "supersonic"})
        {
            Check(particles.FindEffect(name) != nullptr, std::string("the effect ") + name + " exists");
        }

        RlFeedback feedback(Tuning(), &audio, &particles);
        feedback.StartMatch(0);
        audio.Update(0.016f);
        Check(audio.GetStats().voicesActive == 1, "the hum of the stands plays from the start of a match");

        // two cars on the ground: two engines (the own one is not 3D), pitch follows the speed
        rl::RlWorldState a = MakeWorld(1);
        rl::RlWorldState b = MakeWorld(2);
        b.cars[1].linearVelocity = {0.0f, 0.0f, 20.0f};
        feedback.OnTick(a, b);
        feedback.Frame(0.016f, a, b, 1.0f);
        audio.Update(0.016f);
        Check(audio.GetStats().voicesActive == 3, "both cars have an engine");

        // a boost: the amount falls, the flame and the roar start; some ticks later without a fall they stop
        rl::RlWorldState c = b;
        c.tick = 3;
        b.cars[0].boost = 50.0f;
        c.cars[0].boost = 49.7f;
        feedback.OnTick(b, c);
        feedback.Frame(0.016f, b, c, 1.0f);
        audio.Update(0.016f);
        particles.Update(0.016f);
        Check(audio.GetStats().voicesActive == 4, "a car that burns boost has a boost voice");
        Check(particles.GetStats().activeEffects == 1 && particles.LiveParticles() > 0, "and a flame");
        rl::RlWorldState d = c;
        for (int i = 0; i < 20; ++i)
        {
            rl::RlWorldState next = d;
            next.tick = d.tick + 1;
            feedback.OnTick(d, next);
            d = next;
        }
        feedback.Frame(0.016f, d, d, 0.0f);
        audio.Update(0.5f);
        Check(audio.GetStats().voicesActive == 3, "the boost voice fades out when the amount stops falling");

        // one-shots: a jump of the own car is not 3D, a hit has 3D and sparks
        const std::uint64_t played = audio.GetStats().played;
        rl::RlWorldState jumpA = d;
        jumpA.tick = 500;
        rl::RlWorldState jumpB = jumpA;
        jumpB.tick = 501;
        jumpB.cars[0].jumped = true;
        jumpB.cars[0].onGround = false;
        jumpB.cars[0].linearVelocity.y = 2.9f;
        jumpB.match.touches[1] = 1;
        jumpB.ball.linearVelocity = {0.0f, 0.0f, 20.0f};
        const std::uint32_t before = particles.GetStats().activeEffects;
        feedback.OnTick(jumpA, jumpB);
        Check(audio.GetStats().played == played + 2, "a jump and a hit start two sounds");
        Check(particles.GetStats().activeEffects == before + 1, "the hit starts sparks");

        // the end of the match and the menu: the loops stop, the music plays and stops
        feedback.EndMatch();
        audio.Update(1.0f);
        Check(audio.GetStats().voicesActive <= 2, "after the match the loops are gone (only the tails of the one-shots may be left)");
        feedback.PlayMenuMusic();
        audio.Update(0.016f);
        Check(feedback.MenuMusicPlaying(), "the menu music plays");
        feedback.StartMatch(0);
        audio.Update(1.0f);
        Check(!feedback.MenuMusicPlaying(), "a match stops the menu music");

        feedback.SetVolumes(0.5f, 0.6f, 0.7f, 0.8f);
        Check(std::fabs(audio.GetBusVolume(audio::Bus::Master) - 0.5f) < 1e-4f && std::fabs(audio.GetBusVolume(audio::Bus::Sfx) - 0.6f) < 1e-4f &&
                  std::fabs(audio.GetBusVolume(audio::Bus::Music) - 0.7f) < 1e-4f && std::fabs(audio.GetBusVolume(audio::Bus::Ui) - 0.8f) < 1e-4f,
              "the volumes of the buses are set");
    }

    // ---------------------------------------------------------------- a match of bots on the real simulation

    struct BotRun
    {
        std::vector<RlFeedbackEvent> journal;
        std::uint64_t suppressed = 0;
        std::uint64_t rollbacks = 0;
    };

    // `rollbackTo` > 0: at tick `rollbackFrom` the match goes back to the tick `rollbackTo` of the ring and simulates the ticks again
    BotRun RunBots(const int seconds, const std::uint32_t rollbackFrom, const std::uint32_t rollbackTo)
    {
        rl::RlMatchConfig config;
        config.mode = 2;
        config.seed = 7;
        config.humanMask = 0;
        config.botLevel = 1;
        rl::RlMatch match(Tuning(), config);
        RlFeedback feedback(Tuning(), nullptr, nullptr);
        feedback.SetJournal(true);
        feedback.StartMatch(0);
        rl::RlMatchRing ring(512);
        std::vector<rl::RlCarInput> inputs(static_cast<std::size_t>(match.SlotCount()));
        const std::uint32_t total = static_cast<std::uint32_t>(seconds * rl::kTickRate);
        const std::uint32_t goalTick = 2400;  // a goal is forced so that the run has one for sure (the same tick in a replay)
        bool rolledBack = false;
        while (match.World().tick < total)
        {
            match.Step(inputs.data());
            if (match.World().tick == goalTick)
            {
                match.MutableSim().ResetBall({0.0f, 1.2f, 51.2f + 1.3f});
            }
            feedback.OnTick(match.PreviousWorld(), match.World());
            ring.Push(match.World().tick, match);
            if (rollbackTo > 0 && !rolledBack && match.World().tick == rollbackFrom)
            {
                rolledBack = true;
                Check(ring.Restore(rollbackTo, match), "the ring still holds the tick to go back to");
            }
        }
        BotRun run;
        run.journal = feedback.Journal();
        run.suppressed = feedback.Stats().suppressed;
        run.rollbacks = feedback.Stats().rollbacks;
        return run;
    }

    bool SameJournal(const std::vector<RlFeedbackEvent>& a, const std::vector<RlFeedbackEvent>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].kind != b[i].kind || a[i].tick != b[i].tick || a[i].slot != b[i].slot || a[i].number != b[i].number)
            {
                return false;
            }
        }
        return true;
    }

    void TestBotMatch()
    {
        const BotRun first = RunBots(60, 0, 0);
        const BotRun again = RunBots(60, 0, 0);
        Check(SameJournal(first.journal, again.journal), "the same match gives the same journal");

        const auto count = [&](const RlFeedbackKind kind)
        {
            return static_cast<int>(std::count_if(first.journal.begin(), first.journal.end(), [&](const RlFeedbackEvent& e) { return e.kind == kind; }));
        };
        // the match starts with a kick-off, the forced goal brings a second one: every kick-off counts 3, 2, 1 and starts
        const int goals = count(RlFeedbackKind::Goal);
        // (a goal in the last seconds ends the match: its kick-off does not come, and G4i moved the goals of the match)
        const int kickoffs = count(RlFeedbackKind::KickoffGo);
        Check(count(RlFeedbackKind::CountdownBeep) == 3 * kickoffs && (kickoffs == 1 + goals || kickoffs == goals), "every kick-off counts 3, 2, 1 and starts");
        Check(first.journal.front().kind == RlFeedbackKind::CountdownBeep && first.journal.front().number == 3, "the first sound of a match is the 3");
        Check(count(RlFeedbackKind::Goal) >= 1, "the forced goal is in the journal");
        Check(count(RlFeedbackKind::BallHit) >= 3, "the bots hit the ball");
        Check(count(RlFeedbackKind::PadPickup) >= 1, "the bots take pads");
        Check(count(RlFeedbackKind::Jump) + count(RlFeedbackKind::Land) >= 1 || count(RlFeedbackKind::BallBounce) >= 1, "the cars or the ball touch the floor");

        // a rollback from tick 3600 to tick 3300 (across the forced goal at 2400? no: across a stretch of play) and the same ticks again
        const BotRun replayed = RunBots(60, 3600, 3300);
        Check(replayed.rollbacks > 0, "the run did simulate ticks again");
        Check(SameJournal(first.journal, replayed.journal), "a rollback of 300 ticks plays nothing twice and misses nothing");
        // and across the forced goal
        const BotRun overGoal = RunBots(60, 2600, 2300);
        Check(overGoal.rollbacks > 0 && SameJournal(first.journal, overGoal.journal), "a rollback across the goal plays the goal once");
        std::cout << "bot match: " << first.journal.size() << " events, replays suppressed " << replayed.suppressed << " + " << overGoal.suppressed << "\n";

        char* path = nullptr;
        std::size_t pathLength = 0;
        _dupenv_s(&path, &pathLength, "RL_FEEDBACK_JOURNAL");
        if (path != nullptr && *path != '\0')
        {
            std::ofstream file(fs::path(path), std::ios::binary);  // the environment is in the ANSI code page
            file << "Bot match, 2v2, seed 7, normal bots, 60 s of the match (7200 ticks at 120 Hz); a goal is forced at tick 2400.\n"
                    "What RlFeedback played and when (the journal is the same with a rollback of 300 ticks and with one across the goal).\n\n";
            for (const RlFeedbackEvent& event : first.journal)
            {
                file << game::FormatFeedbackEvent(event) << "\n";
            }
            file << "\nTotals:\n";
            for (int kind = 0; kind < static_cast<int>(RlFeedbackKind::Count); ++kind)
            {
                file << "  " << game::RlFeedbackKindName(static_cast<RlFeedbackKind>(kind)) << ": " << count(static_cast<RlFeedbackKind>(kind)) << "\n";
            }
        }
        std::free(path);
    }
}

int main()
{
    try
    {
        const fs::path source = fs::u8path(MYENGINE_SOURCE_DIR);
        TestCountdownAndGoal();
        TestCars();
        TestBallAndPads();
        TestSystems(source);
        TestBotMatch();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "rl_feedback tests passed\n";
    return 0;
}
