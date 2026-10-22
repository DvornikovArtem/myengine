// The particle simulation without a renderer (G7c): the parser of the presets, curves, bursts and rates, the budget, stop and kill,
// following a moving source, the integration, the instance data of the batches, determinism, the presets of the project and
// the cost of 5000 particles.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <myengine/fx/ParticleEffect.h>
#include <myengine/fx/ParticleSystem.h>

namespace
{
    using namespace myengine::fx;
    using myengine::render::BlendMode;
    using myengine::render::Float3;
    namespace fs = std::filesystem;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Near(const float a, const float b, const float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

    ParticleSystem MakeSystem(const std::string& json)
    {
        ParticleSystem system;
        EffectDesc effect;
        std::string error;
        Check(ParseEffectText(json, effect, error), "the test effect parses: " + error);
        Check(system.AddEffect(std::move(effect)), "the effect is added");
        system.SetSeed(99);
        return system;
    }

    void Run(ParticleSystem& system, const float seconds, const float step = 1.0f / 60.0f)
    {
        for (float t = 0.0f; t < seconds; t += step)
        {
            system.Update(step);
        }
    }

    // ---- the parser ----

    void TestParser()
    {
        EffectDesc effect;
        std::string error;
        Check(ParseEffectText(R"({"name":"a","emitters":[{"rate":10,"loop":true,"lifetime":[1,2],"speed":3,"size":[0.1,0.2],"blend":"alpha",
            "color":[[1,[1,1,1,0]],[0,[1,0.5,0,1]]],"sizeOverLife":[[0,1],[1,0]],"gravity":[0,-9.8,0],"shape":"disc","shapeRadius":2,"frame":3}]})",
            effect, error), "a full emitter parses: " + error);
        Check(effect.name == "a" && effect.emitters.size() == 1, "name and emitters");
        const EmitterDesc& e = effect.emitters[0];
        Check(e.blend == BlendMode::Alpha && e.frame == 3 && e.shape == EmitterShape::Disc && e.loop, "blend, frame, shape, loop");
        Check(Near(e.lifetimeMin, 1.0f) && Near(e.lifetimeMax, 2.0f) && Near(e.speedMin, 3.0f) && Near(e.speedMax, 3.0f), "ranges: [min, max] and a single number");
        Check(e.colorOverLife.size() == 2 && e.colorOverLife[0].t < e.colorOverLife[1].t, "the gradient is sorted by time");
        Check(Near(e.gravity[1], -9.8f), "gravity");

        const auto fails = [](const std::string& text, const std::string& needle)
        {
            EffectDesc out;
            std::string message;
            const bool ok = ParseEffectText(text, out, message);
            Check(!ok && message.find(needle) != std::string::npos, "the error mentions '" + needle + "': " + message);
        };
        fails("not json", "");
        fails(R"({"emitters":[{"rate":1,"lifetime":1}]})", "name");
        fails(R"({"name":"x","emitters":[]})", "emitters");
        fails(R"({"name":"x","emitters":[{"rate":1,"lifetime":1,"blend":"multiply"}]})", "blend");
        fails(R"({"name":"x","emitters":[{"lifetime":1}]})", "emission");
        fails(R"({"name":"x","emitters":[{"rate":5,"lifetime":0}]})", "lifetime");
        fails(R"({"name":"x","emitters":[{"rate":5,"lifetime":1,"shape":"cube"}]})", "shape");
        fails(R"({"name":"x","emitters":[{"rate":5,"lifetime":1,"gravity":[1,2]}]})", "gravity");
        fails(R"({"name":"x","emitters":[{"rate":5,"lifetime":1,"color":[[0,[1,1]]]}]})", "color");
    }

    void TestCurves()
    {
        const std::vector<CurveKey> curve = {{0.0f, 1.0f}, {0.5f, 3.0f}, {1.0f, 0.0f}};
        Check(Near(EvaluateCurve(curve, 0.0f), 1.0f) && Near(EvaluateCurve(curve, 0.25f), 2.0f) && Near(EvaluateCurve(curve, 0.5f), 3.0f) &&
                Near(EvaluateCurve(curve, 0.75f), 1.5f) && Near(EvaluateCurve(curve, 1.0f), 0.0f), "a piecewise linear curve");
        Check(Near(EvaluateCurve(curve, -1.0f), 1.0f) && Near(EvaluateCurve(curve, 7.0f), 0.0f), "clamped at the ends");
        Check(Near(EvaluateCurve({}, 0.4f), 1.0f), "an empty curve is 1");
        std::vector<ColorKey> gradient(2);
        gradient[0].t = 0.0f;
        gradient[1].t = 1.0f;
        gradient[1].rgba[0] = 0.0f;
        gradient[1].rgba[3] = 0.0f;
        float out[4];
        EvaluateGradient(gradient, 0.5f, out);
        Check(Near(out[0], 0.5f) && Near(out[1], 1.0f) && Near(out[3], 0.5f), "a gradient blends the keys");
    }

    // ---- emission ----

    const char* kBurst = R"({"name":"burst","emitters":[{"bursts":[[0,30]],"lifetime":[0.5,0.5],"speed":0,"size":0.2,"maxParticles":100}]})";
    const char* kLoop = R"({"name":"loop","emitters":[{"loop":true,"rate":100,"lifetime":[10,10],"speed":0,"size":0.2,"maxParticles":2000}]})";

    void TestBurstAndLife()
    {
        ParticleSystem system = MakeSystem(kBurst);
        const EffectId id = system.Spawn("burst");
        Check(id.IsValid() && system.IsAlive(id), "an effect starts");
        Check(!system.Spawn("nothing").IsValid(), "an unknown effect does not");
        system.Update(1.0f / 60.0f);
        Check(system.LiveParticles() == 30, "the burst of 30 at once: " + std::to_string(system.LiveParticles()));
        Run(system, 0.4f);
        Check(system.LiveParticles() == 30 && system.IsAlive(id), "they live their 0.5 s");
        Run(system, 0.2f);
        Check(system.LiveParticles() == 0 && !system.IsAlive(id), "then they die and the effect is over");
        Check(system.GetStats().spawned == 30 && system.GetStats().effectsStarted == 1, "the stats");

        // the intensity scales a burst
        SpawnParams strong;
        strong.intensity = 2.0f;
        system.Spawn("burst", strong);
        system.Update(1.0f / 60.0f);
        Check(system.LiveParticles() == 60, "intensity 2: a burst twice as big");
    }

    void TestRate()
    {
        ParticleSystem system = MakeSystem(kLoop);
        const EffectId id = system.Spawn("loop");
        Run(system, 1.0f);
        Check(std::abs(static_cast<int>(system.LiveParticles()) - 100) <= 2, "100 per second for a second: " + std::to_string(system.LiveParticles()));
        SpawnParams half;
        half.rateScale = 0.5f;
        system.Clear();
        const EffectId slow = system.Spawn("loop", half);
        Run(system, 1.0f);
        Check(std::abs(static_cast<int>(system.LiveParticles()) - 50) <= 2, "rateScale 0.5: " + std::to_string(system.LiveParticles()));
        system.SetRateScale(slow, 2.0f);
        const std::uint32_t before = system.LiveParticles();
        Run(system, 0.5f);
        Check(std::abs(static_cast<int>(system.LiveParticles() - before) - 100) <= 3, "SetRateScale changes the rate on the fly");
        (void)id;
    }

    void TestBudgetAndLimits()
    {
        ParticleSystem system = MakeSystem(R"({"name":"big","emitters":[{"bursts":[[0,500]],"lifetime":[1,1],"speed":0,"size":0.2,"maxParticles":5000}]})");
        system.SetBudget(100);
        system.Spawn("big");
        system.Update(0.016f);
        Check(system.LiveParticles() == 100 && system.GetStats().dropped == 400, "the budget of 100: 100 live, 400 dropped");

        ParticleSystem limited = MakeSystem(R"({"name":"cap","emitters":[{"bursts":[[0,500]],"lifetime":[1,1],"speed":0,"size":0.2,"maxParticles":40}]})");
        limited.Spawn("cap");
        limited.Update(0.016f);
        Check(limited.LiveParticles() == 40, "the limit of the emitter: 40");
        static_assert(ParticleSystem::kDefaultBudget == 5000, "the default budget is 5000");
        Check(ParticleSystem{}.Budget() == ParticleSystem::kDefaultBudget, "a system has the default budget");
    }

    void TestStop()
    {
        ParticleSystem system = MakeSystem(kLoop);
        const EffectId id = system.Spawn("loop");
        Run(system, 0.5f);
        const std::uint32_t live = system.LiveParticles();
        Check(live > 40, "emitting");
        system.Stop(id);
        Run(system, 0.5f);
        Check(system.LiveParticles() == live && system.IsAlive(id), "after Stop nothing new, the particles live on");
        system.Stop(id, true);
        system.Update(0.016f);
        Check(system.LiveParticles() == 0 && !system.IsAlive(id), "Stop(kill): gone at once");
        Check(!system.IsAlive(id), "a dead effect's id is dead for good");
        const EffectId again = system.Spawn("loop");
        Check(again.IsValid() && !system.IsAlive(id) && system.IsAlive(again), "a new effect in the same slot is another id");
    }

    void TestFollowingASource()
    {
        ParticleSystem system = MakeSystem(kLoop);
        SpawnParams params;
        params.position = {0.0f, 0.0f, 0.0f};
        const EffectId id = system.Spawn("loop", params);
        system.Update(1.0f / 60.0f);
        system.Clear();
        const EffectId moving = system.Spawn("loop", params);
        // one frame in which the source goes 10 m: the particles of that frame lie along the way, not in a heap
        system.SetTransform(moving, {10.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {600.0f, 0.0f, 0.0f});
        SpawnParams fast = params;
        (void)fast;
        ParticleSystem trail = MakeSystem(R"({"name":"t","emitters":[{"loop":true,"rate":3000,"lifetime":[5,5],"speed":0,"size":0.1,"maxParticles":4000}]})");
        const EffectId t = trail.Spawn("t", params);
        trail.SetTransform(t, {10.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {});
        trail.Update(1.0f / 60.0f);
        std::vector<ParticleBatch> batches;
        trail.BuildBatches({0.0f, 0.0f, 0.0f}, batches);
        Check(batches.size() == 1 && batches[0].count >= 40, "the frame emitted its particles");
        float lowest = 1e9f;
        float highest = -1e9f;
        for (std::uint32_t i = 0; i < batches[0].count; ++i)
        {
            const float x = batches[0].instances[static_cast<std::size_t>(i) * 16];
            lowest = std::min(lowest, x);
            highest = std::max(highest, x);
        }
        Check(lowest < 2.0f && highest > 8.0f, "spread along the way of the source (" + std::to_string(lowest) + " .. " + std::to_string(highest) + ")");
        (void)id;
    }

    void TestIntegration()
    {
        // one particle, thrown up at 10 m/s with gravity -10 and no drag: y(1 s) = 5 m, y(2 s) = 0
        ParticleSystem system = MakeSystem(R"({"name":"throw","emitters":[{"bursts":[[0,1]],"lifetime":[5,5],"speed":10,"size":0.2,"gravity":[0,-10,0]}]})");
        SpawnParams params;
        params.direction = {0.0f, 1.0f, 0.0f};
        system.Spawn("throw", params);
        Run(system, 1.0f, 1.0f / 240.0f);
        std::vector<ParticleBatch> batches;
        system.BuildBatches({}, batches);
        Check(batches.size() == 1 && batches[0].count == 1, "one particle");
        Check(std::fabs(batches[0].instances[1] - 5.0f) < 0.1f, "after 1 s it is at 5 m: " + std::to_string(batches[0].instances[1]));

        // drag: the velocity falls as exp(-drag t): 10 m/s with drag 1 travels 10 (1 - exp(-1)) = 6.32 m in a second
        ParticleSystem dragged = MakeSystem(R"({"name":"d","emitters":[{"bursts":[[0,1]],"lifetime":[5,5],"speed":10,"size":0.2,"drag":1}]})");
        dragged.Spawn("d", params);
        Run(dragged, 1.0f, 1.0f / 240.0f);
        dragged.BuildBatches({}, batches);
        Check(std::fabs(batches[0].instances[1] - 6.32f) < 0.15f, "with drag 1 it reaches 6.3 m: " + std::to_string(batches[0].instances[1]));

        // the floor: a bounce, then death at the second touch
        ParticleSystem bouncing = MakeSystem(R"({"name":"b","emitters":[{"bursts":[[0,1]],"lifetime":[20,20],"speed":0,"size":0.2,"gravity":[0,-10,0],"floorY":-1,"bounce":0.5}]})");
        bouncing.Spawn("b", params);
        Run(bouncing, 0.7f);
        Check(bouncing.LiveParticles() == 1, "it bounces once");
        Run(bouncing, 3.0f);
        Check(bouncing.LiveParticles() == 0, "the second touch of the floor ends it");
    }

    void TestBatches()
    {
        ParticleSystem system = MakeSystem(R"({"name":"mixed","emitters":[
            {"name":"glow","blend":"additive","frame":2,"bursts":[[0,3]],"lifetime":[1,1],"speed":0,"size":[0.5,0.5],"intensity":2,
             "color":[[0,[1,0.5,0.25,0.8]]],"stretch":3},
            {"name":"smoke","blend":"alpha","frame":1,"bursts":[[0,3]],"lifetime":[1,1],"speed":0,"size":[1,1],"color":[[0,[0.5,0.5,0.5,0.4]]],"shape":"sphere","shapeRadius":6}]})");
        SpawnParams params;
        params.position = {0.0f, 0.0f, 20.0f};
        params.scale = 2.0f;
        params.tint[0] = 0.5f;
        system.Spawn("mixed", params);
        system.Update(0.0001f);
        std::vector<ParticleBatch> batches;
        system.BuildBatches({0.0f, 0.0f, 0.0f}, batches);
        Check(batches.size() == 2 && batches[0].blend == BlendMode::Additive && batches[1].blend == BlendMode::Alpha, "an additive batch and an alpha batch");
        const ParticleBatch& glow = batches[0];
        Check(glow.count == 3 && glow.instances.size() == 3 * 16, "three particles, 16 floats each");
        Check(Near(glow.instances[3], 1.0f), "size 0.5 x scale 2 = 1");
        Check(Near(glow.instances[8], 1.0f * 0.5f * 2.0f, 0.01f) && Near(glow.instances[9], 0.5f * 2.0f, 0.01f) && Near(glow.instances[11], 0.8f, 0.01f),
            "colour x tint x intensity: rgb x 2, the tint on red, alpha as it is");
        Check(Near(glow.instances[13], 2.0f), "the frame of the atlas");
        // a particle without speed has no direction: it faces the camera even with a stretch
        Check(Near(glow.instances[7], 0.0f), "no speed, no stretch");

        // the alpha batch goes from the far particle to the near one
        const ParticleBatch& smoke = batches[1];
        float previousDistance = 1e9f;
        bool sorted = true;
        for (std::uint32_t i = 0; i < smoke.count; ++i)
        {
            const float* p = smoke.instances.data() + static_cast<std::size_t>(i) * 16;
            const float distance = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
            sorted = sorted && distance <= previousDistance + 1e-3f;
            previousDistance = distance;
        }
        Check(sorted, "the alpha batch is sorted back to front");

        // a streak along the velocity
        ParticleSystem streaks = MakeSystem(R"({"name":"s","emitters":[{"bursts":[[0,1]],"lifetime":[1,1],"speed":10,"size":0.5,"stretch":3,"stretchSpeed":0.1}]})");
        SpawnParams along;
        along.direction = {1.0f, 0.0f, 0.0f};
        streaks.Spawn("s", along);
        streaks.Update(0.0001f);
        streaks.BuildBatches({}, batches);
        Check(Near(batches[0].instances[4], 1.0f, 0.01f) && Near(batches[0].instances[7], 4.0f, 0.05f), "the direction of the velocity and the stretch 3 + 0.1 x 10");
    }

    void TestDeterminism()
    {
        std::vector<float> first;
        for (int run = 0; run < 2; ++run)
        {
            ParticleSystem system = MakeSystem(R"({"name":"r","emitters":[{"loop":true,"rate":300,"lifetime":[0.5,1],"speed":[1,5],"size":[0.1,0.3],
                "shape":"sphere","shapeRadius":1,"rotation":[0,360],"coneAngle":30}]})");
            system.Spawn("r");
            Run(system, 1.5f);
            std::vector<ParticleBatch> batches;
            system.BuildBatches({}, batches);
            if (run == 0)
            {
                first = batches[0].instances;
            }
            else
            {
                Check(first == batches[0].instances && !first.empty(), "the same seed gives the same particles");
            }
        }
    }

    // ---- the presets of the project ----

    void TestProjectEffects(const fs::path& source)
    {
        ParticleSystem system;
        std::vector<std::string> messages;
        const std::uint32_t count = system.LoadEffects(source / "assets/data/fx", &messages);
        std::string all;
        for (const std::string& message : messages)
        {
            all += "\n  " + message;
        }
        Check(messages.empty(), "every preset reads:" + all);
        Check(count >= 9, "the presets of the project (" + std::to_string(count) + ")");
        for (const char* name : {"boost", "ball_trail", "sparks_hit", "goal_explosion", "demolish", "landing", "pad_pickup", "supersonic"})
        {
            Check(system.FindEffect(name) != nullptr, std::string("the preset ") + name);
        }

        for (const std::string& name : system.EffectNames())
        {
            if (name == "stress_5000")
            {
                continue;
            }
            ParticleSystem single;
            single.LoadEffects(source / "assets/data/fx");
            single.SetSeed(5);
            SpawnParams params;
            params.position = {0.0f, 1.0f, 0.0f};
            params.velocity = {20.0f, 0.0f, 0.0f};
            const EffectId id = single.Spawn(name, params);
            Check(id.IsValid(), name + " spawns");
            std::uint32_t peak = 0;
            for (int frame = 0; frame < 60 * 3; ++frame)
            {
                single.Update(1.0f / 60.0f);
                peak = std::max(peak, single.LiveParticles());
            }
            Check(peak > 0 && peak <= 1500, name + ": particles appear and stay within reason (peak " + std::to_string(peak) + ")");
            single.Stop(id);
            for (int frame = 0; frame < 60 * 8; ++frame)
            {
                single.Update(1.0f / 60.0f);
            }
            Check(single.LiveParticles() == 0 && !single.IsAlive(id), name + ": after Stop everything dies out within 8 s");
        }
    }

    // ---- the cost of 5000 particles ----

    void TestCost(const fs::path& source)
    {
        ParticleSystem system;
        system.LoadEffects(source / "assets/data/fx");
        system.SetSeed(1);
        system.Spawn("stress_5000");
        for (int frame = 0; frame < 60 * 4; ++frame)
        {
            system.Update(1.0f / 60.0f);
        }
        Check(system.LiveParticles() >= 4800 && system.LiveParticles() <= 5000, "the stress effect holds about 5000 live particles: " + std::to_string(system.LiveParticles()));

        std::vector<double> update;
        std::vector<double> batch;
        std::vector<ParticleBatch> batches;
        for (int frame = 0; frame < 300; ++frame)
        {
            const auto a = std::chrono::steady_clock::now();
            system.Update(1.0f / 60.0f);
            const auto b = std::chrono::steady_clock::now();
            system.BuildBatches({0.0f, 5.0f, -20.0f}, batches);
            const auto c = std::chrono::steady_clock::now();
            update.push_back(std::chrono::duration<double, std::milli>(b - a).count());
            batch.push_back(std::chrono::duration<double, std::milli>(c - b).count());
        }
        std::sort(update.begin(), update.end());
        std::sort(batch.begin(), batch.end());
        const double updateMs = update[update.size() / 2];
        const double batchMs = batch[batch.size() / 2];
        std::cout << "5000 particles: simulation " << updateMs << " ms, instance data " << batchMs << " ms (median of 300 frames), "
                  << system.LiveParticles() << " live\n";
#ifdef _DEBUG
        constexpr double kBudgetMs = 10.0;  // a Debug build has no optimisation: about ten times slower than the 0.5 ms that RelWithDebInfo and Release keep
#else
        constexpr double kBudgetMs = 0.5;
#endif
        Check(updateMs + batchMs <= kBudgetMs, "5000 particles cost at most " + std::to_string(kBudgetMs) + " ms of CPU (" + std::to_string(updateMs + batchMs) + " ms)");
    }
}

int main()
{
    try
    {
        const fs::path source = fs::u8path(MYENGINE_SOURCE_DIR);
        TestParser();
        TestCurves();
        TestBurstAndLife();
        TestRate();
        TestBudgetAndLimits();
        TestStop();
        TestFollowingASource();
        TestIntegration();
        TestBatches();
        TestDeterminism();
        TestProjectEffects(source);
        TestCost(source);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "fx tests passed\n";
    return 0;
}
