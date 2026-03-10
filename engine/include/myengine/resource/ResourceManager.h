// ResourceManager.h

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace myengine::core
{
    class Logger;
}

namespace myengine::resource
{
    struct TextureData
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> pixelsRgba8;
    };

    class ResourceManager
    {
    public:
        explicit ResourceManager(core::Logger& logger);

        bool LoadTextureRgba8(const std::filesystem::path& path, TextureData& outTexture) const;

    private:
        core::Logger& logger_;
    };
}