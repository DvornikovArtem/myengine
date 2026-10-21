// Hot reload of shaders in the resource manager on a recording adapter (no GPU): a reload of a loaded shader goes to the
// same program and never to the fallback, an error leaves the old version, a shader that failed at the first load does not
// pile up programs, a changed include reloads the program.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <myengine/core/Logger.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/resource/ResourceManager.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    void WriteText(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
    }

    // A source that contains "BROKEN" does not compile. The adapter counts the programs it holds, like the real one.
    class FakeAdapter final : public render::IRenderAdapter
    {
    public:
        bool Initialize() override { return true; }
        render::MeshHandle UploadMesh(const render::MeshData&) override { return {nextMesh_++}; }
        render::TextureHandle CreateTexture(const render::TextureData&) override { return {nextTexture_++}; }
        void DestroyTexture(render::TextureHandle) override {}
        render::RenderSurfaceHandle CreateSurface(HWND, std::uint32_t, std::uint32_t) override { return {1}; }
        void ResizeSurface(render::RenderSurfaceHandle, std::uint32_t, std::uint32_t) override {}
        bool BeginFrame(render::RenderSurfaceHandle, const core::Color&) override { return true; }
        void SetRenderRegion(render::RenderSurfaceHandle, const render::IntRect*) override {}
        void SetViewProjection(render::RenderSurfaceHandle, const render::Matrix4&, const render::Matrix4&) override {}
        void Draw(render::RenderSurfaceHandle, const render::DrawItem&) override {}
        void DrawDebugLines(render::RenderSurfaceHandle, const std::vector<render::DebugLine>&) override {}
        void DrawUiGeometry(render::RenderSurfaceHandle, const render::UiDrawData&) override {}
        void EndFrame(render::RenderSurfaceHandle) override {}
        void Shutdown() override {}

        render::ShaderHandle CreateShaderProgram(const render::ShaderProgramData& program) override
        {
            ++createCalls;
            if (!Compiles(program))
            {
                diagnostics = {BrokenDiagnostic()};
                return {};
            }
            const std::uint32_t id = nextShader_++;
            versions[id] = 1;
            return {id};
        }

        void DestroyShader(render::ShaderHandle shader) override
        {
            ++destroyCalls;
            versions.erase(shader.value);
        }

        bool ReloadShaderProgram(render::ShaderHandle shader, const render::ShaderProgramData& program) override
        {
            ++reloadCalls;
            const auto it = versions.find(shader.value);
            if (it == versions.end() || !Compiles(program))
            {
                ++reloadFailures;
                diagnostics = {BrokenDiagnostic()};
                lastErrors[shader.value] = render::FormatShaderDiagnostic(BrokenDiagnostic());
                return false;
            }
            ++it->second;
            lastErrors.erase(shader.value);
            return true;
        }

        std::vector<render::ShaderDiagnostic> GetLastShaderDiagnostics() const override { return diagnostics; }

        std::vector<render::ShaderProgramInfo> GetShaderPrograms() const override
        {
            std::vector<render::ShaderProgramInfo> programs;
            for (const auto& [id, version] : versions)
            {
                render::ShaderProgramInfo info;
                info.handle = {id};
                info.version = version;
                if (const auto error = lastErrors.find(id); error != lastErrors.end())
                {
                    info.lastError = error->second;
                }
                programs.push_back(info);
            }
            return programs;
        }

        std::uint32_t GetShaderVersion(render::ShaderHandle shader) const override
        {
            const auto it = versions.find(shader.value);
            return it == versions.end() ? 0 : it->second;
        }

        std::size_t LivePrograms() const { return versions.size(); }

        int createCalls = 0;
        int destroyCalls = 0;
        int reloadCalls = 0;
        int reloadFailures = 0;
        std::map<std::uint32_t, std::uint32_t> versions; // live programs and their versions
        std::map<std::uint32_t, std::string> lastErrors; // programs whose last reload failed
        std::vector<render::ShaderDiagnostic> diagnostics;

        static render::ShaderDiagnostic BrokenDiagnostic()
        {
            render::ShaderDiagnostic diagnostic;
            diagnostic.file = "lit.hlsl";
            diagnostic.line = 2;
            diagnostic.column = 1;
            diagnostic.message = "BROKEN token";
            return diagnostic;
        }

    private:
        static bool Compiles(const render::ShaderProgramData& program)
        {
            return ReadText(program.sourcePath).find("BROKEN") == std::string::npos;
        }

        std::uint32_t nextMesh_ = 1;
        std::uint32_t nextTexture_ = 1;
        std::uint32_t nextShader_ = 1;
    };

    struct Harness
    {
        Harness() : resources(adapter, logger) {}

        core::Logger logger;
        FakeAdapter adapter;
        resource::ResourceManager resources;
    };

    struct Files
    {
        fs::path directory;
        fs::path descriptor;
        fs::path source;
        fs::path include;
    };

    Files MakeFiles()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        Files files;
        files.directory = fs::temp_directory_path() / fs::u8path("myengine A-check шейдеры " + std::to_string(stamp));
        files.descriptor = files.directory / "lit.shader.json";
        files.source = files.directory / "lit.hlsl";
        files.include = files.directory / "common.hlsli";
        WriteText(files.descriptor, R"({"source": "lit.hlsl", "vertexEntry": "VSMain", "pixelEntry": "PSMain"})");
        WriteText(files.source, "#include \"common.hlsli\"\n// v1\n");
        WriteText(files.include, "// the common part v1\n");
        return files;
    }

    // The file time changes with every write; the scan of the resource manager compares it
    void Touch(const fs::path& path, const std::string& text)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        WriteText(path, text);
    }

    // ---- a loaded shader is rebuilt on its own handle ----

    void TestReloadKeepsTheHandle(const Files& files)
    {
        Harness harness;
        const std::size_t baseline = harness.adapter.LivePrograms(); // the fallback program of the resource manager

        const auto loaded = harness.resources.Load<resource::ShaderAsset>(files.descriptor);
        Check(loaded != nullptr && loaded->asset.gpuHandle.IsValid() && !loaded->asset.fallback, "the shader loads");
        Check(harness.adapter.LivePrograms() == baseline + 1, "one program for the shader");
        Check(loaded->asset.version == 1, "version 1 at the load");
        const render::ShaderHandle handle = loaded->asset.gpuHandle;

        Touch(files.source, "#include \"common.hlsli\"\n// v2\n");
        const auto reloaded = harness.resources.Reload<resource::ShaderAsset>(files.descriptor);
        Check(reloaded == loaded, "the resource is the same object: everybody who holds it sees the new version");
        Check(reloaded->asset.gpuHandle.value == handle.value, "the handle stays");
        Check(reloaded->asset.version == 2 && harness.adapter.GetShaderVersion(handle) == 2, "the version grows");
        Check(harness.adapter.reloadCalls == 1 && harness.adapter.createCalls == 2, "one reload call, no new program (two creates: the fallback and the shader)");
        Check(harness.adapter.LivePrograms() == baseline + 1, "still one program for the shader");
        Check(harness.adapter.destroyCalls == 0, "nothing is destroyed: the handle is reused");

        // The scan of the resource manager does the same by itself when the file changes
        Touch(files.source, "#include \"common.hlsli\"\n// v3\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        harness.resources.UpdateHotReload();
        Check(harness.adapter.reloadCalls == 2 && loaded->asset.version == 3, "UpdateHotReload reloads a changed source");
    }

    // ---- an error leaves the old version, and is not replaced by the fallback ----

    void TestErrorKeepsTheOldVersion(const Files& files)
    {
        Harness harness;
        const auto loaded = harness.resources.Load<resource::ShaderAsset>(files.descriptor);
        const render::ShaderHandle handle = loaded->asset.gpuHandle;

        Touch(files.source, "#include \"common.hlsli\"\n// BROKEN\n");
        const auto afterError = harness.resources.Reload<resource::ShaderAsset>(files.descriptor);
        Check(afterError == loaded, "a failed reload keeps the resource");
        Check(!afterError->asset.fallback, "the fallback does not replace a program that worked");
        Check(afterError->asset.gpuHandle.value == handle.value && afterError->asset.version == 1, "the handle and the version are those of the working program");
        Check(harness.adapter.reloadFailures == 1 && harness.adapter.GetShaderVersion(handle) == 1, "the adapter was asked and kept the old program");

        // The same broken files are not tried again by the scan
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        harness.resources.UpdateHotReload();
        Check(harness.adapter.reloadCalls == 1, "a file that failed is not reloaded again until it changes");

        // The fix is picked up
        Touch(files.source, "#include \"common.hlsli\"\n// fixed\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        harness.resources.UpdateHotReload();
        Check(harness.adapter.reloadCalls == 2 && afterError->asset.version == 2, "the fixed file is picked up (version 2)");
    }

    // ---- a shader that fails at the first load: no programs pile up ----

    void TestFailedFirstLoad(const Files& files)
    {
        WriteText(files.source, "#include \"common.hlsli\"\n// BROKEN\n");
        Harness harness;
        const std::size_t baseline = harness.adapter.LivePrograms();
        const int createsBefore = harness.adapter.createCalls;

        const auto first = harness.resources.Load<resource::ShaderAsset>(files.descriptor);
        Check(first != nullptr && first->asset.fallback, "a shader that does not compile stands in as the fallback");
        Check(harness.adapter.LivePrograms() == baseline, "no program is made for it");

        for (int attempt = 1; attempt <= 6; ++attempt)
        {
            Touch(files.source, "#include \"common.hlsli\"\n// BROKEN " + std::to_string(attempt) + "\n");
            const auto again = harness.resources.Reload<resource::ShaderAsset>(files.descriptor);
            Check(again != nullptr && again->asset.fallback, "still the fallback");
            Check(harness.adapter.LivePrograms() == baseline, "attempt " + std::to_string(attempt) + ": no program piles up");
        }
        Check(harness.adapter.createCalls == createsBefore + 7, "every attempt asked the adapter once");
        Check(harness.adapter.destroyCalls == 0, "nothing was made, nothing is destroyed");

        // The fix: one program of its own, and later edits reload that program
        Touch(files.source, "#include \"common.hlsli\"\n// fixed\n");
        const auto fixed = harness.resources.Reload<resource::ShaderAsset>(files.descriptor);
        Check(!fixed->asset.fallback && fixed->asset.gpuHandle.IsValid() && fixed->asset.version == 1, "the fixed shader is a program of its own");
        Check(harness.adapter.LivePrograms() == baseline + 1, "one program after the fix");

        Touch(files.source, "#include \"common.hlsli\"\n// edited\n");
        const auto edited = harness.resources.Reload<resource::ShaderAsset>(files.descriptor);
        Check(edited == fixed && edited->asset.version == 2 && harness.adapter.LivePrograms() == baseline + 1, "the next edit reloads that program");
        Check(harness.adapter.destroyCalls == 0, "no program is ever left behind");
    }

    // ---- the panel says why a shader is not what its file says ----

    void TestShaderErrorForThePanel(const Files& files)
    {
        WriteText(files.source, "#include \"common.hlsli\"\n// BROKEN\n");
        Harness harness;
        const auto first = harness.resources.Load<resource::ShaderAsset>(files.descriptor);
        Check(first != nullptr && first->asset.fallback, "the first compile failed: the fallback");
        Check(first->asset.error.find("BROKEN token") != std::string::npos && first->asset.error.find("lit.hlsl(2,1)") != std::string::npos,
            "the fallback keeps the first error with the file and the place: " + first->asset.error);
        Check(harness.resources.GetShaderError(first) == first->asset.error, "GetShaderError gives it");

        Touch(files.source, "#include \"common.hlsli\"\n// fixed\n");
        const auto fixed = harness.resources.Reload<resource::ShaderAsset>(files.descriptor);
        Check(!fixed->asset.fallback && fixed->asset.error.empty() && harness.resources.GetShaderError(fixed).empty(), "a shader that works has no error");

        // A reload that fails while the program works: the error is the program's, the asset is not a fallback
        Touch(files.source, "#include \"common.hlsli\"\n// BROKEN again\n");
        const auto kept = harness.resources.Reload<resource::ShaderAsset>(files.descriptor);
        Check(kept == fixed && !kept->asset.fallback, "the working program stays");
        Check(harness.resources.GetShaderError(kept).find("BROKEN token") != std::string::npos, "the error of the last reload is told");

        Touch(files.source, "#include \"common.hlsli\"\n// fixed again\n");
        harness.resources.Reload<resource::ShaderAsset>(files.descriptor);
        Check(harness.resources.GetShaderError(kept).empty(), "a good reload clears it");

        // A descriptor that cannot be read at the first load is a fallback with its reason (a working program keeps its version)
        WriteText(files.descriptor, "{ not json");
        Harness fresh;
        const auto broken = fresh.resources.Load<resource::ShaderAsset>(files.descriptor);
        Check(broken != nullptr && broken->asset.fallback && !broken->asset.error.empty(), "an unreadable descriptor: the fallback with a reason");
    }

    // ---- the include is a dependency ----

    void TestIncludeIsADependency(const Files& files)
    {
        Harness harness;
        const auto loaded = harness.resources.Load<resource::ShaderAsset>(files.descriptor);
        bool tracksInclude = false;
        for (const resource::ResourceDependency& dependency : loaded->dependencies)
        {
            tracksInclude = tracksInclude || dependency.path.filename() == "common.hlsli";
        }
        Check(tracksInclude, "the include is among the dependencies of the shader");

        Touch(files.include, "// the common part v2\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        harness.resources.UpdateHotReload();
        Check(harness.adapter.reloadCalls == 1 && loaded->asset.version == 2, "a changed include reloads the program");

        // An include that is created later counts too: it was listed while missing
        WriteText(files.source, "#include \"common.hlsli\"\n#include \"later.hlsli\"\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        harness.resources.UpdateHotReload();
        Check(harness.adapter.reloadCalls == 2, "a changed source reloads the program");
        Touch(files.directory / "later.hlsli", "// created\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        harness.resources.UpdateHotReload();
        Check(harness.adapter.reloadCalls == 3, "an include that appears later reloads the program");
    }
}

int main()
{
    struct Case
    {
        const char* name;
        void (*run)(const Files&);
    };
    const Case cases[] = {
        {"reload keeps the handle", TestReloadKeepsTheHandle},
        {"an error keeps the old version", TestErrorKeepsTheOldVersion},
        {"a failed first load piles up nothing", TestFailedFirstLoad},
        {"the panel gets the error of the shader", TestShaderErrorForThePanel},
        {"the include is a dependency", TestIncludeIsADependency},
    };

    jobs::Initialize(2);
    int failed = 0;
    for (const Case& test : cases)
    {
        const Files files = MakeFiles();
        try
        {
            test.run(files);
            std::cout << "[ OK ] " << test.name << "\n";
        }
        catch (const std::exception& ex)
        {
            ++failed;
            std::cout << "[FAIL] " << test.name << ": " << ex.what() << "\n";
        }
        std::error_code ignored;
        fs::remove_all(files.directory, ignored);
    }
    jobs::Shutdown();
    std::cout << (failed == 0 ? "shader_hot_reload: all passed\n" : "shader_hot_reload: FAILED\n");
    return failed == 0 ? 0 : 1;
}
