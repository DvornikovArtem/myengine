// PrefabLibrary.h

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/ecs/Entity.h>
#include <myengine/ecs/components/Vector3.h>

namespace myengine::ecs
{
    class World;
}

namespace myengine::scene
{
    // Prefabs: assets/prefabs/<name>.prefab.json, components in the same format as the scene
    class PrefabLibrary
    {
    public:
        // Load (cached) and create a copy. Returns the root entity id or kInvalidEntity
        ecs::EntityId Instantiate(ecs::World& world, std::string_view prefabName, const ecs::components::Vec3* position = nullptr);

        std::vector<std::string> ListPrefabs() const; // for the Prefabs panel
        nlohmann::json* GetPrefabJson(std::string_view prefabName); // for editing props in the editor
        bool SavePrefab(std::string_view prefabName); // write .prefab.json
    };
}