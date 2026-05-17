#pragma once

#include <DirectXMath.h>

#include <myengine/core/Types.h>
#include <myengine/editor/EditorState.h>
#include <myengine/ecs/Entity.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::ecs
{
    class World;
}

namespace myengine::editor
{
    class TransformGizmo
    {
    public:
        struct Context
        {
            ecs::World& world;
            ecs::EntityId entity = ecs::kInvalidEntity;
            core::WindowId windowId = 0;
            ViewportRect viewport{};
            DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixIdentity();
            DirectX::XMMATRIX projectionMatrix = DirectX::XMMatrixIdentity();
            GizmoOperation operation = GizmoOperation::Translate;
            GizmoSpace space = GizmoSpace::Local;
            bool enabled = true;
            core::Logger* logger = nullptr;
        };

        bool DrawAndHandle(const Context& context);
        bool IsHovered() const;
        bool IsUsing() const;

    private:
        bool hovered_ = false;
        bool using_ = false;
    };
}