// Dx12RenderAdapter.h

#pragma once

#include <array>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "myengine/core/Types.h"
#include "myengine/render/IRenderAdapter.h"
#include "myengine/render/dx12/Dx12Context.h"

namespace myengine::core
{
    class Logger;
}

namespace myengine::render::dx12
{
    class Dx12RenderAdapter final : public IRenderAdapter
    {
    public:
        explicit Dx12RenderAdapter(core::Logger& logger);
        ~Dx12RenderAdapter() override;

        bool Initialize() override;

        RenderSurfaceHandle CreateSurface(HWND hwnd, std::uint32_t width, std::uint32_t height) override;
        void ResizeSurface(RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height) override;

        bool BeginFrame(RenderSurfaceHandle surface, const core::Color& clearColor) override;
        void DrawPrimitive(RenderSurfaceHandle surface, const Transform2D& transform) override;
        void EndFrame(RenderSurfaceHandle surface) override;

        void Shutdown() override;

    private:
        static constexpr UINT kBackBufferCount = 2;

        struct SurfaceData
        {
            HWND hwnd = nullptr;
            std::uint32_t width = 0;
            std::uint32_t height = 0;

            Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
            std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBackBufferCount> backBuffers;

            UINT rtvDescriptorSize = 0;
            UINT currentBackBuffer = 0;

            D3D12_VIEWPORT viewport{};
            D3D12_RECT scissorRect{};
        };

        struct Vertex
        {
            float position[3];
            float color[4];
        };

        bool CreateDevice();
        bool CreateCommandObjects();
        bool CreateFence();
        bool BuildPipeline();
        bool BuildTriangleResources();

        void RebuildSurfaceBuffers(SurfaceData& surface);
        void WaitForGpu();

        SurfaceData* FindSurface(RenderSurfaceHandle handle);
        const SurfaceData* FindSurface(RenderSurfaceHandle handle) const;

        std::wstring ResolveShaderPath() const;

        core::Logger& logger_;
        Dx12Context context_;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;
        Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
        Microsoft::WRL::ComPtr<ID3D12Resource> vertexUploadBuffer_;
        D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};

        std::unordered_map<std::uint32_t, SurfaceData> surfaces_;
        std::uint32_t nextSurfaceId_ = 1;

        SurfaceData* activeSurface_ = nullptr;
        HANDLE fenceEvent_ = nullptr;
    };
}