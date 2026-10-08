// EntityRef.h
// Private header of the scripting subsystem: how a script refers to an entity.

#pragma once

#include <myengine/ecs/Entity.h>

#include "ScriptContext.h"

namespace myengine::scripting::detail
{
    // Handle, not a pointer: store the id and scene version, check liveness on every component access.
    // The scene version also rejects references saved before Stop / a scene reload, where ids may repeat.
    struct EntityRef
    {
        ecs::EntityId id = ecs::kInvalidEntity;
        std::uint64_t sceneVersion = GetScriptContext().sceneVersion;
    };
}
