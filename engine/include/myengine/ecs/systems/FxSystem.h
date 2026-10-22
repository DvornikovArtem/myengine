// FxSystem.h
//
// Sound and particles of the scene (G7c). FxUpdateSystem: the audio listener follows the primary camera, the AudioSource and
// ParticleEmitter components of the entities are played, the particles and the sound are stepped. FxRenderSystem: draws the particles
// after the meshes (one instanced draw per blend mode). The services themselves are global (audio::GetAudioSystem(),
// fx::GetParticleSystem()): the game plays through them too, the systems are for the scene.

#pragma once

#include <myengine/ecs/System.h>

namespace myengine::ecs
{
    class World;
}

namespace myengine::ecs::systems
{
    class FxUpdateSystem final : public IUpdateSystem
    {
    public:
        void Update(World& world, float deltaTime) override;
    };

    class FxRenderSystem final : public IRenderSystem
    {
    public:
        void Render(World& world, const RenderFrameContext& context) override;
    };
}
