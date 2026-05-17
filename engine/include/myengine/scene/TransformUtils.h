#pragma once

#include <unordered_map>
#include <unordered_set>

#include <DirectXMath.h>

#include <myengine/ecs/Entity.h>
#include <myengine/render/RenderTypes.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::ecs
{
    class World;
}

namespace myengine::ecs::components
{
    struct TransformComponent;
}

namespace myengine::scene
{
    DirectX::XMMATRIX BuildLocalMatrix(const ecs::components::TransformComponent& transform);
    render::Matrix4 ToRenderMatrix(const DirectX::XMMATRIX& matrix);
    DirectX::XMMATRIX ToDirectXMatrix(const render::Matrix4& matrix);

    DirectX::XMMATRIX ResolveWorldMatrix(
        ecs::World& world,
        ecs::EntityId entity,
        std::unordered_map<ecs::EntityId, DirectX::XMFLOAT4X4>& cache,
        std::unordered_set<ecs::EntityId>& visiting,
        std::unordered_set<ecs::EntityId>* cycleWarningLogged = nullptr,
        core::Logger* logger = nullptr);

    DirectX::XMMATRIX ResolveParentWorldMatrix(
        ecs::World& world,
        ecs::EntityId entity,
        std::unordered_map<ecs::EntityId, DirectX::XMFLOAT4X4>& cache,
        std::unordered_set<ecs::EntityId>& visiting,
        std::unordered_set<ecs::EntityId>* cycleWarningLogged = nullptr,
        core::Logger* logger = nullptr);

    bool DecomposeMatrix(const DirectX::XMMATRIX& matrix, ecs::components::TransformComponent& outTransform);

    bool ApplyWorldMatrixToLocalTransform(
        ecs::World& world,
        ecs::EntityId entity,
        const DirectX::XMMATRIX& desiredWorldMatrix,
        std::unordered_map<ecs::EntityId, DirectX::XMFLOAT4X4>& cache,
        std::unordered_set<ecs::EntityId>& visiting,
        std::unordered_set<ecs::EntityId>* cycleWarningLogged = nullptr,
        core::Logger* logger = nullptr);
}
