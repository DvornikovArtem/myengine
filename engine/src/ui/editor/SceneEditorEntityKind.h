#pragma once

// What kind of thing an entity is, for the Outliner and the Details header (spec 5.2 / 5.3).
// Internal to engine/src/ui/editor.

#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/ScriptComponent.h>

#include "EditorIcons.h"
#include "EditorStyle.h"

namespace myengine::ui::detail
{
    struct EntityKindInfo
    {
        const char* icon;
        const char* name;
        ImU32 color;
    };

    // In order of priority: a camera, then a mesh, then a script, otherwise an empty entity
    inline EntityKindInfo ClassifyEntity(ecs::World& world, const ecs::EntityId entity)
    {
        if (world.TryGet<ecs::components::CameraComponent>(entity) != nullptr)
        {
            return {ICON_CAMERA, "Camera", style::kText};
        }
        if (world.TryGet<ecs::components::MeshRendererComponent>(entity) != nullptr)
        {
            return {ICON_BOX, "Mesh", style::kTypeMesh};
        }
        if (world.TryGet<ecs::components::ScriptComponent>(entity) != nullptr)
        {
            return {ICON_FILE_CODE, "Script", style::kTypeScript};
        }
        return {ICON_CIRCLE_DASHED, "Empty", style::kTextDim};
    }
}
