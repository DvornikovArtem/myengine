// SceneSerializer.h

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

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
    bool SaveWorldToJson(const ecs::World& world, const std::filesystem::path& path, core::Logger* logger = nullptr);
    bool LoadWorldFromJson(ecs::World& world, const std::filesystem::path& path, core::Logger* logger = nullptr);
    std::string SerializeWorldToString(const ecs::World& world);
    bool LoadWorldFromString(ecs::World& world, std::string_view jsonText, core::Logger* logger = nullptr);
}