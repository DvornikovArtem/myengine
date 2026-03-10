// ResourceManager.cpp

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <limits>
#include <string>

#include <directxtex/DirectXTex.h>

#include <myengine/core/Logger.h>
#include <myengine/resource/ResourceManager.h>

namespace myengine::resource
{
    namespace
    {
        std::wstring ToLower(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](const wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            return value;
        }

        bool CopyRgba8Image(const DirectX::Image& image, TextureData& outTexture)
        {
            if (image.width == 0 || image.height == 0 || image.pixels == nullptr)
            {
                return false;
            }
            if (image.width > static_cast<size_t>(std::numeric_limits<std::uint32_t>::max()) ||
                image.height > static_cast<size_t>(std::numeric_limits<std::uint32_t>::max()))
            {
                return false;
            }

            const std::uint32_t width = static_cast<std::uint32_t>(image.width);
            const std::uint32_t height = static_cast<std::uint32_t>(image.height);
            const size_t dstRowPitch = static_cast<size_t>(width) * 4u;
            if (image.rowPitch < dstRowPitch)
            {
                return false;
            }

            outTexture = TextureData{};
            outTexture.width = width;
            outTexture.height = height;
            outTexture.pixelsRgba8.resize(dstRowPitch * static_cast<size_t>(height));

            for (std::uint32_t y = 0; y < height; ++y)
            {
                const auto* src = image.pixels + static_cast<size_t>(y) * image.rowPitch;
                auto* dst = outTexture.pixelsRgba8.data() + static_cast<size_t>(y) * dstRowPitch;
                std::memcpy(dst, src, dstRowPitch);
            }

            return true;
        }
    }

    ResourceManager::ResourceManager(core::Logger& logger) : logger_(logger)
    {
    }

    bool ResourceManager::LoadTextureRgba8(const std::filesystem::path& path, TextureData& outTexture) const
    {
        DirectX::ScratchImage loaded;
        DirectX::TexMetadata metadata{};

        const std::wstring ext = ToLower(path.extension().wstring());
        HRESULT hr = E_FAIL;

        if (ext == L".dds")
        {
            hr = DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, loaded);
        }
        else if (ext == L".tga")
        {
            hr = DirectX::LoadFromTGAFile(path.c_str(), &metadata, loaded);
        }
        else if (ext == L".hdr")
        {
            hr = DirectX::LoadFromHDRFile(path.c_str(), &metadata, loaded);
        }
        else
        {
            hr = DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_NONE, &metadata, loaded);
        }

        if (FAILED(hr))
        {
            logger_.Warning("ResourceManager: failed to load texture file through DirectXTex: " + path.string());
            return false;
        }

        DirectX::ScratchImage rgba;
        const DirectX::Image* finalImage = nullptr;

        if (metadata.format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            metadata.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        {
            finalImage = loaded.GetImage(0, 0, 0);
        }
        else
        {
            HRESULT convertResult = E_FAIL;

            if (DirectX::IsCompressed(metadata.format))
            {
                convertResult = DirectX::Decompress(
                    loaded.GetImages(),
                    loaded.GetImageCount(),
                    metadata,
                    DXGI_FORMAT_R8G8B8A8_UNORM,
                    rgba);
            }
            else
            {
                convertResult = DirectX::Convert(
                    loaded.GetImages(),
                    loaded.GetImageCount(),
                    metadata,
                    DXGI_FORMAT_R8G8B8A8_UNORM,
                    DirectX::TEX_FILTER_DEFAULT,
                    DirectX::TEX_THRESHOLD_DEFAULT,
                    rgba);
            }

            if (FAILED(convertResult))
            {
                logger_.Warning(
                    "ResourceManager: failed to convert texture to RGBA8: " +
                    path.string());
                return false;
            }

            finalImage = rgba.GetImage(0, 0, 0);
        }

        if (finalImage == nullptr)
        {
            logger_.Warning(
                "ResourceManager: loaded texture contains no image data: " +
                path.string());
            return false;
        }

        if (!CopyRgba8Image(*finalImage, outTexture))
        {
            logger_.Warning(
                "ResourceManager: failed to copy RGBA8 texture pixels: " +
                path.string());
            return false;
        }

        return true;
    }
}
