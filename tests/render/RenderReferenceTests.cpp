// B4.2 (LR3): a picture of a fixed scene is compared with a reference picture. The scene is drawn into a render target of 256 x 256
// on the WARP software adapter of DX12 (the same rasterizer on every machine, so one reference is good for all of them), with a
// fixed camera, a fixed light and three materials that go through the way of the lab: the old keys of a material on the shader
// of the project, a second texture and constants by name on a test shader, and a texture that is not set (the white stub). A change
// of the reflection, of the materials or of the root signatures that spoils the picture fails the test, and the test says where
// (actual.png and diff.png are saved).
//
// The reference is tests/render/reference/scene_256.png. It is never written by a normal run: only `--update-reference` (or the
// variable MYENGINE_UPDATE_REFERENCE=1) writes it, on purpose, after a change of the picture that is meant.
// Without WARP (or a desktop to make the window on) the test is skipped: it exits with 77.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

#include <directxtex/DirectXTex.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine;

    constexpr int kSkipped = 77; // CTest: SKIP_RETURN_CODE
    constexpr std::uint32_t kSize = 256;

    // What counts as the same picture. A channel may differ by 2 of 255 (WARP may round a filtered texture or a division the other way
    // from one version of Windows to the next), and at most 0.1 % of the pixels (65 of 65536) may differ by more: the edge of a
    // triangle may move by a pixel. A wrong colour, a texture in the wrong slot or a missing object changes thousands of pixels.
    constexpr int kChannelTolerance = 2;
    constexpr double kPixelFractionTolerance = 0.001;

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
        windowClass.lpszClassName = L"MyEngineRenderReference";
        RegisterClassW(&windowClass);
        HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"render reference", WS_OVERLAPPEDWINDOW,
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

    fs::path SourceDirectory()
    {
        return fs::u8path(MYENGINE_SOURCE_DIR);
    }

    // ---- the textures of the scene: BMP files made here, so that the picture does not depend on a file of the project ----

    struct Rgb
    {
        std::uint8_t r;
        std::uint8_t g;
        std::uint8_t b;
    };

    template <typename Colour>
    void WriteBmp(const fs::path& path, const int size, Colour colourAt)
    {
        Check(size % 4 == 0, "the rows of the BMP have no padding only when the width is a multiple of 4");
        const int row = size * 3;
        std::vector<std::uint8_t> file(54 + static_cast<std::size_t>(row) * static_cast<std::size_t>(size), 0);
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
        put32(18, static_cast<std::uint32_t>(size));
        put32(22, static_cast<std::uint32_t>(size));
        file[26] = 1;
        file[28] = 24;
        put32(34, static_cast<std::uint32_t>(row * size));
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                // BMP rows go from the bottom: y = 0 here is the top of the picture
                const Rgb colour = colourAt(x, y);
                const std::size_t at = 54 + static_cast<std::size_t>(size - 1 - y) * static_cast<std::size_t>(row) + static_cast<std::size_t>(x) * 3;
                file[at + 0] = colour.b;
                file[at + 1] = colour.g;
                file[at + 2] = colour.r;
            }
        }
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        Check(static_cast<bool>(stream), "cannot write " + path.u8string());
    }

    // ---- the meshes ----

    // A sphere of radius 1: the normals look out, the triangles are clockwise for the camera that looks along +z
    render::MeshData Sphere()
    {
        constexpr int kSegments = 40;
        constexpr int kRings = 20;
        constexpr float kPi = 3.14159265358979f;
        render::MeshData mesh;
        for (int ring = 0; ring <= kRings; ++ring)
        {
            const float v = static_cast<float>(ring) / static_cast<float>(kRings);
            const float theta = v * kPi;
            for (int segment = 0; segment <= kSegments; ++segment)
            {
                const float u = static_cast<float>(segment) / static_cast<float>(kSegments);
                const float phi = u * 2.0f * kPi;
                const render::Float3 position{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
                mesh.vertices.push_back({position, position, {u, v}});
            }
        }
        for (int ring = 0; ring < kRings; ++ring)
        {
            for (int segment = 0; segment < kSegments; ++segment)
            {
                const std::uint32_t a = static_cast<std::uint32_t>(ring * (kSegments + 1) + segment);
                const std::uint32_t b = a + static_cast<std::uint32_t>(kSegments + 1);
                mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
            }
        }
        return mesh;
    }

    // A square of side 2 in the plane z = 0, clockwise for the camera
    render::MeshData Quad()
    {
        render::MeshData mesh;
        mesh.vertices = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
            {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
            {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
            {{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        };
        mesh.indices = {0, 1, 2, 0, 2, 3};
        return mesh;
    }

    // Uniform scale and a move: the matrices of the engine are row-major, the move is in the last row
    render::Matrix4 Place(const float scale, const float x, const float y)
    {
        render::Matrix4 matrix = render::Matrix4::Identity();
        matrix.data[0] = scale;
        matrix.data[5] = scale;
        matrix.data[10] = scale;
        matrix.data[12] = x;
        matrix.data[13] = y;
        return matrix;
    }

    // The camera: a view that is the identity and a projection that keeps x and y and maps z from [-1, 1] around the middle to the depth range
    render::Matrix4 Projection()
    {
        render::Matrix4 matrix = render::Matrix4::Identity();
        matrix.data[10] = 0.25f;
        matrix.data[14] = 0.5f;
        return matrix;
    }

    // ---- the picture and the PNG files ----

    struct Picture
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba;
    };

    bool SavePng(const fs::path& path, const Picture& picture)
    {
        fs::create_directories(path.parent_path());
        DirectX::Image image{};
        image.width = picture.width;
        image.height = picture.height;
        image.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        image.rowPitch = static_cast<std::size_t>(picture.width) * 4;
        image.slicePitch = image.rowPitch * picture.height;
        image.pixels = const_cast<std::uint8_t*>(picture.rgba.data());
        return SUCCEEDED(DirectX::SaveToWICFile(image, DirectX::WIC_FLAGS_IGNORE_SRGB, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), path.c_str()));
    }

    bool LoadPng(const fs::path& path, Picture& picture, std::string& error)
    {
        DirectX::TexMetadata metadata{};
        DirectX::ScratchImage scratch;
        const HRESULT result = DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, &metadata, scratch);
        if (FAILED(result))
        {
            error = "cannot read " + path.u8string() + " (HRESULT " + std::to_string(static_cast<long>(result)) + ")";
            return false;
        }
        const DirectX::Image* image = scratch.GetImage(0, 0, 0);
        const bool bgra = image->format == DXGI_FORMAT_B8G8R8A8_UNORM || image->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool rgba = image->format == DXGI_FORMAT_R8G8B8A8_UNORM || image->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (!bgra && !rgba)
        {
            error = "the reference is not an 8 bit RGBA or BGRA picture";
            return false;
        }
        picture.width = static_cast<std::uint32_t>(image->width);
        picture.height = static_cast<std::uint32_t>(image->height);
        picture.rgba.assign(static_cast<std::size_t>(picture.width) * picture.height * 4, 0);
        for (std::uint32_t y = 0; y < picture.height; ++y)
        {
            const std::uint8_t* row = image->pixels + static_cast<std::size_t>(y) * image->rowPitch;
            for (std::uint32_t x = 0; x < picture.width; ++x)
            {
                std::uint8_t* out = picture.rgba.data() + (static_cast<std::size_t>(y) * picture.width + x) * 4;
                out[0] = bgra ? row[x * 4 + 2] : row[x * 4 + 0];
                out[1] = row[x * 4 + 1];
                out[2] = bgra ? row[x * 4 + 0] : row[x * 4 + 2];
                out[3] = row[x * 4 + 3];
            }
        }
        return true;
    }

    struct Difference
    {
        std::size_t pixelsOver = 0;     // the pixels where a channel differs by more than the tolerance
        std::size_t pixelsDiffer = 0;   // the pixels that differ at all
        int maxDifference = 0;
        Picture picture;                // the difference, to look at: black where equal, red where it differs, magenta over the tolerance
    };

    Difference Compare(const Picture& actual, const Picture& reference)
    {
        Difference difference;
        difference.picture.width = actual.width;
        difference.picture.height = actual.height;
        difference.picture.rgba.assign(actual.rgba.size(), 255);
        for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(actual.width) * actual.height; ++pixel)
        {
            int largest = 0;
            for (int channel = 0; channel < 4; ++channel)
            {
                largest = std::max(largest, std::abs(static_cast<int>(actual.rgba[pixel * 4 + static_cast<std::size_t>(channel)]) -
                    static_cast<int>(reference.rgba[pixel * 4 + static_cast<std::size_t>(channel)])));
            }
            difference.maxDifference = std::max(difference.maxDifference, largest);
            std::uint8_t* out = difference.picture.rgba.data() + pixel * 4;
            out[0] = 0;
            out[1] = 0;
            out[2] = 0;
            if (largest > 0)
            {
                ++difference.pixelsDiffer;
                out[0] = static_cast<std::uint8_t>(std::min(255, 64 + largest * 32));
            }
            if (largest > kChannelTolerance)
            {
                ++difference.pixelsOver;
                out[0] = 255;
                out[2] = 255;
            }
        }
        return difference;
    }

    // ---- the scene ----

    class Scene
    {
    public:
        explicit Scene(const fs::path& directory)
            : directory_(directory)
        {
            // WARP and no debug layer: one rasterizer on every machine
            _putenv_s("MYENGINE_DX12_WARP", "1");
            _putenv_s("MYENGINE_DX12_DEBUG", "");
            logger_.Initialize(directory / "render_reference.log");
            adapter_ = std::make_unique<render::dx12::Dx12RenderAdapter>(logger_);
            if (!adapter_->Initialize())
            {
                throw Skip{"the DX12 adapter did not initialize: WARP is not available on this machine"};
            }
            Check(adapter_->GetAdapterName().find("WARP") != std::string::npos,
                "the adapter is '" + adapter_->GetAdapterName() + "', not WARP: MYENGINE_DX12_WARP was not obeyed");
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
            target_ = adapter_->CreateRenderTarget(kSize, kSize);
            Check(target_.IsValid(), "the render target was not created");
            sphere_ = adapter_->UploadMesh(Sphere());
            quad_ = adapter_->UploadMesh(Quad());
            Check(sphere_.IsValid() && quad_.IsValid(), "the meshes were not uploaded");
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

        const std::string& AdapterName() const { return adapterName_; }

        // The files of the scene: the shader of the project and the shader of the test (copies), two textures and three materials
        void MakeFiles()
        {
            fs::copy_file(SourceDirectory() / "assets/shaders/textured_lit.hlsl", directory_ / "textured_lit.hlsl", fs::copy_options::overwrite_existing);
            fs::copy_file(SourceDirectory() / "assets/shaders/textured_lit.shader.json", directory_ / "textured_lit.shader.json", fs::copy_options::overwrite_existing);
            fs::copy_file(SourceDirectory() / "tests/render/shaders/material_probe.hlsl", directory_ / "material_probe.hlsl", fs::copy_options::overwrite_existing);
            fs::copy_file(SourceDirectory() / "tests/render/shaders/material_probe.shader.json", directory_ / "material_probe.shader.json", fs::copy_options::overwrite_existing);

            // 8 x 8 pixels, squares of 2: orange and cream
            WriteBmp(directory_ / "checker.bmp", 8, [](const int x, const int y) -> Rgb
            {
                return ((x / 2 + y / 2) % 2 == 0) ? Rgb{232, 118, 28} : Rgb{238, 232, 206};
            });
            // 4 x 4 pixels: a gradient that tells the left from the right and the top from the bottom
            WriteBmp(directory_ / "gradient.bmp", 4, [](const int x, const int y) -> Rgb
            {
                return Rgb{static_cast<std::uint8_t>(40 + x * 60), static_cast<std::uint8_t>(40 + y * 60), 110};
            });

            // The old keys, on the shader of the project: the first texture and the tint
            WriteText(directory_ / "old.material.json",
                "{ \"shader\": \"textured_lit.shader.json\", \"texture\": \"checker.bmp\", \"tint\": [1.0, 0.85, 0.7, 1.0] }\n");
            // A second texture by name and constants by name, on the shader of the test: lerp(gTexture, gTexture2, gMix) * gColor + gBoost
            WriteText(directory_ / "named.material.json",
                "{ \"shader\": \"material_probe.shader.json\",\n"
                "  \"textures\": { \"gTexture\": \"checker.bmp\", \"gTexture2\": \"gradient.bmp\" },\n"
                "  \"params\": { \"gColor\": [0.9, 1.0, 0.9, 1.0], \"gMix\": 0.5, \"gBoost\": 0.05 } }\n");
            // The second texture is not set: the white stub (gMix 1 shows only it)
            WriteText(directory_ / "stub.material.json",
                "{ \"shader\": \"material_probe.shader.json\",\n"
                "  \"textures\": { \"gTexture\": \"checker.bmp\" },\n"
                "  \"params\": { \"gColor\": [0.45, 0.65, 1.0, 1.0], \"gMix\": 1.0 } }\n");
        }

        // The textures of the project stream in: wait until the files of the materials have arrived
        void WaitForTextures(const std::vector<fs::path>& materials)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            for (;;)
            {
                resources_->UpdateHotReload();
                bool pending = false;
                for (const fs::path& path : materials)
                {
                    const auto material = resources_->Load<resource::MaterialAsset>(path);
                    Check(material != nullptr, "the material loads: " + path.u8string());
                    if (!material->asset.texturePath.empty())
                    {
                        pending = pending || resources_->IsLoadPending(material->asset.texturePath);
                    }
                    for (const auto& [name, texture] : material->asset.textures)
                    {
                        pending = pending || resources_->IsLoadPending(texture);
                    }
                }
                if (!pending)
                {
                    return;
                }
                Check(std::chrono::steady_clock::now() < deadline, "the textures of the scene did not load");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        render::DrawItem Item(const fs::path& materialPath, const render::MeshHandle mesh, const render::Matrix4& model)
        {
            const auto material = resources_->Load<resource::MaterialAsset>(materialPath);
            const auto shader = resources_->Load<resource::ShaderAsset>(material->asset.shaderPath);
            Check(shader != nullptr && shader->asset.gpuHandle.IsValid() && !shader->asset.fallback, "the shader of " + materialPath.u8string() + " compiles");
            render::DrawItem item;
            item.mesh = mesh;
            item.shader = shader->asset.gpuHandle;
            item.material = resources_->GetMaterialParams(material);
            Check(item.material != nullptr, "the material has its parameters: " + materialPath.u8string());
            item.model = model;
            return item;
        }

        // The scene: a lit sphere with the old material at the left, a quad of the named material at the right, a quad with the stub below
        Picture Render()
        {
            const fs::path oldMaterial = directory_ / "old.material.json";
            const fs::path namedMaterial = directory_ / "named.material.json";
            const fs::path stubMaterial = directory_ / "stub.material.json";
            WaitForTextures({oldMaterial, namedMaterial, stubMaterial});
            resources_->UpdateHotReload();
            const std::array<render::DrawItem, 3> items{
                Item(oldMaterial, sphere_, Place(0.46f, -0.42f, 0.28f)),
                Item(namedMaterial, quad_, Place(0.36f, 0.5f, 0.45f)),
                Item(stubMaterial, quad_, Place(0.3f, 0.12f, -0.55f)),
            };

            adapter_->SetTargetLighting({0.35f, -0.55f, 0.75f}, 0.3f);
            Check(adapter_->BeginFrame(surface_, core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginFrame failed");
            Check(adapter_->BeginRenderTarget(target_, core::Color{0.08f, 0.1f, 0.14f, 1.0f}), "BeginRenderTarget failed");
            adapter_->SetViewProjection(surface_, render::Matrix4::Identity(), Projection());
            for (const render::DrawItem& item : items)
            {
                adapter_->Draw(surface_, item);
            }
            adapter_->EndRenderTarget(target_);
            adapter_->EndFrame(surface_);

            Picture picture;
            Check(adapter_->ReadRenderTargetPixels(target_, picture.rgba), "ReadRenderTargetPixels failed");
            Check(picture.rgba.size() == static_cast<std::size_t>(kSize) * kSize * 4, "the readback has the wrong size");
            picture.width = kSize;
            picture.height = kSize;
            adapterName_ = adapter_->GetAdapterName();
            return picture;
        }

    private:
        fs::path directory_;
        core::Logger logger_;
        std::unique_ptr<render::dx12::Dx12RenderAdapter> adapter_;
        std::unique_ptr<resource::ResourceManager> resources_;
        HWND hwnd_ = nullptr;
        render::RenderSurfaceHandle surface_;
        render::RenderTargetHandle target_;
        render::MeshHandle sphere_;
        render::MeshHandle quad_;
        std::string adapterName_;
    };

    bool UpdateRequested(const int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--update-reference")
            {
                return true;
            }
        }
        char* text = nullptr;
        std::size_t length = 0;
        bool requested = false;
        if (_dupenv_s(&text, &length, "MYENGINE_UPDATE_REFERENCE") == 0 && text != nullptr)
        {
            requested = std::string(text) == "1";
            std::free(text);
        }
        return requested;
    }

    // A picture that is only the clear colour would be a reference that tests nothing: the three objects must be in it
    void CheckThePictureHasTheObjects(const Picture& picture)
    {
        const auto pixel = [&picture](const int x, const int y)
        {
            const std::size_t at = (static_cast<std::size_t>(y) * picture.width + static_cast<std::size_t>(x)) * 4;
            return std::array<int, 3>{picture.rgba[at], picture.rgba[at + 1], picture.rgba[at + 2]};
        };
        const std::array<int, 3> clear = pixel(2, 2);
        // The centres of the objects: the sphere (-0.42, 0.28), the named quad (0.5, 0.45), the stub quad (0.12, -0.55); the view is 256 px for [-1, 1]
        const std::array<std::array<int, 2>, 3> centres{{{{74, 92}}, {{192, 70}}, {{143, 198}}}};
        const std::array<std::string, 3> names{"the sphere", "the quad of the named material", "the quad of the white stub"};
        for (std::size_t i = 0; i < centres.size(); ++i)
        {
            Check(pixel(centres[i][0], centres[i][1]) != clear, names[i] + " is not in the picture");
        }
    }
}

int main(const int argc, char** argv)
{
    int result = 0;
    const bool update = UpdateRequested(argc, argv);
    const fs::path referencePath = SourceDirectory() / "tests/render/reference/scene_256.png";
    const fs::path outputDirectory = fs::u8path(MYENGINE_REFERENCE_OUTPUT_DIR);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try
    {
        Check(SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE, "COM did not start (the PNG files need it)");
        jobs::Initialize(2);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path directory = fs::temp_directory_path() / fs::u8path("myengine B4 эталон " + std::to_string(stamp));
        fs::create_directories(directory);

        try
        {
            Picture actual;
            {
                Scene scene(directory);
                scene.MakeFiles();
                actual = scene.Render();
                std::cout << "adapter: " << scene.AdapterName() << "\n";
            }
            CheckThePictureHasTheObjects(actual);

            if (update)
            {
                Check(SavePng(referencePath, actual), "cannot write the reference " + referencePath.u8string());
                std::cout << "UPDATED the reference: " << referencePath.u8string() << "\n"
                          << "Look at it before it is committed: it is what every later run is compared with.\n";
            }
            else
            {
                Picture reference;
                std::string error;
                Check(LoadPng(referencePath, reference, error), error + "\n  (a reference is made on purpose: run the test with --update-reference)");
                Check(reference.width == actual.width && reference.height == actual.height,
                    "the reference is " + std::to_string(reference.width) + "x" + std::to_string(reference.height) + ", the picture is " +
                    std::to_string(actual.width) + "x" + std::to_string(actual.height));

                const Difference difference = Compare(actual, reference);
                const std::size_t total = static_cast<std::size_t>(actual.width) * actual.height;
                const std::size_t allowed = static_cast<std::size_t>(std::floor(static_cast<double>(total) * kPixelFractionTolerance));
                std::cout << "compared with " << referencePath.u8string() << ": " << difference.pixelsDiffer << " pixels differ, " << difference.pixelsOver
                          << " of them by more than " << kChannelTolerance << " (allowed " << allowed << "), the largest difference of a channel "
                          << difference.maxDifference << "\n";
                if (difference.pixelsOver > allowed)
                {
                    fs::create_directories(outputDirectory);
                    const fs::path actualPath = outputDirectory / "actual.png";
                    const fs::path diffPath = outputDirectory / "diff.png";
                    SavePng(actualPath, actual);
                    SavePng(diffPath, difference.picture);
                    std::cout << "FAILED: the picture is not the reference.\n"
                              << "  actual: " << actualPath.u8string() << "\n"
                              << "  diff:   " << diffPath.u8string() << " (red: differs, magenta: over the tolerance)\n"
                              << "  reference: " << referencePath.u8string() << "\n"
                              << "If the change of the picture is meant, run the test with --update-reference and commit the new reference.\n";
                    result = 1;
                }
            }
        }
        catch (const Skip& skip)
        {
            std::cout << "SKIPPED: " << skip.reason << "\n";
            result = kSkipped;
        }
        jobs::Shutdown();
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }
    catch (const std::exception& error)
    {
        std::cout << "FAILED: " << error.what() << "\n";
        result = 1;
    }
    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }
    if (result == 0)
    {
        std::cout << (update ? "render_reference: reference updated\n" : "render_reference: the picture is the reference\n");
    }
    return result;
}
