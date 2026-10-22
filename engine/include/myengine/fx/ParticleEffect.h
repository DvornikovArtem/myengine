// ParticleEffect.h
//
// The description of a particle effect, read from assets/data/fx/<name>.json (docs/render/particles.md): emitters with their
// emission, the start values of a particle and its curves over the lifetime. Plain data; the parser needs no device.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <myengine/render/RenderTypes.h>

namespace myengine::fx
{
    struct CurveKey
    {
        float t = 0.0f;      // 0 .. 1 of the lifetime
        float value = 1.0f;
    };

    struct ColorKey
    {
        float t = 0.0f;
        float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct Burst
    {
        float time = 0.0f;   // seconds after the start of the emitter
        std::uint32_t count = 0;
    };

    // Where particles appear and the way they leave
    enum class EmitterShape : std::uint8_t
    {
        Point,    // at the origin, in a cone around the direction
        Sphere,   // inside a sphere of `shapeRadius`, away from its centre (explosions)
        Disc,     // on a disc of `shapeRadius` perpendicular to the direction, along the direction (flames, rings)
        Cone,     // like Point, with the origin spread over `shapeRadius`
    };

    struct EmitterDesc
    {
        std::string name;
        render::BlendMode blend = render::BlendMode::Additive;
        int frame = 0;                               // the frame of the sprite atlas (0 dot, 1 smoke, 2 streak, 3 ring, 4 confetti, 5 star)
        std::uint32_t maxParticles = 200;            // live at once from this emitter
        // emission
        float rate = 0.0f;                           // particles per second while it runs
        std::vector<Burst> bursts;                   // and these at their times
        bool loop = false;                           // runs until stopped; otherwise for `duration` seconds (0: only the bursts)
        float duration = 0.0f;
        float startDelay = 0.0f;
        // the start of a particle
        EmitterShape shape = EmitterShape::Point;
        float shapeRadius = 0.0f;
        float coneAngleDeg = 0.0f;                   // half angle around the direction
        float lifetimeMin = 1.0f;
        float lifetimeMax = 1.0f;
        float speedMin = 0.0f;
        float speedMax = 0.0f;
        float inheritVelocity = 0.0f;                // the share of the velocity of the effect that a particle gets
        float sizeMin = 0.2f;
        float sizeMax = 0.2f;
        float rotationMinDeg = 0.0f;
        float rotationMaxDeg = 0.0f;
        float rotationSpeedMinDeg = 0.0f;
        float rotationSpeedMaxDeg = 0.0f;
        // motion
        float gravity[3] = {0.0f, 0.0f, 0.0f};       // m/s^2
        float drag = 0.0f;                           // 1/s: the velocity falls as exp(-drag t)
        float floorY = -1.0e30f;                     // a particle that falls below it bounces once with `bounce` and dies on the second touch
        float bounce = 0.0f;
        // look over the lifetime
        std::vector<CurveKey> sizeOverLife;          // a factor of the start size
        std::vector<ColorKey> colorOverLife;         // the colour of the sprite; rgb may be above 1 (glows and blooms)
        float intensity = 1.0f;                      // multiplies rgb
        float stretch = 0.0f;                        // > 0 (or stretchSpeed > 0): a streak along the velocity this many times longer than wide
        float stretchSpeed = 0.0f;                   // + this per m/s of speed
        bool useTint = true;                         // multiplied by the tint of the spawn (the colour of a team)
        bool sortBackToFront = true;                 // alpha blended only
    };

    struct EffectDesc
    {
        std::string name;
        std::vector<EmitterDesc> emitters;
    };

    // false: `error` says what is wrong (a field of the wrong type, no emitters, a lifetime of 0, an unknown blend)
    bool ParseEffectText(const std::string& jsonText, EffectDesc& out, std::string& error);

    // The value of a curve / gradient at t (clamped to its ends; an empty curve is 1 / white)
    float EvaluateCurve(const std::vector<CurveKey>& keys, float t);
    void EvaluateGradient(const std::vector<ColorKey>& keys, float t, float out[4]);
}
