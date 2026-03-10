// HierarchyComponent.h

#pragma once

#include <vector>

#include <myengine/ecs/Component.h>
#include <myengine/ecs/Entity.h>

namespace myengine::ecs::components
{
    struct HierarchyComponent : Component
    {
        EntityId parent = kInvalidEntity;
        std::vector<EntityId> children;
    };
}