// ParticleSystem.cpp

#include <myengine/fx/ParticleSystem.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <tracy/Tracy.hpp>

namespace myengine::fx
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        using Float3 = render::Float3;

        float Dot(const Float3& a, const Float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        Float3 Cross(const Float3& a, const Float3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        float Length(const Float3& a) { return std::sqrt(Dot(a, a)); }

        Float3 Normalized(const Float3& a, const Float3& fallback = {0.0f, 1.0f, 0.0f})
        {
            const float length = Length(a);
            return length > 1e-6f ? Float3{a.x / length, a.y / length, a.z / length} : fallback;
        }

        // two vectors perpendicular to n and to each other
        void Basis(const Float3& n, Float3& u, Float3& v)
        {
            const Float3 helper = std::fabs(n.y) < 0.99f ? Float3{0.0f, 1.0f, 0.0f} : Float3{1.0f, 0.0f, 0.0f};
            u = Normalized(Cross(helper, n));
            v = Cross(n, u);
        }
    }

    ParticleSystem& GetParticleSystem()
    {
        static ParticleSystem instance;
        return instance;
    }

    // --------------------------------------------------------------------------- presets

    std::uint32_t ParticleSystem::LoadEffects(const std::filesystem::path& directory, std::vector<std::string>* messages)
    {
        std::error_code ec;
        std::uint32_t loaded = 0;
        if (!std::filesystem::is_directory(directory, ec))
        {
            if (messages != nullptr)
            {
                messages->push_back("no folder of effects: " + directory.generic_string());
            }
            return 0;
        }
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
            {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files)
        {
            std::ifstream stream(file, std::ios::binary);
            std::stringstream text;
            text << stream.rdbuf();
            EffectDesc effect;
            std::string error;
            if (!ParseEffectText(text.str(), effect, error))
            {
                if (messages != nullptr)
                {
                    messages->push_back(file.filename().generic_string() + ": " + error);
                }
                continue;
            }
            if (AddEffect(std::move(effect)))
            {
                ++loaded;
            }
        }
        return loaded;
    }

    bool ParticleSystem::AddEffect(EffectDesc effect)
    {
        if (effect.name.empty() || effect.emitters.empty())
        {
            return false;
        }
        const std::string name = effect.name;
        effects_[name] = std::make_shared<const EffectDesc>(std::move(effect));
        return true;
    }

    const EffectDesc* ParticleSystem::FindEffect(const std::string& name) const
    {
        const auto it = effects_.find(name);
        return it == effects_.end() ? nullptr : it->second.get();
    }

    std::vector<std::string> ParticleSystem::EffectNames() const
    {
        std::vector<std::string> names;
        for (const auto& entry : effects_)
        {
            names.push_back(entry.first);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    // --------------------------------------------------------------------------- playing

    ParticleSystem::EffectRuntime* ParticleSystem::Resolve(const EffectId id)
    {
        return const_cast<EffectRuntime*>(static_cast<const ParticleSystem*>(this)->Resolve(id));
    }

    const ParticleSystem::EffectRuntime* ParticleSystem::Resolve(const EffectId id) const
    {
        const std::uint32_t slot = id.value & 0xFFFFu;
        if (slot == 0 || slot > runtimes_.size())
        {
            return nullptr;
        }
        const EffectRuntime& runtime = runtimes_[slot - 1];
        return runtime.alive && runtime.generation == (id.value >> 16) ? &runtime : nullptr;
    }

    EffectId ParticleSystem::Spawn(const std::string& name, const SpawnParams& params)
    {
        const auto found = effects_.find(name);
        if (found == effects_.end())
        {
            return {};
        }
        std::size_t slot = runtimes_.size();
        for (std::size_t i = 0; i < runtimes_.size(); ++i)
        {
            if (!runtimes_[i].alive)
            {
                slot = i;
                break;
            }
        }
        if (slot == runtimes_.size())
        {
            if (runtimes_.size() >= 0xFFFEu)
            {
                return {};
            }
            runtimes_.emplace_back();
        }
        EffectRuntime& runtime = runtimes_[slot];
        const std::uint32_t generation = runtime.generation;
        runtime = EffectRuntime{};
        runtime.generation = generation;
        runtime.desc = found->second;
        runtime.params = params;
        runtime.params.direction = Normalized(params.direction);
        runtime.lastPosition = params.position;
        runtime.alive = true;
        for (const EmitterDesc& desc : runtime.desc->emitters)
        {
            EmitterRuntime emitter;
            emitter.desc = &desc;
            runtime.emitters.push_back(emitter);
        }
        ++stats_.effectsStarted;
        return EffectId{(runtime.generation << 16) | static_cast<std::uint32_t>(slot + 1)};
    }

    void ParticleSystem::Stop(const EffectId id, const bool killParticles)
    {
        EffectRuntime* runtime = Resolve(id);
        if (runtime == nullptr)
        {
            return;
        }
        runtime->stopped = true;
        if (killParticles)
        {
            const std::uint32_t slot = id.value & 0xFFFFu;
            for (Particle& particle : particles_)
            {
                if (particle.effectSlot == slot - 1)
                {
                    particle.age = particle.life; // dies in the next update
                }
            }
        }
    }

    bool ParticleSystem::IsAlive(const EffectId id) const
    {
        return Resolve(id) != nullptr;
    }

    void ParticleSystem::SetTransform(const EffectId id, const Float3& position, const Float3& direction, const Float3& velocity)
    {
        if (EffectRuntime* runtime = Resolve(id))
        {
            runtime->params.position = position;
            runtime->params.direction = Normalized(direction, runtime->params.direction);
            runtime->params.velocity = velocity;
        }
    }

    void ParticleSystem::SetRateScale(const EffectId id, const float scale)
    {
        if (EffectRuntime* runtime = Resolve(id))
        {
            runtime->params.rateScale = std::max(scale, 0.0f);
        }
    }

    void ParticleSystem::SetTint(const EffectId id, const float tint[4])
    {
        if (EffectRuntime* runtime = Resolve(id))
        {
            std::copy(tint, tint + 4, runtime->params.tint);
        }
    }

    void ParticleSystem::Clear()
    {
        particles_.clear();
        for (EffectRuntime& runtime : runtimes_)
        {
            runtime.alive = false;
            ++runtime.generation;
            if ((runtime.generation & 0xFFFFu) == 0)
            {
                runtime.generation = 1;
            }
        }
    }

    // --------------------------------------------------------------------------- random

    std::uint32_t ParticleSystem::NextRandom()
    {
        random_ ^= random_ << 13;
        random_ ^= random_ >> 17;
        random_ ^= random_ << 5;
        return random_;
    }

    float ParticleSystem::Rand01()
    {
        return static_cast<float>(NextRandom() & 0xFFFFFFu) / static_cast<float>(0x1000000u);
    }

    float ParticleSystem::RandRange(const float low, const float high)
    {
        return high <= low ? low : low + (high - low) * Rand01();
    }

    // --------------------------------------------------------------------------- simulation

    void ParticleSystem::SpawnParticle(const std::uint32_t effectSlot, const std::uint16_t emitterIndex, const float origin[3])
    {
        EffectRuntime& effect = runtimes_[effectSlot];
        EmitterRuntime& emitter = effect.emitters[emitterIndex];
        const EmitterDesc& desc = *emitter.desc;
        if (particles_.size() >= budget_ || emitter.live >= desc.maxParticles)
        {
            ++stats_.dropped;
            return;
        }
        const SpawnParams& params = effect.params;
        const float scale = params.scale;

        Particle particle{};
        const Float3 direction = params.direction;
        Float3 u;
        Float3 v;
        Basis(direction, u, v);

        // where, and which way
        Float3 offset{};
        Float3 heading = direction;
        const auto randomUnit = [this]()
        {
            // uniform on the sphere
            const float z = RandRange(-1.0f, 1.0f);
            const float a = RandRange(0.0f, 2.0f * kPi);
            const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            return Float3{r * std::cos(a), z, r * std::sin(a)};
        };
        switch (desc.shape)
        {
            case EmitterShape::Sphere:
            {
                const Float3 unit = randomUnit();
                const float radius = desc.shapeRadius * scale * std::cbrt(Rand01());
                offset = {unit.x * radius, unit.y * radius, unit.z * radius};
                heading = unit;
                break;
            }
            case EmitterShape::Disc:
            {
                const float a = RandRange(0.0f, 2.0f * kPi);
                const float r = desc.shapeRadius * scale * std::sqrt(Rand01());
                offset = {u.x * std::cos(a) * r + v.x * std::sin(a) * r, u.y * std::cos(a) * r + v.y * std::sin(a) * r, u.z * std::cos(a) * r + v.z * std::sin(a) * r};
                break;
            }
            case EmitterShape::Cone:
            {
                const float a = RandRange(0.0f, 2.0f * kPi);
                const float r = desc.shapeRadius * scale * std::sqrt(Rand01());
                offset = {u.x * std::cos(a) * r + v.x * std::sin(a) * r, u.y * std::cos(a) * r + v.y * std::sin(a) * r, u.z * std::cos(a) * r + v.z * std::sin(a) * r};
                break;
            }
            case EmitterShape::Point:
                break;
        }
        if (desc.shape != EmitterShape::Sphere && desc.coneAngleDeg > 0.0f)
        {
            // a direction in the cone: the angle from the axis grows with the square root (an even spread over the cap)
            const float maxAngle = desc.coneAngleDeg * kPi / 180.0f;
            const float cosMax = std::cos(maxAngle);
            const float cosTheta = 1.0f - Rand01() * (1.0f - cosMax);
            const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            const float phi = RandRange(0.0f, 2.0f * kPi);
            heading = Normalized({direction.x * cosTheta + (u.x * std::cos(phi) + v.x * std::sin(phi)) * sinTheta,
                                  direction.y * cosTheta + (u.y * std::cos(phi) + v.y * std::sin(phi)) * sinTheta,
                                  direction.z * cosTheta + (u.z * std::cos(phi) + v.z * std::sin(phi)) * sinTheta});
        }

        const float speed = RandRange(desc.speedMin, desc.speedMax) * scale;
        particle.position[0] = origin[0] + offset.x;
        particle.position[1] = origin[1] + offset.y;
        particle.position[2] = origin[2] + offset.z;
        particle.velocity[0] = heading.x * speed + params.velocity.x * desc.inheritVelocity;
        particle.velocity[1] = heading.y * speed + params.velocity.y * desc.inheritVelocity;
        particle.velocity[2] = heading.z * speed + params.velocity.z * desc.inheritVelocity;
        particle.life = RandRange(desc.lifetimeMin, desc.lifetimeMax);
        particle.size = RandRange(desc.sizeMin, desc.sizeMax) * scale;
        particle.rotation = RandRange(desc.rotationMinDeg, desc.rotationMaxDeg) * kPi / 180.0f;
        particle.rotationSpeed = RandRange(desc.rotationSpeedMinDeg, desc.rotationSpeedMaxDeg) * kPi / 180.0f;
        particle.effectSlot = effectSlot;
        particle.emitterIndex = emitterIndex;
        particles_.push_back(particle);
        ++emitter.live;
        ++stats_.spawned;
        stats_.peakParticles = std::max(stats_.peakParticles, static_cast<std::uint32_t>(particles_.size()));
    }

    void ParticleSystem::UpdateEmitters(const float dt)
    {
        for (std::size_t slot = 0; slot < runtimes_.size(); ++slot)
        {
            EffectRuntime& effect = runtimes_[slot];
            if (!effect.alive)
            {
                continue;
            }
            const float origin[3] = {effect.params.position.x, effect.params.position.y, effect.params.position.z};
            const Float3 last = effect.lastPosition;
            for (std::size_t index = 0; index < effect.emitters.size(); ++index)
            {
                EmitterRuntime& emitter = effect.emitters[index];
                const EmitterDesc& desc = *emitter.desc;
                if (!emitter.finished)
                {
                    const float before = emitter.time;
                    emitter.time += dt;
                    const bool emitting = !effect.stopped;
                    // the bursts
                    while (emitting && emitter.nextBurst < desc.bursts.size() && emitter.time - desc.startDelay >= desc.bursts[emitter.nextBurst].time)
                    {
                        const std::uint32_t count = static_cast<std::uint32_t>(std::lround(desc.bursts[emitter.nextBurst].count * effect.params.intensity));
                        for (std::uint32_t i = 0; i < count; ++i)
                        {
                            SpawnParticle(static_cast<std::uint32_t>(slot), static_cast<std::uint16_t>(index), origin);
                        }
                        ++emitter.nextBurst;
                    }
                    // the rate: the particles of a frame are spread along the way the source went (a trail has no gaps)
                    if (emitting && desc.rate > 0.0f && emitter.time >= desc.startDelay)
                    {
                        float runningDt = dt;
                        if (!desc.loop && desc.duration > 0.0f)
                        {
                            runningDt = std::max(0.0f, std::min(emitter.time, desc.startDelay + desc.duration) - std::max(before, desc.startDelay));
                        }
                        else if (!desc.loop && desc.duration <= 0.0f)
                        {
                            runningDt = 0.0f;
                        }
                        emitter.accumulator += desc.rate * effect.params.rateScale * runningDt;
                        const int count = static_cast<int>(emitter.accumulator);
                        emitter.accumulator -= static_cast<float>(count);
                        for (int i = 0; i < count; ++i)
                        {
                            const float t = (static_cast<float>(i) + Rand01()) / static_cast<float>(count);
                            const float at[3] = {last.x + (origin[0] - last.x) * t, last.y + (origin[1] - last.y) * t, last.z + (origin[2] - last.z) * t};
                            SpawnParticle(static_cast<std::uint32_t>(slot), static_cast<std::uint16_t>(index), at);
                        }
                    }
                    const bool bursting = emitter.nextBurst < desc.bursts.size();
                    const bool running = desc.loop ? !effect.stopped : (desc.duration > 0.0f && emitter.time < desc.startDelay + desc.duration);
                    if (effect.stopped || (!running && !bursting))
                    {
                        emitter.finished = true;
                    }
                }
            }
            effect.lastPosition = effect.params.position;
        }
    }

    // An effect is over when every emitter has finished and none of its particles is alive. Run after the particles are stepped, so
    // that a Stop(kill) ends the effect in the same frame.
    void ParticleSystem::ReapEffects()
    {
        for (EffectRuntime& effect : runtimes_)
        {
            if (!effect.alive)
            {
                continue;
            }
            bool over = true;
            for (const EmitterRuntime& emitter : effect.emitters)
            {
                over = over && emitter.finished && emitter.live == 0;
            }
            if (over)
            {
                effect.alive = false;
                ++effect.generation;
                if ((effect.generation & 0xFFFFu) == 0)
                {
                    effect.generation = 1;
                }
                effect.desc.reset();
                effect.emitters.clear();
            }
        }
    }

    void ParticleSystem::UpdateParticles(const float dt)
    {
        std::size_t write = 0;
        for (std::size_t read = 0; read < particles_.size(); ++read)
        {
            Particle particle = particles_[read];
            EffectRuntime& effect = runtimes_[particle.effectSlot];
            EmitterRuntime& emitter = effect.emitters[particle.emitterIndex];
            const EmitterDesc& desc = *emitter.desc;

            particle.age += dt;
            bool dead = particle.age >= particle.life;
            if (!dead)
            {
                particle.velocity[0] += desc.gravity[0] * dt;
                particle.velocity[1] += desc.gravity[1] * dt;
                particle.velocity[2] += desc.gravity[2] * dt;
                if (desc.drag > 0.0f)
                {
                    const float damping = std::exp(-desc.drag * dt);
                    particle.velocity[0] *= damping;
                    particle.velocity[1] *= damping;
                    particle.velocity[2] *= damping;
                }
                particle.position[0] += particle.velocity[0] * dt;
                particle.position[1] += particle.velocity[1] * dt;
                particle.position[2] += particle.velocity[2] * dt;
                particle.rotation += particle.rotationSpeed * dt;
                if (particle.position[1] < desc.floorY)
                {
                    if (particle.bounced || desc.bounce <= 0.0f)
                    {
                        dead = true;
                    }
                    else
                    {
                        particle.position[1] = desc.floorY;
                        particle.velocity[1] = -particle.velocity[1] * desc.bounce;
                        particle.velocity[0] *= 0.6f;
                        particle.velocity[2] *= 0.6f;
                        particle.bounced = true;
                    }
                }
            }
            if (dead)
            {
                if (emitter.live > 0)
                {
                    --emitter.live;
                }
                continue;
            }
            particles_[write++] = particle;
        }
        particles_.resize(write);
    }

    void ParticleSystem::Update(float deltaTime)
    {
        ZoneScopedN("Fx::Update");
        const float dt = std::clamp(deltaTime, 0.0f, kMaxStep);
        if (dt <= 0.0f && particles_.empty())
        {
            return;
        }
        UpdateEmitters(dt);
        UpdateParticles(dt);
        ReapEffects();
        stats_.liveParticles = static_cast<std::uint32_t>(particles_.size());
    }

    // --------------------------------------------------------------------------- batches

    void ParticleSystem::BuildBatches(const Float3& cameraPosition, std::vector<ParticleBatch>& out) const
    {
        ZoneScopedN("Fx::BuildBatches");
        out.clear();
        if (particles_.empty())
        {
            return;
        }
        ParticleBatch additive;
        additive.blend = render::BlendMode::Additive;
        ParticleBatch alpha;
        alpha.blend = render::BlendMode::Alpha;
        additive.instances.reserve(particles_.size() * 16);

        struct AlphaEntry
        {
            float distanceSquared;
            std::uint32_t index;
        };
        std::vector<AlphaEntry> sorted;

        const auto write = [&](ParticleBatch& batch, const Particle& particle)
        {
            const EffectRuntime& effect = runtimes_[particle.effectSlot];
            const EmitterDesc& desc = *effect.emitters[particle.emitterIndex].desc;
            const float t = particle.life > 0.0f ? particle.age / particle.life : 1.0f;
            float color[4];
            EvaluateGradient(desc.colorOverLife, t, color);
            const float brightness = desc.intensity * effect.params.intensity;
            if (desc.useTint)
            {
                for (int c = 0; c < 4; ++c)
                {
                    color[c] *= effect.params.tint[c];
                }
            }
            const float size = particle.size * EvaluateCurve(desc.sizeOverLife, t);
            float stretch = 0.0f;
            float direction[3] = {0.0f, 1.0f, 0.0f};
            if (desc.stretch > 0.0f || desc.stretchSpeed > 0.0f)
            {
                const float speed = std::sqrt(particle.velocity[0] * particle.velocity[0] + particle.velocity[1] * particle.velocity[1] +
                    particle.velocity[2] * particle.velocity[2]);
                if (speed > 1e-4f)
                {
                    direction[0] = particle.velocity[0] / speed;
                    direction[1] = particle.velocity[1] / speed;
                    direction[2] = particle.velocity[2] / speed;
                    stretch = std::max(desc.stretch + desc.stretchSpeed * speed, 1.0f);
                }
            }
            const float data[16] = {particle.position[0], particle.position[1], particle.position[2], size,
                                    direction[0], direction[1], direction[2], stretch,
                                    color[0] * brightness, color[1] * brightness, color[2] * brightness, color[3],
                                    particle.rotation, static_cast<float>(desc.frame), 0.0f, 0.0f};
            batch.instances.insert(batch.instances.end(), data, data + 16);
            ++batch.count;
            batch.center.x += particle.position[0];
            batch.center.y += particle.position[1];
            batch.center.z += particle.position[2];
        };

        for (std::uint32_t i = 0; i < particles_.size(); ++i)
        {
            const Particle& particle = particles_[i];
            const EmitterDesc& desc = *runtimes_[particle.effectSlot].emitters[particle.emitterIndex].desc;
            if (desc.blend == render::BlendMode::Additive)
            {
                write(additive, particle);
            }
            else
            {
                const float dx = particle.position[0] - cameraPosition.x;
                const float dy = particle.position[1] - cameraPosition.y;
                const float dz = particle.position[2] - cameraPosition.z;
                sorted.push_back({dx * dx + dy * dy + dz * dz, i});
            }
        }
        if (!sorted.empty())
        {
            std::sort(sorted.begin(), sorted.end(), [](const AlphaEntry& a, const AlphaEntry& b) { return a.distanceSquared > b.distanceSquared; });
            alpha.instances.reserve(sorted.size() * 16);
            for (const AlphaEntry& entry : sorted)
            {
                write(alpha, particles_[entry.index]);
            }
        }

        for (ParticleBatch* batch : {&additive, &alpha})
        {
            if (batch->count == 0)
            {
                continue;
            }
            const float inverse = 1.0f / static_cast<float>(batch->count);
            batch->center = {batch->center.x * inverse, batch->center.y * inverse, batch->center.z * inverse};
            float radius = 0.0f;
            for (std::uint32_t i = 0; i < batch->count; ++i)
            {
                const float* p = batch->instances.data() + static_cast<std::size_t>(i) * 16;
                const float dx = p[0] - batch->center.x;
                const float dy = p[1] - batch->center.y;
                const float dz = p[2] - batch->center.z;
                radius = std::max(radius, dx * dx + dy * dy + dz * dz);
            }
            batch->radius = std::sqrt(radius);
            out.push_back(std::move(*batch));
        }
    }

    ParticleStats ParticleSystem::GetStats() const
    {
        ParticleStats stats = stats_;
        stats.liveParticles = static_cast<std::uint32_t>(particles_.size());
        stats.budget = budget_;
        std::uint32_t active = 0;
        for (const EffectRuntime& runtime : runtimes_)
        {
            active += runtime.alive ? 1u : 0u;
        }
        stats.activeEffects = active;
        return stats;
    }
}
