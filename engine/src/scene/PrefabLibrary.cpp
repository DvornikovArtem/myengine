// PrefabLibrary.cpp
// Scene-format templates. Local ids are remapped for every copy, including parent links.

#include <myengine/scripting/PrefabLibrary.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <windows.h>

#include <myengine/core/FileWatcher.h>
#include <myengine/core/Logger.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/scene/SceneSerializer.h>

namespace myengine::scene
{
    namespace
    {
        using json = nlohmann::json;
        constexpr std::string_view kPrefabSuffix = ".prefab.json";

        bool IsPrefabName(const std::string_view name)
        {
            if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
            {
                return false;
            }
            for (const unsigned char ch : name)
            {
                if (ch < 32 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos)
                {
                    return false;
                }
            }
            // A Windows device name remains reserved even with the .prefab.json extension.
            std::string stem(name.substr(0, name.find('.')));
            std::transform(stem.begin(), stem.end(), stem.begin(), [](const unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CLOCK$")
            {
                return false;
            }
            if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '1' && stem[3] <= '9')
            {
                return false;
            }
            return true;
        }

        std::string PrefabName(const std::filesystem::path& path)
        {
            const auto file = path.filename().u8string();
            if (file.size() <= kPrefabSuffix.size() || file.compare(file.size() - kPrefabSuffix.size(), kPrefabSuffix.size(), kPrefabSuffix) != 0)
            {
                return std::string();
            }
            const auto name = file.substr(0, file.size() - kPrefabSuffix.size());
            return IsPrefabName(name) ? name : std::string();
        }

        std::filesystem::path PrefabPath(const std::filesystem::path& directory, const std::string_view name)
        {
            if (!IsPrefabName(name))
            {
                throw std::invalid_argument("Invalid prefab name: " + std::string(name));
            }
            const auto path = directory / std::filesystem::u8path(std::string(name) + std::string(kPrefabSuffix));
            // Reject a symlink to a file outside assets/prefabs as well as path traversal in the name.
            if (std::filesystem::weakly_canonical(path).parent_path() != directory)
            {
                throw std::invalid_argument("Prefab path leaves the configured directory: " + std::string(name));
            }
            return path;
        }

        ecs::EntityId LocalId(const json& value, const bool allowZero = false)
        {
            if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
            {
                throw std::invalid_argument("Prefab ids must be non-negative integers");
            }
            const auto id = value.get<std::uint64_t>();
            if ((!allowZero && id == ecs::kInvalidEntity) || id >= std::numeric_limits<ecs::EntityId>::max())
            {
                throw std::invalid_argument("Prefab id is out of range");
            }
            return static_cast<ecs::EntityId>(id);
        }

        ecs::EntityId ParentId(const json& entity)
        {
            if (!entity.contains("Hierarchy"))
            {
                return ecs::kInvalidEntity;
            }
            const auto& hierarchy = entity.at("Hierarchy");
            if (!hierarchy.is_object())
            {
                throw std::invalid_argument("Hierarchy must be an object");
            }
            return hierarchy.contains("parent") ? LocalId(hierarchy.at("parent"), true) : ecs::kInvalidEntity;
        }

        ecs::EntityId ValidatePrefab(const json& prefab)
        {
            if (!prefab.is_object() || !prefab.contains("entities") || !prefab.at("entities").is_array() || prefab.at("entities").empty())
            {
                throw std::invalid_argument("Prefab requires a non-empty entities array");
            }

            // Validate component data before touching the live world. No SceneLoadedEvent is published.
            ecs::World checked;
            for (const auto& entity : prefab.at("entities"))
            {
                if (!entity.is_object() || !entity.contains("id"))
                {
                    throw std::invalid_argument("Every prefab entity requires an id");
                }
                const auto id = LocalId(entity.at("id"));
                if (checked.IsAlive(id))
                {
                    throw std::invalid_argument("Duplicate local prefab id");
                }
                checked.CreateEntityWithId(id);
                DeserializeEntity(checked, id, entity);
            }

            ecs::EntityId root = ecs::kInvalidEntity;
            for (const auto& entity : prefab.at("entities"))
            {
                const auto id = LocalId(entity.at("id"));
                const auto parent = ParentId(entity);
                if (parent == ecs::kInvalidEntity)
                {
                    if (root != ecs::kInvalidEntity)
                    {
                        throw std::invalid_argument("Prefab must have exactly one root entity");
                    }
                    root = id;
                }
                else if (!checked.SetParent(id, parent))
                {
                    throw std::invalid_argument("Prefab hierarchy has a missing parent, a self-link or a cycle");
                }
            }
            if (root == ecs::kInvalidEntity)
            {
                throw std::invalid_argument("Prefab has no root entity");
            }
            return root;
        }

        void CheckPosition(const ecs::components::Vec3& position)
        {
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
            {
                throw std::invalid_argument("Prefab position must contain finite numbers");
            }
        }
    }

    struct PrefabLibrary::Impl
    {
        struct CachedPrefab
        {
            json data;
            std::string source; // compare disk contents, not unsaved props edited through GetPrefabJson
            std::filesystem::path path;
        };

        std::filesystem::path directory;
        std::map<std::string, CachedPrefab> cache;
        core::FileWatcher watcher;
    };

    PrefabLibrary::PrefabLibrary() = default;

    PrefabLibrary::~PrefabLibrary()
    {
        Shutdown();
    }

    bool PrefabLibrary::Initialize(const std::filesystem::path& directory, core::Logger* logger)
    {
        Shutdown();
        logger_ = logger;
        lastError_.clear();
        try
        {
            const auto resolved = std::filesystem::weakly_canonical(std::filesystem::absolute(directory));
            if (!std::filesystem::is_directory(resolved))
            {
                throw std::runtime_error("Prefab directory does not exist: " + resolved.u8string());
            }
            impl_ = std::make_unique<Impl>();
            impl_->directory = resolved;
            // path.extension() is ".json", not ".prefab.json". Filter the complete suffix in the callback.
            impl_->watcher.WatchDirectory(resolved, ".json", [this](const core::FileChange& change)
                {
                    if (change.path.parent_path() != impl_->directory)
                    {
                        return;
                    }
                    const auto name = PrefabName(change.path);
                    if (name.empty())
                    {
                        return;
                    }
                    // Windows accepts "COIN" and "coin" for the same file. Invalidate every cached alias.
                    for (auto it = impl_->cache.begin(); it != impl_->cache.end();)
                    {
                        const bool samePath = CompareStringOrdinal(change.path.c_str(), -1, it->second.path.c_str(), -1, TRUE) == CSTR_EQUAL;
                        if (samePath && (change.removed || change.contents != it->second.source))
                        {
                            it = impl_->cache.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                });
            // Report the first scan too: a cached file can change before the watcher establishes its baseline.
            impl_->watcher.RequestFullRescan();
            impl_->watcher.Poll();
            return true;
        }
        catch (const std::exception& error)
        {
            impl_.reset();
            ReportError(error.what());
            return false;
        }
    }

    void PrefabLibrary::Poll()
    {
        if (impl_ != nullptr)
        {
            impl_->watcher.Poll();
        }
    }

    void PrefabLibrary::Shutdown()
    {
        impl_.reset(); // FileWatcher waits for its Streaming scan before releasing the cache / callbacks
        logger_ = nullptr;
    }

    nlohmann::json* PrefabLibrary::GetPrefabJson(const std::string_view prefabName)
    {
        lastError_.clear();
        if (impl_ == nullptr)
        {
            ReportError("PrefabLibrary is not initialized");
            return nullptr;
        }
        try
        {
            if (!IsPrefabName(prefabName))
            {
                throw std::invalid_argument("Invalid prefab name");
            }
            const std::string name(prefabName);
            if (const auto found = impl_->cache.find(name); found != impl_->cache.end())
            {
                return &found->second.data;
            }
            const auto path = PrefabPath(impl_->directory, prefabName);
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                throw std::runtime_error("Could not open " + path.u8string());
            }
            std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            if (file.bad())
            {
                throw std::runtime_error("Could not read " + path.u8string());
            }
            auto data = json::parse(source);
            ValidatePrefab(data);
            const auto entry = impl_->cache.emplace(name, Impl::CachedPrefab{std::move(data), std::move(source), path}).first;
            return &entry->second.data;
        }
        catch (const std::exception& error)
        {
            ReportError(std::string(prefabName) + ": " + error.what());
            return nullptr;
        }
    }

    nlohmann::json* PrefabLibrary::ReloadPrefab(const std::string_view prefabName)
    {
        try
        {
            if (impl_ == nullptr)
            {
                throw std::runtime_error("PrefabLibrary is not initialized");
            }
            const auto path = PrefabPath(impl_->directory, prefabName);
            for (auto it = impl_->cache.begin(); it != impl_->cache.end();)
            {
                if (CompareStringOrdinal(path.c_str(), -1, it->second.path.c_str(), -1, TRUE) == CSTR_EQUAL)
                {
                    it = impl_->cache.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            return GetPrefabJson(prefabName);
        }
        catch (const std::exception& error)
        {
            ReportError(std::string(prefabName) + ": " + error.what());
            return nullptr;
        }
    }

    ecs::EntityId PrefabLibrary::Instantiate(ecs::World& world, const std::string_view prefabName, const ecs::components::Vec3* position)
    {
        std::vector<ecs::EntityId> created;
        try
        {
            if (position != nullptr)
            {
                CheckPosition(*position);
            }
            const auto* prefab = GetPrefabJson(prefabName);
            if (prefab == nullptr)
            {
                return ecs::kInvalidEntity;
            }
            const auto localRoot = ValidatePrefab(*prefab); // also validates unsaved editor changes
            const auto& entities = prefab->at("entities");
            created.reserve(entities.size());
            std::unordered_map<ecs::EntityId, ecs::EntityId> remap;
            remap.reserve(entities.size());
            for (const auto& entity : entities)
            {
                const auto id = world.CreateEntity();
                created.push_back(id);
                remap.emplace(LocalId(entity.at("id")), id);
            }
            // Deserialize all components before linking: SetParent creates Hierarchy on both ends.
            for (const auto& entity : entities)
            {
                DeserializeEntity(world, remap.at(LocalId(entity.at("id"))), entity);
            }
            for (const auto& entity : entities)
            {
                const auto parent = ParentId(entity);
                if (parent != ecs::kInvalidEntity && !world.SetParent(remap.at(LocalId(entity.at("id"))), remap.at(parent)))
                {
                    throw std::runtime_error("Could not restore prefab hierarchy");
                }
            }
            const auto root = remap.at(localRoot);
            if (position != nullptr)
            {
                auto* transform = world.TryGet<ecs::components::TransformComponent>(root);
                if (transform == nullptr)
                {
                    transform = &world.Emplace<ecs::components::TransformComponent>(root);
                }
                transform->position = *position;
            }
            return root;
        }
        catch (const std::exception& error)
        {
            for (auto it = created.rbegin(); it != created.rend(); ++it)
            {
                world.DestroyEntity(*it);
            }
            ReportError(std::string(prefabName) + ": " + error.what());
            return ecs::kInvalidEntity;
        }
    }

    std::vector<std::string> PrefabLibrary::ListPrefabs() const
    {
        std::vector<std::string> names;
        if (impl_ == nullptr)
        {
            return names;
        }
        std::error_code error;
        for (std::filesystem::directory_iterator it(impl_->directory, error), end; !error && it != end; it.increment(error))
        {
            if (it->is_regular_file(error))
            {
                const auto name = PrefabName(it->path());
                if (!name.empty())
                {
                    names.push_back(name);
                }
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    bool PrefabLibrary::SavePrefab(const std::string_view prefabName)
    {
        const auto* prefab = GetPrefabJson(prefabName);
        return prefab != nullptr && SavePrefab(prefabName, *prefab);
    }

    bool PrefabLibrary::SavePrefab(const std::string_view prefabName, const json& prefab)
    {
        lastError_.clear();
        std::filesystem::path temporary;
        try
        {
            if (impl_ == nullptr)
            {
                throw std::runtime_error("PrefabLibrary is not initialized");
            }
            ValidatePrefab(prefab);
            auto data = prefab; // prepare the copy before writing, including when prefab refers to the cache
            const auto path = PrefabPath(impl_->directory, prefabName);
            const auto source = data.dump(2) + '\n';
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            auto candidate = path;
            candidate += ".tmp." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(stamp);
            if (std::filesystem::exists(candidate))
            {
                throw std::runtime_error("Temporary prefab save path already exists");
            }
            std::ofstream file(candidate, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                throw std::runtime_error("Could not open a temporary prefab file");
            }
            temporary = candidate; // only this newly created temporary file belongs to us
            file << source;
            file.flush();
            const bool written = static_cast<bool>(file);
            file.close();
            if (!written || !file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                throw std::runtime_error("Could not replace " + path.u8string());
            }
            temporary.clear();
            // Alias names on Windows (coin / COIN) refer to the same file. Do not keep an older spawn template.
            for (auto it = impl_->cache.begin(); it != impl_->cache.end();)
            {
                if (CompareStringOrdinal(path.c_str(), -1, it->second.path.c_str(), -1, TRUE) == CSTR_EQUAL)
                {
                    it = impl_->cache.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            impl_->cache.emplace(std::string(prefabName), Impl::CachedPrefab{std::move(data), source, path});
            return true;
        }
        catch (const std::exception& error)
        {
            if (!temporary.empty())
            {
                std::error_code removeError;
                std::filesystem::remove(temporary, removeError);
            }
            ReportError(std::string(prefabName) + ": " + error.what());
            return false;
        }
    }

    void PrefabLibrary::ReportError(std::string message)
    {
        lastError_ = std::move(message);
        if (logger_ != nullptr)
        {
            logger_->Warning("PrefabLibrary: " + lastError_);
        }
    }

    const std::string& PrefabLibrary::GetLastError() const
    {
        return lastError_;
    }
}
