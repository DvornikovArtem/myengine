// ThumbnailService on a recording adapter (no GPU): placeholder, then the image, the budget, the pool,
// the disk cache and the invalidation by file time.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <myengine/core/Logger.h>
#include <myengine/editor/ThumbnailService.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/resource/ResourceManager.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // Counts what the service asks for; the "GPU" is a table of ids
    class FakeAdapter final : public render::IRenderAdapter
    {
    public:
        bool Initialize() override { return true; }
        render::MeshHandle UploadMesh(const render::MeshData&) override { return {nextMesh_++}; }
        render::TextureHandle CreateTexture(const render::TextureData& data) override
        {
            ++createdTextures;
            lastTextureWidth = data.width;
            return {nextTexture_++};
        }
        void DestroyTexture(render::TextureHandle) override { ++destroyedTextures; }
        void SetTargetLighting(const render::Float3& direction, float ambient) override
        {
            ++lightingCalls;
            lastAmbient = ambient;
            lastLightY = direction.y;
        }
        render::ShaderHandle CreateShaderProgram(const render::ShaderProgramData&) override { return {nextShader_++}; }
        render::RenderSurfaceHandle CreateSurface(HWND, std::uint32_t, std::uint32_t) override { return {1}; }
        void ResizeSurface(render::RenderSurfaceHandle, std::uint32_t, std::uint32_t) override {}
        bool BeginFrame(render::RenderSurfaceHandle, const core::Color&) override { return true; }
        void SetRenderRegion(render::RenderSurfaceHandle, const render::IntRect*) override {}
        void SetViewProjection(render::RenderSurfaceHandle, const render::Matrix4&, const render::Matrix4&) override { ++viewProjections; }
        void Draw(render::RenderSurfaceHandle, const render::DrawItem&) override
        {
            if (inTarget)
            {
                ++drawsInTarget;
            }
            else
            {
                ++drawsOutsideTarget;
            }
        }
        void DrawDebugLines(render::RenderSurfaceHandle, const std::vector<render::DebugLine>&) override
        {
            Check(inTarget, "Debug lines of a live view must go to the target");
            ++lineDraws;
        }
        void DrawUiGeometry(render::RenderSurfaceHandle, const render::UiDrawData&) override {}
        void EndFrame(render::RenderSurfaceHandle) override {}
        void Shutdown() override {}

        render::RenderTargetHandle CreateRenderTarget(std::uint32_t width, std::uint32_t height) override
        {
            ++createdTargets;
            const std::uint32_t id = nextTarget_++;
            sizes[id] = {width, height};
            return {id};
        }
        void DestroyRenderTarget(render::RenderTargetHandle target) override
        {
            ++destroyedTargets;
            sizes.erase(target.value);
        }
        render::TextureHandle GetRenderTargetTexture(render::RenderTargetHandle target) const override
        {
            return {0x1000u + target.value};
        }
        bool BeginRenderTarget(render::RenderTargetHandle target, const core::Color&) override
        {
            Check(!inTarget, "BeginRenderTarget nested");
            Check(sizes.count(target.value) == 1, "BeginRenderTarget of an unknown target");
            inTarget = true;
            ++passes;
            return true;
        }
        void EndRenderTarget(render::RenderTargetHandle) override
        {
            Check(inTarget, "EndRenderTarget without Begin");
            inTarget = false;
        }
        bool ReadRenderTargetPixels(render::RenderTargetHandle target, std::vector<std::uint8_t>& out) override
        {
            ++readbacks;
            const auto it = sizes.find(target.value);
            if (it == sizes.end())
            {
                return false;
            }
            out.assign(static_cast<std::size_t>(it->second.first) * it->second.second * 4, 0);
            for (std::size_t index = 0; index < out.size(); ++index)
            {
                out[index] = static_cast<std::uint8_t>(index * 7 + 3);
            }
            return true;
        }
        bool GetRenderTargetSize(render::RenderTargetHandle target, std::uint32_t& width, std::uint32_t& height) const override
        {
            const auto it = sizes.find(target.value);
            if (it == sizes.end())
            {
                return false;
            }
            width = it->second.first;
            height = it->second.second;
            return true;
        }

        std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> sizes;
        bool inTarget = false;
        int passes = 0;
        int drawsInTarget = 0;
        int drawsOutsideTarget = 0;
        int viewProjections = 0;
        int lineDraws = 0;
        int createdTargets = 0;
        int destroyedTargets = 0;
        int createdTextures = 0;
        int destroyedTextures = 0;
        int readbacks = 0;
        int lightingCalls = 0;
        float lastAmbient = 0.0f;
        float lastLightY = 0.0f;
        std::uint32_t lastTextureWidth = 0;

    private:
        std::uint32_t nextMesh_ = 1;
        std::uint32_t nextTexture_ = 1;
        std::uint32_t nextShader_ = 1;
        std::uint32_t nextTarget_ = 1;
    };

    struct Harness
    {
        explicit Harness(const fs::path& cacheDirectory, std::size_t maxTargets = 16, std::size_t budget = 2)
            : resources(adapter, logger)
        {
            editor::ThumbnailServiceConfig config;
            config.cacheDirectory = cacheDirectory;
            config.maxRenderTargets = maxTargets;
            config.budgetPerFrame = budget;
            service = std::make_unique<editor::ThumbnailService>(adapter, resources, logger, config);
        }

        // One frame: stream finalization, the between-frames update, the in-frame render
        void Frame()
        {
            resources.UpdateHotReload();
            service->Update();
            service->Render(render::RenderSurfaceHandle{1});
        }

        editor::Thumbnail WaitReady(const std::string& path, std::uint32_t size = 128, int frames = 2000)
        {
            for (int frame = 0; frame < frames; ++frame)
            {
                const auto thumbnail = service->Request(path, size);
                if (thumbnail.ready)
                {
                    return thumbnail;
                }
                Frame();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return service->Request(path, size);
        }

        core::Logger logger;
        FakeAdapter adapter;
        resource::ResourceManager resources;
        std::unique_ptr<editor::ThumbnailService> service;
    };

    fs::path MakeTempDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path directory = fs::temp_directory_path() / fs::u8path("myengine R2 миниатюры " + std::to_string(stamp));
        fs::create_directories(directory);
        return directory;
    }

    std::size_t CountFiles(const fs::path& directory, const char* extension)
    {
        std::size_t count = 0;
        std::error_code error;
        if (!fs::is_directory(directory, error))
        {
            return 0;
        }
        for (const auto& entry : fs::directory_iterator(directory))
        {
            count += entry.path().extension() == extension ? 1 : 0;
        }
        return count;
    }

    void WriteText(const fs::path& path, const std::string& text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
        stream.close();
        static int revision = 0;
        fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(++revision));
    }

    void TestKinds()
    {
        using editor::ThumbnailKind;
        using editor::ThumbnailService;
        Check(ThumbnailService::KindOf("assets/models/crate.obj") == ThumbnailKind::Mesh, "obj is a mesh");
        Check(ThumbnailService::KindOf("assets/materials/gold.material.json") == ThumbnailKind::Material, "material kind");
        Check(ThumbnailService::KindOf("assets/prefabs/coin.prefab.json") == ThumbnailKind::Prefab, "prefab kind");
        Check(ThumbnailService::KindOf("assets/textures/Debug.BMP") == ThumbnailKind::Texture, "texture kind ignores case");
        Check(ThumbnailService::KindOf("assets/scripts/coin.py") == ThumbnailKind::Other, "script has no preview");
        Check(ThumbnailService::BucketSize(10) == 64 && ThumbnailService::BucketSize(100) == 128 && ThumbnailService::BucketSize(200) == 256 &&
              ThumbnailService::BucketSize(900) == 256, "sizes are rounded to 64 / 128 / 256");
    }

    // The first answer is a placeholder, the next ones the image; a texture is its own image
    void TestPlaceholderThenImage(const fs::path& cache)
    {
        Harness harness(cache);
        const auto first = harness.service->Request("assets/materials/gold.material.json", 128);
        Check(!first.ready && !first.texture.IsValid() && first.kind == editor::ThumbnailKind::Material, "A new thumbnail must be a placeholder");

        const auto ready = harness.WaitReady("assets/materials/gold.material.json");
        Check(ready.ready && ready.texture.IsValid(), "The material thumbnail never became ready");
        Check(harness.adapter.passes == 1 && harness.adapter.drawsInTarget == 1 && harness.adapter.drawsOutsideTarget == 0,
            "A material is one sphere drawn inside one pass");
        Check(harness.adapter.viewProjections >= 1, "The thumbnail camera was not set");
        Check(harness.adapter.lightingCalls >= 1 && harness.adapter.lastAmbient >= 0.3f && harness.adapter.lastLightY < 0.0f,
            "A preview must have a fill light (the dark side keeps at least 30 % of the colour) from above");
        Check(!harness.adapter.inTarget, "The pass was left open");

        uint32_t width = 0;
        uint32_t height = 0;
        Check(harness.adapter.GetRenderTargetSize({1}, width, height) && width == 128 && height == 128, "The target must have the requested size");

        // Settled: a second request does not render again
        const int passes = harness.adapter.passes;
        for (int frame = 0; frame < 20; ++frame)
        {
            harness.Frame();
        }
        Check(harness.service->Request("assets/materials/gold.material.json", 128).ready && harness.adapter.passes == passes,
            "A ready thumbnail was rendered again");

        // The disk cache is written between frames
        Check(harness.service->GetStats().diskWrites == 1 && CountFiles(cache, ".tga") == 1, "The image was not written to the disk cache");

        // A mesh and a prefab (two meshes)
        Check(harness.WaitReady("assets/models/african_head.obj").ready, "The mesh thumbnail never became ready");
        const int beforePrefab = harness.adapter.drawsInTarget;
        Check(harness.WaitReady("assets/prefabs/coin.prefab.json").ready, "The prefab thumbnail never became ready");
        Check(harness.adapter.drawsInTarget - beforePrefab == 2, "The coin prefab has two meshes");

        // A texture needs no target
        const int targets = harness.adapter.createdTargets;
        const auto texture = harness.WaitReady("assets/textures/debug.bmp");
        Check(texture.ready && texture.sourceWidth > 0 && harness.adapter.createdTargets == targets, "A texture is its own thumbnail");

        // Scripts have no preview
        const auto script = harness.service->Request("assets/scripts/coin.py", 128);
        Check(!script.ready && script.kind == editor::ThumbnailKind::Other, "A script must stay an icon");
    }

    void TestBudget(const fs::path& cache)
    {
        Harness harness(cache, 16, 2);
        const std::vector<std::string> materials = {
            "assets/materials/gold.material.json", "assets/materials/cool.material.json", "assets/materials/warm.material.json",
            "assets/materials/default.material.json", "assets/materials/africanhead.material.json",
        };

        // However many thumbnails are waiting, one frame draws at most the budget
        int busiestFrame = 0;
        for (int frame = 0; frame < 1500; ++frame)
        {
            bool allReady = true;
            for (const auto& path : materials)
            {
                allReady = harness.service->Request(path, 64).ready && allReady;
            }
            if (allReady)
            {
                break;
            }
            harness.resources.UpdateHotReload();
            harness.service->Update();
            const int before = harness.adapter.passes;
            harness.service->Render(render::RenderSurfaceHandle{1});
            const int drawn = harness.adapter.passes - before;
            Check(drawn <= 2, "More thumbnails than the budget were rendered in one frame");
            busiestFrame = std::max(busiestFrame, drawn);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(busiestFrame == 2, "The budget of two thumbnails per frame was never used");
        Check(harness.service->GetStats().ready == materials.size(), "Not every thumbnail was rendered");
    }

    // A small pool: thumbnails that are not on screen give their target to the new ones
    void TestEviction()
    {
        Harness harness({}, 2, 2);
        const std::string a = "assets/materials/gold.material.json";
        const std::string b = "assets/materials/cool.material.json";
        const std::string c = "assets/materials/warm.material.json";
        for (int frame = 0; frame < 600; ++frame)
        {
            const bool readyA = harness.service->Request(a, 64).ready;
            const bool readyB = harness.service->Request(b, 64).ready;
            if (readyA && readyB)
            {
                break;
            }
            harness.Frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(harness.service->Request(a, 64).ready && harness.service->Request(b, 64).ready, "Two thumbnails fit the pool");
        Check(harness.service->GetStats().leasedTargets == 2, "Two targets must be leased");

        // A and B scroll away, C comes in
        harness.Frame();
        harness.Frame();
        harness.Frame();
        const auto thirdReady = harness.WaitReady(c, 64);
        Check(thirdReady.ready, "A new thumbnail must take the target of an unused one");
        Check(harness.service->GetStats().leasedTargets == 2 && harness.adapter.createdTargets == 2, "The pool must not grow");
    }

    void TestDiskCacheReuse(const fs::path& cache)
    {
        // The first run wrote gold.material.json; a new service (a new session) reads it back
        Check(CountFiles(cache, ".tga") >= 1, "The previous test did not leave a cache file");
        Harness harness(cache);
        const auto ready = harness.WaitReady("assets/materials/gold.material.json");
        Check(ready.ready, "The cached thumbnail never became ready");
        Check(harness.adapter.passes == 0 && harness.adapter.createdTargets == 0, "A cached thumbnail must not be rendered");
        Check(harness.service->GetStats().diskHits == 1 && harness.adapter.createdTextures >= 1, "The image must come from the disk cache");
    }

    // A panel's live view: described every frame, drawn into one target, released when it is not submitted
    void TestLiveView()
    {
        Harness harness({}, 8, 2);
        render::DrawItem item;
        editor::BoundsBox bounds;
        bool ready = false;
        for (int frame = 0; frame < 2000 && !ready; ++frame)
        {
            harness.resources.UpdateHotReload();
            const auto status = harness.service->BuildDrawItem(
                "assets/models/sphere.obj", "assets/materials/gold.material.json", render::Matrix4::Identity(), item, &bounds);
            Check(status != editor::DrawItemStatus::Failed, "The sphere with a material must build");
            ready = status == editor::DrawItemStatus::Ready;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(ready && bounds.IsValid() && bounds.min.x < bounds.max.x, "The draw item never became ready");

        const auto submit = [&](const std::uint32_t width, const std::uint32_t height)
        {
            editor::LiveViewRequest request;
            request.id = "test-view";
            request.width = width;
            request.height = height;
            request.items.push_back(item);
            request.lines.push_back(render::DebugLine{});
            return harness.service->SubmitLiveView(std::move(request));
        };

        Check(!submit(200, 100).IsValid(), "A new live view has no texture before it is drawn");
        harness.service->Update();
        harness.service->Render(render::RenderSurfaceHandle{1});
        Check(harness.adapter.passes == 1 && harness.adapter.drawsInTarget == 1 && harness.adapter.lineDraws == 1,
            "The live view must be one pass with its items and lines");
        uint32_t width = 0;
        uint32_t height = 0;
        Check(harness.adapter.GetRenderTargetSize({1}, width, height) && width == 200 && height == 100, "The live view target size is wrong");

        const auto texture = submit(200, 100);
        Check(texture.IsValid(), "A drawn live view must return its texture");
        harness.service->Update();
        harness.service->Render(render::RenderSurfaceHandle{1});
        Check(harness.adapter.passes == 2 && harness.adapter.createdTargets == 1, "The live view must reuse its target");
        Check(submit(200, 100).value == texture.value, "The texture handle must stay the same");

        // A new size gets a new target
        submit(300, 150);
        harness.service->Update();
        harness.service->Render(render::RenderSurfaceHandle{1});
        Check(harness.adapter.createdTargets == 2, "A resized live view needs a target of the new size");

        // Without submissions the target goes back to the pool
        for (int frame = 0; frame < 40; ++frame)
        {
            harness.service->Update();
        }
        Check(harness.service->GetStats().leasedTargets == 0, "An abandoned live view must release its target");
    }

    // Changing the file changes the signature: the image is dropped and drawn again (no stale image from disk)
    void TestInvalidationByFileTime(const fs::path& workDirectory)
    {
        const fs::path material = workDirectory / "tracked.material.json";
        const std::string source = fs::u8path(MYENGINE_SOURCE_DIR).generic_u8string();
        const std::string body = "{ \"shader\": \"" + source + "/assets/shaders/textured_lit.shader.json\", "
            "\"texture\": \"" + source + "/assets/textures/debug.bmp\", \"tint\": [1.0, 0.2, 0.2, 1.0] }";
        WriteText(material, body);

        Harness harness(workDirectory / "cache");
        const std::string path = material.generic_u8string();
        Check(harness.WaitReady(path).ready, "The temporary material thumbnail never became ready");
        const int firstPasses = harness.adapter.passes;
        Check(firstPasses == 1, "The material must be rendered once");

        WriteText(material, body + " ");
        bool rendered = false;
        for (int frame = 0; frame < 400 && !rendered; ++frame)
        {
            harness.service->Request(path, 128);
            harness.Frame();
            rendered = harness.adapter.passes > firstPasses;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(rendered, "A changed file must be drawn again");
        Check(harness.WaitReady(path).ready, "The redrawn thumbnail never became ready");

        // Explicit invalidation
        const int beforeInvalidate = harness.adapter.passes;
        harness.service->Invalidate(path);
        Check(!harness.service->Request(path, 128).ready, "An invalidated thumbnail must be a placeholder");
        Check(harness.WaitReady(path).ready && harness.adapter.passes > beforeInvalidate, "An invalidated thumbnail must be drawn again");
    }
}

int main()
{
    const fs::path directory = MakeTempDirectory();
    int result = 0;
    try
    {
        jobs::Initialize(2);

        TestKinds();
        const fs::path cache = directory / "Thumbnails";
        TestPlaceholderThenImage(cache);
        TestBudget(directory / "budget");
        TestEviction();
        TestDiskCacheReuse(cache);
        TestInvalidationByFileTime(directory);
        TestLiveView();

        std::cout << "OK: thumbnail service: placeholder and image, kinds, budget, pool eviction, disk cache, invalidation\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        result = 1;
    }

    jobs::Shutdown();
    std::error_code ignored;
    fs::remove_all(directory, ignored);
    return result;
}
