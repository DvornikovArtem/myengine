// FxSystem.cpp

#include <myengine/ecs/systems/FxSystem.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

#include <DirectXMath.h>

#include <myengine/audio/AudioSystem.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/FxComponents.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/fx/ParticleSystem.h>
#include <myengine/resource/ResourceManager.h>

#include <tracy/Tracy.hpp>

namespace myengine::ecs::systems
{
    namespace
    {
        constexpr const char* kQuadMesh = "assets/models/fx/quad.obj";
        constexpr const char* kAdditiveMaterial = "assets/materials/fx/particles_additive.material.json";
        constexpr const char* kAlphaMaterial = "assets/materials/fx/particles_alpha.material.json";

        struct CameraPose
        {
            bool found = false;
            components::Vec3 position{};
            audio::Vec3 forward{0.0f, 0.0f, 1.0f};
            audio::Vec3 up{0.0f, 1.0f, 0.0f};
        };

        // The orientation of a camera or an entity as the renderer takes it: the Euler angles in degrees through RollPitchYaw
        void Axes(const components::Vec3& rotationDeg, audio::Vec3& forward, audio::Vec3& up)
        {
            const DirectX::XMMATRIX rotation = DirectX::XMMatrixRotationRollPitchYaw(
                DirectX::XMConvertToRadians(rotationDeg.x), DirectX::XMConvertToRadians(rotationDeg.y), DirectX::XMConvertToRadians(rotationDeg.z));
            DirectX::XMFLOAT3 f{};
            DirectX::XMFLOAT3 u{};
            DirectX::XMStoreFloat3(&f, DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rotation));
            DirectX::XMStoreFloat3(&u, DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), rotation));
            forward = {f.x, f.y, f.z};
            up = {u.x, u.y, u.z};
        }

        CameraPose FindPrimaryCamera(World& world, const core::WindowId windowId, const bool anyWindow)
        {
            CameraPose pose;
            world.ForEach<components::CameraComponent, components::WindowBindingComponent>(
                [&](const EntityId, components::CameraComponent& camera, const components::WindowBindingComponent& binding)
                {
                    if (pose.found || !camera.isPrimary || (!anyWindow && binding.windowId != windowId))
                    {
                        return;
                    }
                    pose.found = true;
                    pose.position = camera.position;
                    Axes(camera.rotationDeg, pose.forward, pose.up);
                });
            return pose;
        }

        // The effects and the voices that the components started with a loop: the ones that were not touched in a frame belong to
        // entities that are gone (the play stopped and began again) and are stopped
        std::unordered_set<std::uint32_t> g_loopingEffects;
        std::unordered_set<std::uint32_t> g_loopingVoices;
        components::Vec3 g_lastListener{};
        bool g_haveListener = false;
    }

    void FxUpdateSystem::Update(World& world, const float deltaTime)
    {
        ZoneScopedN("Fx::UpdateSystem");
        auto& audioSystem = audio::GetAudioSystem();
        auto& particles = fx::GetParticleSystem();
        const float dt = std::max(deltaTime, 1e-4f);

        // the listener is the primary camera
        if (audioSystem.IsInitialized())
        {
            const CameraPose camera = FindPrimaryCamera(world, 0, true);
            if (camera.found)
            {
                audio::ListenerState listener;
                listener.position = {camera.position.x, camera.position.y, camera.position.z};
                listener.forward = camera.forward;
                listener.up = camera.up;
                if (g_haveListener)
                {
                    listener.velocity = {(camera.position.x - g_lastListener.x) / dt, (camera.position.y - g_lastListener.y) / dt,
                        (camera.position.z - g_lastListener.z) / dt};
                    const float speed = std::sqrt(listener.velocity.x * listener.velocity.x + listener.velocity.y * listener.velocity.y +
                        listener.velocity.z * listener.velocity.z);
                    if (speed > 200.0f) // a camera that jumped (a cut) is not a fast listener
                    {
                        listener.velocity = {};
                    }
                }
                g_lastListener = camera.position;
                g_haveListener = true;
                audioSystem.SetListener(listener);
            }
        }

        // the particle emitters
        std::unordered_set<std::uint32_t> loopsNow;
        world.ForEach<components::TransformComponent, components::ParticleEmitterComponent>(
            [&](const EntityId, components::TransformComponent& transform, components::ParticleEmitterComponent& emitter)
            {
                if (!emitter.initialized)
                {
                    emitter.initialized = true;
                    emitter.orbitCenter = transform.position;
                    emitter.lastPosition = transform.position;
                    emitter.timer = emitter.retriggerInterval; // a burst preset starts at once
                }
                if (emitter.orbitRadius > 0.0f)
                {
                    emitter.orbitAngleDeg += emitter.orbitSpeedDeg * dt;
                    const float angle = DirectX::XMConvertToRadians(emitter.orbitAngleDeg);
                    transform.position = {emitter.orbitCenter.x + std::cos(angle) * emitter.orbitRadius, emitter.orbitCenter.y,
                        emitter.orbitCenter.z + std::sin(angle) * emitter.orbitRadius};
                }
                const components::Vec3 velocity = (transform.position - emitter.lastPosition) / dt;
                emitter.lastPosition = transform.position;

                fx::SpawnParams params;
                params.position = {transform.position.x, transform.position.y, transform.position.z};
                params.velocity = {velocity.x, velocity.y, velocity.z};
                params.scale = emitter.scale;
                params.intensity = emitter.intensity;
                params.rateScale = emitter.rateScale;
                std::copy(emitter.tint, emitter.tint + 4, params.tint);
                if (emitter.directionLocal)
                {
                    audio::Vec3 forward;
                    audio::Vec3 up;
                    Axes(transform.rotationDeg, forward, up);
                    params.direction = {forward.x, forward.y, forward.z};
                }
                else
                {
                    params.direction = {emitter.direction.x, emitter.direction.y, emitter.direction.z};
                }

                const fx::EffectId handle{emitter.handle};
                if (!emitter.playing)
                {
                    if (handle.IsValid())
                    {
                        particles.Stop(handle);
                        emitter.handle = 0;
                    }
                    return;
                }
                if (emitter.retriggerInterval > 0.0f)
                {
                    emitter.timer += dt;
                    if (emitter.timer >= emitter.retriggerInterval)
                    {
                        emitter.timer = 0.0f;
                        particles.Spawn(emitter.effect, params);
                    }
                    return;
                }
                if (handle.IsValid() && particles.IsAlive(handle))
                {
                    particles.SetTransform(handle, params.position, params.direction, params.velocity);
                    particles.SetRateScale(handle, emitter.rateScale);
                    particles.SetTint(handle, emitter.tint);
                    loopsNow.insert(emitter.handle);
                }
                else
                {
                    emitter.handle = particles.Spawn(emitter.effect, params).value;
                    if (emitter.handle != 0)
                    {
                        loopsNow.insert(emitter.handle);
                    }
                }
            });
        for (const std::uint32_t handle : g_loopingEffects)
        {
            if (loopsNow.find(handle) == loopsNow.end())
            {
                particles.Stop(fx::EffectId{handle});
            }
        }
        g_loopingEffects = std::move(loopsNow);

        // the audio sources
        std::unordered_set<std::uint32_t> voicesNow;
        world.ForEach<components::TransformComponent, components::AudioSourceComponent>(
            [&](const EntityId, components::TransformComponent& transform, components::AudioSourceComponent& source)
            {
                if (!audioSystem.IsInitialized())
                {
                    return;
                }
                if (!source.initialized)
                {
                    source.initialized = true;
                    source.lastPosition = transform.position;
                    source.timer = source.retriggerInterval;
                }
                const components::Vec3 velocity = (transform.position - source.lastPosition) / dt;
                source.lastPosition = transform.position;
                const audio::VoiceHandle voice{source.voice};

                if (!source.playing)
                {
                    if (voice.IsValid())
                    {
                        audioSystem.Stop(voice, 0.1f);
                        source.voice = 0;
                    }
                    source.played = false;
                    return;
                }
                audio::EventParams params;
                params.hasPosition = source.spatial;
                params.position = {transform.position.x, transform.position.y, transform.position.z};
                params.velocity = {velocity.x, velocity.y, velocity.z};
                params.volumeScale = source.volume;
                params.pitchScale = source.pitch;

                if (source.retriggerInterval > 0.0f)
                {
                    source.timer += dt;
                    if (source.timer >= source.retriggerInterval)
                    {
                        source.timer = 0.0f;
                        audioSystem.PlayEvent(source.event, params);
                    }
                    return;
                }
                if (voice.IsValid() && audioSystem.IsPlaying(voice))
                {
                    audioSystem.SetVoicePosition(voice, params.position, params.velocity);
                    audioSystem.SetVoiceVolume(voice, source.volume);
                    audioSystem.SetVoicePitch(voice, source.pitch);
                    voicesNow.insert(source.voice);
                }
                else if (!source.played || !voice.IsValid())
                {
                    // a loop starts (again); a one-shot plays once and the component stays "played" until it is switched off
                    if (!source.played)
                    {
                        source.voice = audioSystem.PlayEvent(source.event, params).value;
                        source.played = true;
                        if (source.voice != 0)
                        {
                            voicesNow.insert(source.voice);
                        }
                    }
                }
            });
        for (const std::uint32_t handle : g_loopingVoices)
        {
            if (voicesNow.find(handle) == voicesNow.end())
            {
                audioSystem.Stop(audio::VoiceHandle{handle}, 0.1f);
            }
        }
        g_loopingVoices = std::move(voicesNow);

        particles.Update(deltaTime);
        audioSystem.Update(deltaTime);
    }

    void FxRenderSystem::Render(World& world, const RenderFrameContext& context)
    {
        ZoneScopedN("Fx::Render");
        auto& particles = fx::GetParticleSystem();
        if (context.resourceManager == nullptr || !context.surface.IsValid() || particles.LiveParticles() == 0)
        {
            return;
        }
        const CameraPose camera = FindPrimaryCamera(world, context.windowId, false);
        if (!camera.found)
        {
            return;
        }

        std::vector<fx::ParticleBatch> batches;
        particles.BuildBatches({camera.position.x, camera.position.y, camera.position.z}, batches);
        if (batches.empty())
        {
            return;
        }
        auto mesh = context.resourceManager->Load<resource::MeshAsset>(kQuadMesh);
        if (mesh == nullptr || !mesh->asset.gpuHandle.IsValid())
        {
            return;
        }
        for (const fx::ParticleBatch& batch : batches)
        {
            auto material = context.resourceManager->Load<resource::MaterialAsset>(batch.blend == render::BlendMode::Alpha ? kAlphaMaterial : kAdditiveMaterial);
            if (material == nullptr)
            {
                continue;
            }
            auto shader = context.resourceManager->Load<resource::ShaderAsset>(material->asset.shaderPath);
            if (shader == nullptr || !shader->asset.gpuHandle.IsValid())
            {
                continue;
            }
            render::DrawItem item;
            item.mesh = mesh->asset.gpuHandle;
            item.shader = shader->asset.gpuHandle;
            item.material = context.resourceManager->GetMaterialParams(material);
            item.blend = material->asset.blend;
            item.doubleSided = true;
            item.castShadow = false;
            item.boundsCenter = batch.center;
            item.boundsRadius = batch.radius;
            if (item.material == nullptr)
            {
                continue;
            }
            context.renderAdapter.DrawInstances(context.surface, item, batch.instances.data(), batch.count);
        }
    }
}
