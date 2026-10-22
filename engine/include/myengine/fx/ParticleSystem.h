// ParticleSystem.h
//
// The CPU particle simulation (docs/render/particles.md): effects (named presets of emitters) are spawned, followed and stopped by
// the game; Update(dt) moves the particles; BuildBatches turns them into the instance data that IRenderAdapter::DrawInstances draws
// with particle.hlsl, one batch (one draw) per blend mode. The budget is 5000 live particles; spawns beyond it are dropped and counted.
// One thread. No renderer, no device in this class: the tests run it as it is.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <myengine/fx/ParticleEffect.h>
#include <myengine/render/RenderTypes.h>

namespace myengine::fx
{
    struct EffectId
    {
        std::uint32_t value = 0;
        bool IsValid() const { return value != 0; }
    };

    struct SpawnParams
    {
        render::Float3 position{};
        render::Float3 direction{0.0f, 1.0f, 0.0f};   // the way the effect points (a flame goes against it, sparks leave along it)
        render::Float3 velocity{};                    // the velocity of the source: moving particles inherit a share of it
        float scale = 1.0f;                           // sizes, distances and speeds
        float intensity = 1.0f;                       // the brightness and the amount of the bursts
        float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};     // multiplies the colour (the colour of a team)
        float rateScale = 1.0f;                       // the rate of continuous emission (the boost amount)
    };

    // The particles of one blend mode as the instance data of particle.hlsl: 16 floats each
    struct ParticleBatch
    {
        render::BlendMode blend = render::BlendMode::Additive;
        std::vector<float> instances;
        std::uint32_t count = 0;
        render::Float3 center{};
        float radius = 0.0f;
    };

    struct ParticleStats
    {
        std::uint32_t liveParticles = 0;
        std::uint32_t activeEffects = 0;
        std::uint32_t budget = 0;
        std::uint64_t spawned = 0;       // particles created
        std::uint64_t dropped = 0;       // not created: the budget or the limit of the emitter
        std::uint64_t effectsStarted = 0;
        std::uint32_t peakParticles = 0;
    };

    class ParticleSystem
    {
    public:
        static constexpr std::uint32_t kDefaultBudget = 5000;
        static constexpr float kMaxStep = 0.1f;       // a longer frame is simulated as 0.1 s (a hitch does not make an explosion)

        void SetBudget(std::uint32_t maxParticles) { budget_ = maxParticles; }
        std::uint32_t Budget() const { return budget_; }
        void SetSeed(std::uint32_t seed) { random_ = seed | 1u; }

        // ---- the presets ----
        // Every *.json of the folder; returns how many effects were read. `messages` gets the errors of the files that were not.
        std::uint32_t LoadEffects(const std::filesystem::path& directory, std::vector<std::string>* messages = nullptr);
        bool AddEffect(EffectDesc effect);       // by its name; replaces an effect of that name (not the ones already playing)
        const EffectDesc* FindEffect(const std::string& name) const;
        std::vector<std::string> EffectNames() const;

        // ---- playing ----
        // An invalid id: no such effect. The effect lives until its emitters are done and their particles have died (or Stop).
        EffectId Spawn(const std::string& name, const SpawnParams& params = {});
        // Stops the emission; the particles in the air live out their lives (or die at once with killParticles)
        void Stop(EffectId effect, bool killParticles = false);
        bool IsAlive(EffectId effect) const;
        // A continuing effect follows its source: the position, the direction, the velocity (the particles that are there stay where they are)
        void SetTransform(EffectId effect, const render::Float3& position, const render::Float3& direction, const render::Float3& velocity);
        void SetRateScale(EffectId effect, float scale);
        void SetTint(EffectId effect, const float tint[4]);

        void Update(float deltaTime);
        void Clear();

        // The instance data of the live particles, one batch per blend mode that has any. The alpha batch is sorted back to front.
        void BuildBatches(const render::Float3& cameraPosition, std::vector<ParticleBatch>& out) const;

        ParticleStats GetStats() const;
        std::uint32_t LiveParticles() const { return static_cast<std::uint32_t>(particles_.size()); }

    private:
        struct Particle
        {
            float position[3];
            float velocity[3];
            float age = 0.0f;
            float life = 1.0f;
            float size = 0.2f;
            float rotation = 0.0f;
            float rotationSpeed = 0.0f;
            std::uint32_t effectSlot = 0;
            std::uint16_t emitterIndex = 0;
            bool bounced = false;
        };

        struct EmitterRuntime
        {
            const EmitterDesc* desc = nullptr;
            float time = 0.0f;
            float accumulator = 0.0f;
            std::size_t nextBurst = 0;
            std::uint32_t live = 0;
            bool finished = false;
        };

        struct EffectRuntime
        {
            std::shared_ptr<const EffectDesc> desc;      // alive while it plays, even if the preset is replaced
            SpawnParams params;
            render::Float3 lastPosition{};
            std::vector<EmitterRuntime> emitters;
            std::uint32_t generation = 1;
            bool alive = false;
            bool stopped = false;
        };

        void SpawnParticle(std::uint32_t effectSlot, std::uint16_t emitterIndex, const float origin[3]);
        void UpdateEmitters(float dt);
        void UpdateParticles(float dt);
        void ReapEffects();
        std::uint32_t NextRandom();
        float Rand01();
        float RandRange(float low, float high);
        EffectRuntime* Resolve(EffectId id);
        const EffectRuntime* Resolve(EffectId id) const;

        std::uint32_t budget_ = kDefaultBudget;
        std::uint32_t random_ = 0x9E3779B9u;
        std::unordered_map<std::string, std::shared_ptr<const EffectDesc>> effects_;
        std::vector<EffectRuntime> runtimes_;
        std::vector<Particle> particles_;
        ParticleStats stats_;
    };

    // The particle system of the process: Application loads the presets, FxUpdateSystem steps it, FxRenderSystem draws it, the game spawns into it
    ParticleSystem& GetParticleSystem();
}
