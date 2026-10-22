// FxComponents.h
//
// The sound and the particles of an entity, played by FxSystem (docs/engine/audio.md, docs/render/particles.md): a particle emitter
// that plays a preset of assets/data/fx at the entity, and an audio source that plays an event of assets/data/audio_bank.json at it.

#pragma once

#include <cstdint>
#include <string>

#include <myengine/ecs/Component.h>
#include <myengine/ecs/components/Vector3.h>

namespace myengine::ecs::components
{
    struct ParticleEmitterComponent : Component
    {
        std::string effect;                  // the name of a preset: "boost", "goal_explosion", ...
        bool playing = true;
        // 0: a looping effect plays as long as `playing`; > 0: the effect is started again every so many seconds (for one-shot presets)
        float retriggerInterval = 0.0f;
        float scale = 1.0f;
        float intensity = 1.0f;
        float rateScale = 1.0f;
        float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        // The way the effect points, in the space of the entity: +Z of the entity rotated by its transform when directionLocal is on,
        // else this vector in the world
        Vec3 direction{0.0f, 1.0f, 0.0f};
        bool directionLocal = false;
        // A demo of a moving source (the ball trail in the showcase): the entity goes round a circle of this radius at this angular speed
        float orbitRadius = 0.0f;
        float orbitSpeedDeg = 90.0f;

        // runtime, not saved
        std::uint32_t handle = 0;            // fx::EffectId of the playing effect
        float timer = 0.0f;
        Vec3 lastPosition{};
        Vec3 orbitCenter{};
        float orbitAngleDeg = 0.0f;
        bool initialized = false;
    };

    struct AudioSourceComponent : Component
    {
        std::string event;                   // the name of an event of the sound bank
        bool playing = true;
        float retriggerInterval = 0.0f;      // 0: a loop plays on, a one-shot plays once; > 0: played again every so many seconds
        float volume = 1.0f;
        float pitch = 1.0f;
        bool spatial = true;                 // at the position of the entity (the event must be a 3D one)

        // runtime, not saved
        std::uint32_t voice = 0;             // audio::VoiceHandle
        float timer = 0.0f;
        bool played = false;
        Vec3 lastPosition{};
        bool initialized = false;
    };
}
