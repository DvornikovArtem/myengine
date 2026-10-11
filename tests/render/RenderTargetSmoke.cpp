// DX12 render-to-texture on a real device and window (manual, not part of CTest, like the other smokes):
// a target is created, a triangle is drawn into it between BeginRenderTarget / EndRenderTarget, the texture is
// drawn by the UI pass in the same frame, the pixels are read back, and targets are created and destroyed
// far past the heap limits to prove that slots and descriptors are recycled.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <myengine/core/Logger.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/render/RenderTargetPool.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>

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

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND CreateTestWindow()
    {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"MyEngineRenderTargetSmoke";
        RegisterClassW(&windowClass);
        HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"render target smoke", WS_OVERLAPPEDWINDOW,
            100, 100, 320, 240, nullptr, nullptr, windowClass.hInstance, nullptr);
        Check(hwnd != nullptr, "The test window was not created");
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        return hwnd;
    }

    void PumpMessages()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    // A triangle in clip space (clockwise, so that it faces the camera with the default culling)
    render::MeshData Triangle()
    {
        render::MeshData mesh;
        mesh.vertices = {
            {{-0.6f, -0.6f, 0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
            {{0.0f, 0.6f, 0.5f}, {0.0f, 0.0f, -1.0f}, {0.5f, 0.0f}},
            {{0.6f, -0.6f, 0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        };
        mesh.indices = {0, 1, 2};
        return mesh;
    }

    std::uint8_t AlphaAt(const std::vector<std::uint8_t>& pixels, const std::uint32_t width, const std::uint32_t x, const std::uint32_t y)
    {
        return pixels[(static_cast<std::size_t>(y) * width + x) * 4 + 3];
    }
}

int main()
{
    try
    {
        core::Logger logger;
        logger.Initialize(fs::temp_directory_path() / "myengine_render_target_smoke.log");

        render::dx12::Dx12RenderAdapter adapter(logger);
        Check(adapter.Initialize(), "The DX12 adapter did not initialize");

        // With MYENGINE_DX12_DEBUG=1 in a Debug build, a validation error breaks the process: the test fails
        if (adapter.GetDevice() != nullptr)
        {
            Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
            if (SUCCEEDED(adapter.GetDevice()->QueryInterface(IID_PPV_ARGS(infoQueue.GetAddressOf()))))
            {
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
                std::cout << "D3D12 debug layer: validation errors break the test\n";
            }
        }

        HWND hwnd = CreateTestWindow();
        const render::RenderSurfaceHandle surface = adapter.CreateSurface(hwnd, 320, 240);
        Check(surface.IsValid(), "The surface was not created");

        const render::MeshHandle mesh = adapter.UploadMesh(Triangle());
        render::ShaderProgramData program;
        program.sourcePath = fs::u8path(MYENGINE_SOURCE_DIR) / "assets/shaders/textured_lit.hlsl";
        const render::ShaderHandle shader = adapter.CreateShaderProgram(program);
        render::TextureData white;
        white.width = 1;
        white.height = 1;
        white.pixelsRgba8 = {255, 255, 255, 255};
        const render::TextureHandle whiteTexture = adapter.CreateTexture(white);
        Check(mesh.IsValid() && shader.IsValid() && whiteTexture.IsValid(), "Mesh, shader or texture were not created");

        constexpr std::uint32_t kSize = 64;
        const render::RenderTargetHandle target = adapter.CreateRenderTarget(kSize, kSize);
        Check(target.IsValid(), "The render target was not created");
        const render::TextureHandle targetTexture = adapter.GetRenderTargetTexture(target);
        Check(targetTexture.IsValid(), "The render target has no texture");
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        Check(adapter.GetRenderTargetSize(target, width, height) && width == kSize && height == kSize, "The render target size is wrong");

        // One frame: the triangle goes to the target, the target's texture is drawn by the UI pass
        {
            Check(adapter.BeginFrame(surface, core::Color{0.1f, 0.1f, 0.2f, 1.0f}), "BeginFrame failed");
            Check(adapter.BeginRenderTarget(target, core::Color{0.0f, 0.0f, 0.0f, 0.0f}), "BeginRenderTarget failed");
            adapter.SetViewProjection(surface, render::Matrix4::Identity(), render::Matrix4::Identity());
            render::DrawItem item;
            item.mesh = mesh;
            item.shader = shader;
            item.texture = whiteTexture;
            adapter.Draw(surface, item);
            // Not allowed inside a pass, must be ignored without breaking the pass
            const render::UiVertex vertices[4] = {
                {{0.0f, 0.0f}, {255, 255, 255, 255}, {0.0f, 0.0f}},
                {{64.0f, 0.0f}, {255, 255, 255, 255}, {1.0f, 0.0f}},
                {{64.0f, 64.0f}, {255, 255, 255, 255}, {1.0f, 1.0f}},
                {{0.0f, 64.0f}, {255, 255, 255, 255}, {0.0f, 1.0f}},
            };
            const std::uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
            render::UiDrawData ui;
            ui.vertices = vertices;
            ui.vertexCount = 4;
            ui.indices = indices;
            ui.indexCount = 6;
            ui.texture = targetTexture;
            adapter.DrawUiGeometry(surface, ui);
            adapter.EndRenderTarget(target);

            // The same texture on the back buffer, as ImGui::Image does
            adapter.DrawUiGeometry(surface, ui);
            adapter.EndFrame(surface);
        }

        std::vector<std::uint8_t> pixels;
        Check(adapter.ReadRenderTargetPixels(target, pixels), "ReadRenderTargetPixels failed");
        Check(pixels.size() == static_cast<std::size_t>(kSize) * kSize * 4, "The readback has the wrong size");
        Check(AlphaAt(pixels, kSize, 32, 40) > 0, "The triangle was not drawn into the target");
        Check(AlphaAt(pixels, kSize, 2, 2) == 0, "The corner of the target must stay cleared");
        Check(AlphaAt(pixels, kSize, 61, 2) == 0, "The corner of the target must stay cleared");
        std::cout << "target pixel (32,40): alpha " << static_cast<int>(AlphaAt(pixels, kSize, 32, 40)) << '\n';

        // Created and destroyed far beyond the 64 slots and the descriptor limit: everything must be recycled
        for (int round = 0; round < 300; ++round)
        {
            const render::RenderTargetHandle temporary = adapter.CreateRenderTarget(64, 64);
            Check(temporary.IsValid(), "A render target slot was not recycled");

            Check(adapter.BeginFrame(surface, core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginFrame failed in the loop");
            Check(adapter.BeginRenderTarget(temporary, core::Color{0.0f, 0.0f, 0.0f, 0.0f}), "BeginRenderTarget failed in the loop");
            adapter.SetViewProjection(surface, render::Matrix4::Identity(), render::Matrix4::Identity());
            render::DrawItem item;
            item.mesh = mesh;
            item.shader = shader;
            item.texture = whiteTexture;
            adapter.Draw(surface, item);
            adapter.EndRenderTarget(temporary);
            adapter.EndFrame(surface);

            adapter.DestroyRenderTarget(temporary);
            if (round % 50 == 0)
            {
                PumpMessages();
            }
        }

        // The pool leases, reuses and trims
        {
            render::RenderTargetPool pool(adapter, 3);
            const auto a = pool.Acquire(128, 128);
            const auto b = pool.Acquire(128, 128);
            Check(a.IsValid() && b.IsValid() && a.value != b.value, "The pool must hand out distinct targets");
            pool.Release(a);
            const auto again = pool.Acquire(128, 128);
            Check(again.value == a.value, "The pool must reuse a released target of the same size");
            const auto c = pool.Acquire(256, 256);
            Check(c.IsValid() && pool.TotalCount() == 3, "The pool must create up to its limit");
            Check(!pool.Acquire(64, 64).IsValid(), "A full pool must refuse");
            pool.Release(b);
            Check(pool.Acquire(64, 64).IsValid(), "A free target of another size must make room");
        }

        adapter.DestroyRenderTarget(target);
        adapter.Shutdown();
        DestroyWindow(hwnd);
        std::cout << "OK: render targets: draw, texture in the UI pass, readback, recycling of 300 targets, pool\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
