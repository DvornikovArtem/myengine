// ResourceManager.cpp

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <directxtex/DirectXTex.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <myengine/core/Logger.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/resource/ResourceManager.h>

namespace myengine::resource
{
    namespace
    {
        using json = nlohmann::json;

        constexpr std::array<char, 8> kMeshBinaryMagic{'M', 'Y', 'E', 'M', 'E', 'S', 'H', '1'};
        constexpr std::array<char, 8> kTextureBinaryMagic{'M', 'Y', 'E', 'T', 'E', 'X', '0', '1'};
        constexpr std::uint32_t kMeshBinaryVersion = 2;
        constexpr std::uint32_t kTextureBinaryVersion = 2;

        constexpr unsigned int kMeshImportFlags =
            aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_ImproveCacheLocality |
            aiProcess_GenSmoothNormals |
            aiProcess_PreTransformVertices |
            aiProcess_ConvertToLeftHanded |
            aiProcess_ValidateDataStructure;

        constexpr bool kMeshFlipUvY = false;
        constexpr bool kMeshReverseWinding = false;
        constexpr bool kTextureUseDirectXTexFirst = true;
        constexpr int kTextureRequestedChannels = STBI_rgb_alpha;
        constexpr bool kTextureForceSrgb = true;

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
            std::uint64_t hash = HashString("mesh-import-pipeline");
            hash = HashCombine(hash, static_cast<std::uint64_t>(kMeshImportFlags));
            hash = HashCombine(hash, static_cast<std::uint64_t>(kMeshFlipUvY ? 1u : 0u));
            hash = HashCombine(hash, static_cast<std::uint64_t>(kMeshReverseWinding ? 1u : 0u));
            hash = HashCombine(hash, static_cast<std::uint64_t>(sizeof(render::MeshVertex)));
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

        bool IsFutureReady(std::future<MeshCpuAsset>& future)
        {
            return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }

        bool IsFutureReady(std::future<TextureCpuAsset>& future)
        {
            return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }

        bool IsMeshBinaryPath(const std::filesystem::path& path)
        {
            return ToLower(path.extension().string()) == ".myemesh";
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

        MeshCpuAsset LoadMeshFromSource(const std::filesystem::path& path)
        {
            Assimp::Importer importer;
            const aiScene* scene = importer.ReadFile(path.string(), kMeshImportFlags);

            if (scene == nullptr || scene->mNumMeshes == 0)
            {
                throw std::runtime_error("Assimp failed to load scene: " + std::string(importer.GetErrorString()));
            }

            MeshCpuAsset asset;
            asset.dependencies.push_back(path);

            for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
            {
                const aiMesh* mesh = scene->mMeshes[meshIndex];
                if (mesh == nullptr || mesh->mNumVertices == 0)
                {
                    continue;
                }

                const std::uint32_t baseVertex = static_cast<std::uint32_t>(asset.data.vertices.size());
                asset.data.vertices.reserve(asset.data.vertices.size() + mesh->mNumVertices);

                for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex)
                {
                    render::MeshVertex vertex{};

                    const aiVector3D& position = mesh->mVertices[vertexIndex];
                    vertex.position = {position.x, position.y, position.z};

                    if (mesh->HasNormals())
                    {
                        const aiVector3D& normal = mesh->mNormals[vertexIndex];
                        vertex.normal = {normal.x, normal.y, normal.z};
                    }

                    if (mesh->HasTextureCoords(0))
                    {
                        const aiVector3D& uv = mesh->mTextureCoords[0][vertexIndex];
                        vertex.uv = {uv.x, kMeshFlipUvY ? (1.0f - uv.y) : uv.y};
                    }

                    asset.data.vertices.push_back(vertex);
                }

                for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
                {
                    const aiFace& face = mesh->mFaces[faceIndex];
                    if (face.mNumIndices != 3)
                    {
                        continue;
                    }

                    asset.data.indices.push_back(baseVertex + face.mIndices[0]);
                    if constexpr (kMeshReverseWinding)
                    {
                        asset.data.indices.push_back(baseVertex + face.mIndices[2]);
                        asset.data.indices.push_back(baseVertex + face.mIndices[1]);
                    }
                    else
                    {
                        asset.data.indices.push_back(baseVertex + face.mIndices[1]);
                        asset.data.indices.push_back(baseVertex + face.mIndices[2]);
                    }
                }
            }

            if (asset.data.vertices.empty() || asset.data.indices.empty())
            {
                throw std::runtime_error("Mesh contains no renderable triangles");
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

            MeshCpuAsset asset;
            asset.loadedFromBinaryCache = true;
            asset.dependencies.push_back(binaryPath);
            asset.data.vertices.resize(static_cast<std::size_t>(header.vertexCount));
            asset.data.indices.resize(static_cast<std::size_t>(header.indexCount));

            const std::size_t vertexBytes = asset.data.vertices.size() * sizeof(render::MeshVertex);
            const std::size_t indexBytes = asset.data.indices.size() * sizeof(std::uint32_t);

            if (vertexBytes > 0)
            {
                stream.read(reinterpret_cast<char*>(asset.data.vertices.data()), static_cast<std::streamsize>(vertexBytes));
            }
            if (indexBytes > 0)
            {
                stream.read(reinterpret_cast<char*>(asset.data.indices.data()), static_cast<std::streamsize>(indexBytes));
            }

            if (!stream.good() && !stream.eof())
            {
                throw std::runtime_error("Failed to read mesh binary payload");
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

            outAsset.loadedFromBinaryCache = true;
            outAsset.dependencies = {binaryPath};
            outAsset.data.vertices.resize(static_cast<std::size_t>(header.vertexCount));
            outAsset.data.indices.resize(static_cast<std::size_t>(header.indexCount));

            const std::size_t vertexBytes = outAsset.data.vertices.size() * sizeof(render::MeshVertex);
            const std::size_t indexBytes = outAsset.data.indices.size() * sizeof(std::uint32_t);

            if (vertexBytes > 0)
            {
                stream.read(reinterpret_cast<char*>(outAsset.data.vertices.data()), static_cast<std::streamsize>(vertexBytes));
            }
            if (indexBytes > 0)
            {
                stream.read(reinterpret_cast<char*>(outAsset.data.indices.data()), static_cast<std::streamsize>(indexBytes));
            }

            return stream.good() || stream.eof();
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

            TextureCpuAsset asset;
            asset.loadedFromBinaryCache = true;
            asset.dependencies.push_back(binaryPath);
            asset.data.width = header.width;
            asset.data.height = header.height;
            asset.data.channels = header.channels;
            asset.data.srgb = header.srgb != 0;
            asset.data.pixelsRgba8.resize(static_cast<std::size_t>(header.pixelBytes));

            if (!asset.data.pixelsRgba8.empty())
            {
                stream.read(
                    reinterpret_cast<char*>(asset.data.pixelsRgba8.data()),
                    static_cast<std::streamsize>(asset.data.pixelsRgba8.size()));
            }

            if (!stream.good() && !stream.eof())
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

            outAsset.loadedFromBinaryCache = true;
            outAsset.dependencies = {binaryPath};
            outAsset.data.width = header.width;
            outAsset.data.height = header.height;
            outAsset.data.channels = header.channels;
            outAsset.data.srgb = header.srgb != 0;
            outAsset.data.pixelsRgba8.resize(static_cast<std::size_t>(header.pixelBytes));

            if (!outAsset.data.pixelsRgba8.empty())
            {
                stream.read(
                    reinterpret_cast<char*>(outAsset.data.pixelsRgba8.data()),
                    static_cast<std::streamsize>(outAsset.data.pixelsRgba8.size()));
            }

            return stream.good() || stream.eof();
        }

        MeshCpuAsset LoadMeshCpuAsset(const std::filesystem::path& resolvedPath)
        {
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
    }

    ResourceManager::ResourceManager(render::IRenderAdapter& renderAdapter, core::Logger& logger)
        : renderAdapter_(renderAdapter), logger_(logger)
    {
        fallbackMesh_ = CreateFallbackMesh();
        fallbackTexture_ = CreateFallbackTexture();
        fallbackShader_ = CreateFallbackShader();
        fallbackMaterial_ = CreateFallbackMaterial();
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
        ReloadChangedShaders();
        ReloadChangedMaterials();
        ReloadChangedMeshes();
        ReloadChangedTextures();
    }

    std::filesystem::path ResourceManager::ResolvePath(const std::filesystem::path& path) const
    {
        std::error_code ec;

        if (path.is_absolute())
        {
            const auto canonical = std::filesystem::weakly_canonical(path, ec);
            return ec ? path.lexically_normal() : canonical;
        }

        const std::filesystem::path projectRelative = std::filesystem::path(MYENGINE_SOURCE_DIR) / path;
        if (std::filesystem::exists(projectRelative, ec))
        {
            const auto canonical = std::filesystem::weakly_canonical(projectRelative, ec);
            return ec ? projectRelative.lexically_normal() : canonical;
        }

        if (std::filesystem::exists(path, ec))
        {
            const auto canonical = std::filesystem::weakly_canonical(path, ec);
            return ec ? path.lexically_normal() : canonical;
        }

        return projectRelative.lexically_normal();
    }

    ResourceHandle<MeshAsset> ResourceManager::LoadMesh(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        const std::string key = NormalizeKey(resolvedPath);

        if (const auto ready = TryFinalizeMeshLoad(key); ready != nullptr)
        {
            return ready;
        }

        if (const auto it = meshCache_.find(key); it != meshCache_.end())
        {
            return it->second;
        }

        ScheduleMeshLoad(key, resolvedPath);
        auto placeholder = BuildMeshPlaceholder(key, resolvedPath);
        meshCache_.insert_or_assign(key, placeholder);
        return placeholder;
    }

    ResourceHandle<TextureAsset> ResourceManager::LoadTexture(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        const std::string key = NormalizeKey(resolvedPath);

        if (const auto ready = TryFinalizeTextureLoad(key); ready != nullptr)
        {
            return ready;
        }

        if (const auto it = textureCache_.find(key); it != textureCache_.end())
        {
            return it->second;
        }

        ScheduleTextureLoad(key, resolvedPath);
        auto placeholder = BuildTexturePlaceholder(key, resolvedPath);
        textureCache_.insert_or_assign(key, placeholder);
        return placeholder;
    }

    ResourceHandle<ShaderAsset> ResourceManager::LoadShader(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        const std::string key = NormalizeKey(resolvedPath);

        if (const auto it = shaderCache_.find(key); it != shaderCache_.end())
        {
            return it->second;
        }

        auto resource = LoadShaderInternal(key, resolvedPath);
        if (resource == nullptr)
        {
            resource = BuildShaderFallback(key, resolvedPath);
        }

        shaderCache_.insert_or_assign(key, resource);
        return resource;
    }

    ResourceHandle<MaterialAsset> ResourceManager::LoadMaterial(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        const std::string key = NormalizeKey(resolvedPath);

        if (const auto it = materialCache_.find(key); it != materialCache_.end())
        {
            return it->second;
        }

        auto resource = LoadMaterialInternal(key, resolvedPath);
        if (resource == nullptr)
        {
            resource = BuildMaterialFallback(key, resolvedPath);
        }

        materialCache_.insert_or_assign(key, resource);
        return resource;
    }

    ResourceHandle<MeshAsset> ResourceManager::ReloadMesh(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        const std::string key = NormalizeKey(resolvedPath);
        meshCache_.erase(key);

        if (pendingMeshLoads_.find(key) == pendingMeshLoads_.end())
        {
            ScheduleMeshLoad(key, resolvedPath);
        }

        auto placeholder = BuildMeshPlaceholder(key, resolvedPath);
        meshCache_.insert_or_assign(key, placeholder);
        return placeholder;
    }

    ResourceHandle<TextureAsset> ResourceManager::ReloadTexture(const std::filesystem::path& path)
    {
        const std::filesystem::path resolvedPath = ResolvePath(path);
        const std::string key = NormalizeKey(resolvedPath);
        textureCache_.erase(key);

        if (pendingTextureLoads_.find(key) == pendingTextureLoads_.end())
        {
            ScheduleTextureLoad(key, resolvedPath);
        }

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
        FinalizePendingMeshes();
        FinalizePendingTextures();
    }

    void ResourceManager::FinalizePendingMeshes()
    {
        std::vector<std::string> keys;
        keys.reserve(pendingMeshLoads_.size());

        for (const auto& [key, _] : pendingMeshLoads_)
        {
            keys.push_back(key);
        }

        for (const auto& key : keys)
        {
            TryFinalizeMeshLoad(key);
        }
    }

    void ResourceManager::FinalizePendingTextures()
    {
        std::vector<std::string> keys;
        keys.reserve(pendingTextureLoads_.size());

        for (const auto& [key, _] : pendingTextureLoads_)
        {
            keys.push_back(key);
        }

        for (const auto& key : keys)
        {
            TryFinalizeTextureLoad(key);
        }
    }

    void ResourceManager::ReloadChangedMeshes()
    {
        std::vector<std::filesystem::path> changed;
        changed.reserve(meshCache_.size());

        for (const auto& [key, resource] : meshCache_)
        {
            if (pendingMeshLoads_.find(key) != pendingMeshLoads_.end())
            {
                continue;
            }

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

        for (const auto& [key, resource] : textureCache_)
        {
            if (pendingTextureLoads_.find(key) != pendingTextureLoads_.end())
            {
                continue;
            }

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
        if (pendingMeshLoads_.find(key) != pendingMeshLoads_.end())
        {
            return;
        }

        logger_.Info("ResourceManager: scheduled async mesh load " + path.string());
        MeshLoadJob job;
        job.path = path;
        job.future = std::async(std::launch::async, [path]()
        {
            return LoadMeshCpuAsset(path);
        });

        pendingMeshLoads_.insert_or_assign(key, std::move(job));
    }

    void ResourceManager::ScheduleTextureLoad(const std::string& key, const std::filesystem::path& path)
    {
        if (pendingTextureLoads_.find(key) != pendingTextureLoads_.end())
        {
            return;
        }

        logger_.Info("ResourceManager: scheduled async texture load " + path.string());
        TextureLoadJob job;
        job.path = path;
        job.future = std::async(std::launch::async, [this, path]()
        {
            return LoadTextureCpuAsset(path, logger_);
        });

        pendingTextureLoads_.insert_or_assign(key, std::move(job));
    }

    ResourceHandle<MeshAsset> ResourceManager::TryFinalizeMeshLoad(const std::string& key)
    {
        const auto jobIt = pendingMeshLoads_.find(key);
        if (jobIt == pendingMeshLoads_.end() || !IsFutureReady(jobIt->second.future))
        {
            const auto cacheIt = meshCache_.find(key);
            return cacheIt != meshCache_.end() ? cacheIt->second : nullptr;
        }

        try
        {
            MeshCpuAsset cpuAsset = jobIt->second.future.get();
            auto resource = BuildMeshResource(key, jobIt->second.path, std::move(cpuAsset));
            meshCache_.insert_or_assign(key, resource != nullptr ? resource : BuildMeshPlaceholder(key, jobIt->second.path));
        }
        catch (const std::exception& ex)
        {
            logger_.Warning(
                "ResourceManager: async mesh load failed " +
                jobIt->second.path.string() +
                " error=" + ex.what());
            meshCache_.insert_or_assign(key, BuildMeshPlaceholder(key, jobIt->second.path));
        }

        pendingMeshLoads_.erase(jobIt);
        return meshCache_[key];
    }

    ResourceHandle<TextureAsset> ResourceManager::TryFinalizeTextureLoad(const std::string& key)
    {
        const auto jobIt = pendingTextureLoads_.find(key);
        if (jobIt == pendingTextureLoads_.end() || !IsFutureReady(jobIt->second.future))
        {
            const auto cacheIt = textureCache_.find(key);
            return cacheIt != textureCache_.end() ? cacheIt->second : nullptr;
        }

        try
        {
            TextureCpuAsset cpuAsset = jobIt->second.future.get();
            auto resource = BuildTextureResource(key, jobIt->second.path, std::move(cpuAsset));
            textureCache_.insert_or_assign(key, resource != nullptr ? resource : BuildTexturePlaceholder(key, jobIt->second.path));
        }
        catch (const std::exception& ex)
        {
            logger_.Warning(
                "ResourceManager: async texture load failed " +
                jobIt->second.path.string() +
                " error=" + ex.what());
            textureCache_.insert_or_assign(key, BuildTexturePlaceholder(key, jobIt->second.path));
        }

        pendingTextureLoads_.erase(jobIt);
        return textureCache_[key];
    }

    ResourceHandle<MeshAsset> ResourceManager::BuildMeshResource(
        const std::string& key,
        const std::filesystem::path& path,
        MeshCpuAsset cpuAsset)
    {
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
}