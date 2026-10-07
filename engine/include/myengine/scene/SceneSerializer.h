// SceneSerializer.h

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <myengine/ecs/Entity.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::ecs
{
    class World;
}

namespace myengine::scene
{
    nlohmann::json SerializeEntity(const ecs::World& world, ecs::EntityId entity);
    // Fills an existing entity; ignores the saved id. Resolve Hierarchy links separately after this call.
    void DeserializeEntity(ecs::World& world, ecs::EntityId entity, const nlohmann::json& entityJson);

    bool SaveWorldToJson(const ecs::World& world, const std::filesystem::path& path, core::Logger* logger = nullptr);
    bool LoadWorldFromJson(ecs::World& world, const std::filesystem::path& path, core::Logger* logger = nullptr);
    std::string SerializeWorldToString(const ecs::World& world);
    bool LoadWorldFromString(ecs::World& world, std::string_view jsonText, core::Logger* logger = nullptr);
}
