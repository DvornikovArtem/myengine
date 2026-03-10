// Dx12RenderAdapter.h

#pragma once

#include <array>
#include <filesystem>
#include <string_view>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "myengine/core/Types.h"
#include "myengine/render/IRenderAdapter.h"
#include "myengine/render/dx12/Dx12Context.h"
#include "myengine/resource/ResourceManager.h"

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
        bool PreloadTexture(std::string_view texturePath) override;

        RenderSurfaceHandle CreateSurface(HWND hwnd, std::uint32_t width, std::uint32_t height) override;
        void ResizeSurface(RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height) override;

        bool BeginFrame(RenderSurfaceHandle surface, const core::Color& clearColor) override;
        void SetViewProjection(RenderSurfaceHandle surface, const Matrix4& view, const Matrix4& projection) override;
        void DrawPrimitive(
            RenderSurfaceHandle surface,
            PrimitiveType primitive,
            const Matrix4& model,
            const core::Color& color,
            std::string_view texturePath) override;
        void EndFrame(RenderSurfaceHandle surface) override;

        void Shutdown() override;

    private:
        static constexpr UINT kBackBufferCount = 2;
        static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
        static constexpr UINT kMaxTextureDescriptors = 256;

        struct SurfaceData
        {
            HWND hwnd = nullptr;
            std::uint32_t width = 0;
            std::uint32_t height = 0;

            Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
            std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBackBufferCount> backBuffers;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
            Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil;

            UINT rtvDescriptorSize = 0;
            UINT currentBackBuffer = 0;

            D3D12_VIEWPORT viewport{};
            D3D12_RECT scissorRect{};

            Matrix4 viewProjection = Matrix4::Identity();
        };

        struct Vertex
        {
            float position[3];
            float uv[2];
        };

        struct PrimitiveGeometry
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
            Microsoft::WRL::ComPtr<ID3D12Resource> vertexUploadBuffer;
            D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
            D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            UINT vertexCount = 0;
        };

        struct TextureRecord
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::vector<std::uint8_t> pixels;

            Microsoft::WRL::ComPtr<ID3D12Resource> textureResource;
            Microsoft::WRL::ComPtr<ID3D12Resource> uploadResource;
            UINT descriptorIndex = 0;
            bool uploaded = false;
        };

        bool CreateDevice();
        bool CreateCommandObjects();
        bool CreateFence();
        bool BuildPipeline();
        bool BuildPrimitiveResources();
        bool BuildTextureResources();
        bool UploadPrimitiveGeometry(PrimitiveGeometry& geometry, const Vertex* vertices, UINT vertexCount, D3D_PRIMITIVE_TOPOLOGY topology);
        bool EnsureTextureUploaded(TextureRecord& texture);
        UINT ResolveTextureDescriptorIndex(std::string_view texturePath);

        void RebuildSurfaceBuffers(SurfaceData& surface);
        void WaitForGpu();

        SurfaceData* FindSurface(RenderSurfaceHandle handle);
        const SurfaceData* FindSurface(RenderSurfaceHandle handle) const;

        std::wstring ResolveShaderPath() const;

        core::Logger& logger_;
        Dx12Context context_;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> trianglePipelineState_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> linePipelineState_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> textureSrvHeap_;
        UINT textureSrvDescriptorSize_ = 0;
        UINT nextTextureDescriptorIndex_ = 0;
        std::array<PrimitiveGeometry, 4> primitives_{};

        std::unordered_map<std::uint32_t, SurfaceData> surfaces_;
        std::uint32_t nextSurfaceId_ = 1;

        SurfaceData* activeSurface_ = nullptr;
        HANDLE fenceEvent_ = nullptr;
        std::unordered_map<std::string, TextureRecord> loadedTextures_;
        std::unordered_set<std::string> missingTextures_;
        std::string defaultTextureKey_ = "__default_white__";
        resource::ResourceManager resourceManager_;
    };
}