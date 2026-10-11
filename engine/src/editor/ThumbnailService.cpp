// ThumbnailService.cpp

#include <myengine/editor/ThumbnailService.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <system_error>

#include <DirectXMath.h>
#include <nlohmann/json.hpp>

#include <myengine/core/Logger.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/resource/ResourceManager.h>
#include <myengine/scene/TransformUtils.h>

namespace myengine::editor
{
    namespace
    {
        using json = nlohmann::json;
        namespace fs = std::filesystem;

        constexpr char kSphereMesh[] = "assets/models/sphere.obj";
        constexpr char kDefaultMaterial[] = "assets/materials/default.material.json";
        constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
        constexpr std::uint64_t kFnvPrime = 1099511628211ull;
        constexpr std::uint64_t kCacheVersion = 2; // bump when the way thumbnails are drawn changes
        constexpr int kMaxWaitFrames = 1200;       // a resource that has not loaded by then fails the thumbnail
        constexpr std::size_t kDiskLoadsPerUpdate = 4;
        constexpr std::uint64_t kStalenessInterval = 30; // frames between staleness passes
        constexpr std::size_t kStalenessChecksPerPass = 16;
        constexpr std::uint64_t kForgetAfterFrames = 7200; // an image nobody asked for this long is dropped
        constexpr float kFovYRadians = 0.5235988f;         // 30 degrees
        constexpr auto kDiskCacheMaxAge = std::chrono::hours(24 * 30);

        // Asset paths come as UTF-8 (the Content Browser, JSON) or as the narrow strings of the ResourceManager keys
        // (the system code page): text that is valid UTF-8 is UTF-8, anything else is read as the code page
        bool IsValidUtf8(const std::string& text)
        {
            std::size_t index = 0;
            while (index < text.size())
            {
                const auto byte = static_cast<unsigned char>(text[index]);
                std::size_t extra = 0;
                if (byte < 0x80)
                {
                    extra = 0;
                }
                else if ((byte & 0xE0) == 0xC0 && byte >= 0xC2)
                {
                    extra = 1;
                }
                else if ((byte & 0xF0) == 0xE0)
                {
                    extra = 2;
                }
                else if ((byte & 0xF8) == 0xF0 && byte <= 0xF4)
                {
                    extra = 3;
                }
                else
                {
                    return false;
                }
                for (std::size_t offset = 1; offset <= extra; ++offset)
                {
                    if (index + offset >= text.size() || (static_cast<unsigned char>(text[index + offset]) & 0xC0) != 0x80)
                    {
                        return false;
                    }
                }
                index += extra + 1;
            }
            return true;
        }

        fs::path PathFromText(const std::string& text)
        {
            return IsValidUtf8(text) ? fs::u8path(text) : fs::path(text);
        }

        void HashBytes(std::uint64_t& hash, const void* data, const std::size_t size)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            for (std::size_t index = 0; index < size; ++index)
            {
                hash ^= bytes[index];
                hash *= kFnvPrime;
            }
        }

        void HashString(std::uint64_t& hash, const std::string& text)
        {
            HashBytes(hash, text.data(), text.size());
            const std::uint8_t separator = 0;
            HashBytes(hash, &separator, 1);
        }

        std::string ToLower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // The file times and sizes of the files that a thumbnail is made from
        void HashFile(std::uint64_t& hash, const fs::path& path)
        {
            HashString(hash, ToLower(path.generic_u8string()));
            std::error_code error;
            const auto time = fs::last_write_time(path, error);
            const std::int64_t ticks = error ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
            const std::uint64_t size = error ? 0 : static_cast<std::uint64_t>(fs::file_size(path, error));
            HashBytes(hash, &ticks, sizeof(ticks));
            HashBytes(hash, &size, sizeof(size));
        }

        std::string HexString(const std::uint64_t value)
        {
            char buffer[17];
            std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
            return buffer;
        }

        // 32-bit uncompressed TGA, origin at the top left, BGRA
        bool WriteTga(const fs::path& path, const std::uint32_t width, const std::uint32_t height, const std::vector<std::uint8_t>& rgba)
        {
            if (width == 0 || height == 0 || width > 0xFFFF || height > 0xFFFF || rgba.size() != static_cast<std::size_t>(width) * height * 4)
            {
                return false;
            }

            std::vector<std::uint8_t> file(18 + rgba.size());
            file[2] = 2;
            file[12] = static_cast<std::uint8_t>(width & 0xFF);
            file[13] = static_cast<std::uint8_t>(width >> 8);
            file[14] = static_cast<std::uint8_t>(height & 0xFF);
            file[15] = static_cast<std::uint8_t>(height >> 8);
            file[16] = 32;
            file[17] = 0x28; // 8 alpha bits, top-left origin
            for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; ++pixel)
            {
                file[18 + pixel * 4 + 0] = rgba[pixel * 4 + 2];
                file[18 + pixel * 4 + 1] = rgba[pixel * 4 + 1];
                file[18 + pixel * 4 + 2] = rgba[pixel * 4 + 0];
                file[18 + pixel * 4 + 3] = rgba[pixel * 4 + 3];
            }

            // A temp file and a rename: a crash never leaves a half image under the real name
            fs::path temp = path;
            temp += ".tmp";
            {
                std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
                if (!stream.is_open())
                {
                    return false;
                }
                stream.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
                if (!stream)
                {
                    return false;
                }
            }
            std::error_code error;
            fs::rename(temp, path, error);
            if (error)
            {
                fs::remove(temp, error);
                return false;
            }
            return true;
        }

        bool ReadTga(const fs::path& path, render::TextureData& out)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream.is_open())
            {
                return false;
            }
            std::array<std::uint8_t, 18> header{};
            stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
            if (!stream || header[2] != 2 || header[16] != 32 || header[0] != 0 || header[1] != 0)
            {
                return false;
            }

            const std::uint32_t width = header[12] | (static_cast<std::uint32_t>(header[13]) << 8);
            const std::uint32_t height = header[14] | (static_cast<std::uint32_t>(header[15]) << 8);
            if (width == 0 || height == 0 || width > 1024 || height > 1024)
            {
                return false;
            }

            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
            stream.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
            if (!stream)
            {
                return false;
            }

            const bool topLeft = (header[17] & 0x20) != 0;
            out.width = width;
            out.height = height;
            out.channels = 4;
            out.srgb = false;
            out.pixelsRgba8.resize(pixels.size());
            for (std::uint32_t row = 0; row < height; ++row)
            {
                const std::uint32_t sourceRow = topLeft ? row : (height - 1 - row);
                for (std::uint32_t column = 0; column < width; ++column)
                {
                    const std::size_t from = (static_cast<std::size_t>(sourceRow) * width + column) * 4;
                    const std::size_t to = (static_cast<std::size_t>(row) * width + column) * 4;
                    out.pixelsRgba8[to + 0] = pixels[from + 2];
                    out.pixelsRgba8[to + 1] = pixels[from + 1];
                    out.pixelsRgba8[to + 2] = pixels[from + 0];
                    out.pixelsRgba8[to + 3] = pixels[from + 3];
                }
            }
            return true;
        }

        struct Bounds
        {
            DirectX::XMFLOAT3 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
            DirectX::XMFLOAT3 max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

            bool IsValid() const { return min.x <= max.x; }

            void Add(const DirectX::XMFLOAT3& point)
            {
                min.x = std::min(min.x, point.x);
                min.y = std::min(min.y, point.y);
                min.z = std::min(min.z, point.z);
                max.x = std::max(max.x, point.x);
                max.y = std::max(max.y, point.y);
                max.z = std::max(max.z, point.z);
            }
        };

        // One mesh of a prefab: the paths and its transform in the prefab's space
        struct PrefabMesh
        {
            std::string meshPath;
            std::string materialPath;
            DirectX::XMFLOAT4X4 world{};
        };

        DirectX::XMMATRIX LocalMatrix(const json& transform)
        {
            const auto read = [&](const char* key, const float fallback) -> std::array<float, 3>
            {
                std::array<float, 3> value{fallback, fallback, fallback};
                if (const auto it = transform.find(key); it != transform.end() && it->is_array() && it->size() >= 3)
                {
                    for (std::size_t axis = 0; axis < 3; ++axis)
                    {
                        value[axis] = (*it)[axis].is_number() ? (*it)[axis].get<float>() : fallback;
                    }
                }
                return value;
            };
            const auto position = read("position", 0.0f);
            const auto rotation = read("rotationDeg", 0.0f);
            const auto scale = read("scale", 1.0f);
            return DirectX::XMMatrixScaling(scale[0], scale[1], scale[2]) *
                DirectX::XMMatrixRotationRollPitchYaw(
                    DirectX::XMConvertToRadians(rotation[0]),
                    DirectX::XMConvertToRadians(rotation[1]),
                    DirectX::XMConvertToRadians(rotation[2])) *
                DirectX::XMMatrixTranslation(position[0], position[1], position[2]);
        }

        // The entities of a .prefab.json with their world matrices (children multiply by their parents)
        bool ReadPrefabEntities(const fs::path& file, std::vector<PrefabEntityInfo>& out)
        {
            out.clear();
            std::ifstream stream(file, std::ios::binary);
            if (!stream.is_open())
            {
                return false;
            }
            json root;
            try
            {
                stream >> root;
            }
            catch (const std::exception&)
            {
                return false;
            }
            const auto entitiesIt = root.find("entities");
            if (entitiesIt == root.end() || !entitiesIt->is_array())
            {
                return false;
            }

            struct Node
            {
                long long parent = -1;
                DirectX::XMFLOAT4X4 local{};
            };
            std::unordered_map<long long, Node> nodes;
            for (const auto& entity : *entitiesIt)
            {
                if (!entity.is_object() || !entity.contains("id") || !entity["id"].is_number_integer())
                {
                    continue;
                }
                Node node;
                const long long id = entity["id"].get<long long>();
                PrefabEntityInfo info;
                info.id = id;
                if (const auto it = entity.find("Hierarchy"); it != entity.end() && it->is_object() &&
                    it->contains("parent") && (*it)["parent"].is_number_integer())
                {
                    node.parent = (*it)["parent"].get<long long>();
                    if (node.parent <= 0)
                    {
                        node.parent = -1; // 0 is "no entity" in the files
                    }
                }
                info.parent = node.parent;
                DirectX::XMMATRIX local = DirectX::XMMatrixIdentity();
                if (const auto it = entity.find("Transform"); it != entity.end() && it->is_object())
                {
                    local = LocalMatrix(*it);
                }
                DirectX::XMStoreFloat4x4(&node.local, local);
                nodes[id] = node;

                if (const auto it = entity.find("Tag"); it != entity.end() && it->is_object())
                {
                    const auto name = it->find("name");
                    if (name != it->end() && name->is_string())
                    {
                        info.name = name->get<std::string>();
                    }
                }
                if (const auto it = entity.find("MeshRenderer"); it != entity.end() && it->is_object())
                {
                    const auto meshPath = it->find("meshPath");
                    const auto materialPath = it->find("materialPath");
                    info.meshPath = meshPath != it->end() && meshPath->is_string() ? meshPath->get<std::string>() : std::string();
                    info.materialPath = materialPath != it->end() && materialPath->is_string() ? materialPath->get<std::string>() : std::string();
                    const auto visible = it->find("visible");
                    info.visible = !(visible != it->end() && visible->is_boolean() && !visible->get<bool>());
                    info.hasMesh = !info.meshPath.empty();
                }
                info.hasRigidbody = entity.contains("Rigidbody");
                if (const auto it = entity.find("Rigidbody"); it != entity.end() && it->is_object())
                {
                    const bool gravity = it->value("useGravity", true);
                    const bool kinematic = it->value("isKinematic", false);
                    char buffer[64];
                    std::snprintf(buffer, sizeof(buffer), "%s \xC2\xB7 mass %.3g",
                        kinematic ? "Kinematic" : (gravity ? "Gravity on" : "Gravity off"), static_cast<double>(it->value("mass", 1.0)));
                    info.rigidbody = buffer;
                }
                if (const auto it = entity.find("Collider"); it != entity.end() && it->is_object())
                {
                    const auto number = [&](const char* key, const float fallback)
                    {
                        const auto value = it->find(key);
                        return value != it->end() && value->is_number() ? value->get<float>() : fallback;
                    };
                    const std::string type = it->value("type", std::string("box"));
                    char buffer[96];
                    if (type == "sphere")
                    {
                        std::snprintf(buffer, sizeof(buffer), "Sphere \xC2\xB7 %.2g", static_cast<double>(number("radius", 0.5f)));
                    }
                    else
                    {
                        float extents[3] = {0.5f, 0.5f, 0.5f};
                        if (const auto half = it->find("halfExtents"); half != it->end() && half->is_array() && half->size() >= 3)
                        {
                            for (std::size_t axis = 0; axis < 3; ++axis)
                            {
                                extents[axis] = (*half)[axis].is_number() ? (*half)[axis].get<float>() : 0.5f;
                            }
                        }
                        std::snprintf(buffer, sizeof(buffer), "Box \xC2\xB7 %.2g %.2g %.2g", static_cast<double>(extents[0]),
                            static_cast<double>(extents[1]), static_cast<double>(extents[2]));
                    }
                    info.collider = buffer;
                }
                if (const auto it = entity.find("Script"); it != entity.end() && it->is_object())
                {
                    if (const auto scripts = it->find("scripts"); scripts != it->end() && scripts->is_array() && !scripts->empty() &&
                        (*scripts)[0].is_object())
                    {
                        const std::string module = (*scripts)[0].value("module", std::string());
                        const std::string className = (*scripts)[0].value("class", std::string());
                        info.script = module.empty() ? className : module + "." + className;
                    }
                }
                out.push_back(std::move(info));
            }

            for (PrefabEntityInfo& info : out)
            {
                const auto nodeIt = nodes.find(info.id);
                if (nodeIt == nodes.end())
                {
                    continue;
                }
                DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&nodeIt->second.local);
                long long parent = nodeIt->second.parent;
                for (int depth = 0; parent >= 0 && depth < 32; ++depth)
                {
                    const auto parentIt = nodes.find(parent);
                    if (parentIt == nodes.end())
                    {
                        break;
                    }
                    world = DirectX::XMMatrixMultiply(world, DirectX::XMLoadFloat4x4(&parentIt->second.local));
                    parent = parentIt->second.parent;
                }
                DirectX::XMFLOAT4X4 value{};
                DirectX::XMStoreFloat4x4(&value, world);
                std::memcpy(info.world.data.data(), &value, sizeof(float) * 16);
            }
            return true;
        }

        // The meshes of a .prefab.json with their world matrices
        bool ParsePrefab(const fs::path& file, std::vector<PrefabMesh>& out)
        {
            out.clear();
            std::vector<PrefabEntityInfo> entities;
            if (!ReadPrefabEntities(file, entities))
            {
                return false;
            }
            for (const PrefabEntityInfo& entity : entities)
            {
                if (!entity.hasMesh || !entity.visible || entity.materialPath.empty())
                {
                    continue;
                }
                PrefabMesh mesh;
                mesh.meshPath = entity.meshPath;
                mesh.materialPath = entity.materialPath;
                std::memcpy(&mesh.world, entity.world.data.data(), sizeof(float) * 16);
                out.push_back(std::move(mesh));
            }
            return true;
        }
    }

    struct ThumbnailService::Impl
    {
        enum class State : std::uint8_t
        {
            Queued,   // waiting for the disk cache or a render
            Rendered, // the image is a pooled render target
            Cached,   // the image is a texture read from the disk cache
            Failed,   // the asset cannot be drawn; the caller keeps its icon
        };

        struct Entry
        {
            std::string assetPath;
            ThumbnailKind kind = ThumbnailKind::Other;
            std::uint32_t size = 128;
            State state = State::Queued;
            bool diskChecked = false;
            bool needsDiskWrite = false;
            bool inQueue = false;
            render::RenderTargetHandle target{};
            render::TextureHandle texture{};
            bool textureOwned = false; // a plain texture that must be destroyed with the entry
            std::uint64_t signature = 0;
            std::uint64_t lastUsed = 0;
            int waitFrames = 0;
        };

        enum class Build : std::uint8_t
        {
            Ready,
            Pending, // a mesh or a texture is still streaming
            Failed,
        };

        struct LiveView
        {
            LiveViewRequest request;
            render::RenderTargetHandle target{};
            render::TextureHandle texture{};
            std::uint32_t targetWidth = 0;
            std::uint32_t targetHeight = 0;
            std::uint64_t lastSubmitted = 0;
            bool dirty = false; // a new request that has not been drawn yet
        };

        Impl(render::IRenderAdapter& adapterRef, resource::ResourceManager& resourcesRef, core::Logger& loggerRef, ThumbnailServiceConfig configValue)
            : adapter(adapterRef)
            , resources(resourcesRef)
            , logger(loggerRef)
            , config(std::move(configValue))
            , pool(adapterRef, config.maxRenderTargets)
        {
            PruneDiskCache();
        }

        ~Impl()
        {
            for (auto& [key, entry] : entries)
            {
                (void)key;
                ReleaseImage(entry);
            }
            for (auto& [id, view] : liveViews)
            {
                (void)id;
                if (view.target.IsValid())
                {
                    pool.Release(view.target);
                }
            }
        }

        // ---- helpers ----

        static std::string MakeKey(const std::string& assetPath, const std::uint32_t size)
        {
            std::string key = ToLower(PathFromText(assetPath).lexically_normal().generic_u8string());
            key += '@';
            key += std::to_string(size);
            return key;
        }

        fs::path Resolve(const fs::path& path) const
        {
            return resources.ResolvePath(path);
        }

        std::string SuggestMaterial(const std::string& meshPath) const
        {
            if (config.suggestMaterial)
            {
                std::string material = config.suggestMaterial(meshPath);
                if (!material.empty())
                {
                    return material;
                }
            }
            return kDefaultMaterial;
        }

        void PruneDiskCache() const
        {
            if (config.cacheDirectory.empty())
            {
                return;
            }
            std::error_code error;
            if (!fs::is_directory(config.cacheDirectory, error))
            {
                return;
            }
            const auto now = fs::file_time_type::clock::now();
            for (fs::directory_iterator it(config.cacheDirectory, error), end; !error && it != end; it.increment(error))
            {
                const auto& path = it->path();
                if (path.extension() != ".tga" && path.extension() != ".tmp")
                {
                    continue;
                }
                std::error_code timeError;
                const auto time = fs::last_write_time(path, timeError);
                if (!timeError && now - time > kDiskCacheMaxAge)
                {
                    fs::remove(path, timeError);
                }
            }
        }

        // The files a thumbnail depends on, resolved: their times make the cache signature
        void CollectDependencies(const Entry& entry, std::vector<fs::path>& out)
        {
            out.clear();
            const fs::path source = Resolve(PathFromText(entry.assetPath));
            out.push_back(source);

            const auto addMaterial = [&](const std::string& materialPath)
            {
                const fs::path materialFile = Resolve(PathFromText(materialPath));
                out.push_back(materialFile);
                std::ifstream stream(materialFile, std::ios::binary);
                if (!stream.is_open())
                {
                    return;
                }
                json descriptor;
                try
                {
                    stream >> descriptor;
                }
                catch (const std::exception&)
                {
                    return;
                }
                for (const char* key : {"shader", "texture"})
                {
                    const std::string name = descriptor.value(key, std::string());
                    if (!name.empty())
                    {
                        out.push_back(Resolve(materialFile.parent_path() / fs::u8path(name)));
                    }
                }
            };

            switch (entry.kind)
            {
                case ThumbnailKind::Material:
                    addMaterial(entry.assetPath);
                    out.push_back(Resolve(fs::u8path(kSphereMesh)));
                    break;
                case ThumbnailKind::Mesh:
                    addMaterial(SuggestMaterial(entry.assetPath));
                    break;
                case ThumbnailKind::Prefab:
                {
                    std::vector<PrefabMesh> meshes;
                    if (ParsePrefab(source, meshes))
                    {
                        for (const auto& mesh : meshes)
                        {
                            out.push_back(Resolve(fs::u8path(mesh.meshPath)));
                            addMaterial(mesh.materialPath);
                        }
                    }
                    break;
                }
                case ThumbnailKind::Texture:
                case ThumbnailKind::Other:
                    break;
            }
        }

        std::uint64_t ComputeSignature(const Entry& entry)
        {
            std::vector<fs::path> dependencies;
            CollectDependencies(entry, dependencies);
            std::uint64_t hash = kFnvOffset;
            HashBytes(hash, &kCacheVersion, sizeof(kCacheVersion));
            HashString(hash, std::to_string(static_cast<int>(entry.kind)));
            for (const auto& path : dependencies)
            {
                HashFile(hash, path);
            }
            return hash;
        }

        fs::path CacheFile(const Entry& entry) const
        {
            return config.cacheDirectory / fs::u8path(HexString(entry.signature) + "_" + std::to_string(entry.size) + ".tga");
        }

        void ReleaseImage(Entry& entry)
        {
            if (entry.target.IsValid())
            {
                pool.Release(entry.target);
                entry.target = {};
            }
            if (entry.textureOwned && entry.texture.IsValid())
            {
                adapter.DestroyTexture(entry.texture);
            }
            entry.texture = {};
            entry.textureOwned = false;
            entry.needsDiskWrite = false;
        }

        void Requeue(const std::string& key, Entry& entry)
        {
            ReleaseImage(entry);
            entry.state = State::Queued;
            entry.diskChecked = false;
            entry.waitFrames = 0;
            entry.signature = 0;
            if (!entry.inQueue)
            {
                entry.inQueue = true;
                queue.push_back(key);
            }
        }

        // The least recently used image that was not asked for in the last frames
        Entry* FindEvictionCandidate(const State state)
        {
            Entry* best = nullptr;
            for (auto& [key, entry] : entries)
            {
                (void)key;
                if (entry.state != state || entry.lastUsed + 1 >= frame)
                {
                    continue;
                }
                if (best == nullptr || entry.lastUsed < best->lastUsed)
                {
                    best = &entry;
                }
            }
            return best;
        }

        void EvictImage(Entry& entry)
        {
            ReleaseImage(entry);
            entry.state = State::Queued;
            entry.diskChecked = false; // the disk copy brings it back cheaply when it is asked for again
        }

        // ---- drawing ----

        Build AppendItem(
            const std::string& meshPath,
            const std::string& materialPath,
            const DirectX::XMMATRIX& model,
            std::vector<render::DrawItem>& items,
            Bounds& bounds)
        {
            auto mesh = resources.Load<resource::MeshAsset>(PathFromText(meshPath));
            auto material = resources.Load<resource::MaterialAsset>(PathFromText(materialPath));
            if (mesh == nullptr || material == nullptr)
            {
                return Build::Failed;
            }
            auto shader = resources.Load<resource::ShaderAsset>(PathFromText(material->asset.shaderPath));
            auto texture = resources.Load<resource::TextureAsset>(PathFromText(material->asset.texturePath));
            if (shader == nullptr || texture == nullptr)
            {
                return Build::Failed;
            }
            if (resources.IsLoadPending(PathFromText(meshPath)) || resources.IsLoadPending(PathFromText(material->asset.texturePath)))
            {
                return Build::Pending;
            }
            if (!mesh->asset.gpuHandle.IsValid() || !shader->asset.gpuHandle.IsValid() || !texture->asset.gpuHandle.IsValid())
            {
                return Build::Failed;
            }

            render::DrawItem item;
            item.mesh = mesh->asset.gpuHandle;
            item.shader = shader->asset.gpuHandle;
            item.texture = texture->asset.gpuHandle;
            item.model = scene::ToRenderMatrix(model);
            item.color = material->asset.tint;
            items.push_back(item);

            // The bounds of the mesh in the space of the thumbnail
            Bounds local;
            for (const auto& vertex : mesh->asset.data.vertices)
            {
                local.Add({vertex.position.x, vertex.position.y, vertex.position.z});
            }
            if (!local.IsValid())
            {
                local.Add({-0.5f, -0.5f, -0.5f});
                local.Add({0.5f, 0.5f, 0.5f});
            }
            for (int corner = 0; corner < 8; ++corner)
            {
                const DirectX::XMFLOAT3 point{
                    (corner & 1) ? local.max.x : local.min.x,
                    (corner & 2) ? local.max.y : local.min.y,
                    (corner & 4) ? local.max.z : local.min.z,
                };
                DirectX::XMFLOAT3 world{};
                DirectX::XMStoreFloat3(&world, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&point), model));
                bounds.Add(world);
            }
            return Build::Ready;
        }

        Build BuildDrawList(const Entry& entry, std::vector<render::DrawItem>& items, Bounds& bounds)
        {
            switch (entry.kind)
            {
                case ThumbnailKind::Material:
                    return AppendItem(kSphereMesh, entry.assetPath, DirectX::XMMatrixIdentity(), items, bounds);
                case ThumbnailKind::Mesh:
                    return AppendItem(entry.assetPath, SuggestMaterial(entry.assetPath), DirectX::XMMatrixIdentity(), items, bounds);
                case ThumbnailKind::Prefab:
                {
                    std::vector<PrefabMesh> meshes;
                    if (!ParsePrefab(Resolve(PathFromText(entry.assetPath)), meshes) || meshes.empty())
                    {
                        return Build::Failed;
                    }
                    bool pending = false;
                    for (const auto& mesh : meshes)
                    {
                        const Build result = AppendItem(mesh.meshPath, mesh.materialPath, DirectX::XMLoadFloat4x4(&mesh.world), items, bounds);
                        pending = pending || result == Build::Pending;
                    }
                    if (pending)
                    {
                        return Build::Pending;
                    }
                    return items.empty() ? Build::Failed : Build::Ready;
                }
                case ThumbnailKind::Texture:
                case ThumbnailKind::Other:
                    break;
            }
            return Build::Failed;
        }

        // A three-quarter view from above that fits the bounding sphere of what is drawn
        static void FrameCamera(const Bounds& bounds, render::Matrix4& view, render::Matrix4& projection)
        {
            const DirectX::XMFLOAT3 center{
                (bounds.min.x + bounds.max.x) * 0.5f,
                (bounds.min.y + bounds.max.y) * 0.5f,
                (bounds.min.z + bounds.max.z) * 0.5f,
            };
            const float dx = bounds.max.x - bounds.min.x;
            const float dy = bounds.max.y - bounds.min.y;
            const float dz = bounds.max.z - bounds.min.z;
            const float radius = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 0.01f);
            const float distance = radius / std::sin(kFovYRadians * 0.5f) * 1.08f;

            const DirectX::XMVECTOR direction = DirectX::XMVector3Normalize(DirectX::XMVectorSet(0.55f, 0.42f, -0.72f, 0.0f));
            const DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&center);
            const DirectX::XMVECTOR eye = DirectX::XMVectorAdd(target, DirectX::XMVectorScale(direction, distance));
            const DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
            const DirectX::XMMATRIX projectionMatrix = DirectX::XMMatrixPerspectiveFovLH(
                kFovYRadians, 1.0f, std::max(distance - radius * 1.5f, distance * 0.02f), distance + radius * 2.5f);
            view = scene::ToRenderMatrix(viewMatrix);
            projection = scene::ToRenderMatrix(projectionMatrix);
        }

        bool RenderEntry(const std::string& key, Entry& entry, const render::RenderSurfaceHandle surface)
        {
            (void)key;
            std::vector<render::DrawItem> items;
            Bounds bounds;
            const Build build = BuildDrawList(entry, items, bounds);
            if (build == Build::Pending)
            {
                if (++entry.waitFrames > kMaxWaitFrames)
                {
                    entry.state = State::Failed;
                }
                return false;
            }
            if (build == Build::Failed || !bounds.IsValid())
            {
                entry.state = State::Failed;
                return false;
            }

            render::RenderTargetHandle target = pool.Acquire(entry.size, entry.size);
            if (!target.IsValid())
            {
                if (Entry* victim = FindEvictionCandidate(State::Rendered))
                {
                    EvictImage(*victim);
                    target = pool.Acquire(entry.size, entry.size);
                }
            }
            if (!target.IsValid())
            {
                return false; // the pool is full of images that are on screen: try again later
            }

            render::Matrix4 view = render::Matrix4::Identity();
            render::Matrix4 projection = render::Matrix4::Identity();
            FrameCamera(bounds, view, projection);

            if (!adapter.BeginRenderTarget(target, core::Color{0.0f, 0.0f, 0.0f, 0.0f}))
            {
                pool.Release(target);
                return false;
            }
            adapter.SetViewProjection(surface, view, projection);
            for (const auto& item : items)
            {
                adapter.Draw(surface, item);
            }
            adapter.EndRenderTarget(target);

            entry.target = target;
            entry.texture = adapter.GetRenderTargetTexture(target);
            entry.textureOwned = false;
            entry.state = State::Rendered;
            entry.needsDiskWrite = !config.cacheDirectory.empty();
            ++renderedTotal;
            return true;
        }

        render::IRenderAdapter& adapter;
        resource::ResourceManager& resources;
        core::Logger& logger;
        ThumbnailServiceConfig config;
        render::RenderTargetPool pool;

        // Draws the live views that were submitted since the last render
        void RenderLiveViews(const render::RenderSurfaceHandle surface)
        {
            for (auto& [id, view] : liveViews)
            {
                (void)id;
                if (!view.dirty)
                {
                    continue;
                }
                const std::uint32_t width = std::clamp<std::uint32_t>(view.request.width, 8, 2048);
                const std::uint32_t height = std::clamp<std::uint32_t>(view.request.height, 8, 2048);
                if (view.target.IsValid() && (view.targetWidth != width || view.targetHeight != height))
                {
                    pool.Release(view.target);
                    view.target = {};
                    view.texture = {};
                }
                if (!view.target.IsValid())
                {
                    view.target = pool.Acquire(width, height);
                    if (!view.target.IsValid())
                    {
                        continue; // the pool is exhausted: the panel keeps its old image
                    }
                    view.targetWidth = width;
                    view.targetHeight = height;
                    view.texture = adapter.GetRenderTargetTexture(view.target);
                }

                if (!adapter.BeginRenderTarget(view.target, view.request.clearColor))
                {
                    continue;
                }
                adapter.SetViewProjection(surface, view.request.view, view.request.projection);
                adapter.SetWireframe(view.request.wireframe);
                for (const auto& item : view.request.items)
                {
                    adapter.Draw(surface, item);
                }
                if (!view.request.lines.empty())
                {
                    adapter.DrawDebugLines(surface, view.request.lines);
                }
                adapter.EndRenderTarget(view.target);
                view.dirty = false;
                view.request.items.clear();
                view.request.lines.clear();
            }
        }

        std::unordered_map<std::string, Entry> entries;
        std::unordered_map<std::string, LiveView> liveViews;
        std::deque<std::string> queue;
        std::uint64_t frame = 0;
        std::uint64_t renderedTotal = 0;
        std::uint64_t diskHitsTotal = 0;
        std::uint64_t diskWritesTotal = 0;
        std::size_t stalenessCursor = 0;
    };

    ThumbnailKind ThumbnailService::KindOf(const fs::path& assetPath)
    {
        const std::string name = ToLower(assetPath.filename().generic_u8string());
        const auto endsWith = [&](const char* suffix)
        {
            const std::string text = suffix;
            return name.size() >= text.size() && name.compare(name.size() - text.size(), text.size(), text) == 0;
        };

        if (endsWith(".material.json"))
        {
            return ThumbnailKind::Material;
        }
        if (endsWith(".prefab.json"))
        {
            return ThumbnailKind::Prefab;
        }
        const std::string extension = ToLower(assetPath.extension().generic_u8string());
        // .myemesh and .myetex are the engine's binary caches of the sources: they have no preview of their own
        if (extension == ".obj")
        {
            return ThumbnailKind::Mesh;
        }
        if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".tga")
        {
            return ThumbnailKind::Texture;
        }
        return ThumbnailKind::Other;
    }

    std::uint32_t ThumbnailService::BucketSize(const std::uint32_t requestedSize)
    {
        if (requestedSize <= 64)
        {
            return 64;
        }
        return requestedSize <= 128 ? 128 : 256;
    }

    ThumbnailService::ThumbnailService(
        render::IRenderAdapter& adapter,
        resource::ResourceManager& resources,
        core::Logger& logger,
        ThumbnailServiceConfig config)
        : impl_(std::make_unique<Impl>(adapter, resources, logger, std::move(config)))
    {
    }

    ThumbnailService::~ThumbnailService() = default;

    void BoundsBox::Add(const render::Float3& point)
    {
        min.x = std::min(min.x, point.x);
        min.y = std::min(min.y, point.y);
        min.z = std::min(min.z, point.z);
        max.x = std::max(max.x, point.x);
        max.y = std::max(max.y, point.y);
        max.z = std::max(max.z, point.z);
    }

    void BoundsBox::Add(const BoundsBox& other)
    {
        if (other.IsValid())
        {
            Add(other.min);
            Add(other.max);
        }
    }

    DrawItemStatus ThumbnailService::BuildDrawItem(
        const std::string& meshPath,
        const std::string& materialPath,
        const render::Matrix4& model,
        render::DrawItem& item,
        BoundsBox* worldBounds)
    {
        std::vector<render::DrawItem> items;
        Bounds bounds;
        const auto result = impl_->AppendItem(meshPath, materialPath, scene::ToDirectXMatrix(model), items, bounds);
        if (result != Impl::Build::Ready || items.empty())
        {
            return result == Impl::Build::Pending ? DrawItemStatus::Pending : DrawItemStatus::Failed;
        }
        item = items.front();
        if (worldBounds != nullptr && bounds.IsValid())
        {
            worldBounds->Add(render::Float3{bounds.min.x, bounds.min.y, bounds.min.z});
            worldBounds->Add(render::Float3{bounds.max.x, bounds.max.y, bounds.max.z});
        }
        return DrawItemStatus::Ready;
    }

    bool ThumbnailService::ReadPrefab(const std::string& assetPath, std::vector<PrefabEntityInfo>& out)
    {
        return ReadPrefabEntities(impl_->Resolve(PathFromText(assetPath)), out);
    }

    render::TextureHandle ThumbnailService::CreateViewTexture(const render::TextureData& data)
    {
        return impl_->adapter.CreateTexture(data);
    }

    void ThumbnailService::DestroyViewTexture(const render::TextureHandle texture)
    {
        if (texture.IsValid())
        {
            impl_->adapter.DestroyTexture(texture);
        }
    }

    render::TextureHandle ThumbnailService::SubmitLiveView(LiveViewRequest request)
    {
        auto& view = impl_->liveViews[request.id];
        view.lastSubmitted = impl_->frame;
        view.request = std::move(request);
        view.dirty = true;
        return view.texture;
    }

    void ThumbnailService::SetMaterialSuggestion(std::function<std::string(const std::string& meshPath)> suggest)
    {
        impl_->config.suggestMaterial = std::move(suggest);
    }

    Thumbnail ThumbnailService::Request(const std::string& assetPath, const std::uint32_t size)
    {
        Thumbnail result;
        result.kind = KindOf(PathFromText(assetPath));
        if (assetPath.empty() || result.kind == ThumbnailKind::Other)
        {
            return result;
        }

        if (result.kind == ThumbnailKind::Texture)
        {
            // The texture is its own thumbnail (ImGui scales it); the handle is read each time, hot reload replaces it
            auto texture = impl_->resources.Load<resource::TextureAsset>(PathFromText(assetPath));
            if (texture != nullptr && !impl_->resources.IsLoadPending(PathFromText(assetPath)) && texture->asset.gpuHandle.IsValid())
            {
                result.texture = texture->asset.gpuHandle;
                result.ready = true;
                result.sourceWidth = texture->asset.data.width;
                result.sourceHeight = texture->asset.data.height;
            }
            return result;
        }

        const std::uint32_t bucket = BucketSize(size);
        const std::string key = Impl::MakeKey(assetPath, bucket);
        auto [it, inserted] = impl_->entries.try_emplace(key);
        Impl::Entry& entry = it->second;
        if (inserted)
        {
            entry.assetPath = assetPath;
            entry.kind = result.kind;
            entry.size = bucket;
        }
        entry.lastUsed = impl_->frame;
        if (entry.state == Impl::State::Queued && !entry.inQueue)
        {
            entry.inQueue = true;
            impl_->queue.push_back(key);
        }

        if ((entry.state == Impl::State::Rendered || entry.state == Impl::State::Cached) && entry.texture.IsValid())
        {
            result.texture = entry.texture;
            result.ready = true;
        }
        result.failed = entry.state == Impl::State::Failed;
        return result;
    }

    void ThumbnailService::Update()
    {
        Impl& impl = *impl_;
        ++impl.frame;

        // 1. Queued thumbnails: the disk cache answers before a render is spent
        std::size_t diskLoads = 0;
        for (const std::string& key : impl.queue)
        {
            if (diskLoads >= kDiskLoadsPerUpdate)
            {
                break;
            }
            const auto it = impl.entries.find(key);
            if (it == impl.entries.end())
            {
                continue;
            }
            Impl::Entry& entry = it->second;
            if (entry.state != Impl::State::Queued || entry.diskChecked)
            {
                continue;
            }

            entry.diskChecked = true;
            entry.signature = impl.ComputeSignature(entry);
            if (impl.config.cacheDirectory.empty())
            {
                continue;
            }

            render::TextureData data;
            const fs::path file = impl.CacheFile(entry);
            std::error_code error;
            if (!fs::exists(file, error) || !ReadTga(file, data))
            {
                continue;
            }
            const render::TextureHandle texture = impl.adapter.CreateTexture(data);
            if (!texture.IsValid())
            {
                continue;
            }
            entry.texture = texture;
            entry.textureOwned = true;
            entry.state = Impl::State::Cached;
            ++impl.diskHitsTotal;
            ++diskLoads;
        }

        // The queue keeps only what still waits
        std::deque<std::string> remaining;
        for (const std::string& key : impl.queue)
        {
            const auto it = impl.entries.find(key);
            if (it == impl.entries.end())
            {
                continue;
            }
            if (it->second.state == Impl::State::Queued)
            {
                remaining.push_back(key);
            }
            else
            {
                it->second.inQueue = false;
            }
        }
        impl.queue = std::move(remaining);

        // 2. One rendered image per update goes to the disk cache (a synchronous GPU readback)
        for (auto& [key, entry] : impl.entries)
        {
            (void)key;
            if (!entry.needsDiskWrite || entry.state != Impl::State::Rendered || !entry.target.IsValid())
            {
                continue;
            }
            entry.needsDiskWrite = false;
            std::vector<std::uint8_t> pixels;
            if (impl.adapter.ReadRenderTargetPixels(entry.target, pixels))
            {
                std::error_code error;
                fs::create_directories(impl.config.cacheDirectory, error);
                if (!error && WriteTga(impl.CacheFile(entry), entry.size, entry.size, pixels))
                {
                    ++impl.diskWritesTotal;
                }
            }
            break;
        }

        // 3. Files that changed on disk: the image is dropped and drawn again
        if (impl.frame % kStalenessInterval == 0 && !impl.entries.empty())
        {
            std::vector<std::string> keys;
            keys.reserve(impl.entries.size());
            for (const auto& [key, entry] : impl.entries)
            {
                if (entry.state == Impl::State::Rendered || entry.state == Impl::State::Cached || entry.state == Impl::State::Failed)
                {
                    keys.push_back(key);
                }
            }
            if (!keys.empty())
            {
                impl.stalenessCursor %= keys.size();
                const std::size_t checks = std::min(kStalenessChecksPerPass, keys.size());
                for (std::size_t step = 0; step < checks; ++step)
                {
                    const std::string& key = keys[(impl.stalenessCursor + step) % keys.size()];
                    Impl::Entry& entry = impl.entries[key];
                    if (impl.ComputeSignature(entry) != entry.signature)
                    {
                        impl.Requeue(key, entry);
                    }
                }
                impl.stalenessCursor += checks;
            }
        }

        // 3b. A live view that is not submitted any more gives its target back
        for (auto it = impl.liveViews.begin(); it != impl.liveViews.end();)
        {
            if (impl.frame - it->second.lastSubmitted > 30)
            {
                if (it->second.target.IsValid())
                {
                    impl.pool.Release(it->second.target);
                }
                it = impl.liveViews.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // 4. Cached textures are limited; images nobody asked for in a long time are forgotten
        std::size_t cached = 0;
        for (const auto& [key, entry] : impl.entries)
        {
            (void)key;
            cached += entry.state == Impl::State::Cached ? 1 : 0;
        }
        while (cached > impl.config.maxCachedTextures)
        {
            Impl::Entry* victim = impl.FindEvictionCandidate(Impl::State::Cached);
            if (victim == nullptr)
            {
                break;
            }
            impl.EvictImage(*victim);
            --cached;
        }
        for (auto it = impl.entries.begin(); it != impl.entries.end();)
        {
            Impl::Entry& entry = it->second;
            if (impl.frame - entry.lastUsed > kForgetAfterFrames)
            {
                impl.ReleaseImage(entry);
                it = impl.entries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void ThumbnailService::Render(const render::RenderSurfaceHandle surface)
    {
        Impl& impl = *impl_;
        if (surface.IsValid())
        {
            impl.RenderLiveViews(surface);
        }
        std::size_t budget = impl.config.budgetPerFrame;
        if (budget == 0 || impl.queue.empty() || !surface.IsValid())
        {
            return;
        }

        // Newest requests last: the queue is first-come, first-served
        for (const std::string& key : impl.queue)
        {
            if (budget == 0)
            {
                break;
            }
            const auto it = impl.entries.find(key);
            if (it == impl.entries.end())
            {
                continue;
            }
            Impl::Entry& entry = it->second;
            if (entry.state != Impl::State::Queued || !entry.diskChecked)
            {
                continue;
            }
            // Not asked for during the last frames: the tile has scrolled away
            if (entry.lastUsed + 2 < impl.frame)
            {
                continue;
            }
            if (impl.RenderEntry(key, entry, surface))
            {
                --budget;
            }
        }
    }

    void ThumbnailService::Invalidate(const std::string& assetPath)
    {
        Impl& impl = *impl_;
        const std::string wanted = ToLower(PathFromText(assetPath).lexically_normal().generic_u8string());
        for (auto& [key, entry] : impl.entries)
        {
            if (ToLower(PathFromText(entry.assetPath).lexically_normal().generic_u8string()) == wanted)
            {
                impl.Requeue(key, entry);
            }
        }
    }

    void ThumbnailService::InvalidateAll()
    {
        Impl& impl = *impl_;
        for (auto& [key, entry] : impl.entries)
        {
            impl.Requeue(key, entry);
        }
    }

    void ThumbnailService::Clear()
    {
        Impl& impl = *impl_;
        for (auto& [key, entry] : impl.entries)
        {
            (void)key;
            impl.ReleaseImage(entry);
        }
        impl.entries.clear();
        impl.queue.clear();
        for (auto& [id, view] : impl.liveViews)
        {
            (void)id;
            if (view.target.IsValid())
            {
                impl.pool.Release(view.target);
                view.target = {};
                view.texture = {};
            }
            view.dirty = true;
        }
        impl.pool.Trim();
    }

    ThumbnailStats ThumbnailService::GetStats() const
    {
        const Impl& impl = *impl_;
        ThumbnailStats stats;
        stats.entries = impl.entries.size();
        for (const auto& [key, entry] : impl.entries)
        {
            (void)key;
            stats.queued += entry.state == Impl::State::Queued ? 1 : 0;
            stats.ready += (entry.state == Impl::State::Rendered || entry.state == Impl::State::Cached) ? 1 : 0;
        }
        stats.rendered = impl.renderedTotal;
        stats.diskHits = impl.diskHitsTotal;
        stats.diskWrites = impl.diskWritesTotal;
        stats.leasedTargets = impl.pool.LeasedCount();
        return stats;
    }
}
