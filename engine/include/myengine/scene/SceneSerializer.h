// SceneSerializer.h

#pragma once

#include <filesystem>
#include <functional>
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

    // Loading in two steps so that the slow one can run on a worker thread:
    //  1. ReadWorldFromFile reads, parses and builds the scene into `loaded` (a separate, empty world) and touches
    //     nothing else; it may run on any thread. `progress` gets 0..1 and the name of the stage.
    //  2. CommitLoadedWorld, on the main thread, replaces the entities of `world` with the ones of `loaded` and
    //     publishes SceneLoadedEvent.
    // LoadWorldFromJson is both steps one after another.
    using SceneLoadProgress = std::function<void(float fraction, const char* stage)>;
    bool ReadWorldFromFile(
        ecs::World& loaded,
        const std::filesystem::path& path,
        core::Logger* logger = nullptr,
        const SceneLoadProgress& progress = {});
    void CommitLoadedWorld(ecs::World& world, ecs::World& loaded);
    std::string SerializeWorldToString(const ecs::World& world);
    bool LoadWorldFromString(ecs::World& world, std::string_view jsonText, core::Logger* logger = nullptr);
}
