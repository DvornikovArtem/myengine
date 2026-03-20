// ResourceManager.h

#pragma once

#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <myengine/core/Types.h>
#include <myengine/render/RenderTypes.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::render
{
    class IRenderAdapter;
}

namespace myengine::resource
{
    template <typename>
    inline constexpr bool kUnsupportedResourceType = false;

    struct ResourceDependency
    {
        std::filesystem::path path;
        bool existed = false;
        std::filesystem::file_time_type lastWriteTime{};
    };

    template <typename T>
    struct Resource
    {
        std::string key;
        std::filesystem::path sourcePath;
        T asset;
        std::vector<ResourceDependency> dependencies;
    };

    template <typename T>
    using ResourceHandle = std::shared_ptr<Resource<T>>;

    struct MeshAsset
    {
        render::MeshData data;
        render::MeshHandle gpuHandle;
    };

    struct TextureAsset
    {
        render::TextureData data;
        render::TextureHandle gpuHandle;
    };

    struct ShaderAsset
    {
        render::ShaderProgramData program;
        render::ShaderHandle gpuHandle;
    };

    struct MaterialAsset
    {
        std::string shaderPath;
        std::string texturePath;
        core::Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct MeshCpuAsset
    {
        render::MeshData data;
        std::vector<std::filesystem::path> dependencies;
        bool loadedFromBinaryCache = false;
    };

    struct TextureCpuAsset
    {
        render::TextureData data;
        std::vector<std::filesystem::path> dependencies;
        bool loadedFromBinaryCache = false;
    };

    class ResourceManager
    {
    public:
        ResourceManager(render::IRenderAdapter& renderAdapter, core::Logger& logger);

        template <typename T>
        ResourceHandle<T> Load(const std::filesystem::path& path)
        {
            if constexpr (std::is_same_v<T, MeshAsset>)
            {
                return LoadMesh(path);
            }
            else if constexpr (std::is_same_v<T, TextureAsset>)
            {
                return LoadTexture(path);
            }
            else if constexpr (std::is_same_v<T, ShaderAsset>)
            {
                return LoadShader(path);
            }
            else if constexpr (std::is_same_v<T, MaterialAsset>)
            {
                return LoadMaterial(path);
            }
            else
            {
                static_assert(kUnsupportedResourceType<T>, "Unsupported resource type");
            }
        }

        template <typename T>
        ResourceHandle<T> Reload(const std::filesystem::path& path)
        {
            if constexpr (std::is_same_v<T, MeshAsset>)
            {
                return ReloadMesh(path);
            }
            else if constexpr (std::is_same_v<T, TextureAsset>)
            {
                return ReloadTexture(path);
            }
            else if constexpr (std::is_same_v<T, ShaderAsset>)
            {
                return ReloadShader(path);
            }
            else if constexpr (std::is_same_v<T, MaterialAsset>)
            {
                return ReloadMaterial(path);
            }
            else
            {
                static_assert(kUnsupportedResourceType<T>, "Unsupported resource type");
            }
        }

        bool LoadManifest(const std::filesystem::path& path);
        void UpdateHotReload();

        std::filesystem::path ResolvePath(const std::filesystem::path& path) const;

    private:
        template <typename T>
        using ResourceCache = std::unordered_map<std::string, ResourceHandle<T>>;

        ResourceHandle<MeshAsset> LoadMesh(const std::filesystem::path& path);
        ResourceHandle<TextureAsset> LoadTexture(const std::filesystem::path& path);
        ResourceHandle<ShaderAsset> LoadShader(const std::filesystem::path& path);
        ResourceHandle<MaterialAsset> LoadMaterial(const std::filesystem::path& path);
        ResourceHandle<MeshAsset> ReloadMesh(const std::filesystem::path& path);
        ResourceHandle<TextureAsset> ReloadTexture(const std::filesystem::path& path);
        ResourceHandle<ShaderAsset> ReloadShader(const std::filesystem::path& path);
        ResourceHandle<MaterialAsset> ReloadMaterial(const std::filesystem::path& path);

        void PumpAsyncLoads();
        void FinalizePendingMeshes();
        void FinalizePendingTextures();
        void ReloadChangedMeshes();
        void ReloadChangedTextures();
        void ReloadChangedShaders();
        void ReloadChangedMaterials();
        void ScheduleMeshLoad(const std::string& key, const std::filesystem::path& path);
        void ScheduleTextureLoad(const std::string& key, const std::filesystem::path& path);
        ResourceHandle<MeshAsset> TryFinalizeMeshLoad(const std::string& key);
        ResourceHandle<TextureAsset> TryFinalizeTextureLoad(const std::string& key);
        ResourceHandle<MeshAsset> BuildMeshResource(const std::string& key, const std::filesystem::path& path, MeshCpuAsset cpuAsset);
        ResourceHandle<TextureAsset> BuildTextureResource(const std::string& key, const std::filesystem::path& path, TextureCpuAsset cpuAsset);
        ResourceHandle<MeshAsset> BuildMeshPlaceholder(const std::string& key, const std::filesystem::path& path);
        ResourceHandle<TextureAsset> BuildTexturePlaceholder(const std::string& key, const std::filesystem::path& path);
        ResourceHandle<ShaderAsset> LoadShaderInternal(const std::string& key, const std::filesystem::path& path);
        ResourceHandle<MaterialAsset> LoadMaterialInternal(const std::string& key, const std::filesystem::path& path);
        ResourceHandle<ShaderAsset> BuildShaderFallback(const std::string& key, const std::filesystem::path& path, std::vector<std::filesystem::path> dependencies = {}) const;
        ResourceHandle<MaterialAsset> BuildMaterialFallback(const std::string& key, const std::filesystem::path& path) const;

        template <typename T>
        ResourceHandle<T> CreateResource(const std::string& key, const std::filesystem::path& sourcePath, T asset, std::vector<ResourceDependency> dependencies = {}) const;

        std::string NormalizeKey(const std::filesystem::path& path) const;
        std::vector<ResourceDependency> BuildDependencies(const std::vector<std::filesystem::path>& paths) const;
        bool HasChanged(const std::vector<ResourceDependency>& dependencies) const;

        ResourceHandle<MeshAsset> CreateFallbackMesh();
        ResourceHandle<TextureAsset> CreateFallbackTexture();
        ResourceHandle<ShaderAsset> CreateFallbackShader();
        ResourceHandle<MaterialAsset> CreateFallbackMaterial();

        render::IRenderAdapter& renderAdapter_;
        core::Logger& logger_;

        ResourceCache<MeshAsset> meshCache_;
        ResourceCache<TextureAsset> textureCache_;
        ResourceCache<ShaderAsset> shaderCache_;
        ResourceCache<MaterialAsset> materialCache_;

        struct MeshLoadJob
        {
            std::filesystem::path path;
            std::future<MeshCpuAsset> future;
        };

        struct TextureLoadJob
        {
            std::filesystem::path path;
            std::future<TextureCpuAsset> future;
        };

        std::unordered_map<std::string, MeshLoadJob> pendingMeshLoads_;
        std::unordered_map<std::string, TextureLoadJob> pendingTextureLoads_;

        ResourceHandle<MeshAsset> fallbackMesh_;
        ResourceHandle<TextureAsset> fallbackTexture_;
        ResourceHandle<ShaderAsset> fallbackShader_;
        ResourceHandle<MaterialAsset> fallbackMaterial_;
    };

    template <typename T>
    ResourceHandle<T> ResourceManager::CreateResource(const std::string& key, const std::filesystem::path& sourcePath, T asset, std::vector<ResourceDependency> dependencies) const
    {
        auto resource = std::make_shared<Resource<T>>();
        resource->key = key;
        resource->sourcePath = sourcePath;
        resource->asset = std::move(asset);
        resource->dependencies = std::move(dependencies);
        return resource;
    }
}