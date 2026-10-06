// EntityRef.h
// Private header of the scripting subsystem: how a script refers to an entity.

#pragma once

#include <myengine/ecs/Entity.h>

namespace myengine::scripting::detail
{
    // Handle, not a pointer: only the id is stored, every access checks that the entity is still alive.
    // Entity ids are never reused, so the id alone is enough to detect a stale reference.
    struct EntityRef
    {
        ecs::EntityId id = ecs::kInvalidEntity;
    };
}