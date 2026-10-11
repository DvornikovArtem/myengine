// ThumbnailService.h

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <myengine/render/RenderTargetPool.h>
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
    class ResourceManager;
}

namespace myengine::editor
{
    enum class ThumbnailKind : std::uint8_t
    {
        Texture,  // the texture itself
        Mesh,     // the mesh, framed by its bounds, with the suggested material
        Material, // a sphere with the material
        Prefab,   // every mesh of the template, framed by their common bounds
        Other,    // no preview: the caller draws a type icon
    };

    struct Thumbnail
    {
        render::TextureHandle texture; // valid only when ready; until then draw a type icon (kind)
        ThumbnailKind kind = ThumbnailKind::Other;
        bool ready = false;
        std::uint32_t sourceWidth = 0;  // textures: the size of the image, for the aspect ratio
        std::uint32_t sourceHeight = 0;

        // The value for ImGui::Image: ImTextureID(thumbnail.ImGuiTextureId())
        std::uint64_t ImGuiTextureId() const { return texture.value; }
    };

    struct ThumbnailStats
    {
        std::size_t entries = 0;       // thumbnails that are tracked
        std::size_t queued = 0;        // waiting for a render or a disk read
        std::size_t ready = 0;         // with an image (target or cached texture)
        std::uint64_t rendered = 0;    // total render passes
        std::uint64_t diskHits = 0;    // total images restored from the disk cache
        std::uint64_t diskWrites = 0;  // total images written to the disk cache
        std::size_t leasedTargets = 0;
    };

    // The world-space box of what is drawn (an invalid box has min > max)
    struct BoundsBox
    {
        render::Float3 min{3.4e38f, 3.4e38f, 3.4e38f};
        render::Float3 max{-3.4e38f, -3.4e38f, -3.4e38f};

        bool IsValid() const { return min.x <= max.x; }
        void Add(const render::Float3& point);
        void Add(const BoundsBox& other);
    };

    enum class DrawItemStatus : std::uint8_t
    {
        Ready,
        Pending, // the mesh or the texture is still streaming: ask again in a frame
        Failed,
    };

    // A viewport that a panel draws every frame (the material preview, the viewers). The panel describes what to
    // draw, the service draws it into a render target before the scene, so the image is one frame behind.
    struct LiveViewRequest
    {
        std::string id;                  // stable per view, e.g. "material-editor" or "viewer:<path>"
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        render::Matrix4 view = render::Matrix4::Identity();
        render::Matrix4 projection = render::Matrix4::Identity();
        core::Color clearColor{0.0f, 0.0f, 0.0f, 0.0f}; // transparent: the panel paints the background
        std::vector<render::DrawItem> items;
        std::vector<render::DebugLine> lines;           // grid, bounds
    };

    struct ThumbnailServiceConfig
    {
        // <project>/Saved/Thumbnails; empty disables the disk cache
        std::filesystem::path cacheDirectory;
        std::size_t maxRenderTargets = 48;   // the pool: images that live as render targets
        std::size_t maxCachedTextures = 192; // images restored from disk that stay as plain textures
        std::size_t budgetPerFrame = 2;      // thumbnails rendered per frame
        // The material that a mesh thumbnail is drawn with (the editor's ResolveSuggestedMaterialForMesh);
        // empty: the default material
        std::function<std::string(const std::string& meshPath)> suggestMaterial;
    };

    // Asset previews for the editor (Content Browser tiles, pickers, viewers): `Request` answers at once with
    // what is ready, queues the rest, and `Render` draws up to `budgetPerFrame` queued thumbnails per frame into
    // pooled render targets. The images are cached in memory (LRU by use) and on disk
    // (<cache>/<signature>_<size>.tga, the signature covers the file times of the asset and what it references).
    // Main thread only.
    //
    // Frame order: Update() between frames, Render(surface) after BeginFrame and before the scene and the UI,
    // Request() at any time (the UI build).
    class ThumbnailService
    {
    public:
        ThumbnailService(
            render::IRenderAdapter& adapter,
            resource::ResourceManager& resources,
            core::Logger& logger,
            ThumbnailServiceConfig config);
        ~ThumbnailService();

        ThumbnailService(const ThumbnailService&) = delete;
        ThumbnailService& operator=(const ThumbnailService&) = delete;

        // The material that a mesh thumbnail is drawn with; invalidate to apply a new rule to the images in memory
        void SetMaterialSuggestion(std::function<std::string(const std::string& meshPath)> suggest);

        // Builds what the renderer needs to draw a mesh with a material (loads them through the ResourceManager).
        // `worldBounds`, when given, is extended by the mesh bounds transformed by `model`.
        DrawItemStatus BuildDrawItem(
            const std::string& meshPath,
            const std::string& materialPath,
            const render::Matrix4& model,
            render::DrawItem& item,
            BoundsBox* worldBounds = nullptr);

        // Describes a live view for this frame; returns the texture it was last drawn into (invalid until the first
        // render, the handle is stable while the size does not change). A view that is not submitted for a while
        // gives its target back to the pool.
        render::TextureHandle SubmitLiveView(LiveViewRequest request);

        static ThumbnailKind KindOf(const std::filesystem::path& assetPath);
        // The supported sizes are 64, 128 and 256: a request is rounded up to one of them
        static std::uint32_t BucketSize(std::uint32_t requestedSize);

        Thumbnail Request(const std::string& assetPath, std::uint32_t size = 128);

        // Between frames: disk cache reads and writes, staleness checks, eviction
        void Update();
        // Inside a frame, before anything else is drawn
        void Render(render::RenderSurfaceHandle surface);

        // The asset changed (or the user asked for a refresh): the next Request renders it again
        void Invalidate(const std::string& assetPath);
        void InvalidateAll();
        // Drops every cached image and returns the targets to the adapter (the disk cache stays)
        void Clear();

        ThumbnailStats GetStats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
