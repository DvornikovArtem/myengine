// B1.2 (LR3): the parameters of a material reach the frame. A real DX12 device (the hardware one or WARP), a hidden
// window, a render target and the pixels read back: a material names two textures and constants of a test shader, the
// renderer puts them in by name; the old keys of a material, a texture that is not set, a draw item without a material and
// a shader that is reloaded with a new constant are drawn too.
// Without a device (or a desktop to make the window on) the test is skipped: it exits with 77.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/core/Logger.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/render/MaterialParams.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>
#include <myengine/resource/ResourceManager.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine;
    using json = nlohmann::json;

    constexpr int kSkipped = 77; // CTest: SKIP_RETURN_CODE

    struct Skip
    {
        std::string reason;
    };

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND CreateTestWindow()
    {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"MyEngineMaterialRender";
        RegisterClassW(&windowClass);
        HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"material render", WS_OVERLAPPEDWINDOW,
            100, 100, 320, 240, nullptr, nullptr, windowClass.hInstance, nullptr);
        if (hwnd != nullptr)
        {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        return hwnd;
    }

    void WriteText(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
        Check(static_cast<bool>(stream), "cannot write " + path.u8string());
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        Check(stream.is_open(), "cannot read " + path.u8string());
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    // A 4x4 24-bit BMP of one colour
    void WriteBmp(const fs::path& path, const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
    {
        constexpr int kSize = 4;
        constexpr int kRow = kSize * 3; // a multiple of 4: no padding
        std::vector<std::uint8_t> file(54 + kRow * kSize, 0);
        const auto put32 = [&file](const std::size_t offset, const std::uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
            {
                file[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
            }
        };
        file[0] = 'B';
        file[1] = 'M';
        put32(2, static_cast<std::uint32_t>(file.size()));
        put32(10, 54);
        put32(14, 40);
        put32(18, kSize);
        put32(22, kSize);
        file[26] = 1;
        file[28] = 24;
        put32(34, kRow * kSize);
        for (int pixel = 0; pixel < kSize * kSize; ++pixel)
        {
            file[54 + static_cast<std::size_t>(pixel) * 3 + 0] = b;
            file[54 + static_cast<std::size_t>(pixel) * 3 + 1] = g;
            file[54 + static_cast<std::size_t>(pixel) * 3 + 2] = r;
        }
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        Check(static_cast<bool>(stream), "cannot write " + path.u8string());
    }

    // A quad in clip space, clockwise (it faces the camera with the default culling)
    render::MeshData Quad()
    {
        render::MeshData mesh;
        mesh.vertices = {
            {{-0.8f, -0.8f, 0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
            {{-0.8f, 0.8f, 0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
            {{0.8f, 0.8f, 0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
            {{0.8f, -0.8f, 0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        };
        mesh.indices = {0, 1, 2, 0, 2, 3};
        return mesh;
    }

    struct Pixel
    {
        int r = 0;
        int g = 0;
        int b = 0;
    };

    std::string Describe(const Pixel& pixel)
    {
        return "(" + std::to_string(pixel.r) + ", " + std::to_string(pixel.g) + ", " + std::to_string(pixel.b) + ")";
    }

    void CheckPixel(const Pixel& pixel, const int r, const int g, const int b, const std::string& what)
    {
        const auto close = [](const int a, const int b2) { return std::abs(a - b2) <= 3; };
        Check(close(pixel.r, r) && close(pixel.g, g) && close(pixel.b, b), what + ": " + Describe(pixel) + " instead of " + Describe({r, g, b}));
    }

    class Scene
    {
    public:
        Scene(const fs::path& directory)
            : directory_(directory)
        {
            logger_.Initialize(directory / "material_render.log");
            adapter_ = std::make_unique<render::dx12::Dx12RenderAdapter>(logger_);
            if (!adapter_->Initialize())
            {
                throw Skip{"the DX12 adapter did not initialize (no device)"};
            }
            hwnd_ = CreateTestWindow();
            if (hwnd_ == nullptr)
            {
                throw Skip{"no window could be created (no desktop)"};
            }
            surface_ = adapter_->CreateSurface(hwnd_, 320, 240);
            if (!surface_.IsValid())
            {
                throw Skip{"the surface could not be created"};
            }
            resources_ = std::make_unique<resource::ResourceManager>(*adapter_, logger_);
            mesh_ = adapter_->UploadMesh(Quad());
            Check(mesh_.IsValid(), "the mesh was not uploaded");
            target_ = adapter_->CreateRenderTarget(kSize, kSize);
            Check(target_.IsValid(), "the render target was not created");
        }

        ~Scene()
        {
            if (adapter_ != nullptr)
            {
                resources_.reset();
                adapter_->DestroyRenderTarget(target_);
                adapter_->Shutdown();
            }
            if (hwnd_ != nullptr)
            {
                DestroyWindow(hwnd_);
            }
        }

        resource::ResourceManager& Resources() { return *resources_; }
        render::dx12::Dx12RenderAdapter& Adapter() { return *adapter_; }
        render::MeshHandle Mesh() const { return mesh_; }

        // The textures of the project stream in: wait until the files of a material have arrived
        void WaitForTextures(const fs::path& materialPath)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            for (;;)
            {
                resources_->UpdateHotReload();
                const auto material = resources_->Load<resource::MaterialAsset>(materialPath);
                Check(material != nullptr, "the material loads: " + materialPath.u8string());
                bool pending = false;
                if (!material->asset.texturePath.empty())
                {
                    pending = pending || resources_->IsLoadPending(material->asset.texturePath);
                }
                for (const auto& [name, path] : material->asset.textures)
                {
                    pending = pending || resources_->IsLoadPending(path);
                }
                if (!pending)
                {
                    return;
                }
                Check(std::chrono::steady_clock::now() < deadline, "the textures of " + materialPath.u8string() + " did not load");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        // One frame into the render target; returns the pixel in the middle
        Pixel Draw(const render::DrawItem& item)
        {
            Check(adapter_->BeginFrame(surface_, core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginFrame failed");
            Check(adapter_->BeginRenderTarget(target_, core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginRenderTarget failed");
            adapter_->SetViewProjection(surface_, render::Matrix4::Identity(), render::Matrix4::Identity());
            adapter_->Draw(surface_, item);
            adapter_->EndRenderTarget(target_);
            adapter_->EndFrame(surface_);

            std::vector<std::uint8_t> pixels;
            Check(adapter_->ReadRenderTargetPixels(target_, pixels), "ReadRenderTargetPixels failed");
            Check(pixels.size() == static_cast<std::size_t>(kSize) * kSize * 4, "the readback has the wrong size");
            const std::size_t offset = (static_cast<std::size_t>(kSize / 2) * kSize + kSize / 2) * 4;
            return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
        }

        // A material drawn the way the scene draws it: the parameters from the resource manager
        Pixel DrawMaterial(const fs::path& materialPath)
        {
            WaitForTextures(materialPath);
            resources_->UpdateHotReload(); // one frame of the resource manager
            const auto material = resources_->Load<resource::MaterialAsset>(materialPath);
            const auto shader = resources_->Load<resource::ShaderAsset>(material->asset.shaderPath);
            Check(shader != nullptr && shader->asset.gpuHandle.IsValid(), "the shader of " + materialPath.u8string() + " is there");
            render::DrawItem item;
            item.mesh = mesh_;
            item.shader = shader->asset.gpuHandle;
            item.material = resources_->GetMaterialParams(material);
            Check(item.material != nullptr, "the material has its parameters: " + materialPath.u8string());
            return Draw(item);
        }

        std::string LogText() const { return ReadText(directory_ / "material_render.log"); }

    private:
        static constexpr std::uint32_t kSize = 32;

        fs::path directory_;
        core::Logger logger_;
        std::unique_ptr<render::dx12::Dx12RenderAdapter> adapter_;
        std::unique_ptr<resource::ResourceManager> resources_;
        HWND hwnd_ = nullptr;
        render::RenderSurfaceHandle surface_;
        render::MeshHandle mesh_;
        render::RenderTargetHandle target_;
    };

    fs::path SourceDirectory()
    {
        return fs::u8path(MYENGINE_SOURCE_DIR);
    }

    // The files of the test: the shader (a copy, the reload test changes it), the textures and the materials
    struct Files
    {
        fs::path directory;
        fs::path shaderJson;
        fs::path shaderHlsl;
    };

    std::string Material(const std::string& body)
    {
        return "{\n  \"shader\": \"probe.shader.json\"," + body + "\n}\n";
    }

    Files MakeFiles()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        Files files;
        files.directory = fs::temp_directory_path() / fs::u8path("myengine B1 отрисовка " + std::to_string(stamp));
        fs::create_directories(files.directory);
        files.shaderJson = files.directory / "probe.shader.json";
        files.shaderHlsl = files.directory / "material_probe.hlsl";
        fs::copy_file(SourceDirectory() / "tests/render/shaders/material_probe.shader.json", files.shaderJson);
        fs::copy_file(SourceDirectory() / "tests/render/shaders/material_probe.hlsl", files.shaderHlsl);
        WriteBmp(files.directory / "white.bmp", 255, 255, 255);
        WriteBmp(files.directory / "red.bmp", 255, 0, 0);
        WriteBmp(files.directory / "green.bmp", 0, 255, 0);
        WriteBmp(files.directory / "blue.bmp", 0, 0, 255); // used by the live criterion only: it streams in after the material is built

        // The new keys: two textures by name, the colour and the constants by name
        WriteText(files.directory / "colour.material.json", Material(R"(
  "textures": { "gTexture": "white.bmp", "gTexture2": "green.bmp" },
  "params": { "gColor": [0.25, 0.5, 1.0, 1.0], "gMix": 0.0 })"));
        WriteText(files.directory / "second.material.json", Material(R"(
  "textures": { "gTexture": "red.bmp", "gTexture2": "green.bmp" },
  "params": { "gColor": [1.0, 1.0, 1.0, 1.0], "gMix": 1.0 })"));
        WriteText(files.directory / "constant.material.json", Material(R"(
  "textures": { "gTexture": "white.bmp" },
  "params": { "gColor": [0.25, 0.25, 0.25, 1.0], "gBoost": 0.5 })"));
        // The old keys
        WriteText(files.directory / "old.material.json", Material(R"(
  "texture": "red.bmp",
  "tint": [0.5, 1.0, 1.0, 1.0])"));
        // The second texture is not set but used: the white stub
        WriteText(files.directory / "stub.material.json", Material(R"(
  "textures": { "gTexture": "red.bmp" },
  "params": { "gMix": 1.0 })"));
        // A shader without textures: the material has none either
        WriteText(files.directory / "plain.shader.json", R"({ "source": "plain.hlsl", "vertexEntry": "VSMain", "pixelEntry": "PSMain", "vertexStream": "mesh" })");
        WriteText(files.directory / "plain.hlsl", R"(
cbuffer DrawCB : register(b0)
{
    float4x4 gModel;
    float4x4 gViewProjection;
    float4 gColor;
};
struct VSInput { float3 Position : POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD0; };
float4 VSMain(VSInput input) : SV_POSITION { return mul(mul(float4(input.Position, 1.0f), gModel), gViewProjection); }
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET { return float4(gColor.rgb, 1.0f); }
)");
        WriteText(files.directory / "plain.material.json", R"({ "shader": "plain.shader.json", "params": { "gColor": [0.5, 0.25, 0.75, 1.0] } })");
        // A constant that the shader does not have yet
        WriteText(files.directory / "future.material.json", Material(R"(
  "textures": { "gTexture": "white.bmp" },
  "params": { "gColor": [0.25, 0.25, 0.25, 1.0], "gNew": 0.5 })"));
        return files;
    }

    void TestParametersReachTheFrame(Scene& scene, const Files& files)
    {
        const auto path = [&files](const char* name) { return files.directory / name; };

        // gColor of the material (a value of "params") reaches the frame: white * (0.25, 0.5, 1)
        CheckPixel(scene.DrawMaterial(path("colour.material.json")), 64, 128, 255, "gColor of the material");

        // The second texture by name: gMix = 1 shows gTexture2 (green), red * white in gTexture would be red
        CheckPixel(scene.DrawMaterial(path("second.material.json")), 0, 255, 0, "the second texture by name");

        // A constant of "params" changes the colour: 0.25 + gBoost 0.5
        CheckPixel(scene.DrawMaterial(path("constant.material.json")), 191, 191, 191, "a constant from params");

        // The old keys: texture -> gTexture, tint -> gColor
        CheckPixel(scene.DrawMaterial(path("old.material.json")), 128, 0, 0, "an old material (texture + tint)");

        // gTexture2 is used and not set: the white stub, not garbage; gColor of the file without a colour is white
        CheckPixel(scene.DrawMaterial(path("stub.material.json")), 255, 255, 255, "a texture that the material does not set");
    }

    void TestAMaterialWithoutTextures(Scene& scene, const Files& files)
    {
        // The shader has no texture, so the material needs none: it loads and its colour reaches the frame
        const fs::path materialPath = files.directory / "plain.material.json";
        const auto material = scene.Resources().Load<resource::MaterialAsset>(materialPath);
        Check(material != nullptr && !scene.Resources().LoadFailed(materialPath), "a material without textures loads for a shader without textures");
        Check(material->asset.texturePath.empty() && material->asset.textures.empty(), "and has no texture");
        CheckPixel(scene.DrawMaterial(materialPath), 128, 64, 191, "a shader without textures: gColor of the material");
    }

    void TestDrawWithoutAMaterial(Scene& scene, const Files& files)
    {
        // A draw item with a texture and a colour of its own (a caller that has no material) is drawn as before
        const fs::path materialPath = files.directory / "old.material.json";
        scene.WaitForTextures(materialPath);
        const auto material = scene.Resources().Load<resource::MaterialAsset>(materialPath);
        const auto shader = scene.Resources().Load<resource::ShaderAsset>(material->asset.shaderPath);
        const auto texture = scene.Resources().Load<resource::TextureAsset>(material->asset.texturePath);
        render::DrawItem item;
        item.mesh = scene.Mesh();
        item.shader = shader->asset.gpuHandle;
        item.texture = texture->asset.gpuHandle;
        item.color = core::Color{0.5f, 1.0f, 1.0f, 1.0f};
        CheckPixel(scene.Draw(item), 128, 0, 0, "a draw item without a material (texture + colour)");
    }

    void TestAShaderReloadedWithANewConstant(Scene& scene, const Files& files)
    {
        const fs::path materialPath = files.directory / "future.material.json";
        // The shader has no gNew yet: the material's value is reported (once) and the rest is drawn
        CheckPixel(scene.DrawMaterial(materialPath), 64, 64, 64, "before the shader has the constant");
        Check(scene.LogText().find("gNew") != std::string::npos, "the unknown parameter is reported in the log");
        const auto before = scene.Resources().GetMaterialParams(scene.Resources().Load<resource::MaterialAsset>(materialPath));
        Check(before != nullptr, "parameters before the reload");
        const std::uint64_t hashBefore = before->LayoutHash();

        // The shader gets the constant: no C++ changes, the material is matched with the new layout
        std::string source = ReadText(files.shaderHlsl);
        const auto declaration = source.find("float gBoost;");
        Check(declaration != std::string::npos, "the probe shader declares gBoost");
        source.insert(declaration, "float gNew;\n    ");
        const auto use = source.find("+ gBoost;");
        Check(use != std::string::npos, "the probe shader uses gBoost");
        source.insert(use, "+ gNew ");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        WriteText(files.shaderHlsl, source);
        const auto reloaded = scene.Resources().Reload<resource::ShaderAsset>(files.shaderJson);
        Check(reloaded != nullptr && !reloaded->asset.fallback && reloaded->asset.version == 2, "the shader is reloaded (version 2)");

        CheckPixel(scene.DrawMaterial(materialPath), 191, 191, 191, "after the shader got the constant: 0.25 + gNew 0.5");
        const auto after = scene.Resources().GetMaterialParams(scene.Resources().Load<resource::MaterialAsset>(materialPath));
        Check(after != nullptr && after->LayoutHash() != hashBefore, "the material was matched with the new layout");

        // The materials that were not asked for since are matched when the scan sees the reload
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        WriteText(files.shaderHlsl, ReadText(files.shaderHlsl) + "\n// touched\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        scene.Resources().UpdateHotReload();
        const auto touched = scene.Resources().Load<resource::ShaderAsset>(files.shaderJson);
        Check(touched->asset.version == 3, "the scan reloads a changed shader (version 3)");
        CheckPixel(scene.DrawMaterial(materialPath), 191, 191, 191, "the material is still right after the scan");
    }


    // ---- the live criterion of the lab: a constant and a texture are added to a real shader, and a material names them ----
    // The shader is a copy of assets/shaders/textured_lit.hlsl, edited the way docs/render/b1-material-params.md tells the
    // teacher to edit it; the reload goes through the file scan of the resource manager (the mtime poll), as in the demo.

    std::size_t CountIn(const std::string& text, const std::string& part)
    {
        std::size_t count = 0;
        for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
        {
            ++count;
        }
        return count;
    }

    // Inserts `text` after the first `anchor` of the source (the files may have LF or CRLF)
    void InsertAfter(std::string& source, const std::string& anchor, const std::string& text)
    {
        const auto at = source.find(anchor);
        Check(at != std::string::npos, "the shader has no '" + anchor + "'");
        source.insert(at + anchor.size(), text);
    }

    const char* const kRoughnessDeclaration = "\n    float gRoughness;";
    const char* const kRoughnessUse = "\n    lighting *= 1.0f - gRoughness;";
    const char* const kDetailDeclaration = "\nTexture2D gDetail : register(t1);";
    const char* const kDetailUse = "\n    texColor *= gDetail.Sample(gSampler, input.UV);";

    std::string LiveShaderSource(const std::string& original, const bool roughness, const bool detail)
    {
        std::string source = original;
        if (roughness)
        {
            InsertAfter(source, "float4 gLightDirection;", kRoughnessDeclaration);
            InsertAfter(source, "float lighting = gLightDirection.w + ndotl * (1.0f - gLightDirection.w);", kRoughnessUse);
        }
        if (detail)
        {
            InsertAfter(source, "Texture2D gTexture : register(t0);", kDetailDeclaration);
            InsertAfter(source, "float4 texColor = gTexture.Sample(gSampler, input.UV);", kDetailUse);
        }
        return source;
    }

    // The file scan of the resource manager sees a changed shader after its interval; wait for the new version
    void WaitForShaderVersion(Scene& scene, const fs::path& shaderJson, const std::uint32_t version)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        for (;;)
        {
            scene.Resources().UpdateHotReload();
            const auto shader = scene.Resources().Load<resource::ShaderAsset>(shaderJson);
            if (shader != nullptr && shader->asset.version >= version)
            {
                Check(!shader->asset.fallback, "the shader is a real program after the reload");
                return;
            }
            Check(std::chrono::steady_clock::now() < deadline, "the scan did not reload the shader");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void WaitForMaterialReload(Scene& scene, const fs::path& materialPath, const resource::ResourceHandle<resource::MaterialAsset>& before)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        for (;;)
        {
            scene.Resources().UpdateHotReload();
            const auto material = scene.Resources().Load<resource::MaterialAsset>(materialPath);
            if (material != before) // (the old one is held: a new object cannot take its address)
            {
                return;
            }
            Check(std::chrono::steady_clock::now() < deadline, "the scan did not reload the material");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void TestTheLiveCriterion(Scene& scene, const Files& files)
    {
        const fs::path live = files.directory / "live";
        fs::create_directories(live);
        const fs::path shaderJson = live / "textured_lit.shader.json";
        const fs::path shaderHlsl = live / "textured_lit.hlsl";
        fs::copy_file(SourceDirectory() / "assets/shaders/textured_lit.shader.json", shaderJson);
        fs::copy_file(SourceDirectory() / "assets/shaders/textured_lit.hlsl", shaderHlsl);
        const std::string original = ReadText(shaderHlsl);
        const fs::path materialPath = files.directory / "live.material.json";
        const auto writeMaterial = [&](const json& textures, const json& params)
        {
            json material = {{"shader", "live/textured_lit.shader.json"}, {"textures", textures}, {"params", params}};
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            WriteText(materialPath, material.dump(2));
        };
        const auto materialObject = [&]() { return scene.Resources().Load<resource::MaterialAsset>(materialPath); };
        const auto drawLive = [&]() { return scene.DrawMaterial(materialPath); };
        const auto log = [&scene]() { return scene.LogText(); };
        const auto edit = [&](const bool roughness, const bool detail, const std::uint32_t version)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            WriteText(shaderHlsl, LiveShaderSource(original, roughness, detail));
            WaitForShaderVersion(scene, shaderJson, version);
        };
        const auto changeMaterial = [&](const json& textures, const json& params)
        {
            const auto before = materialObject();
            writeMaterial(textures, params);
            WaitForMaterialReload(scene, materialPath, before);
        };
        const json white = {{"gTexture", "white.bmp"}};
        const json whiteColour = {{"gColor", {1.0, 1.0, 1.0, 1.0}}};

        // 0. The shader as it is: a white texture and a white colour; the lit pixel is the reference of the rest
        writeMaterial(white, whiteColour);
        const Pixel reference = drawLive();
        Check(reference.r > 40 && reference.r == reference.g && reference.g == reference.b, "the reference pixel is lit white: " + Describe(reference));
        const auto scaled = [](const int value, const double factor) { return static_cast<int>(value * factor + 0.5); };

        std::cout << "  live criterion: 4, a constant (gRoughness)\n";
        // 4. A constant is added to the shader and used: the picture does not change until the material names it
        edit(true, false, 2);
        CheckPixel(drawLive(), reference.r, reference.g, reference.b, "gRoughness is in the shader, the material does not set it: zero changes nothing");
        changeMaterial(white, {{"gColor", {1.0, 1.0, 1.0, 1.0}}, {"gRoughness", 0.4}});
        CheckPixel(drawLive(), scaled(reference.r, 0.6), scaled(reference.g, 0.6), scaled(reference.b, 0.6), "gRoughness 0.4 from params reaches the frame (the light x 0.6)");
        Check(CountIn(log(), "'gRoughness' is not in") == 0, "no message while the name is right");

        std::cout << "  live criterion: 5, a texture (gDetail)\n";
        // 5. A texture is added: the root signature is rebuilt from the reflection; unset, it is the white stub
        edit(true, true, 3);
        CheckPixel(drawLive(), scaled(reference.r, 0.6), scaled(reference.g, 0.6), scaled(reference.b, 0.6), "gDetail is in the shader, the material does not set it: the white stub changes nothing");
        changeMaterial({{"gTexture", "white.bmp"}, {"gDetail", "green.bmp"}}, {{"gColor", {1.0, 1.0, 1.0, 1.0}}, {"gRoughness", 0.4}});
        CheckPixel(drawLive(), 0, scaled(reference.g, 0.6), 0, "gDetail (green) from textures reaches the frame");

        std::cout << "  live criterion: 6, a removed name\n";
        // 6. The constant disappears from the shader while the material still names it: one warning, the frame is drawn
        edit(false, true, 4);
        const Pixel withoutRoughness = drawLive();
        CheckPixel(withoutRoughness, 0, reference.g, 0, "gRoughness is gone: ignored, the rest is drawn");
        for (int frame = 0; frame < 3; ++frame)
        {
            drawLive();
        }
        const std::string removed = log();
        Check(CountIn(removed, "'gRoughness' is not in textured_lit.shader.json; names: ") == 1, "exactly one warning about the removed gRoughness, " + std::to_string(CountIn(removed, "'gRoughness' is not in")) + " found");
        Check(CountIn(removed, "gColor (Float4)") >= 1 && CountIn(removed, "textures: ") >= 1, "the warning lists the names of the shader");

        std::cout << "  live criterion: 7, a typo\n";
        // 7. A name that the shader never had (a typo): one message with the name and the list, nothing wrong in the frame
        // (the detail texture is a new file: it arrives after the material was built, and the rebuild must not repeat the message)
        changeMaterial({{"gTexture", "white.bmp"}, {"gDetail", "blue.bmp"}}, {{"gColor", {1.0, 1.0, 1.0, 1.0}}, {"gNoSuchName", 1}});
        CheckPixel(drawLive(), 0, 0, reference.b, "an unknown parameter does not spoil the frame");
        drawLive();
        Check(CountIn(log(), "'gNoSuchName' is not in textured_lit.shader.json; names: ") == 1, "exactly one message about gNoSuchName");

        std::cout << "  live criterion: 8, a wrong type\n";
        // 8. A value of the wrong type: an error that says what was expected; the variable stays at its default, which for gColor of
        // textured_lit is the @default of the shader (white), so the picture is the plain lit one, not garbage
        changeMaterial({{"gTexture", "white.bmp"}, {"gDetail", "green.bmp"}}, {{"gColor", 0.5}});
        CheckPixel(drawLive(), 0, reference.g, 0, "gColor as a number: gColor stays at the @default of the shader (white)");
        drawLive();
        Check(CountIn(log(), "'gColor': expected Float4, got Float") == 1, "exactly one error about the type of gColor");

        std::cout << "  live criterion: a missing texture file\n";
        // A texture file that is not there: an error, and the fallback texture (magenta and black) instead of white
        changeMaterial({{"gTexture", "no_such_file.bmp"}}, whiteColour);
        Pixel fallback = drawLive();
        for (int frame = 0; frame < 20; ++frame)
        {
            scene.Resources().UpdateHotReload(); // the failed load is known some frames later, the material is built again
            fallback = drawLive();
        }
        Check(fallback.r > 20 && fallback.b > 20 && fallback.g * 2 < fallback.r, "the fallback texture is visible (not white, not black): " + Describe(fallback));
        const std::string missing = log();
        Check(CountIn(missing, "no_such_file.bmp") >= 1 && CountIn(missing, "could not be loaded: the fallback texture") == 1, "exactly one error about the texture file that cannot be loaded");

        std::cout << "  live criterion: parameters of another layout\n";
        // Parameters built for another layout (the shader was reloaded, the material has not caught up): white, not a black flash
        changeMaterial(white, whiteColour);
        const auto otherMaterial = scene.Resources().Load<resource::MaterialAsset>(files.directory / "colour.material.json");
        const render::MaterialParams* otherLayout = scene.Resources().GetMaterialParams(otherMaterial);
        Check(otherLayout != nullptr, "the parameters of a material of the probe shader");
        const auto liveMaterial = scene.Resources().Load<resource::MaterialAsset>(materialPath);
        const auto liveShader = scene.Resources().Load<resource::ShaderAsset>(liveMaterial->asset.shaderPath);
        scene.WaitForTextures(materialPath);
        render::DrawItem item;
        item.mesh = scene.Mesh();
        item.shader = liveShader->asset.gpuHandle;
        item.material = otherLayout;
        CheckPixel(scene.Draw(item), reference.r, reference.g, reference.b, "parameters of another layout: a white colour and white textures, no black flash");
        Check(CountIn(log(), "built for another layout") == 1, "the mismatch is reported once");
    }

    int Run()
    {
        const Files files = MakeFiles();
        int result = 0;
        try
        {
            jobs::Initialize(2);
            {
                Scene scene(files.directory);
                TestParametersReachTheFrame(scene, files);
                TestAMaterialWithoutTextures(scene, files);
                TestDrawWithoutAMaterial(scene, files);
                TestAShaderReloadedWithANewConstant(scene, files);
                TestTheLiveCriterion(scene, files);
            }
            std::cout << "OK: material parameters reach the frame: gColor, a second texture and constants by name, the old keys, a texture that is not set, a shader without textures, a draw item without a material, a shader reloaded with a new constant, the live criterion (gRoughness, gDetail, a removed name, a typo, a wrong type, a missing file, a layout mismatch)\n";
        }
        catch (const Skip& skip)
        {
            std::cout << "SKIPPED: " << skip.reason << '\n';
            result = kSkipped;
        }
        catch (const std::exception& error)
        {
            std::cerr << "FAILED: " << error.what() << '\n';
            result = 1;
        }
        jobs::Shutdown();
        if (result != 1)
        {
            std::error_code ignored;
            fs::remove_all(files.directory, ignored);
        }
        else
        {
            std::cerr << "Files and log: " << files.directory.u8string() << '\n';
        }
        return result;
    }
}

int main()
{
    return Run();
}
