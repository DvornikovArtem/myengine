// B4.1 (LR3): the scenarios of stability of the document of the lab (docs/render/architecture.md, section 4) that can be played
// without a person at the window: a shader with an error, a missing entry or profile, a series of reloads, a slot conflict, an
// input that the mesh does not have, a file that is gone, a reload while a render target is drawn, a long session in fast
// motion and an exit in a dirty state. A real DX12 device (the hardware one or WARP) with the debug layer, a hidden window, a render
// target and the pixels read back: what draws is told by the colour of the pixel, what the engine says is told by the log (one
// message with the file) and what is alive is told by the statistics of the adapter and by the live objects of the device after
// Shutdown. Without a device (or a desktop to make the window on) the test is skipped: it exits with 77.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <myengine/core/Logger.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/render/MaterialParams.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>
#include <myengine/resource/ResourceManager.h>

#include <psapi.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine;

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
        windowClass.lpszClassName = L"MyEngineRenderStability";
        RegisterClassW(&windowClass);
        HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"render stability", WS_OVERLAPPEDWINDOW,
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
        constexpr int kRow = kSize * 3;
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

    void CheckPixel(const Pixel& pixel, const Pixel& expected, const std::string& what)
    {
        const auto close = [](const int a, const int b) { return std::abs(a - b) <= 3; };
        Check(close(pixel.r, expected.r) && close(pixel.g, expected.g) && close(pixel.b, expected.b),
            what + ": " + Describe(pixel) + " instead of " + Describe(expected));
    }

    constexpr Pixel kRed{255, 0, 0};
    constexpr Pixel kGreen{0, 255, 0};
    constexpr Pixel kBlue{0, 0, 255};

    // ---- the shader of the test: the colour of the pixel is its own colour times the tint and the texture (both white), so what
    // draws is told by the pixel. The options make the errors of the scenarios.
    struct Source
    {
        std::string rgb = "1.0f, 0.0f, 0.0f";
        std::string pixelEntry = "PSMain";
        bool dropSemicolon = false;   // scenario 1: a syntax error
        bool tangent = false;         // scenario 10: an input that the mesh does not have
        bool conflict = false;        // scenario 9: the vertex shader and the pixel shader name two textures at t1
        bool boost = false;           // a constant that a material sets by name
    };

    std::string Hlsl(const Source& source)
    {
        std::string text;
        text += "cbuffer DrawCB : register(b0)\n{\n    float4x4 gModel;\n    float4x4 gViewProjection;\n    float4 gColor;\n    float4 gLightDirection;\n";
        if (source.boost)
        {
            text += "    float gBoost;\n";
        }
        text += "};\n\nTexture2D gTexture : register(t0);\n";
        if (source.conflict)
        {
            text += "Texture2D gA : register(t1);\nTexture2D gB : register(t1);\n";
        }
        text += "SamplerState gSampler : register(s0);\n\n";
        text += "struct VSInput\n{\n    float3 Position : POSITION;\n    float3 Normal : NORMAL;\n    float2 UV : TEXCOORD0;\n";
        if (source.tangent)
        {
            text += "    float3 Tangent : TANGENT;\n";
        }
        text += "};\n\nstruct PSInput\n{\n    float4 Position : SV_POSITION;\n    float2 UV : TEXCOORD0;\n};\n\n";
        text += "PSInput VSMain(VSInput input)\n{\n    PSInput output;\n";
        text += "    output.Position = mul(mul(float4(input.Position, 1.0f), gModel), gViewProjection);\n    output.UV = input.UV;\n";
        if (source.tangent)
        {
            text += "    output.UV += input.Tangent.xy;\n";
        }
        if (source.conflict)
        {
            text += "    output.UV += gA.SampleLevel(gSampler, input.UV, 0).xy * 0.0001f;\n";
        }
        text += "    return output;\n}\n\n";
        text += "float4 " + source.pixelEntry + "(PSInput input) : SV_TARGET\n{\n";
        text += "    float4 color = float4(" + source.rgb + ", 1.0f) * gColor * gTexture.Sample(gSampler, input.UV);\n";
        if (source.conflict)
        {
            text += "    color *= gB.Sample(gSampler, input.UV);\n";
        }
        if (source.boost)
        {
            text += "    color.rgb += gBoost;\n";
        }
        text += std::string("    return color") + (source.dropSemicolon ? "" : ";") + "\n}\n";
        return text;
    }

    std::string Descriptor(const std::string& pixelProfile = "ps_5_0")
    {
        return "{\n  \"source\": \"textured_lit.hlsl\",\n  \"vertexEntry\": \"VSMain\",\n  \"pixelEntry\": \"PSMain\",\n"
               "  \"vertexProfile\": \"vs_5_0\",\n  \"pixelProfile\": \"" + pixelProfile + "\",\n  \"vertexStream\": \"mesh\",\n"
               "  \"samplers\": { \"gSampler\": \"linear_wrap\" }\n}\n";
    }

    // ---- what the log says ----

    std::vector<std::string> Lines(const std::string& text, const std::string& part)
    {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            if (line.find(part) != std::string::npos)
            {
                lines.push_back(line);
            }
        }
        return lines;
    }

    // "textured_lit.hlsl(27,14)": the file and a place in it
    bool HasFileAndLine(const std::string& line, const std::string& file)
    {
        const std::size_t at = line.find(file + "(");
        return at != std::string::npos && std::isdigit(static_cast<unsigned char>(line[at + file.size() + 1])) != 0;
    }

    // Live objects after the Shutdown of the adapters, counted over all the stages
    int g_stagesWithLiveCheck = 0;
    int g_stagesWithoutLiveCheck = 0;

    // The whole stage of a scenario: the adapter with its debug layer, a hidden window, a render target, the resource manager and the
    // files of one shader in a folder of its own
    class Stage
    {
    public:
        explicit Stage(const std::string& name)
            : name_(name)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory_ = fs::temp_directory_path() / fs::u8path("myengine B4 стабильность " + name + " " + std::to_string(stamp));
            fs::create_directories(directory_);
            descriptor_ = directory_ / "textured_lit.shader.json";
            hlsl_ = directory_ / "textured_lit.hlsl";
            Write(Source{});
            WriteText(descriptor_, ::Descriptor());
            WriteBmp(directory_ / "white.bmp", 255, 255, 255);

            logger_.Initialize(directory_ / "stability.log");
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
            // The white texture is made on its first use, and that is not a thing to do inside a pass of a render target
            white_ = resources_->GetWhiteTextureHandle();
            Check(white_.IsValid(), "the white texture was not made");
        }

        ~Stage()
        {
            try
            {
                Teardown();
            }
            catch (...)
            {
            }
            std::error_code ignored;
            fs::remove_all(directory_, ignored);
        }

        // The window is closed: everything is released in the order of the application, and what the device still holds is looked at
        void Close()
        {
            Teardown();
            const auto& report = adapter_->GetLiveObjectReport();
            if (report.checked)
            {
                ++g_stagesWithLiveCheck;
                std::string objects;
                for (const std::string& object : report.objects)
                {
                    objects += "\n    " + object;
                }
                Check(report.liveObjects == 0, name_ + ": " + std::to_string(report.liveObjects) + " DX12 object(s) are alive after Shutdown:" + objects);
            }
            else
            {
                ++g_stagesWithoutLiveCheck;
            }
        }

        render::dx12::Dx12RenderAdapter& Adapter() { return *adapter_; }
        resource::ResourceManager& Resources() { return *resources_; }
        const fs::path& Directory() const { return directory_; }
        const fs::path& DescriptorPath() const { return descriptor_; }
        const fs::path& HlslPath() const { return hlsl_; }

        // The file of the shader, as the person saves it
        void Write(const Source& source)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(40)); // the write time of a file must change
            WriteText(hlsl_, ::Hlsl(source));
        }

        resource::ResourceHandle<resource::ShaderAsset> Load()
        {
            shader_ = resources_->Load<resource::ShaderAsset>(descriptor_);
            Check(shader_ != nullptr && shader_->asset.gpuHandle.IsValid() && !shader_->asset.fallback, "the shader loads: " + ReadText(hlsl_));
            return shader_;
        }

        resource::ResourceHandle<resource::ShaderAsset> Reload()
        {
            const auto reloaded = resources_->Reload<resource::ShaderAsset>(descriptor_);
            Check(reloaded != nullptr, "the reload gives a resource");
            return reloaded;
        }

        // The shader of the stage draws the quad into the render target: the pixel in the middle
        Pixel Draw()
        {
            return DrawItemFrame(MakeItem(nullptr));
        }

        // A material of the stage: the parameters from the resource manager, as a scene draws it
        Pixel DrawMaterial(const fs::path& materialPath)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            for (;;)
            {
                resources_->UpdateHotReload();
                const auto material = resources_->Load<resource::MaterialAsset>(materialPath);
                Check(material != nullptr, "the material loads: " + materialPath.u8string());
                if (material->asset.textures.empty() || !resources_->IsLoadPending(material->asset.textures.begin()->second))
                {
                    break;
                }
                Check(std::chrono::steady_clock::now() < deadline, "the texture of " + materialPath.u8string() + " did not load");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const auto material = resources_->Load<resource::MaterialAsset>(materialPath);
            const auto shader = resources_->Load<resource::ShaderAsset>(material->asset.shaderPath);
            Check(shader != nullptr && shader->asset.gpuHandle.IsValid(), "the shader of the material is there");
            render::DrawItem item;
            item.mesh = mesh_;
            item.shader = shader->asset.gpuHandle;
            item.material = resources_->GetMaterialParams(material);
            Check(item.material != nullptr, "the material has its parameters");
            return DrawItemFrame(item);
        }

        // The inside of a frame in two parts, for the scenario that reloads while the render target is drawn
        void BeginTargetFrame()
        {
            Check(adapter_->BeginFrame(surface_, core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginFrame failed");
            Check(adapter_->BeginRenderTarget(target_, core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginRenderTarget failed");
            adapter_->SetViewProjection(surface_, render::Matrix4::Identity(), render::Matrix4::Identity());
            drawsInPass_ = 0;
        }

        // Every draw of a pass is a little nearer than the one before (the depth test is "less"): the last draw is the one that is seen
        void DrawInTargetFrame()
        {
            render::DrawItem item = MakeItem(nullptr);
            item.model.data[14] = -0.04f * static_cast<float>(drawsInPass_++);
            adapter_->Draw(surface_, item);
        }

        Pixel EndTargetFrame()
        {
            adapter_->EndRenderTarget(target_);
            adapter_->EndFrame(surface_);
            return ReadMiddle();
        }

        // Frames without a draw: the GPU finishes, what was retired is freed
        void Idle(const int frames)
        {
            for (int i = 0; i < frames; ++i)
            {
                resources_->UpdateHotReload();
                Check(adapter_->BeginFrame(surface_, core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginFrame failed");
                adapter_->EndFrame(surface_);
            }
        }

        std::size_t LogMark() const { return ReadText(directory_ / "stability.log").size(); }
        std::string LogSince(const std::size_t mark) const { return ReadText(directory_ / "stability.log").substr(mark); }

        // The errors of the shader in the log since the mark: each is one line "Shader error: file(line,column): ..."
        std::vector<std::string> ShaderErrors(const std::size_t mark) const { return Lines(LogSince(mark), "Shader error:"); }

        render::ShaderStatistics Stats() const { return adapter_->GetShaderStatistics(); }

    private:
        static constexpr std::uint32_t kSize = 32;

        render::DrawItem MakeItem(const render::MaterialParams* material)
        {
            render::DrawItem item;
            item.mesh = mesh_;
            item.shader = shader_ != nullptr ? shader_->asset.gpuHandle : render::ShaderHandle{};
            item.material = material;
            item.texture = white_;
            return item;
        }

        Pixel ReadMiddle()
        {
            std::vector<std::uint8_t> pixels;
            Check(adapter_->ReadRenderTargetPixels(target_, pixels), "ReadRenderTargetPixels failed");
            Check(pixels.size() == static_cast<std::size_t>(kSize) * kSize * 4, "the readback has the wrong size");
            const std::size_t offset = (static_cast<std::size_t>(kSize / 2) * kSize + kSize / 2) * 4;
            return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
        }

        Pixel DrawItemFrame(const render::DrawItem& item)
        {
            Check(adapter_->BeginFrame(surface_, core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginFrame failed");
            Check(adapter_->BeginRenderTarget(target_, core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginRenderTarget failed");
            adapter_->SetViewProjection(surface_, render::Matrix4::Identity(), render::Matrix4::Identity());
            adapter_->Draw(surface_, item);
            adapter_->EndRenderTarget(target_);
            adapter_->EndFrame(surface_);
            return ReadMiddle();
        }

        void Teardown()
        {
            if (adapter_ == nullptr || closed_)
            {
                return;
            }
            closed_ = true;
            shader_.reset();
            resources_.reset();
            adapter_->DestroyRenderTarget(target_);
            adapter_->Shutdown();
            if (hwnd_ != nullptr)
            {
                DestroyWindow(hwnd_);
                hwnd_ = nullptr;
            }
        }

        std::string name_;
        fs::path directory_;
        fs::path descriptor_;
        fs::path hlsl_;
        core::Logger logger_;
        std::unique_ptr<render::dx12::Dx12RenderAdapter> adapter_;
        std::unique_ptr<resource::ResourceManager> resources_;
        resource::ResourceHandle<resource::ShaderAsset> shader_;
        HWND hwnd_ = nullptr;
        render::RenderSurfaceHandle surface_;
        render::MeshHandle mesh_;
        render::RenderTargetHandle target_;
        render::TextureHandle white_;
        int drawsInPass_ = 0;
        bool closed_ = false;
    };

    // A reload that must fail: the old program lives and draws, the log has one message with the file, the shader tells why
    void ExpectRejected(
        Stage& stage,
        const std::string& what,
        const std::uint32_t versionBefore,
        const Pixel& drawsBefore,
        const std::string& mustMention,
        const bool needsLine)
    {
        const std::size_t mark = stage.LogMark();
        const auto reloaded = stage.Reload();
        Check(!reloaded->asset.fallback, what + ": the fallback does not replace a program that works");
        Check(reloaded->asset.version == versionBefore, what + ": the version stays " + std::to_string(versionBefore));
        const std::vector<std::string> errors = stage.ShaderErrors(mark);
        std::string found;
        for (const std::string& line : errors)
        {
            found += "\n  [" + line + "]";
        }
        Check(errors.size() == 1, what + ": exactly one error message, " + std::to_string(errors.size()) + " found:" + found);
        Check(errors[0].find("textured_lit") != std::string::npos, what + ": the message names the file: " + errors[0]);
        if (needsLine)
        {
            Check(HasFileAndLine(errors[0], "textured_lit.hlsl"), what + ": the message has the file and the line: " + errors[0]);
        }
        if (!mustMention.empty())
        {
            Check(errors[0].find(mustMention) != std::string::npos, what + ": the message mentions '" + mustMention + "': " + errors[0]);
        }
        Check(!stage.Resources().GetShaderError(reloaded).empty(), what + ": the resource manager keeps the reason for the panel");
        // The message as the person reads it, without the temporary folder
        std::cout << "      " << what << ": Shader error: " << errors[0].substr(errors[0].find("textured_lit")) << "\n";
        CheckPixel(stage.Draw(), drawsBefore, what + ": the old program still draws");
    }

    // ---- scenario 1: a syntax error ----
    void Scenario1SyntaxError()
    {
        Stage stage("sc1");
        const auto shader = stage.Load();
        CheckPixel(stage.Draw(), kRed, "the first version draws");

        Source broken;
        broken.rgb = "0.0f, 1.0f, 0.0f";
        broken.dropSemicolon = true;
        stage.Write(broken);
        ExpectRejected(stage, "a ';' is missing", 1, kRed, "", true);

        Source fixed;
        fixed.rgb = "0.0f, 1.0f, 0.0f";
        stage.Write(fixed);
        const auto again = stage.Reload();
        Check(again == shader && again->asset.version == 2, "the fixed file is picked up (version 2)");
        Check(stage.Resources().GetShaderError(again).empty(), "and the reason is gone");
        CheckPixel(stage.Draw(), kGreen, "the new version draws");
        stage.Close();
    }

    // ---- scenario 2: no entry, a wrong profile ----
    void Scenario2EntryAndProfile()
    {
        Stage stage("sc2");
        stage.Load();
        CheckPixel(stage.Draw(), kRed, "the first version draws");

        Source renamed;
        renamed.pixelEntry = "PSMainRenamed";
        stage.Write(renamed);
        ExpectRejected(stage, "the entry PSMain is renamed", 1, kRed, "PSMain", false);

        stage.Write(Source{});
        WriteText(stage.DescriptorPath(), Descriptor("ps_9_9"));
        ExpectRejected(stage, "a wrong profile", 1, kRed, "ps_9_9", false);

        WriteText(stage.DescriptorPath(), Descriptor());
        const auto fixed = stage.Reload();
        Check(fixed->asset.version == 2 && stage.Resources().GetShaderError(fixed).empty(), "the fixed shader is version 2 without a reason");
        CheckPixel(stage.Draw(), kRed, "and it draws");
        stage.Close();
    }

    // ---- scenario 3: a series of reloads, some of them with errors ----
    enum class Step
    {
        Good,
        Syntax,
        NoEntry,
        BadProfile,
        Conflict,
        Tangent,
    };

    void Scenario3Series()
    {
        Stage stage("sc3");
        const auto shader = stage.Load();
        stage.Draw();
        const std::array<std::string, 3> colours{"0.0f, 1.0f, 0.0f", "0.0f, 0.0f, 1.0f", "1.0f, 0.0f, 0.0f"};
        const std::array<Pixel, 3> expected{kGreen, kBlue, kRed};
        const std::vector<Step> steps{
            Step::Good, Step::Good, Step::Syntax, Step::Good, Step::NoEntry, Step::Good, Step::BadProfile, Step::Conflict, Step::Good,
            Step::Tangent, Step::Good, Step::Syntax, Step::Syntax, Step::Good, Step::Good, Step::NoEntry, Step::Conflict, Step::Good,
            Step::Tangent, Step::Good,
        };

        std::uint32_t version = 1;
        Pixel drawing = kRed;
        int colour = 0;
        int rejected = 0;
        render::ShaderStatistics baseline{};
        bool haveBaseline = false;
        for (std::size_t index = 0; index < steps.size(); ++index)
        {
            const std::string what = "step " + std::to_string(index + 1);
            const Step step = steps[index];
            Source source;
            source.rgb = colours[static_cast<std::size_t>(colour) % colours.size()];
            WriteText(stage.DescriptorPath(), Descriptor());
            switch (step)
            {
                case Step::Good:
                    break;
                case Step::Syntax: source.dropSemicolon = true; break;
                case Step::NoEntry: source.pixelEntry = "PSMainRenamed"; break;
                case Step::BadProfile: WriteText(stage.DescriptorPath(), Descriptor("ps_9_9")); break;
                case Step::Conflict: source.conflict = true; break;
                case Step::Tangent: source.tangent = true; break;
            }
            stage.Write(source);
            if (step == Step::Good)
            {
                const std::size_t mark = stage.LogMark();
                const auto reloaded = stage.Reload();
                ++version;
                Check(reloaded == shader && reloaded->asset.version == version && !reloaded->asset.fallback, what + ": a good reload is the next version");
                Check(stage.ShaderErrors(mark).empty(), what + ": a good reload says no error:\n" + stage.LogSince(mark));
                drawing = expected[static_cast<std::size_t>(colour) % colours.size()];
                ++colour;
                CheckPixel(stage.Draw(), drawing, what + ": the new version draws");
            }
            else
            {
                ++rejected;
                ExpectRejected(stage, what, version, drawing, "", false);
            }
            stage.Idle(2);
            if (!haveBaseline && version >= 3)
            {
                stage.Idle(6);
                baseline = stage.Stats();
                haveBaseline = true;
            }
        }
        stage.Idle(8);
        const render::ShaderStatistics after = stage.Stats();
        Check(haveBaseline, "a baseline was taken");
        Check(after.programs == baseline.programs, "the programs do not grow: " + std::to_string(baseline.programs) + " -> " + std::to_string(after.programs));
        Check(after.pipelineStates == baseline.pipelineStates, "the pipeline states do not grow: " + std::to_string(baseline.pipelineStates) + " -> " + std::to_string(after.pipelineStates));
        Check(after.rootSignatures == baseline.rootSignatures, "the root signatures do not grow: " + std::to_string(baseline.rootSignatures) + " -> " + std::to_string(after.rootSignatures));
        Check(after.retiredObjects == 0, "nothing waits for the GPU any more: " + std::to_string(after.retiredObjects));
        Check(after.reloads >= static_cast<std::uint64_t>(version - 1), "the reloads that worked are counted");
        std::cout << "    " << steps.size() << " reloads, " << rejected << " rejected; programs " << after.programs << ", PSO " << after.pipelineStates
                  << ", root signatures " << after.rootSignatures << ", reloads " << after.reloads << " ok / " << after.reloadFailures << " failed\n";
        stage.Close();
    }

    // ---- scenario 9: the vertex shader and the pixel shader do not agree about a slot ----
    void Scenario9SlotConflict()
    {
        Stage stage("sc9");
        stage.Load();
        CheckPixel(stage.Draw(), kRed, "the first version draws");

        Source conflicting;
        conflicting.conflict = true;
        stage.Write(conflicting);
        ExpectRejected(stage, "two textures at t1", 1, kRed, "conflict", false);

        stage.Write(Source{});
        const auto fixed = stage.Reload();
        Check(fixed->asset.version == 2, "the fixed shader is picked up");
        stage.Close();
    }

    // ---- scenario 10: an input of the vertex shader that the mesh does not have ----
    void Scenario10MissingInput()
    {
        Stage stage("sc10");
        stage.Load();
        CheckPixel(stage.Draw(), kRed, "the first version draws");

        Source tangent;
        tangent.tangent = true;
        stage.Write(tangent);
        ExpectRejected(stage, "TANGENT is not in the mesh", 1, kRed, "TANGENT", false);

        stage.Write(Source{});
        const auto fixed = stage.Reload();
        Check(fixed->asset.version == 2, "the fixed shader is picked up");
        stage.Close();
    }

    // The scan of the resource manager finds the changed file by itself: one pass after the interval of 500 ms
    void ScanAfterInterval(Stage& stage)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        stage.Resources().UpdateHotReload();
    }

    // ---- scenario 11: the file of the shader is deleted or renamed ----
    void Scenario11FileIsGone()
    {
        Stage stage("sc11");
        const auto shader = stage.Load();
        CheckPixel(stage.Draw(), kRed, "the first version draws");
        ScanAfterInterval(stage); // the files are known: nothing changed

        // The source is renamed: the loaded version is drawn, one message with the file
        const fs::path moved = stage.Directory() / "textured_lit.hlsl.moved";
        std::size_t mark = stage.LogMark();
        fs::rename(stage.HlslPath(), moved);
        ScanAfterInterval(stage);
        std::vector<std::string> errors = stage.ShaderErrors(mark);
        Check(errors.size() == 1 && errors[0].find("textured_lit") != std::string::npos, "the source is gone: one message with the file:\n" + stage.LogSince(mark));
        Check(shader->asset.version == 1 && !shader->asset.fallback, "the loaded version stays");
        CheckPixel(stage.Draw(), kRed, "the loaded version draws without its file");
        mark = stage.LogMark();
        ScanAfterInterval(stage);
        Check(stage.ShaderErrors(mark).empty(), "the same missing file is not reported again");

        // It comes back changed: picked up
        Source green;
        green.rgb = "0.0f, 1.0f, 0.0f";
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        WriteText(stage.HlslPath(), Hlsl(green));
        ScanAfterInterval(stage);
        Check(shader->asset.version == 2, "the file that came back is picked up (version 2)");
        CheckPixel(stage.Draw(), kGreen, "and draws");

        // The descriptor is renamed: the same
        const fs::path movedDescriptor = stage.Directory() / "textured_lit.shader.json.moved";
        mark = stage.LogMark();
        fs::rename(stage.DescriptorPath(), movedDescriptor);
        ScanAfterInterval(stage);
        errors = stage.ShaderErrors(mark);
        Check(errors.size() == 1 && errors[0].find("textured_lit") != std::string::npos, "the descriptor is gone: one message with the file:\n" + stage.LogSince(mark));
        Check(shader->asset.version == 2 && !shader->asset.fallback, "the loaded version stays");
        CheckPixel(stage.Draw(), kGreen, "the loaded version draws without its descriptor");
        stage.Close();
    }

    // ---- scenario 12: a reload while a render target is drawn (what a thumbnail and a preview are) ----
    void Scenario12ReloadWhileDrawing()
    {
        Stage stage("sc12");
        const auto shader = stage.Load();

        // A pass of the render target that draws twice (the reference for the passes below)
        stage.BeginTargetFrame();
        stage.DrawInTargetFrame();
        stage.DrawInTargetFrame();
        CheckPixel(stage.EndTargetFrame(), kRed, "a pass with two draws and no reload");

        // Inside one pass of the render target: a draw, a reload, a draw, a rejected reload, a draw
        stage.BeginTargetFrame();
        stage.DrawInTargetFrame();
        Source green;
        green.rgb = "0.0f, 1.0f, 0.0f";
        stage.Write(green);
        stage.Reload();
        Check(shader->asset.version == 2, "the reload inside the pass worked");
        stage.DrawInTargetFrame();
        Source broken;
        broken.dropSemicolon = true;
        stage.Write(broken);
        const std::size_t mark = stage.LogMark();
        stage.Reload();
        Check(shader->asset.version == 2 && stage.ShaderErrors(mark).size() == 1, "the rejected reload inside the pass says one message");
        stage.DrawInTargetFrame();
        const Pixel ended = stage.EndTargetFrame();
        CheckPixel(ended, kGreen, "the pass ends with the last good version");

        // Reloads between the frames of the target, with retired programs waiting for the GPU
        Source blue;
        blue.rgb = "0.0f, 0.0f, 1.0f";
        for (int i = 0; i < 6; ++i)
        {
            stage.BeginTargetFrame();
            stage.DrawInTargetFrame();
            blue.rgb = i % 2 == 0 ? "0.0f, 0.0f, 1.0f" : "1.0f, 0.0f, 0.0f";
            stage.Write(blue);
            stage.Reload();
            stage.DrawInTargetFrame();
            CheckPixel(stage.EndTargetFrame(), i % 2 == 0 ? kBlue : kRed, "frame " + std::to_string(i) + ": the pass ends with the version of the reload");
        }
        stage.Idle(8);
        Check(stage.Stats().retiredObjects == 0, "what the passes retired is freed");
        stage.Close();
    }

    // ---- scenario 13: a long session in fast motion ----
    std::size_t ProcessBytes()
    {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
        return counters.PrivateUsage;
    }

    int CyclesOfTheLongSession()
    {
        char* text = nullptr;
        std::size_t length = 0;
        int cycles = 60;
        if (_dupenv_s(&text, &length, "MYENGINE_STABILITY_CYCLES") == 0 && text != nullptr)
        {
            cycles = std::max(std::atoi(text), 10);
            std::free(text);
        }
        return cycles;
    }

    void Scenario13LongSession()
    {
        Stage stage("sc13");
        // Two materials on one shader that has a constant: the long session switches between them
        const fs::path first = stage.Directory() / "first.material.json";
        const fs::path second = stage.Directory() / "second.material.json";
        WriteText(first, "{ \"shader\": \"textured_lit.shader.json\", \"textures\": { \"gTexture\": \"white.bmp\" }, \"params\": { \"gBoost\": 0.0 } }\n");
        WriteText(second, "{ \"shader\": \"textured_lit.shader.json\", \"textures\": { \"gTexture\": \"white.bmp\" }, \"params\": { \"gBoost\": 0.25 } }\n");
        Source source;
        source.boost = true;
        stage.Write(source);
        const auto shader = stage.Load();

        const std::array<std::string, 4> colours{"1.0f, 0.0f, 0.0f", "0.0f, 1.0f, 0.0f", "0.0f, 0.0f, 1.0f", "1.0f, 1.0f, 0.0f"};
        const std::array<Pixel, 4> channels{{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}}};
        const int cycles = CyclesOfTheLongSession();
        const std::size_t mark = stage.LogMark();
        int rejected = 0;
        std::uint32_t version = 1;
        render::ShaderStatistics baseline{};
        std::size_t baselineParams = 0;
        std::size_t baselineBytes = 0;
        for (int cycle = 0; cycle < cycles; ++cycle)
        {
            const std::string what = "cycle " + std::to_string(cycle + 1);
            Source next = source;
            next.rgb = colours[static_cast<std::size_t>(cycle) % colours.size()];
            const bool broken = cycle % 5 == 4;
            next.dropSemicolon = broken;
            stage.Write(next);
            stage.Reload();
            if (broken)
            {
                ++rejected;
            }
            else
            {
                ++version;
            }
            Check(shader->asset.version == version, what + ": the version is " + std::to_string(version));

            // The drawing of the cycle: both materials, one after the other, then the texture of the first
            const int shown = broken ? (cycle - 1) % static_cast<int>(colours.size()) : cycle % static_cast<int>(colours.size());
            Pixel expectedFirst = channels[static_cast<std::size_t>(shown)];
            Pixel expectedSecond = expectedFirst;
            expectedSecond.r = std::min(255, expectedSecond.r + 64);
            expectedSecond.g = std::min(255, expectedSecond.g + 64);
            expectedSecond.b = std::min(255, expectedSecond.b + 64);
            CheckPixel(stage.DrawMaterial(first), expectedFirst, what + ": the first material");
            CheckPixel(stage.DrawMaterial(second), expectedSecond, what + ": the second material");
            stage.Idle(2);

            if (cycle == 5)
            {
                stage.Idle(8);
                baseline = stage.Stats();
                baselineParams = stage.Resources().GetMaterialParamsCount();
                baselineBytes = ProcessBytes();
            }
        }
        stage.Idle(8);
        const render::ShaderStatistics after = stage.Stats();
        const std::size_t paramsAfter = stage.Resources().GetMaterialParamsCount();
        const std::size_t bytesAfter = ProcessBytes();
        Check(after.programs == baseline.programs, "the programs do not grow: " + std::to_string(baseline.programs) + " -> " + std::to_string(after.programs));
        Check(after.pipelineStates == baseline.pipelineStates, "the pipeline states do not grow: " + std::to_string(baseline.pipelineStates) + " -> " + std::to_string(after.pipelineStates));
        Check(after.rootSignatures == baseline.rootSignatures, "the root signatures do not grow: " + std::to_string(baseline.rootSignatures) + " -> " + std::to_string(after.rootSignatures));
        Check(after.retiredObjects == 0, "nothing waits for the GPU any more: " + std::to_string(after.retiredObjects));
        Check(paramsAfter == baselineParams, "the parameters of the materials do not grow: " + std::to_string(baselineParams) + " -> " + std::to_string(paramsAfter));
        Check(stage.ShaderErrors(mark).size() == static_cast<std::size_t>(rejected),
            "one message for every rejected reload (" + std::to_string(rejected) + "):\n" + stage.LogSince(mark));
        // The memory of the process is told, and only a runaway fails: the allocator keeps what it took
        Check(bytesAfter < baselineBytes + 96u * 1024u * 1024u, "the memory of the process does not run away");
        std::cout << "    " << cycles << " cycles (" << rejected << " reloads rejected); programs " << after.programs << ", PSO " << after.pipelineStates
                  << ", root signatures " << after.rootSignatures << ", parameters of materials " << paramsAfter << "; process memory "
                  << baselineBytes / (1024 * 1024) << " -> " << bytesAfter / (1024 * 1024) << " MB\n";
        stage.Close();
    }

    // ---- scenario 14: the window is closed in a dirty state ----
    void Scenario14aBrokenShader()
    {
        Stage stage("sc14a");
        stage.Load();
        stage.Draw();
        Source broken;
        broken.dropSemicolon = true;
        stage.Write(broken);
        stage.Reload();
        stage.Close(); // the shader that did not compile is the last thing that happened
    }

    void Scenario14bRightAfterAReload()
    {
        Stage stage("sc14b");
        stage.Load();
        stage.Draw();
        Source green;
        green.rgb = "0.0f, 1.0f, 0.0f";
        stage.Write(green);
        stage.Reload(); // the old program is retired and waits for the GPU
        stage.Close();  // no frame between
    }

    void Scenario14cInsideAFrame()
    {
        Stage stage("sc14c");
        stage.Load();
        stage.BeginTargetFrame();
        stage.DrawInTargetFrame();
        Source green;
        green.rgb = "0.0f, 1.0f, 0.0f";
        stage.Write(green);
        stage.Reload();
        stage.DrawInTargetFrame();
        stage.EndTargetFrame();
        Source broken;
        broken.dropSemicolon = true;
        stage.Write(broken);
        stage.Reload();
        stage.Close(); // the frame ended, the reload that failed is the last one
    }

    void Scenario14dUnusedStage()
    {
        Stage stage("sc14d");
        stage.Close(); // nothing was loaded or drawn
    }
}

int main()
{
    int failed = 0;
    try
    {
        // The debug layer of DX12 is on for the whole test: a validation error or a leak is a failure, not a silence
        char* noLayer = nullptr;
        std::size_t noLayerLength = 0;
        const bool layerOff = _dupenv_s(&noLayer, &noLayerLength, "MYENGINE_STABILITY_NO_DEBUG_LAYER") == 0 && noLayer != nullptr;
        std::free(noLayer);
        if (!layerOff)
        {
            _putenv_s("MYENGINE_DX12_DEBUG", "1");
        }
        jobs::Initialize(2);

        struct Case
        {
            const char* name;
            void (*run)();
        };
        const Case cases[] = {
            {"1  a syntax error: the old program lives, one message with file and line", Scenario1SyntaxError},
            {"2  no entry, a wrong profile", Scenario2EntryAndProfile},
            {"3  a series of reloads, some of them with errors: nothing grows", Scenario3Series},
            {"9  a slot conflict between the shaders: the program is not made", Scenario9SlotConflict},
            {"10 an input of the vertex shader that the mesh does not have", Scenario10MissingInput},
            {"11 the file of the shader is deleted or renamed", Scenario11FileIsGone},
            {"12 a reload while the render target is drawn", Scenario12ReloadWhileDrawing},
            {"13 a long session in fast motion: nothing grows", Scenario13LongSession},
            {"14a exit with a shader that did not compile", Scenario14aBrokenShader},
            {"14b exit right after a reload", Scenario14bRightAfterAReload},
            {"14c exit after a reload inside a frame", Scenario14cInsideAFrame},
            {"14d exit of an unused stage", Scenario14dUnusedStage},
        };
        for (const Case& test : cases)
        {
            try
            {
                test.run();
                std::cout << "[ OK ] " << test.name << "\n";
            }
            catch (const Skip& skip)
            {
                std::cout << "SKIPPED: " << skip.reason << "\n";
                jobs::Shutdown();
                return kSkipped;
            }
            catch (const std::exception& error)
            {
                ++failed;
                std::cout << "[FAIL] " << test.name << ": " << error.what() << "\n";
            }
        }
        jobs::Shutdown();
    }
    catch (const Skip& skip)
    {
        std::cout << "SKIPPED: " << skip.reason << "\n";
        return kSkipped;
    }
    catch (const std::exception& error)
    {
        std::cout << "FAILED: " << error.what() << "\n";
        return 1;
    }

    std::cout << "live objects of the device after Shutdown: "
              << (g_stagesWithLiveCheck > 0 ? "none in " + std::to_string(g_stagesWithLiveCheck) + " stages (the debug layer is on)"
                                            : std::string("not checked (the debug layer is not available)"))
              << "\n";
    std::cout << (failed == 0 ? "render_stability: all passed\n" : "render_stability: FAILED\n");
    return failed == 0 ? 0 : 1;
}
