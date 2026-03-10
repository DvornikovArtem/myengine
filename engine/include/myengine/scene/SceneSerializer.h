// SceneSerializer.h

#pragma once

#include <filesystem>

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
}