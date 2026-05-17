#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <imgui/imgui.h>
#include "ImGuizmo.h"

#include <myengine/editor/TransformGizmo.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/scene/TransformUtils.h>

namespace myengine::editor
{
    namespace
    {
        ImGuizmo::OPERATION ToImGuizmoOperation(const GizmoOperation operation)
        {
            switch (operation)
            {
                case GizmoOperation::Translate: return ImGuizmo::TRANSLATE;
                case GizmoOperation::Rotate: return ImGuizmo::ROTATE;
                case GizmoOperation::Scale: return ImGuizmo::SCALE;
            }

            return ImGuizmo::TRANSLATE;
        }

        ImGuizmo::MODE ToImGuizmoMode(const GizmoOperation operation, const GizmoSpace space)
        {
            if (operation == GizmoOperation::Scale)
            {
                return ImGuizmo::LOCAL;
            }

            return space == GizmoSpace::World ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        }

        std::array<float, 16> StoreMatrix(const DirectX::XMMATRIX& matrix)
        {
            DirectX::XMFLOAT4X4 value{};
            DirectX::XMStoreFloat4x4(&value, matrix);

            std::array<float, 16> result{};
            std::memcpy(result.data(), &value, sizeof(float) * result.size());
            return result;
        }

        DirectX::XMMATRIX LoadMatrix(const float* data)
        {
            DirectX::XMFLOAT4X4 value{};
            std::memcpy(&value, data, sizeof(float) * 16u);
            return DirectX::XMLoadFloat4x4(&value);
        }
    }

    bool TransformGizmo::DrawAndHandle(const Context& context)
    {
        hovered_ = false;
        using_ = false;

        if (!context.enabled ||
            context.entity == ecs::kInvalidEntity ||
            !context.viewport.IsValid() ||
            !context.world.IsAlive(context.entity) ||
            !context.world.Has<ecs::components::TransformComponent>(context.entity))
        {
            ImGuizmo::Enable(false);
            return false;
        }

        std::unordered_map<ecs::EntityId, DirectX::XMFLOAT4X4> worldMatrixCache;
        std::unordered_set<ecs::EntityId> visiting;
        const DirectX::XMMATRIX worldMatrix = scene::ResolveWorldMatrix(
            context.world,
            context.entity,
            worldMatrixCache,
            visiting,
            nullptr,
            context.logger);

        auto view = StoreMatrix(context.viewMatrix);
        auto projection = StoreMatrix(context.projectionMatrix);
        auto model = StoreMatrix(worldMatrix);
        ImGuizmo::BeginFrame();
        ImGuizmo::Enable(true);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetRect(context.viewport.x, context.viewport.y, context.viewport.width, context.viewport.height);
        ImGuizmo::PushID(static_cast<int>(context.entity));

        const bool changed = ImGuizmo::Manipulate(
            view.data(),
            projection.data(),
            ToImGuizmoOperation(context.operation),
            ToImGuizmoMode(context.operation, context.space),
            model.data());
        ImGuizmo::PopID();

        hovered_ = ImGuizmo::IsOver();
        using_ = ImGuizmo::IsUsing();

        if (!changed)
        {
            return false;
        }

        worldMatrixCache.clear();
        visiting.clear();
        return scene::ApplyWorldMatrixToLocalTransform(
            context.world,
            context.entity,
            LoadMatrix(model.data()),
            worldMatrixCache,
            visiting,
            nullptr,
            context.logger);
    }

    bool TransformGizmo::IsHovered() const
    {
        return hovered_;
    }

    bool TransformGizmo::IsUsing() const
    {
        return using_;
    }
}