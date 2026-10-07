// ScriptComponent.h

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/ecs/Component.h>

namespace myengine::ecs::components
{
    // Only scene data lives here. Python instances are owned by ScriptSystem.
    struct ScriptComponent : Component
    {
        struct Entry
        {
            std::string module;
            std::string className;
            nlohmann::json props = nlohmann::json::object();
        };

        std::vector<Entry> scripts;
    };
}
