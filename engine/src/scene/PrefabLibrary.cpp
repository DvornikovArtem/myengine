// PrefabLibrary.cpp

#include <myengine/scripting/PrefabLibrary.h>

namespace myengine::scene
{
    // T4: cache, id remapping, hierarchy, position override, cache reset by FileWatcher

    ecs::EntityId PrefabLibrary::Instantiate(ecs::World& /*world*/, std::string_view /*prefabName*/, const ecs::components::Vec3* /*position*/)
    {
        return ecs::kInvalidEntity;
    }

    std::vector<std::string> PrefabLibrary::ListPrefabs() const
    {
        return {};
    }

    nlohmann::json* PrefabLibrary::GetPrefabJson(std::string_view /*prefabName*/)
    {
        return nullptr;
    }

    bool PrefabLibrary::SavePrefab(std::string_view /*prefabName*/)
    {
        return false;
    }
}