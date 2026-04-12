// RenderSystem.cpp

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <DirectXCollision.h>
#include <DirectXMath.h>

#include <myengine/core/Logger.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/ColliderComponent.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/ecs/systems/RenderSystem.h>
#include <myengine/resource/ResourceManager.h>
#include <myengine/spatial/Octree.h>

namespace myengine::ecs::systems
{
    namespace
    {
        constexpr float kDefaultRenderableRadius = 0.8660254f;

        struct RenderableEntry
        {
            EntityId entity = kInvalidEntity;
            components::MeshRendererComponent* renderer = nullptr;
            DirectX::XMMATRIX worldMatrix = DirectX::XMMatrixIdentity();
            DirectX::BoundingSphere bounds{};
        };

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

        DirectX::BoundingSphere BuildRenderableBounds(World& world, const EntityId entity, const DirectX::XMMATRIX& worldMatrix)
        {
            DirectX::BoundingSphere result{};

            DirectX::XMVECTOR scaleVector = DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
            DirectX::XMVECTOR rotationVector = DirectX::XMQuaternionIdentity();
            DirectX::XMVECTOR translationVector = DirectX::XMVectorZero();
            if (!DirectX::XMMatrixDecompose(&scaleVector, &rotationVector, &translationVector, worldMatrix))
            {
                translationVector = DirectX::XMVectorZero();
            }

            DirectX::XMFLOAT3 worldScale{};
            DirectX::XMStoreFloat3(&worldScale, scaleVector);
            worldScale.x = std::max(std::abs(worldScale.x), 0.001f);
            worldScale.y = std::max(std::abs(worldScale.y), 0.001f);
            worldScale.z = std::max(std::abs(worldScale.z), 0.001f);

            DirectX::XMFLOAT3 center{};
            DirectX::XMStoreFloat3(&center, translationVector);
            float radius = std::max({worldScale.x, worldScale.y, worldScale.z}) * kDefaultRenderableRadius;

            if (const auto* collider = world.TryGet<components::ColliderComponent>(entity); collider != nullptr)
            {
                const DirectX::XMVECTOR colliderCenter = DirectX::XMVector3TransformCoord(
                    DirectX::XMVectorSet(collider->offset.x, collider->offset.y, collider->offset.z, 1.0f),
                    worldMatrix);
                DirectX::XMStoreFloat3(&center, colliderCenter);

                if (collider->type == components::ColliderType::Sphere)
                {
                    radius = collider->radius * std::max({worldScale.x, worldScale.y, worldScale.z});
                }
                else
                {
                    const float extentX = collider->halfExtents.x * worldScale.x;
                    const float extentY = collider->halfExtents.y * worldScale.y;
                    const float extentZ = collider->halfExtents.z * worldScale.z;
                    radius = std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ);
                }
            }

            result.Center = center;
            result.Radius = std::max(radius, 0.05f);
            return result;
        }

        DirectX::BoundingBox BuildOctreeRootBounds(const std::vector<RenderableEntry>& renderables)
        {
            DirectX::BoundingBox rootBounds{};

            if (renderables.empty())
            {
                rootBounds.Center = {0.0f, 0.0f, 0.0f};
                rootBounds.Extents = {1.0f, 1.0f, 1.0f};
                return rootBounds;
            }

            DirectX::XMFLOAT3 minPoint{
                renderables.front().bounds.Center.x - renderables.front().bounds.Radius,
                renderables.front().bounds.Center.y - renderables.front().bounds.Radius,
                renderables.front().bounds.Center.z - renderables.front().bounds.Radius,
            };
            DirectX::XMFLOAT3 maxPoint{
                renderables.front().bounds.Center.x + renderables.front().bounds.Radius,
                renderables.front().bounds.Center.y + renderables.front().bounds.Radius,
                renderables.front().bounds.Center.z + renderables.front().bounds.Radius,
            };

            for (const RenderableEntry& entry : renderables)
            {
                minPoint.x = std::min(minPoint.x, entry.bounds.Center.x - entry.bounds.Radius);
                minPoint.y = std::min(minPoint.y, entry.bounds.Center.y - entry.bounds.Radius);
                minPoint.z = std::min(minPoint.z, entry.bounds.Center.z - entry.bounds.Radius);
                maxPoint.x = std::max(maxPoint.x, entry.bounds.Center.x + entry.bounds.Radius);
                maxPoint.y = std::max(maxPoint.y, entry.bounds.Center.y + entry.bounds.Radius);
                maxPoint.z = std::max(maxPoint.z, entry.bounds.Center.z + entry.bounds.Radius);
            }

            const DirectX::XMFLOAT3 center{
                (minPoint.x + maxPoint.x) * 0.5f,
                (minPoint.y + maxPoint.y) * 0.5f,
                (minPoint.z + maxPoint.z) * 0.5f,
            };
            const DirectX::XMFLOAT3 extents{
                std::max((maxPoint.x - minPoint.x) * 0.5f, 1.0f),
                std::max((maxPoint.y - minPoint.y) * 0.5f, 1.0f),
                std::max((maxPoint.z - minPoint.z) * 0.5f, 1.0f),
            };
            const float uniformExtent = std::max({extents.x, extents.y, extents.z});

            rootBounds.Center = center;
            rootBounds.Extents = {uniformExtent, uniformExtent, uniformExtent};
            return rootBounds;
        }

        DirectX::BoundingFrustum BuildWorldFrustum(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projectionMatrix)
        {
            DirectX::BoundingFrustum viewFrustum;
            DirectX::BoundingFrustum::CreateFromMatrix(viewFrustum, projectionMatrix);

            DirectX::BoundingFrustum worldFrustum;
            DirectX::XMVECTOR determinant = DirectX::XMVectorZero();
            const DirectX::XMMATRIX inverseView = DirectX::XMMatrixInverse(&determinant, viewMatrix);
            viewFrustum.Transform(worldFrustum, inverseView);
            return worldFrustum;
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

                cameraFound = true;
            });

        std::unordered_map<EntityId, DirectX::XMFLOAT4X4> worldMatrixCache;
        std::unordered_set<EntityId> visiting;
        std::vector<RenderableEntry> renderables;
        std::vector<render::DrawItem> drawItems;

        world.ForEach<components::TransformComponent, components::MeshRendererComponent>(
            [&](const EntityId entity, components::TransformComponent&, components::MeshRendererComponent& renderer)
            {
                if (const auto* binding = world.TryGet<components::WindowBindingComponent>(entity);
                    binding != nullptr && binding->windowId != context.windowId)
                {
                    return;
                }

                if (!renderer.visible || renderer.meshPath.empty() || renderer.materialPath.empty())
                {
                    return;
                }

                const DirectX::XMMATRIX worldMatrix = ResolveWorldMatrix(entity, world, worldMatrixCache, visiting, hierarchyCycleWarningLogged_, context);
                renderables.push_back(RenderableEntry{
                    entity,
                    &renderer,
                    worldMatrix,
                    BuildRenderableBounds(world, entity, worldMatrix),
                });
            });

        std::vector<std::size_t> visibleIndices;
        visibleIndices.reserve(renderables.size());

        if (cameraFound && !renderables.empty())
        {
            const DirectX::BoundingBox rootBounds = BuildOctreeRootBounds(renderables);
            spatial::Octree<std::size_t> octree(rootBounds, 8, 6);
            for (std::size_t index = 0; index < renderables.size(); ++index)
            {
                octree.Insert(index, renderables[index].bounds);
            }

            const DirectX::BoundingFrustum worldFrustum = BuildWorldFrustum(viewMatrix, projectionMatrix);
            octree.Query(worldFrustum, [&](const std::size_t index)
            {
                visibleIndices.push_back(index);
            });

            std::sort(visibleIndices.begin(), visibleIndices.end());
            visibleIndices.erase(std::unique(visibleIndices.begin(), visibleIndices.end()), visibleIndices.end());
        }
        else
        {
            visibleIndices.resize(renderables.size());
            for (std::size_t index = 0; index < renderables.size(); ++index)
            {
                visibleIndices[index] = index;
            }
        }

        if (context.resourceManager != nullptr)
        {
            for (const std::size_t visibleIndex : visibleIndices)
            {
                if (visibleIndex >= renderables.size())
                {
                    continue;
                }

                const RenderableEntry& entry = renderables[visibleIndex];
                auto& renderer = *entry.renderer;

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

                render::DrawItem drawItem;
                drawItem.mesh = meshResource->asset.gpuHandle;
                drawItem.shader = shaderResource->asset.gpuHandle;
                drawItem.texture = textureResource->asset.gpuHandle;
                drawItem.model = ToRenderMatrix(entry.worldMatrix);
                drawItem.color = materialResource->asset.tint;
                drawItems.push_back(drawItem);
            }
        }

        context.renderAdapter.SetViewProjection(context.surface, ToRenderMatrix(viewMatrix), ToRenderMatrix(projectionMatrix));

        for (const auto& drawItem : drawItems)
        {
            context.renderAdapter.Draw(context.surface, drawItem);
        }
    }
}