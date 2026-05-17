#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <DirectXCollision.h>

#include <myengine/core/Logger.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/scene/TransformUtils.h>

namespace myengine::scene
{
    namespace
    {
        constexpr float kRadToDeg = 57.295779513082320876f;

        void LogCycleWarning(
            core::Logger* logger,
            std::unordered_set<ecs::EntityId>* cycleWarningLogged,
            const ecs::EntityId entity,
            const char* suffix)
        {
            if (logger == nullptr)
            {
                return;
            }

            if (cycleWarningLogged != nullptr && !cycleWarningLogged->insert(entity).second)
            {
                return;
            }

            logger->Warning("TransformUtils: hierarchy cycle detected for entity=" + std::to_string(entity) + suffix);
        }

        ecs::components::Vec3 QuaternionToEulerDegrees(const DirectX::XMVECTOR quaternion)
        {
            DirectX::XMFLOAT4 q{};
            DirectX::XMStoreFloat4(&q, DirectX::XMQuaternionNormalize(quaternion));

            const double sinPitch = 2.0 * (static_cast<double>(q.w) * q.x - static_cast<double>(q.y) * q.z);
            double pitch = 0.0;
            if (std::abs(sinPitch) >= 1.0)
            {
                pitch = std::copysign(DirectX::XM_PIDIV2, sinPitch);
            }
            else
            {
                pitch = std::asin(sinPitch);
            }

            const double yaw = std::atan2(
                2.0 * (static_cast<double>(q.w) * q.y + static_cast<double>(q.x) * q.z),
                1.0 - 2.0 * (static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y));

            const double roll = std::atan2(
                2.0 * (static_cast<double>(q.w) * q.z + static_cast<double>(q.x) * q.y),
                1.0 - 2.0 * (static_cast<double>(q.x) * q.x + static_cast<double>(q.z) * q.z));

            ecs::components::Vec3 result{};
            result.x = static_cast<float>(pitch * kRadToDeg);
            result.y = static_cast<float>(yaw * kRadToDeg);
            result.z = static_cast<float>(roll * kRadToDeg);
            return result;
        }
    }

    DirectX::XMMATRIX BuildLocalMatrix(const ecs::components::TransformComponent& transform)
    {
        return DirectX::XMMatrixScaling(transform.scale.x, transform.scale.y, transform.scale.z) *
            DirectX::XMMatrixRotationRollPitchYaw(
                DirectX::XMConvertToRadians(transform.rotationDeg.x),
                DirectX::XMConvertToRadians(transform.rotationDeg.y),
                DirectX::XMConvertToRadians(transform.rotationDeg.z)) *
            DirectX::XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);
    }

    render::Matrix4 ToRenderMatrix(const DirectX::XMMATRIX& matrix)
    {
        DirectX::XMFLOAT4X4 value{};
        DirectX::XMStoreFloat4x4(&value, matrix);

        render::Matrix4 result{};
        std::memcpy(result.data.data(), &value, sizeof(float) * 16);
        return result;
    }

    DirectX::XMMATRIX ToDirectXMatrix(const render::Matrix4& matrix)
    {
        DirectX::XMFLOAT4X4 value{};
        std::memcpy(&value, matrix.data.data(), sizeof(float) * 16);
        return DirectX::XMLoadFloat4x4(&value);
    }

    DirectX::XMMATRIX ResolveWorldMatrix(
        ecs::World& world,
        const ecs::EntityId entity,
        std::unordered_map<ecs::EntityId, DirectX::XMFLOAT4X4>& cache,
        std::unordered_set<ecs::EntityId>& visiting,
        std::unordered_set<ecs::EntityId>* cycleWarningLogged,
        core::Logger* logger)
    {
        if (const auto cacheIt = cache.find(entity); cacheIt != cache.end())
        {
            return DirectX::XMLoadFloat4x4(&cacheIt->second);
        }

        const auto* transform = world.TryGet<ecs::components::TransformComponent>(entity);
        if (transform == nullptr)
        {
            return DirectX::XMMatrixIdentity();
        }

        if (!visiting.insert(entity).second)
        {
            LogCycleWarning(logger, cycleWarningLogged, entity, ". Local transform is used.");
            return BuildLocalMatrix(*transform);
        }

        const DirectX::XMMATRIX local = BuildLocalMatrix(*transform);
        DirectX::XMMATRIX worldMatrix = local;

        if (const auto* hierarchy = world.TryGet<ecs::components::HierarchyComponent>(entity);
            hierarchy != nullptr && hierarchy->parent != ecs::kInvalidEntity)
        {
            const ecs::EntityId parent = hierarchy->parent;
            if (world.IsAlive(parent) && world.Has<ecs::components::TransformComponent>(parent))
            {
                if (visiting.find(parent) != visiting.end())
                {
                    LogCycleWarning(logger, cycleWarningLogged, entity, ". Parent loop is ignored.");
                }
                else
                {
                    worldMatrix = DirectX::XMMatrixMultiply(
                        local,
                        ResolveWorldMatrix(world, parent, cache, visiting, cycleWarningLogged, logger));
                }
            }
        }

        visiting.erase(entity);

        DirectX::XMFLOAT4X4 cached{};
        DirectX::XMStoreFloat4x4(&cached, worldMatrix);
        cache[entity] = cached;
        return worldMatrix;
    }

    DirectX::XMMATRIX ResolveParentWorldMatrix(
        ecs::World& world,
        const ecs::EntityId entity,
        std::unordered_map<ecs::EntityId, DirectX::XMFLOAT4X4>& cache,
        std::unordered_set<ecs::EntityId>& visiting,
        std::unordered_set<ecs::EntityId>* cycleWarningLogged,
        core::Logger* logger)
    {
        const auto* hierarchy = world.TryGet<ecs::components::HierarchyComponent>(entity);
        if (hierarchy == nullptr || hierarchy->parent == ecs::kInvalidEntity || !world.IsAlive(hierarchy->parent))
        {
            return DirectX::XMMatrixIdentity();
        }

        if (!world.Has<ecs::components::TransformComponent>(hierarchy->parent))
        {
            return DirectX::XMMatrixIdentity();
        }

        return ResolveWorldMatrix(world, hierarchy->parent, cache, visiting, cycleWarningLogged, logger);
    }

    bool DecomposeMatrix(const DirectX::XMMATRIX& matrix, ecs::components::TransformComponent& outTransform)
    {
        DirectX::XMVECTOR scaleVector = DirectX::XMVectorZero();
        DirectX::XMVECTOR rotationQuaternion = DirectX::XMQuaternionIdentity();
        DirectX::XMVECTOR translationVector = DirectX::XMVectorZero();
        if (!DirectX::XMMatrixDecompose(&scaleVector, &rotationQuaternion, &translationVector, matrix))
        {
            return false;
        }

        DirectX::XMFLOAT3 scale{};
        DirectX::XMFLOAT3 translation{};
        DirectX::XMStoreFloat3(&scale, scaleVector);
        DirectX::XMStoreFloat3(&translation, translationVector);

        outTransform.position = {translation.x, translation.y, translation.z};
        outTransform.rotationDeg = QuaternionToEulerDegrees(rotationQuaternion);
        outTransform.scale = {scale.x, scale.y, scale.z};
        return true;
    }

    bool ApplyWorldMatrixToLocalTransform(
        ecs::World& world,
        const ecs::EntityId entity,
        const DirectX::XMMATRIX& desiredWorldMatrix,
        std::unordered_map<ecs::EntityId, DirectX::XMFLOAT4X4>& cache,
        std::unordered_set<ecs::EntityId>& visiting,
        std::unordered_set<ecs::EntityId>* cycleWarningLogged,
        core::Logger* logger)
    {
        auto* transform = world.TryGet<ecs::components::TransformComponent>(entity);
        if (transform == nullptr)
        {
            return false;
        }

        const DirectX::XMMATRIX parentWorld =
            ResolveParentWorldMatrix(world, entity, cache, visiting, cycleWarningLogged, logger);

        DirectX::XMVECTOR determinant = DirectX::XMVectorZero();
        const DirectX::XMMATRIX inverseParentWorld = DirectX::XMMatrixInverse(&determinant, parentWorld);
        if (DirectX::XMVector3NearEqual(determinant, DirectX::XMVectorZero(), DirectX::XMVectorReplicate(1e-6f)))
        {
            return false;
        }

        const DirectX::XMMATRIX desiredLocalMatrix = DirectX::XMMatrixMultiply(desiredWorldMatrix, inverseParentWorld);
        return DecomposeMatrix(desiredLocalMatrix, *transform);
    }
}