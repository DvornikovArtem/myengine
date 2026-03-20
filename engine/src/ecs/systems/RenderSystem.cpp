// RenderSystem.cpp

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <DirectXMath.h>

#include <myengine/core/Logger.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/ecs/systems/RenderSystem.h>
#include <myengine/resource/ResourceManager.h>
#include <myengine/spatial/UniformGrid2D.h>

namespace myengine::ecs::systems
{
    namespace
    {
        render::Matrix4 ToRenderMatrix(const DirectX::XMMATRIX& matrix)
        {
            DirectX::XMFLOAT4X4 value{};
            DirectX::XMStoreFloat4x4(&value, matrix);

            render::Matrix4 result{};
            std::memcpy(result.data.data(), &value, sizeof(float) * 16);
            return result;
        }

        DirectX::XMMATRIX BuildLocalMatrix(const components::TransformComponent& transform)
        {
            return DirectX::XMMatrixScaling(transform.scale.x, transform.scale.y, transform.scale.z) *
                DirectX::XMMatrixRotationRollPitchYaw(
                    DirectX::XMConvertToRadians(transform.rotationDeg.x),
                    DirectX::XMConvertToRadians(transform.rotationDeg.y),
                    DirectX::XMConvertToRadians(transform.rotationDeg.z)) *
                DirectX::XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);
        }

        DirectX::XMMATRIX ResolveWorldMatrix(
            const EntityId entity,
            World& world,
            std::unordered_map<EntityId, DirectX::XMFLOAT4X4>& cache,
            std::unordered_set<EntityId>& visiting,
            std::unordered_set<EntityId>& cycleWarningLogged,
            const RenderFrameContext& context)
        {
            if (const auto cacheIt = cache.find(entity); cacheIt != cache.end())
            {
                return DirectX::XMLoadFloat4x4(&cacheIt->second);
            }

            const auto* transform = world.TryGet<components::TransformComponent>(entity);
            if (transform == nullptr)
            {
                return DirectX::XMMatrixIdentity();
            }

            if (!visiting.insert(entity).second)
            {
                if (context.logger != nullptr && cycleWarningLogged.insert(entity).second)
                {
                    context.logger->Warning(
                        "RenderSystem: hierarchy cycle detected for entity=" + std::to_string(entity) + ". Local transform is used.");
                }
                return BuildLocalMatrix(*transform);
            }

            const DirectX::XMMATRIX local = BuildLocalMatrix(*transform);
            DirectX::XMMATRIX worldMatrix = local;

            if (const auto* hierarchy = world.TryGet<components::HierarchyComponent>(entity);
                hierarchy != nullptr && hierarchy->parent != kInvalidEntity)
            {
                const EntityId parent = hierarchy->parent;
                if (world.IsAlive(parent) && world.Has<components::TransformComponent>(parent))
                {
                    if (visiting.find(parent) != visiting.end())
                    {
                        if (context.logger != nullptr && cycleWarningLogged.insert(entity).second)
                        {
                            context.logger->Warning(
                                "RenderSystem: parent loop detected for entity=" + std::to_string(entity) + ". Local transform is used.");
                        }
                    }
                    else
                    {
                        const DirectX::XMMATRIX parentWorld =
                            ResolveWorldMatrix(parent, world, cache, visiting, cycleWarningLogged, context);
                        worldMatrix = DirectX::XMMatrixMultiply(local, parentWorld);
                    }
                }
            }

            visiting.erase(entity);

            DirectX::XMFLOAT4X4 cached{};
            DirectX::XMStoreFloat4x4(&cached, worldMatrix);
            cache[entity] = cached;
            return worldMatrix;
        }

        DirectX::XMFLOAT3 ExtractTranslation(const DirectX::XMMATRIX& matrix)
        {
            DirectX::XMFLOAT4X4 value{};
            DirectX::XMStoreFloat4x4(&value, matrix);
            return {value._41, value._42, value._43};
        }
    }

    void RenderSystem::Render(World& world, const RenderFrameContext& context)
    {
        if (!context.surface.IsValid())
        {
            return;
        }

        const float aspect = context.windowHeight == 0
            ? 1.0f
            : static_cast<float>(context.windowWidth) / static_cast<float>(context.windowHeight);

        DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixIdentity();
        DirectX::XMMATRIX projectionMatrix = DirectX::XMMatrixIdentity();

        float visibleHalfHeight = 1.0f;
        float visibleHalfWidth = aspect;
        float cameraPositionX = 0.0f;
        float cameraPositionY = 0.0f;

        bool cameraFound = false;
        world.ForEach<components::CameraComponent, components::WindowBindingComponent>(
            [&](const EntityId, components::CameraComponent& camera, const components::WindowBindingComponent& binding)
            {
                if (cameraFound || binding.windowId != context.windowId || !camera.isPrimary)
                {
                    return;
                }

                const float clampedFovYDeg = std::clamp(camera.fovYDeg, 20.0f, 120.0f);
                const float fovYRad = DirectX::XMConvertToRadians(clampedFovYDeg);
                const float nearPlane = std::max(camera.nearPlane, 0.001f);
                const float farPlane = std::max(camera.farPlane, nearPlane + 0.1f);

                cameraPositionX = camera.position.x;
                cameraPositionY = camera.position.y;

                const DirectX::XMMATRIX rotation = DirectX::XMMatrixRotationRollPitchYaw(
                    DirectX::XMConvertToRadians(camera.rotationDeg.x),
                    DirectX::XMConvertToRadians(camera.rotationDeg.y),
                    DirectX::XMConvertToRadians(camera.rotationDeg.z));

                DirectX::XMVECTOR forward = DirectX::XMVector3TransformNormal(
                    DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rotation);
                forward = DirectX::XMVector3Normalize(forward);

                DirectX::XMVECTOR up = DirectX::XMVector3TransformNormal(
                    DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), rotation);
                up = DirectX::XMVector3Normalize(up);

                const DirectX::XMVECTOR eye = DirectX::XMVectorSet(camera.position.x, camera.position.y, camera.position.z, 1.0f);
                viewMatrix = DirectX::XMMatrixLookToLH(eye, forward, up);

                projectionMatrix = DirectX::XMMatrixPerspectiveFovLH(
                    fovYRad,
                    std::max(aspect, 0.01f),
                    nearPlane,
                    farPlane);

                const float cullingDepth = std::min(farPlane, 24.0f);
                const float halfHeightAtCullDepth = std::tan(fovYRad * 0.5f) * cullingDepth;
                visibleHalfHeight = std::max(1.0f, halfHeightAtCullDepth);
                visibleHalfWidth = std::max(1.0f, halfHeightAtCullDepth * aspect);

                cameraFound = true;
            });

        std::unordered_map<EntityId, DirectX::XMFLOAT4X4> worldMatrixCache;
        std::unordered_set<EntityId> visiting;
        spatial::UniformGrid2D grid(1.0f);
        std::vector<EntityId> indexedEntities;
        std::unordered_set<EntityId> uniqueEntities;
        std::vector<render::DrawItem> drawItems;

        world.ForEach<components::TransformComponent, components::MeshRendererComponent>(
            [&](const EntityId entity, components::TransformComponent&, components::MeshRendererComponent&)
            {
                if (const auto* binding = world.TryGet<components::WindowBindingComponent>(entity);
                    binding != nullptr && binding->windowId != context.windowId)
                {
                    return;
                }

                const DirectX::XMMATRIX worldMatrix = ResolveWorldMatrix(entity, world, worldMatrixCache, visiting, hierarchyCycleWarningLogged_, context);

                const DirectX::XMFLOAT3 position = ExtractTranslation(worldMatrix);
                grid.Insert(entity, position.x, position.y);
            });

        const float minX = cameraPositionX - visibleHalfWidth;
        const float maxX = cameraPositionX + visibleHalfWidth;
        const float minY = cameraPositionY - visibleHalfHeight;
        const float maxY = cameraPositionY + visibleHalfHeight;

        const std::vector<EntityId> candidateEntities = grid.Query(minX, minY, maxX, maxY);
        indexedEntities.reserve(candidateEntities.size());
        for (const EntityId entity : candidateEntities)
        {
            if (uniqueEntities.insert(entity).second)
            {
                indexedEntities.push_back(entity);
            }
        }

        if (context.resourceManager != nullptr)
        {
            for (const EntityId entity : indexedEntities)
            {
                if (!world.Has<components::MeshRendererComponent>(entity))
                {
                    continue;
                }

                auto& renderer = world.Get<components::MeshRendererComponent>(entity);
                if (!renderer.visible || renderer.meshPath.empty() || renderer.materialPath.empty())
                {
                    continue;
                }

                auto meshResource = context.resourceManager->Load<resource::MeshAsset>(renderer.meshPath);
                auto materialResource = context.resourceManager->Load<resource::MaterialAsset>(renderer.materialPath);
                if (meshResource == nullptr || materialResource == nullptr)
                {
                    continue;
                }

                auto shaderResource = context.resourceManager->Load<resource::ShaderAsset>(materialResource->asset.shaderPath);
                auto textureResource = context.resourceManager->Load<resource::TextureAsset>(materialResource->asset.texturePath);
                if (shaderResource == nullptr || textureResource == nullptr)
                {
                    continue;
                }
                if (!meshResource->asset.gpuHandle.IsValid() ||
                    !shaderResource->asset.gpuHandle.IsValid() ||
                    !textureResource->asset.gpuHandle.IsValid())
                {
                    continue;
                }

                const DirectX::XMMATRIX worldMatrix = ResolveWorldMatrix(entity, world, worldMatrixCache, visiting, hierarchyCycleWarningLogged_, context);

                render::DrawItem drawItem;
                drawItem.mesh = meshResource->asset.gpuHandle;
                drawItem.shader = shaderResource->asset.gpuHandle;
                drawItem.texture = textureResource->asset.gpuHandle;
                drawItem.model = ToRenderMatrix(worldMatrix);
                drawItem.color = materialResource->asset.tint;
                drawItems.push_back(drawItem);
            }
        }

        if (!context.renderAdapter.BeginFrame(context.surface, context.clearColor))
        {
            return;
        }

        context.renderAdapter.SetViewProjection(context.surface, ToRenderMatrix(viewMatrix), ToRenderMatrix(projectionMatrix));

        for (const auto& drawItem : drawItems)
        {
            context.renderAdapter.Draw(context.surface, drawItem);
        }

        context.renderAdapter.EndFrame(context.surface);
    }
}
