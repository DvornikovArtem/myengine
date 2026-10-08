// PrefabLibrary.h

#pragma once

#include <filesystem>
#include <memory>
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

namespace myengine::core
{
    class Logger;
}

namespace myengine::scene
{
    // Prefabs: assets/prefabs/<name>.prefab.json, components in the same format as the scene
    class PrefabLibrary
    {
    public:
        PrefabLibrary();
        ~PrefabLibrary();

        PrefabLibrary(const PrefabLibrary&) = delete;
        PrefabLibrary& operator=(const PrefabLibrary&) = delete;

        bool Initialize(const std::filesystem::path& directory, core::Logger* logger = nullptr);
        void Poll(); // main thread: invalidate changed templates, disk scan runs on the Streaming pool
        void Shutdown(); // stop the watcher before the job system stops

        // Load (cached) and create a copy. Returns the root entity id or kInvalidEntity
        ecs::EntityId Instantiate(ecs::World& world, std::string_view prefabName, const ecs::components::Vec3* position = nullptr);

        std::vector<std::string> ListPrefabs() const; // for the Prefabs panel
        // Main thread only. The pointer is valid until this cache entry is invalidated by Poll / Shutdown.
        nlohmann::json* GetPrefabJson(std::string_view prefabName); // for editing props in the editor
        bool SavePrefab(std::string_view prefabName); // write .prefab.json
        const std::string& GetLastError() const;

    private:
        struct Impl;
        void ReportError(std::string message);

        std::unique_ptr<Impl> impl_;
        core::Logger* logger_ = nullptr;
        std::string lastError_;
    };
}
