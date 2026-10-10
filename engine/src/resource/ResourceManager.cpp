// ResourceManager.cpp

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <directxtex/DirectXTex.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/resource/ResourceManager.h>

#include <tracy/Tracy.hpp>

namespace myengine::resource
{
    namespace
    {
        using json = nlohmann::json;

        constexpr std::array<char, 8> kMeshBinaryMagic{'M', 'Y', 'E', 'M', 'E', 'S', 'H', '1'};
        constexpr std::array<char, 8> kTextureBinaryMagic{'M', 'Y', 'E', 'T', 'E', 'X', '0', '1'};
        constexpr std::uint32_t kMeshBinaryVersion = 2;
        constexpr std::uint32_t kTextureBinaryVersion = 2;

        // Match the coordinate conversion previously supplied by Assimp's
        // aiProcess_ConvertToLeftHanded flag for the repository's OBJ assets.
        constexpr bool kMeshFlipUvY = true;
        constexpr bool kMeshReverseWinding = true;
        constexpr bool kTextureUseDirectXTexFirst = true;
        constexpr int kTextureRequestedChannels = STBI_rgb_alpha;
        constexpr bool kTextureForceSrgb = true;
        constexpr std::uint64_t kProceduralSphereMeshVersion = 1;
        constexpr std::uint32_t kProceduralSphereLatitudeSegments = 24;
        constexpr std::uint32_t kProceduralSphereLongitudeSegments = 48;
        constexpr float kProceduralSphereRadius = 0.5f;
        constexpr float kPi = 3.14159265358979323846f;
        constexpr std::size_t kMaxFinalizationsPerFrame = 2;

        constexpr std::uint64_t kHashOffsetBasis = 14695981039346656037ull;
        constexpr std::uint64_t kHashPrime = 1099511628211ull;

        struct MeshBinaryHeader
        {
            std::array<char, 8> magic{};
            std::uint32_t version = 0;
            std::int64_t sourceWriteTime = 0;
            std::uint64_t sourceFileSize = 0;
            std::uint64_t pipelineSignature = 0;
            std::uint64_t vertexCount = 0;
            std::uint64_t indexCount = 0;
        };

        struct TextureBinaryHeader
        {
            std::array<char, 8> magic{};
            std::uint32_t version = 0;
            std::int64_t sourceWriteTime = 0;
            std::uint64_t sourceFileSize = 0;
            std::uint64_t pipelineSignature = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t channels = 0;
            std::uint32_t srgb = 0;
            std::uint64_t pixelBytes = 0;
        };

        struct FileStamp
        {
            std::int64_t writeTime = 0;
            std::uint64_t fileSize = 0;
        };

        static_assert(std::is_trivially_copyable_v<MeshBinaryHeader>);
        static_assert(std::is_trivially_copyable_v<TextureBinaryHeader>);
        static_assert(std::is_trivially_copyable_v<render::MeshVertex>);

        constexpr std::uint64_t HashCombine(const std::uint64_t seed, const std::uint64_t value)
        {
            return (seed ^ value) * kHashPrime;
        }

        constexpr std::uint64_t HashString(const char* text)
        {
            std::uint64_t hash = kHashOffsetBasis;
            for (std::size_t index = 0; text[index] != '\0'; ++index)
            {
                hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(text[index]));
                hash *= kHashPrime;
            }
            return hash;
        }

        constexpr std::uint64_t BuildMeshPipelineSignature()
        {
            std::uint64_t hash = HashString("mesh-import-pipeline-obj");
            hash = HashCombine(hash, static_cast<std::uint64_t>(kMeshFlipUvY ? 1u : 0u));
            hash = HashCombine(hash, static_cast<std::uint64_t>(kMeshReverseWinding ? 1u : 0u));
            hash = HashCombine(hash, static_cast<std::uint64_t>(sizeof(render::MeshVertex)));
            hash = HashCombine(hash, kProceduralSphereMeshVersion);
            return hash;
        }

        constexpr std::uint64_t BuildTexturePipelineSignature()
        {
            std::uint64_t hash = HashString("texture-import-pipeline");
            hash = HashCombine(hash, static_cast<std::uint64_t>(kTextureUseDirectXTexFirst ? 1u : 0u));
            hash = HashCombine(hash, static_cast<std::uint64_t>(kTextureRequestedChannels));
            hash = HashCombine(hash, static_cast<std::uint64_t>(kTextureForceSrgb ? 1u : 0u));
            return hash;
        }

        constexpr std::uint64_t kMeshPipelineSignature = BuildMeshPipelineSignature();
        constexpr std::uint64_t kTexturePipelineSignature = BuildTexturePipelineSignature();

        std::string ToLower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch)
                {
                    return static_cast<char>(std::tolower(ch));
                });
            return value;
        }

        bool IsMeshBinaryPath(const std::filesystem::path& path)
        {
            return ToLower(path.extension().string()) == ".myemesh";
        }

        bool IsBuiltinSpherePath(const std::filesystem::path& path)
        {
            constexpr char kSphereSuffix[] = "assets/models/sphere.obj";
            constexpr std::size_t kSphereSuffixLength = sizeof(kSphereSuffix) - 1;

            const std::string normalizedPath = ToLower(path.generic_string());
            return normalizedPath.size() >= kSphereSuffixLength &&
                normalizedPath.compare(normalizedPath.size() - kSphereSuffixLength, kSphereSuffixLength, kSphereSuffix) == 0;
        }

        bool IsTextureBinaryPath(const std::filesystem::path& path)
        {
            return ToLower(path.extension().string()) == ".myetex";
        }

        std::filesystem::path BuildMeshBinaryPath(const std::filesystem::path& sourcePath)
        {
            if (IsMeshBinaryPath(sourcePath))
            {
                return sourcePath;
            }

            std::filesystem::path binaryPath = sourcePath;
            binaryPath.replace_extension(".myemesh");
            return binaryPath;
        }

        std::filesystem::path BuildTextureBinaryPath(const std::filesystem::path& sourcePath)
        {
            if (IsTextureBinaryPath(sourcePath))
            {
                return sourcePath;
            }

            std::filesystem::path binaryPath = sourcePath;
            binaryPath.replace_extension(".myetex");
            return binaryPath;
        }

        bool TryGetFileStamp(const std::filesystem::path& path, FileStamp& outStamp)
        {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || ec)
            {
                return false;
            }

            const auto writeTime = std::filesystem::last_write_time(path, ec);
            if (ec)
            {
                return false;
            }

            if (!std::filesystem::is_regular_file(path, ec) || ec)
            {
                return false;
            }

            const auto fileSize = std::filesystem::file_size(path, ec);
            if (ec)
            {
                return false;
            }

            outStamp.writeTime = static_cast<std::int64_t>(writeTime.time_since_epoch().count());
            outStamp.fileSize = fileSize;
            return true;
        }

        bool IsCacheCurrent(
            const FileStamp& sourceStamp,
            const std::int64_t cachedWriteTime,
            const std::uint64_t cachedFileSize,
            const std::uint64_t cachedPipelineSignature,
            const std::uint64_t expectedPipelineSignature)
        {
            return cachedWriteTime == sourceStamp.writeTime && cachedFileSize == sourceStamp.fileSize && cachedPipelineSignature == expectedPipelineSignature;
        }

        bool WriteBinaryFile(
            const std::filesystem::path& path,
            const void* headerData,
            const std::size_t headerSize,
            const void* payloadA,
            const std::size_t payloadASize,
            const void* payloadB,
            const std::size_t payloadBSize)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);

            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream.is_open())
            {
                return false;
            }

            stream.write(static_cast<const char*>(headerData), static_cast<std::streamsize>(headerSize));
            if (payloadASize > 0)
            {
                stream.write(static_cast<const char*>(payloadA), static_cast<std::streamsize>(payloadASize));
            }
            if (payloadBSize > 0)
            {
                stream.write(static_cast<const char*>(payloadB), static_cast<std::streamsize>(payloadBSize));
            }

            return stream.good();
        }

        bool ReadBinaryFileHeader(
            const std::filesystem::path& path,
            void* headerData,
            const std::size_t headerSize,
            std::ifstream& outStream)
        {
            outStream = std::ifstream(path, std::ios::binary);
            if (!outStream.is_open())
            {
                return false;
            }

            outStream.read(static_cast<char*>(headerData), static_cast<std::streamsize>(headerSize));
            return outStream.good();
        }

        bool IsValidMeshData(const render::MeshData& meshData)
        {
            if (meshData.vertices.empty() || meshData.indices.empty())
            {
                return false;
            }

            return std::all_of(
                meshData.indices.begin(),
                meshData.indices.end(),
                [&meshData](const std::uint32_t index)
                {
                    return static_cast<std::size_t>(index) < meshData.vertices.size();
                });
        }

        bool GetMeshBinaryPayloadSizes(
            const std::filesystem::path& binaryPath,
            const MeshBinaryHeader& header,
            std::size_t& outVertexCount,
            std::size_t& outIndexCount,
            std::size_t& outVertexBytes,
            std::size_t& outIndexBytes)
        {
            const std::size_t maxSize = std::numeric_limits<std::size_t>::max();
            if (header.vertexCount == 0 ||
                header.indexCount == 0 ||
                header.vertexCount > maxSize ||
                header.indexCount > maxSize ||
                header.vertexCount > maxSize / sizeof(render::MeshVertex) ||
                header.indexCount > maxSize / sizeof(std::uint32_t))
            {
                return false;
            }

            outVertexCount = static_cast<std::size_t>(header.vertexCount);
            outIndexCount = static_cast<std::size_t>(header.indexCount);
            outVertexBytes = outVertexCount * sizeof(render::MeshVertex);
            outIndexBytes = outIndexCount * sizeof(std::uint32_t);
            if (outVertexBytes > maxSize - outIndexBytes)
            {
                return false;
            }

            std::error_code ec;
            const std::uintmax_t fileSize = std::filesystem::file_size(binaryPath, ec);
            if (ec || fileSize < sizeof(MeshBinaryHeader))
            {
                return false;
            }

            const std::uintmax_t payloadBytes =
                static_cast<std::uintmax_t>(outVertexBytes + outIndexBytes);
            return payloadBytes <= fileSize - sizeof(MeshBinaryHeader);
        }

        bool GetTextureBinaryPayloadSize(
            const std::filesystem::path& binaryPath,
            const TextureBinaryHeader& header,
            std::size_t& outPixelBytes)
        {
            constexpr std::size_t kRgba8Channels = 4;
            const std::size_t maxSize = std::numeric_limits<std::size_t>::max();
            if (header.width == 0 ||
                header.height == 0 ||
                header.channels != kRgba8Channels ||
                header.srgb > 1 ||
                static_cast<std::size_t>(header.width) > maxSize / kRgba8Channels)
            {
                return false;
            }

            const std::size_t rowBytes = static_cast<std::size_t>(header.width) * kRgba8Channels;
            if (static_cast<std::size_t>(header.height) > maxSize / rowBytes)
            {
                return false;
            }

            const std::size_t expectedPixelBytes = rowBytes * static_cast<std::size_t>(header.height);
            if (header.pixelBytes != expectedPixelBytes)
            {
                return false;
            }

            std::error_code ec;
            const std::uintmax_t fileSize = std::filesystem::file_size(binaryPath, ec);
            if (ec || fileSize < sizeof(TextureBinaryHeader))
            {
                return false;
            }

            if (static_cast<std::uintmax_t>(expectedPixelBytes) > fileSize - sizeof(TextureBinaryHeader))
            {
                return false;
            }

            outPixelBytes = expectedPixelBytes;
            return true;
        }

        bool CopyRgba8Image(const DirectX::Image& image, render::TextureData& outTexture)
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

            outTexture = render::TextureData{};
            outTexture.width = width;
            outTexture.height = height;
            outTexture.channels = 4;
            outTexture.pixelsRgba8.resize(dstRowPitch * static_cast<size_t>(height));

            for (std::uint32_t y = 0; y < height; ++y)
            {
                const auto* src = image.pixels + static_cast<size_t>(y) * image.rowPitch;
                auto* dst = outTexture.pixelsRgba8.data() + static_cast<size_t>(y) * dstRowPitch;
                std::memcpy(dst, src, dstRowPitch);
            }

            return true;
        }

        bool LoadTextureViaDirectXTex(const std::filesystem::path& path, render::TextureData& outTexture, core::Logger& logger)
        {
            DirectX::ScratchImage loaded;
            DirectX::TexMetadata metadata{};

            const std::string extension = ToLower(path.extension().string());
            HRESULT hr = E_FAIL;

            if (extension == ".dds")
            {
                hr = DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, loaded);
            }
            else if (extension == ".tga")
            {
                hr = DirectX::LoadFromTGAFile(path.c_str(), &metadata, loaded);
            }
            else if (extension == ".hdr")
            {
                hr = DirectX::LoadFromHDRFile(path.c_str(), &metadata, loaded);
            }
            else
            {
                return false;
            }

            if (FAILED(hr))
            {
                logger.Warning("ResourceManager: DirectXTex load failed for " + path.string());
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
                    logger.Warning("ResourceManager: DirectXTex RGBA8 conversion failed for " + path.string());
                    return false;
                }

                finalImage = rgba.GetImage(0, 0, 0);
            }

            if (finalImage == nullptr || !CopyRgba8Image(*finalImage, outTexture))
            {
                logger.Warning("ResourceManager: DirectXTex image copy failed for " + path.string());
                return false;
            }

            return true;
        }

        struct ObjVertexKey
        {
            int position = -1;
            int uv = -1;
            int normal = -1;

            bool operator==(const ObjVertexKey& other) const
            {
                return position == other.position && uv == other.uv && normal == other.normal;
            }
        };

        struct ObjVertexKeyHash
        {
            std::size_t operator()(const ObjVertexKey& key) const
            {
                std::size_t hash = std::hash<int>{}(key.position);
                hash ^= std::hash<int>{}(key.uv) + static_cast<std::size_t>(0x9e3779b9) + (hash << 6) + (hash >> 2);
                hash ^= std::hash<int>{}(key.normal) + static_cast<std::size_t>(0x9e3779b9) + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

        int ResolveObjIndex(const int rawIndex, const std::size_t valueCount)
        {
            if (rawIndex > 0)
            {
                const std::size_t index = static_cast<std::size_t>(rawIndex - 1);
                return index < valueCount ? static_cast<int>(index) : -1;
            }

            if (rawIndex < 0)
            {
                const auto index = static_cast<std::ptrdiff_t>(valueCount) + rawIndex;
                return index >= 0 && index < static_cast<std::ptrdiff_t>(valueCount) ? static_cast<int>(index) : -1;
            }

            return -1;
        }

        ObjVertexKey ParseObjVertexKey(
            const std::string& token,
            const std::size_t positionCount,
            const std::size_t uvCount,
            const std::size_t normalCount)
        {
            ObjVertexKey key;
            std::istringstream tokenStream(token);
            std::string field;
            int fieldIndex = 0;

            while (std::getline(tokenStream, field, '/'))
            {
                if (!field.empty())
                {
                    try
                    {
                        const int rawIndex = std::stoi(field);
                        if (fieldIndex == 0)
                        {
                            key.position = ResolveObjIndex(rawIndex, positionCount);
                        }
                        else if (fieldIndex == 1)
                        {
                            key.uv = ResolveObjIndex(rawIndex, uvCount);
                        }
                        else if (fieldIndex == 2)
                        {
                            key.normal = ResolveObjIndex(rawIndex, normalCount);
                        }
                    }
                    catch (const std::exception&)
                    {
                        throw std::runtime_error("Invalid OBJ face index: " + token);
                    }
                }
                ++fieldIndex;
            }

            if (key.position < 0)
            {
                throw std::runtime_error("OBJ face references a missing vertex: " + token);
            }

            return key;
        }

        MeshCpuAsset LoadMeshFromSource(const std::filesystem::path& path)
        {
            if (IsBuiltinSpherePath(path))
            {
                MeshCpuAsset asset;
                asset.dependencies.push_back(path);

                auto& meshData = asset.data;
                const std::uint32_t latitudeSegments = std::max<std::uint32_t>(kProceduralSphereLatitudeSegments, 3);
                const std::uint32_t longitudeSegments = std::max<std::uint32_t>(kProceduralSphereLongitudeSegments, 6);
                const std::uint32_t stride = longitudeSegments + 1;

                meshData.vertices.reserve(static_cast<std::size_t>(latitudeSegments + 1) * static_cast<std::size_t>(stride));
                meshData.indices.reserve(static_cast<std::size_t>(latitudeSegments - 1) * static_cast<std::size_t>(longitudeSegments) * 6);

                for (std::uint32_t latitude = 0; latitude <= latitudeSegments; ++latitude)
                {
                    const float v = static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
                    const float phi = v * kPi;
                    const float sinPhi = std::sin(phi);
                    const float cosPhi = std::cos(phi);

                    for (std::uint32_t longitude = 0; longitude <= longitudeSegments; ++longitude)
                    {
                        const float u = static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
                        const float theta = u * (2.0f * kPi);
                        const float sinTheta = std::sin(theta);
                        const float cosTheta = std::cos(theta);

                        const render::Float3 normal{
                            cosTheta * sinPhi,
                            cosPhi,
                            sinTheta * sinPhi,
                        };

                        render::MeshVertex vertex{};
                        vertex.position =
                        {
                            normal.x * kProceduralSphereRadius,
                            normal.y * kProceduralSphereRadius,
                            normal.z * kProceduralSphereRadius,
                        };
                        vertex.normal = normal;
                        vertex.uv = {u, 1.0f - v};
                        meshData.vertices.push_back(vertex);
                    }
                }

                for (std::uint32_t latitude = 0; latitude < latitudeSegments; ++latitude)
                {
                    for (std::uint32_t longitude = 0; longitude < longitudeSegments; ++longitude)
                    {
                        const std::uint32_t topLeft = latitude * stride + longitude;
                        const std::uint32_t bottomLeft = topLeft + stride;
                        const std::uint32_t topRight = topLeft + 1;
                        const std::uint32_t bottomRight = bottomLeft + 1;

                        if (latitude != 0)
                        {
                            meshData.indices.push_back(topLeft);
                            meshData.indices.push_back(bottomLeft);
                            meshData.indices.push_back(topRight);
                        }

                        if (latitude + 1 != latitudeSegments)
                        {
                            meshData.indices.push_back(topRight);
                            meshData.indices.push_back(bottomLeft);
                            meshData.indices.push_back(bottomRight);
                        }
                    }
                }

                return asset;
            }

            if (ToLower(path.extension().string()) != ".obj")
            {
                throw std::runtime_error("Unsupported mesh format without Assimp runtime: " + path.string());
            }

            std::ifstream stream(path);
            if (!stream.is_open())
            {
                throw std::runtime_error("Failed to open OBJ mesh: " + path.string());
            }

            MeshCpuAsset asset;
            asset.dependencies.push_back(path);
            std::vector<render::Float3> positions;
            std::vector<render::Float2> uvs;
            std::vector<render::Float3> normals;
            std::unordered_map<ObjVertexKey, std::uint32_t, ObjVertexKeyHash> vertexMap;
            std::vector<bool> hasNormal;
            std::string line;
            std::size_t lineNumber = 0;

            const auto addVertex = [&](const ObjVertexKey key) -> std::uint32_t
            {
                const auto found = vertexMap.find(key);
                if (found != vertexMap.end())
                {
                    return found->second;
                }

                render::MeshVertex vertex{};
                const auto& position = positions[static_cast<std::size_t>(key.position)];
                vertex.position = {position.x, position.y, -position.z};
                if (key.uv >= 0)
                {
                    const auto& uv = uvs[static_cast<std::size_t>(key.uv)];
                    vertex.uv = {uv.x, kMeshFlipUvY ? (1.0f - uv.y) : uv.y};
                }
                if (key.normal >= 0)
                {
                    const auto& normal = normals[static_cast<std::size_t>(key.normal)];
                    vertex.normal = {normal.x, normal.y, -normal.z};
                }

                const auto index = static_cast<std::uint32_t>(asset.data.vertices.size());
                vertexMap.emplace(key, index);
                asset.data.vertices.push_back(vertex);
                hasNormal.push_back(key.normal >= 0);
                return index;
            };

            while (std::getline(stream, line))
            {
                ++lineNumber;
                std::istringstream lineStream(line);
                std::string type;
                lineStream >> type;
                if (type.empty() || type.front() == '#')
                {
                    continue;
                }

                if (type == "v")
                {
                    render::Float3 position{};
                    if (!(lineStream >> position.x >> position.y >> position.z))
                    {
                        throw std::runtime_error("Invalid OBJ vertex at line " + std::to_string(lineNumber));
                    }
                    positions.push_back(position);
                    continue;
                }

                if (type == "vt")
                {
                    render::Float2 uv{};
                    if (!(lineStream >> uv.x >> uv.y))
                    {
                        throw std::runtime_error("Invalid OBJ texture coordinate at line " + std::to_string(lineNumber));
                    }
                    uvs.push_back(uv);
                    continue;
                }

                if (type == "vn")
                {
                    render::Float3 normal{};
                    if (!(lineStream >> normal.x >> normal.y >> normal.z))
                    {
                        throw std::runtime_error("Invalid OBJ normal at line " + std::to_string(lineNumber));
                    }
                    normals.push_back(normal);
                    continue;
                }

                if (type != "f")
                {
                    continue;
                }

                std::vector<ObjVertexKey> face;
                std::string token;
                while (lineStream >> token)
                {
                    face.push_back(ParseObjVertexKey(token, positions.size(), uvs.size(), normals.size()));
                }

                if (face.size() < 3)
                {
                    throw std::runtime_error("OBJ face has fewer than three vertices at line " + std::to_string(lineNumber));
                }

                for (std::size_t index = 1; index + 1 < face.size(); ++index)
                {
                    const std::uint32_t first = addVertex(face[0]);
                    const std::uint32_t second = addVertex(face[index]);
                    const std::uint32_t third = addVertex(face[index + 1]);
                    asset.data.indices.push_back(first);
                    if constexpr (kMeshReverseWinding)
                    {
                        asset.data.indices.push_back(third);
                        asset.data.indices.push_back(second);
                    }
                    else
                    {
                        asset.data.indices.push_back(second);
                        asset.data.indices.push_back(third);
                    }
                }
            }

            for (std::size_t index = 0; index + 2 < asset.data.indices.size(); index += 3)
            {
                const auto first = asset.data.indices[index];
                const auto second = asset.data.indices[index + 1];
                const auto third = asset.data.indices[index + 2];
                if (hasNormal[first] && hasNormal[second] && hasNormal[third])
                {
                    continue;
                }

                const auto& a = asset.data.vertices[first].position;
                const auto& b = asset.data.vertices[second].position;
                const auto& c = asset.data.vertices[third].position;
                const render::Float3 ab{b.x - a.x, b.y - a.y, b.z - a.z};
                const render::Float3 ac{c.x - a.x, c.y - a.y, c.z - a.z};
                const render::Float3 faceNormal{
                    ab.y * ac.z - ab.z * ac.y,
                    ab.z * ac.x - ab.x * ac.z,
                    ab.x * ac.y - ab.y * ac.x,
                };

                if (!hasNormal[first]) asset.data.vertices[first].normal = faceNormal;
                if (!hasNormal[second]) asset.data.vertices[second].normal = faceNormal;
                if (!hasNormal[third]) asset.data.vertices[third].normal = faceNormal;
            }

            for (std::size_t index = 0; index < asset.data.vertices.size(); ++index)
            {
                if (hasNormal[index])
                {
                    continue;
                }

                auto& normal = asset.data.vertices[index].normal;
                const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                if (length > std::numeric_limits<float>::epsilon())
                {
                    normal.x /= length;
                    normal.y /= length;
                    normal.z /= length;
                }
            }

            if (asset.data.vertices.empty() || asset.data.indices.empty())
            {
                throw std::runtime_error("OBJ contains no renderable triangles: " + path.string());
            }

            return asset;
        }

        TextureCpuAsset LoadTextureFromSource(const std::filesystem::path& path, core::Logger& logger)
        {
            TextureCpuAsset asset;
            asset.dependencies.push_back(path);

            if (kTextureUseDirectXTexFirst && LoadTextureViaDirectXTex(path, asset.data, logger))
            {
                return asset;
            }

            int width = 0;
            int height = 0;
            int channels = 0;

            stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, kTextureRequestedChannels);
            if (pixels == nullptr)
            {
                throw std::runtime_error("stb_image failed to load texture");
            }

            const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(kTextureRequestedChannels);
            asset.data.width = static_cast<std::uint32_t>(width);
            asset.data.height = static_cast<std::uint32_t>(height);
            asset.data.channels = static_cast<std::uint32_t>(kTextureRequestedChannels);
            asset.data.srgb = kTextureForceSrgb;
            asset.data.pixelsRgba8.assign(pixels, pixels + pixelCount);

            stbi_image_free(pixels);
            return asset;
        }

        bool WriteMeshBinary(
            const std::filesystem::path& binaryPath,
            const std::filesystem::path& sourcePath,
            const render::MeshData& meshData)
        {
            FileStamp sourceStamp;
            if (!TryGetFileStamp(sourcePath, sourceStamp))
            {
                return false;
            }

            MeshBinaryHeader header{};
            header.magic = kMeshBinaryMagic;
            header.version = kMeshBinaryVersion;
            header.sourceWriteTime = sourceStamp.writeTime;
            header.sourceFileSize = sourceStamp.fileSize;
            header.pipelineSignature = kMeshPipelineSignature;
            header.vertexCount = static_cast<std::uint64_t>(meshData.vertices.size());
            header.indexCount = static_cast<std::uint64_t>(meshData.indices.size());

            return WriteBinaryFile(
                binaryPath,
                &header,
                sizeof(header),
                meshData.vertices.data(),
                meshData.vertices.size() * sizeof(render::MeshVertex),
                meshData.indices.data(),
                meshData.indices.size() * sizeof(std::uint32_t));
        }

        bool WriteTextureBinary(
            const std::filesystem::path& binaryPath,
            const std::filesystem::path& sourcePath,
            const render::TextureData& textureData)
        {
            FileStamp sourceStamp;
            if (!TryGetFileStamp(sourcePath, sourceStamp))
            {
                return false;
            }

            TextureBinaryHeader header{};
            header.magic = kTextureBinaryMagic;
            header.version = kTextureBinaryVersion;
            header.sourceWriteTime = sourceStamp.writeTime;
            header.sourceFileSize = sourceStamp.fileSize;
            header.pipelineSignature = kTexturePipelineSignature;
            header.width = textureData.width;
            header.height = textureData.height;
            header.channels = textureData.channels;
            header.srgb = textureData.srgb ? 1u : 0u;
            header.pixelBytes = static_cast<std::uint64_t>(textureData.pixelsRgba8.size());

            return WriteBinaryFile(
                binaryPath,
                &header,
                sizeof(header),
                textureData.pixelsRgba8.data(),
                textureData.pixelsRgba8.size(),
                nullptr,
                0);
        }

        MeshCpuAsset ReadMeshBinary(const std::filesystem::path& binaryPath)
        {
            MeshBinaryHeader header{};
            std::ifstream stream;
            if (!ReadBinaryFileHeader(binaryPath, &header, sizeof(header), stream))
            {
                throw std::runtime_error("Failed to read mesh binary header");
            }

            if (header.magic != kMeshBinaryMagic || header.version != kMeshBinaryVersion)
            {
                throw std::runtime_error("Invalid mesh binary header");
            }

            std::size_t vertexCount = 0;
            std::size_t indexCount = 0;
            std::size_t vertexBytes = 0;
            std::size_t indexBytes = 0;
            if (!GetMeshBinaryPayloadSizes(
                    binaryPath,
                    header,
                    vertexCount,
                    indexCount,
                    vertexBytes,
                    indexBytes))
            {
                throw std::runtime_error("Invalid mesh binary payload size");
            }

            MeshCpuAsset asset;
            asset.loadedFromBinaryCache = true;
            asset.dependencies.push_back(binaryPath);
            asset.data.vertices.resize(vertexCount);
            asset.data.indices.resize(indexCount);

            stream.read(reinterpret_cast<char*>(asset.data.vertices.data()), static_cast<std::streamsize>(vertexBytes));
            stream.read(reinterpret_cast<char*>(asset.data.indices.data()), static_cast<std::streamsize>(indexBytes));

            if (!stream || !IsValidMeshData(asset.data))
            {
                throw std::runtime_error("Invalid mesh binary payload");
            }

            return asset;
        }

        bool TryReadMeshBinaryCache(
            const std::filesystem::path& sourcePath,
            const std::filesystem::path& binaryPath,
            MeshCpuAsset& outAsset)
        {
            FileStamp sourceStamp;
            if (!TryGetFileStamp(sourcePath, sourceStamp))
            {
                return false;
            }

            MeshBinaryHeader header{};
            std::ifstream stream;
            if (!ReadBinaryFileHeader(binaryPath, &header, sizeof(header), stream))
            {
                return false;
            }

            if (header.magic != kMeshBinaryMagic || header.version != kMeshBinaryVersion)
            {
                return false;
            }

            if (!IsCacheCurrent(
                sourceStamp,
                header.sourceWriteTime,
                header.sourceFileSize,
                header.pipelineSignature,
                kMeshPipelineSignature))
            {
                return false;
            }

            std::size_t vertexCount = 0;
            std::size_t indexCount = 0;
            std::size_t vertexBytes = 0;
            std::size_t indexBytes = 0;
            if (!GetMeshBinaryPayloadSizes(
                    binaryPath,
                    header,
                    vertexCount,
                    indexCount,
                    vertexBytes,
                    indexBytes))
            {
                return false;
            }

            MeshCpuAsset cachedAsset;
            cachedAsset.loadedFromBinaryCache = true;
            cachedAsset.dependencies = {binaryPath};
            cachedAsset.data.vertices.resize(vertexCount);
            cachedAsset.data.indices.resize(indexCount);

            stream.read(reinterpret_cast<char*>(cachedAsset.data.vertices.data()), static_cast<std::streamsize>(vertexBytes));
            stream.read(reinterpret_cast<char*>(cachedAsset.data.indices.data()), static_cast<std::streamsize>(indexBytes));

            if (!stream || !IsValidMeshData(cachedAsset.data))
            {
                return false;
            }

            outAsset = std::move(cachedAsset);
            return true;
        }

        TextureCpuAsset ReadTextureBinary(const std::filesystem::path& binaryPath)
        {
            TextureBinaryHeader header{};
            std::ifstream stream;
            if (!ReadBinaryFileHeader(binaryPath, &header, sizeof(header), stream))
            {
                throw std::runtime_error("Failed to read texture binary header");
            }

            if (header.magic != kTextureBinaryMagic || header.version != kTextureBinaryVersion)
            {
                throw std::runtime_error("Invalid texture binary header");
            }

            std::size_t pixelBytes = 0;
            if (!GetTextureBinaryPayloadSize(binaryPath, header, pixelBytes))
            {
                throw std::runtime_error("Invalid texture binary payload size");
            }

            TextureCpuAsset asset;
            asset.loadedFromBinaryCache = true;
            asset.dependencies.push_back(binaryPath);
            asset.data.width = header.width;
            asset.data.height = header.height;
            asset.data.channels = header.channels;
            asset.data.srgb = header.srgb != 0;
            asset.data.pixelsRgba8.resize(pixelBytes);

            stream.read(
                reinterpret_cast<char*>(asset.data.pixelsRgba8.data()),
                static_cast<std::streamsize>(asset.data.pixelsRgba8.size()));

            if (!stream)
            {
                throw std::runtime_error("Failed to read texture binary payload");
            }

            return asset;
        }

        bool TryReadTextureBinaryCache(
            const std::filesystem::path& sourcePath,
            const std::filesystem::path& binaryPath,
            TextureCpuAsset& outAsset)
        {
            FileStamp sourceStamp;
            if (!TryGetFileStamp(sourcePath, sourceStamp))
            {
                return false;
            }

            TextureBinaryHeader header{};
            std::ifstream stream;
            if (!ReadBinaryFileHeader(binaryPath, &header, sizeof(header), stream))
            {
                return false;
            }

            if (header.magic != kTextureBinaryMagic || header.version != kTextureBinaryVersion)
            {
                return false;
            }

            if (!IsCacheCurrent(
                sourceStamp,
                header.sourceWriteTime,
                header.sourceFileSize,
                header.pipelineSignature,
                kTexturePipelineSignature))
            {
                return false;
            }

            std::size_t pixelBytes = 0;
            if (!GetTextureBinaryPayloadSize(binaryPath, header, pixelBytes))
            {
                return false;
            }

            TextureCpuAsset cachedAsset;
            cachedAsset.loadedFromBinaryCache = true;
            cachedAsset.dependencies = {binaryPath};
            cachedAsset.data.width = header.width;
            cachedAsset.data.height = header.height;
            cachedAsset.data.channels = header.channels;
            cachedAsset.data.srgb = header.srgb != 0;
            cachedAsset.data.pixelsRgba8.resize(pixelBytes);

            stream.read(
                reinterpret_cast<char*>(cachedAsset.data.pixelsRgba8.data()),
                static_cast<std::streamsize>(cachedAsset.data.pixelsRgba8.size()));

            if (!stream)
            {
                return false;
            }

            outAsset = std::move(cachedAsset);
            return true;
        }

        MeshCpuAsset LoadMeshCpuAsset(const std::filesystem::path& resolvedPath)
        {
            ZoneScoped;
#ifdef TRACY_ENABLE
            const std::string zonePath = resolvedPath.filename().string();
            ZoneText(zonePath.c_str(), zonePath.size());
#endif

            if (IsMeshBinaryPath(resolvedPath))
            {
                return ReadMeshBinary(resolvedPath);
            }

            const std::filesystem::path binaryPath = BuildMeshBinaryPath(resolvedPath);
            MeshCpuAsset cachedAsset;
            if (TryReadMeshBinaryCache(resolvedPath, binaryPath, cachedAsset))
            {
                cachedAsset.dependencies.insert(cachedAsset.dependencies.begin(), resolvedPath);
                return cachedAsset;
            }

            MeshCpuAsset sourceAsset = LoadMeshFromSource(resolvedPath);
            if (WriteMeshBinary(binaryPath, resolvedPath, sourceAsset.data))
            {
                sourceAsset.dependencies.push_back(binaryPath);
            }

            return sourceAsset;
        }

        TextureCpuAsset LoadTextureCpuAsset(const std::filesystem::path& resolvedPath, core::Logger& logger)
        {
            ZoneScoped;
#ifdef TRACY_ENABLE
            const std::string zonePath = resolvedPath.filename().string();
            ZoneText(zonePath.c_str(), zonePath.size());
#endif

            if (IsTextureBinaryPath(resolvedPath))
            {
                return ReadTextureBinary(resolvedPath);
            }

            const std::filesystem::path binaryPath = BuildTextureBinaryPath(resolvedPath);
            TextureCpuAsset cachedAsset;
            if (TryReadTextureBinaryCache(resolvedPath, binaryPath, cachedAsset))
            {
                cachedAsset.dependencies.insert(cachedAsset.dependencies.begin(), resolvedPath);
                return cachedAsset;
            }

            TextureCpuAsset sourceAsset = LoadTextureFromSource(resolvedPath, logger);
            if (WriteTextureBinary(binaryPath, resolvedPath, sourceAsset.data))
            {
                sourceAsset.dependencies.push_back(binaryPath);
            }

            return sourceAsset;
        }

        std::filesystem::path GetExecutableDirectory();

        core::Color ParseColor(const json& value, const core::Color& fallback)
        {
            if (!value.is_array() || value.size() != 4)
            {
                return fallback;
            }

            core::Color color = fallback;
            color.r = value[0].get<float>();
            color.g = value[1].get<float>();
            color.b = value[2].get<float>();
            color.a = value[3].get<float>();
            return color;
        }

        template <typename Cache>
        std::vector<std::string> CollectSortedKeys(const Cache& cache)
        {
            std::vector<std::string> keys;
            keys.reserve(cache.size());
            for (const auto& [key, _] : cache)
            {
                // NormalizeKey stores native Windows narrow path text.  Do
                // not feed it to u8path(): a non-ASCII user directory can be
                // encoded with the active Windows code page and would throw
                // a conversion system_error.  The native path constructor
                // performs the matching code-page conversion.
                const std::filesystem::path assetPath(key);
                std::string stableKey;
                // The project folder first: its assets get keys relative to it ("Content/Models/a.obj")
                std::vector<std::filesystem::path> roots;
                if (const auto& project = core::ServiceLocator::GetProjectContext(); project.IsInitialized())
                {
                    roots.push_back(project.Root());
                }
                roots.push_back(std::filesystem::u8path(MYENGINE_SOURCE_DIR));
                roots.push_back(GetExecutableDirectory());

                for (const auto& root : roots)
                {
                    if (root.empty())
                    {
                        continue;
                    }

                    std::error_code ec;
                    const std::filesystem::path relativePath = std::filesystem::relative(assetPath, root, ec);
                    if (ec || relativePath.empty() || relativePath.is_absolute())
                    {
                        continue;
                    }

                    const std::string relativeText = relativePath.generic_string();
                    if (relativeText == ".." || relativeText.rfind("../", 0) == 0)
                    {
                        continue;
                    }

                    stableKey = relativeText;
                    break;
                }

                keys.push_back(ToLower(stableKey.empty() ? assetPath.lexically_normal().generic_string() : stableKey));
            }

            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
            return keys;
        }

        std::filesystem::path GetExecutableDirectory()
        {
            wchar_t modulePath[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
            {
                return {};
            }

            return std::filesystem::path(modulePath).parent_path();
        }
    }

    ResourceManager::ResourceManager(render::IRenderAdapter& renderAdapter, core::Logger& logger)
        : renderAdapter_(renderAdapter), logger_(logger)
    {
        streamingContext_.priority = jobs::Priority::Streaming;
        lastHotReloadScanTime_ = std::chrono::steady_clock::now();
        fallbackMesh_ = CreateFallbackMesh();
        fallbackTexture_ = CreateFallbackTexture();
        fallbackShader_ = CreateFallbackShader();
        fallbackMaterial_ = CreateFallbackMaterial();
    }

    ResourceManager::~ResourceManager()
    {
        // The jobs capture this ResourceManager, so all of them must finish before its fields are destroyed
        shuttingDown_ = true;
        jobs::Wait(streamingContext_);
    }

    bool ResourceManager::LoadManifest(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        std::ifstream stream(resolvedPath);
        if (!stream.is_open())
        {
            logger_.Warning("ResourceManager: manifest open failed: " + resolvedPath.string());
            return false;
        }

        try
        {
            json root;
            stream >> root;

            const std::filesystem::path basePath = resolvedPath.parent_path();
            std::size_t meshCount = 0;
            std::size_t textureCount = 0;
            std::size_t shaderCount = 0;
            std::size_t materialCount = 0;

            const auto loadArray = [&](const char* key, auto&& loader, std::size_t& counter)
            {
                if (!root.contains(key) || !root[key].is_array())
                {
                    return;
                }

                for (const auto& value : root[key])
                {
                    if (!value.is_string())
                    {
                        continue;
                    }

                    loader(basePath / value.get<std::string>());
                    ++counter;
                }
            };

            loadArray("meshes", [this](const std::filesystem::path& assetPath) { Load<MeshAsset>(assetPath); }, meshCount);
            loadArray("textures", [this](const std::filesystem::path& assetPath) { Load<TextureAsset>(assetPath); }, textureCount);
            loadArray("shaders", [this](const std::filesystem::path& assetPath) { Load<ShaderAsset>(assetPath); }, shaderCount);
            loadArray("materials", [this](const std::filesystem::path& assetPath) { Load<MaterialAsset>(assetPath); }, materialCount);

            logger_.Info(
                "ResourceManager: manifest loaded " +
                resolvedPath.string() +
                " meshes=" + std::to_string(meshCount) +
                " textures=" + std::to_string(textureCount) +
                " shaders=" + std::to_string(shaderCount) +
                " materials=" + std::to_string(materialCount));
            return true;
        }
        catch (const std::exception& ex)
        {
            logger_.Error(
                "ResourceManager: manifest parse failed " +
                resolvedPath.string() +
                " error=" + ex.what());
            return false;
        }
    }

    void ResourceManager::UpdateHotReload()
    {
        PumpAsyncLoads();

        const auto now = std::chrono::steady_clock::now();
        if (now - lastHotReloadScanTime_ < hotReloadInterval_)
        {
            return;
        }

        lastHotReloadScanTime_ = now;
        ReloadChangedShaders();
        ReloadChangedMaterials();
        ReloadChangedMeshes();
        ReloadChangedTextures();
    }

    bool ResourceManager::SaveMaterial(const std::filesystem::path& path, const MaterialAsset& asset)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        const std::filesystem::path materialDirectory = resolvedPath.parent_path();

        auto makeRelativeAssetPath =
            [this, &materialDirectory](const std::string& normalizedPath)
            {
                const std::filesystem::path assetPath = ResolvePath(normalizedPath);
                std::error_code ec;
                std::filesystem::path relativePath = std::filesystem::relative(assetPath, materialDirectory, ec);
                if (ec || relativePath.empty())
                {
                    relativePath = assetPath;
                }

                return relativePath.generic_string();
            };

        json descriptor;
        descriptor["shader"] = makeRelativeAssetPath(asset.shaderPath);
        descriptor["texture"] = makeRelativeAssetPath(asset.texturePath);
        descriptor["tint"] = json::array({asset.tint.r, asset.tint.g, asset.tint.b, asset.tint.a});

        try
        {
            std::filesystem::create_directories(materialDirectory);
            std::ofstream stream(resolvedPath);
            if (!stream.is_open())
            {
                logger_.Warning("ResourceManager: material save open failed: " + resolvedPath.string());
                return false;
            }

            stream << descriptor.dump(2);
            if (!stream.good())
            {
                logger_.Warning("ResourceManager: material save write failed: " + resolvedPath.string());
                return false;
            }

            logger_.Info("ResourceManager: material saved " + resolvedPath.string());
            Reload<MaterialAsset>(resolvedPath);
            return true;
        }
        catch (const std::exception& ex)
        {
            logger_.Warning(
                "ResourceManager: material save failed: " +
                resolvedPath.string() +
                " error=" + ex.what());
            return false;
        }
    }

    std::filesystem::path ResourceManager::ResolvePath(const std::filesystem::path& path) const
    {
        ZoneScoped;

        // The project, then the engine content, the working directory and the executable folder
        if (const auto& project = core::ServiceLocator::GetProjectContext(); project.IsInitialized())
        {
            return project.ResolveContentPath(path);
        }

        std::error_code ec;

        if (path.is_absolute())
        {
            const auto canonical = std::filesystem::weakly_canonical(path, ec);
            return ec ? path.lexically_normal() : canonical;
        }

        const auto normalizeExisting = [&](const std::filesystem::path& candidate)
        {
            const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
            return ec ? candidate.lexically_normal() : canonical;
        };

        const std::filesystem::path projectRelative = std::filesystem::u8path(MYENGINE_SOURCE_DIR) / path;
        if (std::filesystem::exists(projectRelative, ec))
        {
            return normalizeExisting(projectRelative);
        }

        if (std::filesystem::exists(path, ec))
        {
            return normalizeExisting(path);
        }

        const std::filesystem::path executableRelative = GetExecutableDirectory() / path;
        if (!executableRelative.empty() && std::filesystem::exists(executableRelative, ec))
        {
            return normalizeExisting(executableRelative);
        }

        return projectRelative.lexically_normal();
    }

    std::vector<std::string> ResourceManager::GetKnownMeshKeys() const
    {
        return CollectSortedKeys(meshCache_);
    }

    std::vector<std::string> ResourceManager::GetKnownTextureKeys() const
    {
        return CollectSortedKeys(textureCache_);
    }

    std::vector<std::string> ResourceManager::GetKnownShaderKeys() const
    {
        return CollectSortedKeys(shaderCache_);
    }

    std::vector<std::string> ResourceManager::GetKnownMaterialKeys() const
    {
        return CollectSortedKeys(materialCache_);
    }

    std::uint64_t ResourceManager::EstimateResourceMemoryUsageBytes() const
    {
        std::uint64_t totalBytes = 0;

        for (const auto& [_, resource] : meshCache_)
        {
            if (resource == nullptr)
            {
                continue;
            }

            totalBytes += static_cast<std::uint64_t>(resource->asset.data.vertices.size()) * sizeof(render::MeshVertex);
            totalBytes += static_cast<std::uint64_t>(resource->asset.data.indices.size()) * sizeof(std::uint32_t);
        }

        for (const auto& [_, resource] : textureCache_)
        {
            if (resource == nullptr)
            {
                continue;
            }

            totalBytes += static_cast<std::uint64_t>(resource->asset.data.pixelsRgba8.size());
        }

        totalBytes += static_cast<std::uint64_t>(shaderCache_.size()) * 4096ull;
        totalBytes += static_cast<std::uint64_t>(materialCache_.size()) * 512ull;
        return totalBytes;
    }

    ResourceHandle<MeshAsset> ResourceManager::LoadMesh(const std::filesystem::path& path)
    {
        const auto& request = ResolveRequest(path);
        if (const auto it = meshCache_.find(request.key); it != meshCache_.end())
        {
            return it->second;
        }

        ScheduleMeshLoad(request.key, request.path);
        auto placeholder = BuildMeshPlaceholder(request.key, request.path);
        meshCache_.insert_or_assign(request.key, placeholder);
        return placeholder;
    }

    ResourceHandle<TextureAsset> ResourceManager::LoadTexture(const std::filesystem::path& path)
    {
        const auto& request = ResolveRequest(path);
        if (const auto it = textureCache_.find(request.key); it != textureCache_.end())
        {
            return it->second;
        }

        ScheduleTextureLoad(request.key, request.path);
        auto placeholder = BuildTexturePlaceholder(request.key, request.path);
        textureCache_.insert_or_assign(request.key, placeholder);
        return placeholder;
    }

    ResourceHandle<ShaderAsset> ResourceManager::LoadShader(const std::filesystem::path& path)
    {
        const auto& request = ResolveRequest(path);
        if (const auto it = shaderCache_.find(request.key); it != shaderCache_.end())
        {
            return it->second;
        }

        auto resource = LoadShaderInternal(request.key, request.path);
        if (resource == nullptr)
        {
            resource = BuildShaderFallback(request.key, request.path);
        }

        shaderCache_.insert_or_assign(request.key, resource);
        return resource;
    }

    ResourceHandle<MaterialAsset> ResourceManager::LoadMaterial(const std::filesystem::path& path)
    {
        const auto& request = ResolveRequest(path);
        if (const auto it = materialCache_.find(request.key); it != materialCache_.end())
        {
            return it->second;
        }

        auto resource = LoadMaterialInternal(request.key, request.path);
        if (resource == nullptr)
        {
            resource = BuildMaterialFallback(request.key, request.path);
        }

        materialCache_.insert_or_assign(request.key, resource);
        return resource;
    }

    ResourceHandle<MeshAsset> ResourceManager::ReloadMesh(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        const std::string key = NormalizeKey(resolvedPath);
        meshCache_.erase(key);
        ScheduleMeshLoad(key, resolvedPath);

        auto placeholder = BuildMeshPlaceholder(key, resolvedPath);
        meshCache_.insert_or_assign(key, placeholder);
        return placeholder;
    }

    ResourceHandle<TextureAsset> ResourceManager::ReloadTexture(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        const std::string key = NormalizeKey(resolvedPath);
        textureCache_.erase(key);
        ScheduleTextureLoad(key, resolvedPath);

        auto placeholder = BuildTexturePlaceholder(key, resolvedPath);
        textureCache_.insert_or_assign(key, placeholder);
        return placeholder;
    }

    ResourceHandle<ShaderAsset> ResourceManager::ReloadShader(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        shaderCache_.erase(NormalizeKey(resolvedPath));
        return LoadShader(resolvedPath);
    }

    ResourceHandle<MaterialAsset> ResourceManager::ReloadMaterial(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        materialCache_.erase(NormalizeKey(resolvedPath));
        return LoadMaterial(resolvedPath);
    }

    void ResourceManager::PumpAsyncLoads()
    {
        ZoneScoped;

        std::vector<ReadyLoad> loadsToFinalize;
        std::size_t remainingReadyLoadCount = 0;
        {
            std::lock_guard<std::mutex> lock(readyLoadsMutex_);
            const std::size_t loadCount = std::min(kMaxFinalizationsPerFrame, readyLoads_.size());
            loadsToFinalize.reserve(loadCount);

            for (std::size_t index = 0; index < loadCount; ++index)
            {
                loadsToFinalize.push_back(std::move(readyLoads_[index]));
            }

            if (loadCount > 0)
            {
                readyLoads_.erase(readyLoads_.begin(), readyLoads_.begin() + loadCount);
            }
            remainingReadyLoadCount = readyLoads_.size();
        }

        // GPU resources are created on the main thread and never while the ready queue is locked
        for (ReadyLoad& readyLoad : loadsToFinalize)
        {
            std::visit(
                [this](auto& result)
                {
                    using Result = std::decay_t<decltype(result)>;
                    if constexpr (std::is_same_v<Result, MeshLoadResult>)
                    {
                        FinalizeMeshLoad(std::move(result));
                    }
                    else
                    {
                        FinalizeTextureLoad(std::move(result));
                    }
                },
                readyLoad);
        }

        TracyPlot("Resources/PendingMeshLoads", static_cast<std::int64_t>(pendingMeshLoads_.size()));
        TracyPlot("Resources/PendingTextureLoads", static_cast<std::int64_t>(pendingTextureLoads_.size()));
        TracyPlot("Resources/ReadyLoads", static_cast<std::int64_t>(remainingReadyLoadCount));
    }

    void ResourceManager::ReloadChangedMeshes()
    {
        std::vector<std::filesystem::path> changed;
        changed.reserve(meshCache_.size());

        for (const auto& [_, resource] : meshCache_)
        {
            if (resource != nullptr && HasChanged(resource->dependencies))
            {
                changed.push_back(resource->sourcePath);
            }
        }

        for (const auto& path : changed)
        {
            logger_.Info("ResourceManager: hot reload mesh " + path.string());
            Reload<MeshAsset>(path);
        }
    }

    void ResourceManager::ReloadChangedTextures()
    {
        std::vector<std::filesystem::path> changed;
        changed.reserve(textureCache_.size());

        for (const auto& [_, resource] : textureCache_)
        {
            if (resource != nullptr && HasChanged(resource->dependencies))
            {
                changed.push_back(resource->sourcePath);
            }
        }

        for (const auto& path : changed)
        {
            logger_.Info("ResourceManager: hot reload texture " + path.string());
            Reload<TextureAsset>(path);
        }
    }

    void ResourceManager::ReloadChangedShaders()
    {
        std::vector<std::filesystem::path> changed;
        changed.reserve(shaderCache_.size());

        for (const auto& [_, resource] : shaderCache_)
        {
            if (resource != nullptr && HasChanged(resource->dependencies))
            {
                changed.push_back(resource->sourcePath);
            }
        }

        for (const auto& path : changed)
        {
            logger_.Info("ResourceManager: hot reload shader " + path.string());
            Reload<ShaderAsset>(path);
        }
    }

    void ResourceManager::ReloadChangedMaterials()
    {
        std::vector<std::filesystem::path> changed;
        changed.reserve(materialCache_.size());

        for (const auto& [_, resource] : materialCache_)
        {
            if (resource != nullptr && HasChanged(resource->dependencies))
            {
                changed.push_back(resource->sourcePath);
            }
        }

        for (const auto& path : changed)
        {
            logger_.Info("ResourceManager: hot reload material " + path.string());
            Reload<MaterialAsset>(path);
        }
    }

    void ResourceManager::ScheduleMeshLoad(const std::string& key, const std::filesystem::path& path)
    {
        if (shuttingDown_)
        {
            return;
        }

        auto request = std::make_shared<LoadRequest>();
        request->key = key;
        request->path = path;
        request->generation = ++meshLoadGenerations_[key];
        pendingMeshLoads_.insert_or_assign(key, request->generation);

        logger_.Info("ResourceManager: scheduled streaming mesh load " + path.string());
        jobs::Execute(streamingContext_, [this, request](jobs::JobArgs)
        {
            if (shuttingDown_)
            {
                return;
            }

            MeshLoadResult result;
            result.request = request;
            try
            {
                result.cpuAsset.emplace(LoadMeshCpuAsset(request->path));
            }
            catch (const std::exception& ex)
            {
                result.error = ex.what();
            }
            catch (...)
            {
                result.error = "unknown error";
            }

            if (shuttingDown_)
            {
                return;
            }

            std::lock_guard<std::mutex> lock(readyLoadsMutex_);
            if (!shuttingDown_)
            {
                readyLoads_.emplace_back(std::move(result));
            }
        });
    }

    void ResourceManager::ScheduleTextureLoad(const std::string& key, const std::filesystem::path& path)
    {
        if (shuttingDown_)
        {
            return;
        }

        auto request = std::make_shared<LoadRequest>();
        request->key = key;
        request->path = path;
        request->generation = ++textureLoadGenerations_[key];
        pendingTextureLoads_.insert_or_assign(key, request->generation);

        logger_.Info("ResourceManager: scheduled streaming texture load " + path.string());
        jobs::Execute(streamingContext_, [this, request](jobs::JobArgs)
        {
            if (shuttingDown_)
            {
                return;
            }

            TextureLoadResult result;
            result.request = request;
            try
            {
                result.cpuAsset.emplace(LoadTextureCpuAsset(request->path, logger_));
            }
            catch (const std::exception& ex)
            {
                result.error = ex.what();
            }
            catch (...)
            {
                result.error = "unknown error";
            }

            if (shuttingDown_)
            {
                return;
            }

            std::lock_guard<std::mutex> lock(readyLoadsMutex_);
            if (!shuttingDown_)
            {
                readyLoads_.emplace_back(std::move(result));
            }
        });
    }

    void ResourceManager::FinalizeMeshLoad(MeshLoadResult result)
    {
        const LoadRequest& request = *result.request;
        const auto pendingIt = pendingMeshLoads_.find(request.key);
        if (pendingIt == pendingMeshLoads_.end() || pendingIt->second != request.generation)
        {
            // A newer hot reload request owns this key now
            return;
        }

        ZoneScopedN("ResourceManager::FinalizeMesh");

        try
        {
            if (!result.cpuAsset.has_value())
            {
                throw std::runtime_error(result.error.empty() ? "load returned no result" : result.error);
            }

            auto resource = BuildMeshResource(request.key, request.path, std::move(*result.cpuAsset));
            meshCache_.insert_or_assign(
                request.key,
                resource != nullptr ? resource : BuildMeshPlaceholder(request.key, request.path));
        }
        catch (const std::exception& ex)
        {
            logger_.Warning(
                "ResourceManager: streaming mesh load failed " +
                request.path.string() +
                " error=" + ex.what());
            meshCache_.insert_or_assign(request.key, BuildMeshPlaceholder(request.key, request.path));
        }

        pendingMeshLoads_.erase(pendingIt);
    }

    void ResourceManager::FinalizeTextureLoad(TextureLoadResult result)
    {
        const LoadRequest& request = *result.request;
        const auto pendingIt = pendingTextureLoads_.find(request.key);
        if (pendingIt == pendingTextureLoads_.end() || pendingIt->second != request.generation)
        {
            // A newer hot reload request owns this key now
            return;
        }

        ZoneScopedN("ResourceManager::FinalizeTexture");

        try
        {
            if (!result.cpuAsset.has_value())
            {
                throw std::runtime_error(result.error.empty() ? "load returned no result" : result.error);
            }

            auto resource = BuildTextureResource(request.key, request.path, std::move(*result.cpuAsset));
            textureCache_.insert_or_assign(
                request.key,
                resource != nullptr ? resource : BuildTexturePlaceholder(request.key, request.path));
        }
        catch (const std::exception& ex)
        {
            logger_.Warning(
                "ResourceManager: streaming texture load failed " +
                request.path.string() +
                " error=" + ex.what());
            textureCache_.insert_or_assign(request.key, BuildTexturePlaceholder(request.key, request.path));
        }

        pendingTextureLoads_.erase(pendingIt);
    }

    ResourceHandle<MeshAsset> ResourceManager::BuildMeshResource(
        const std::string& key,
        const std::filesystem::path& path,
        MeshCpuAsset cpuAsset)
    {
        if (!IsValidMeshData(cpuAsset.data))
        {
            logger_.Warning("ResourceManager: invalid mesh data for " + path.string());
            return nullptr;
        }

        MeshAsset asset;
        asset.data = std::move(cpuAsset.data);
        asset.gpuHandle = renderAdapter_.UploadMesh(asset.data);
        if (!asset.gpuHandle.IsValid())
        {
            logger_.Warning("ResourceManager: UploadMesh failed for " + path.string());
            return nullptr;
        }

        logger_.Info(
            "ResourceManager: mesh ready " +
            path.string() +
            " vertices=" + std::to_string(asset.data.vertices.size()) +
            " indices=" + std::to_string(asset.data.indices.size()) +
            " source=" + std::string(cpuAsset.loadedFromBinaryCache ? "binary" : "source"));

        return CreateResource(key, path, std::move(asset), BuildDependencies(cpuAsset.dependencies));
    }

    ResourceHandle<TextureAsset> ResourceManager::BuildTextureResource(
        const std::string& key,
        const std::filesystem::path& path,
        TextureCpuAsset cpuAsset)
    {
        TextureAsset asset;
        asset.data = std::move(cpuAsset.data);
        asset.gpuHandle = renderAdapter_.CreateTexture(asset.data);
        if (!asset.gpuHandle.IsValid())
        {
            logger_.Warning("ResourceManager: CreateTexture failed for " + path.string());
            return nullptr;
        }

        logger_.Info(
            "ResourceManager: texture ready " +
            path.string() +
            " size=" + std::to_string(asset.data.width) +
            "x" + std::to_string(asset.data.height) +
            " source=" + std::string(cpuAsset.loadedFromBinaryCache ? "binary" : "source"));

        return CreateResource(key, path, std::move(asset), BuildDependencies(cpuAsset.dependencies));
    }

    ResourceHandle<MeshAsset> ResourceManager::BuildMeshPlaceholder(
        const std::string& key,
        const std::filesystem::path& path)
    {
        MeshAsset asset = fallbackMesh_ != nullptr ? fallbackMesh_->asset : MeshAsset{};
        return CreateResource(key, path, std::move(asset), BuildDependencies({path}));
    }

    ResourceHandle<TextureAsset> ResourceManager::BuildTexturePlaceholder(
        const std::string& key,
        const std::filesystem::path& path)
    {
        TextureAsset asset = fallbackTexture_ != nullptr ? fallbackTexture_->asset : TextureAsset{};
        return CreateResource(key, path, std::move(asset), BuildDependencies({path}));
    }

    ResourceHandle<ShaderAsset> ResourceManager::LoadShaderInternal(
        const std::string& key,
        const std::filesystem::path& path)
    {
        render::ShaderProgramData program;
        std::vector<std::filesystem::path> dependencies;

        try
        {
            const std::string extension = ToLower(path.extension().string());
            if (extension == ".json")
            {
                std::ifstream stream(path);
                if (!stream.is_open())
                {
                    logger_.Warning("ResourceManager: shader descriptor open failed: " + path.string());
                    return nullptr;
                }

                json descriptor;
                stream >> descriptor;

                const std::string sourceValue = descriptor.value("source", std::string());
                if (sourceValue.empty())
                {
                    logger_.Warning("ResourceManager: shader descriptor has empty source: " + path.string());
                    return nullptr;
                }

                program.sourcePath = ResolvePath(path.parent_path() / sourceValue);
                program.vertexEntry = descriptor.value("vertexEntry", program.vertexEntry);
                program.pixelEntry = descriptor.value("pixelEntry", program.pixelEntry);
                program.vertexProfile = descriptor.value("vertexProfile", program.vertexProfile);
                program.pixelProfile = descriptor.value("pixelProfile", program.pixelProfile);
                dependencies = {path, program.sourcePath};
            }
            else
            {
                program.sourcePath = path;
                dependencies = {path};
            }
        }
        catch (const std::exception& ex)
        {
            logger_.Warning(
                "ResourceManager: shader descriptor parse failed: " +
                path.string() +
                " error=" + ex.what());
            return nullptr;
        }

        ShaderAsset asset;
        asset.program = program;
        asset.gpuHandle = renderAdapter_.CreateShaderProgram(asset.program);
        if (!asset.gpuHandle.IsValid())
        {
            logger_.Warning("ResourceManager: shader compile failed: " + path.string());
            return BuildShaderFallback(key, path, dependencies);
        }

        logger_.Info("ResourceManager: shader loaded " + path.string());
        return CreateResource(key, path, std::move(asset), BuildDependencies(dependencies));
    }

    ResourceHandle<MaterialAsset> ResourceManager::LoadMaterialInternal(
        const std::string& key,
        const std::filesystem::path& path)
    {
        std::ifstream stream(path);
        if (!stream.is_open())
        {
            logger_.Warning("ResourceManager: material open failed: " + path.string());
            return nullptr;
        }

        json descriptor;
        try
        {
            stream >> descriptor;
        }
        catch (const std::exception& ex)
        {
            logger_.Warning(
                "ResourceManager: material parse failed: " +
                path.string() +
                " error=" + ex.what());
            return nullptr;
        }

        MaterialAsset asset;
        asset.shaderPath = NormalizeKey(ResolvePath(path.parent_path() / descriptor.value("shader", std::string())));
        asset.texturePath = NormalizeKey(ResolvePath(path.parent_path() / descriptor.value("texture", std::string())));
        asset.tint = ParseColor(descriptor.value("tint", json::array()), asset.tint);

        if (asset.shaderPath.empty() || asset.texturePath.empty())
        {
            logger_.Warning("ResourceManager: material descriptor incomplete: " + path.string());
            return nullptr;
        }

        Load<ShaderAsset>(asset.shaderPath);
        Load<TextureAsset>(asset.texturePath);

        logger_.Info("ResourceManager: material loaded " + path.string());
        return CreateResource(key, path, std::move(asset), BuildDependencies({path}));
    }

    ResourceHandle<ShaderAsset> ResourceManager::BuildShaderFallback(
        const std::string& key,
        const std::filesystem::path& path,
        std::vector<std::filesystem::path> dependencies) const
    {
        if (dependencies.empty())
        {
            dependencies.push_back(path);
        }

        ShaderAsset asset = fallbackShader_ != nullptr ? fallbackShader_->asset : ShaderAsset{};
        return CreateResource(key, path, std::move(asset), BuildDependencies(dependencies));
    }

    ResourceHandle<MaterialAsset> ResourceManager::BuildMaterialFallback(
        const std::string& key,
        const std::filesystem::path& path) const
    {
        MaterialAsset asset = fallbackMaterial_ != nullptr ? fallbackMaterial_->asset : MaterialAsset{};
        return CreateResource(key, path, std::move(asset), BuildDependencies({path}));
    }

    std::string ResourceManager::NormalizeKey(const std::filesystem::path& path) const
    {
        return ToLower(path.lexically_normal().generic_string());
    }

    std::vector<ResourceDependency> ResourceManager::BuildDependencies(
        const std::vector<std::filesystem::path>& paths) const
    {
        std::vector<ResourceDependency> dependencies;
        dependencies.reserve(paths.size());

        for (const auto& rawPath : paths)
        {
            if (rawPath.empty())
            {
                continue;
            }

            const std::filesystem::path resolvedPath = ResolvePath(rawPath);
            ResourceDependency dependency;
            dependency.path = resolvedPath;

            std::error_code ec;
            const bool exists = std::filesystem::exists(resolvedPath, ec);
            if (ec)
            {
                continue;
            }

            dependency.existed = exists;
            if (exists)
            {
                dependency.lastWriteTime = std::filesystem::last_write_time(resolvedPath, ec);
                if (ec)
                {
                    continue;
                }
            }

            dependencies.push_back(std::move(dependency));
        }

        return dependencies;
    }

    bool ResourceManager::HasChanged(const std::vector<ResourceDependency>& dependencies) const
    {
        for (const auto& dependency : dependencies)
        {
            std::error_code ec;
            const bool exists = std::filesystem::exists(dependency.path, ec);
            if (ec)
            {
                return true;
            }

            if (exists != dependency.existed)
            {
                return true;
            }

            if (!exists)
            {
                continue;
            }

            const auto currentWriteTime = std::filesystem::last_write_time(dependency.path, ec);
            if (ec || currentWriteTime != dependency.lastWriteTime)
            {
                return true;
            }
        }

        return false;
    }

    ResourceHandle<MeshAsset> ResourceManager::CreateFallbackMesh()
    {
        render::MeshData meshData;
        meshData.vertices =
        {
            {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
            {{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
            {{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
            {{ 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        };
        meshData.indices = {0, 1, 2, 0, 2, 3};

        MeshAsset asset;
        asset.data = meshData;
        asset.gpuHandle = renderAdapter_.UploadMesh(asset.data);

        return CreateResource("__fallback_mesh__", {}, std::move(asset));
    }

    ResourceHandle<TextureAsset> ResourceManager::CreateFallbackTexture()
    {
        render::TextureData textureData;
        textureData.width = 2;
        textureData.height = 2;
        textureData.channels = 4;
        textureData.srgb = true;
        textureData.pixelsRgba8 =
        {
            255, 0, 255, 255,   0, 0, 0, 255,
            0, 0, 0, 255,       255, 0, 255, 255,
        };

        TextureAsset asset;
        asset.data = textureData;
        asset.gpuHandle = renderAdapter_.CreateTexture(asset.data);

        return CreateResource("__fallback_texture__", {}, std::move(asset));
    }

    ResourceHandle<ShaderAsset> ResourceManager::CreateFallbackShader()
    {
        ShaderAsset asset;
        asset.program.sourcePath = ResolvePath("assets/shaders/textured_lit.hlsl");
        asset.program.vertexEntry = "VSMain";
        asset.program.pixelEntry = "PSMain";
        asset.program.vertexProfile = "vs_5_0";
        asset.program.pixelProfile = "ps_5_0";
        asset.gpuHandle = renderAdapter_.CreateShaderProgram(asset.program);

        return CreateResource("__fallback_shader__", {}, std::move(asset));
    }

    ResourceHandle<MaterialAsset> ResourceManager::CreateFallbackMaterial()
    {
        MaterialAsset asset;
        asset.shaderPath = NormalizeKey(ResolvePath("assets/shaders/textured_lit.shader.json"));
        asset.texturePath = NormalizeKey(ResolvePath("assets/textures/debug.bmp"));
        asset.tint = core::Color{1.0f, 0.5f, 1.0f, 1.0f};

        return CreateResource("__fallback_material__", {}, std::move(asset));
    }

    const ResourceManager::ResolvedRequest& ResourceManager::ResolveRequest(const std::filesystem::path& path)
    {
        const std::string requested = path.generic_string();
        if (const auto it = resolvedRequests_.find(requested); it != resolvedRequests_.end())
        {
            return it->second;
        }

        const std::filesystem::path resolvedPath = ResolvePath(path);
        const auto [it, inserted] = resolvedRequests_.emplace(requested, ResolvedRequest{ resolvedPath, NormalizeKey(resolvedPath) });
        return it->second;
    }
}
