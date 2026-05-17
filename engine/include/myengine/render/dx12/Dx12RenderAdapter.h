// Dx12RenderAdapter.h

#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <myengine/render/IRenderAdapter.h>
#include <myengine/render/dx12/Dx12Context.h>

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

        MeshHandle UploadMesh(const MeshData& meshData) override;
        TextureHandle CreateTexture(const TextureData& textureData) override;
        void DestroyTexture(TextureHandle texture) override;
        ShaderHandle CreateShaderProgram(const ShaderProgramData& shaderProgram) override;

        RenderSurfaceHandle CreateSurface(HWND hwnd, std::uint32_t width, std::uint32_t height) override;
        void ResizeSurface(RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height) override;

        bool BeginFrame(RenderSurfaceHandle surface, const core::Color& clearColor) override;
        void SetRenderRegion(RenderSurfaceHandle surface, const IntRect* region) override;
        void SetViewProjection(RenderSurfaceHandle surface, const Matrix4& view, const Matrix4& projection) override;
        void Draw(RenderSurfaceHandle surface, const DrawItem& drawItem) override;
        void DrawDebugLines(RenderSurfaceHandle surface, const std::vector<DebugLine>& lines) override;
        void DrawUiGeometry(RenderSurfaceHandle surface, const UiDrawData& drawData) override;
        void EndFrame(RenderSurfaceHandle surface) override;

        void Shutdown() override;

        ID3D12Device* GetDevice() const;
        ID3D12CommandQueue* GetCommandQueue() const;
        ID3D12GraphicsCommandList* GetCommandList() const;
        DXGI_FORMAT GetBackBufferFormat() const;
        DXGI_FORMAT GetDepthStencilFormat() const;
        UINT GetFramesInFlight() const;

    private:
        static constexpr UINT kBackBufferCount = 2;
        static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
        static constexpr UINT kMaxTextureDescriptors = 512;

        struct FrameUploadBuffer
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            std::uint8_t* mappedData = nullptr;
            UINT64 capacity = 0;
            UINT64 used = 0;
        };

        struct SurfaceData
        {
            HWND hwnd = nullptr;
            std::uint32_t width = 0;
            std::uint32_t height = 0;

            Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
            std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBackBufferCount> backBuffers;
            std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kBackBufferCount> frameAllocators;
            std::array<UINT64, kBackBufferCount> frameFenceValues{};
            std::array<std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>, kBackBufferCount> frameTransientResources;
            std::array<FrameUploadBuffer, kBackBufferCount> uiVertexUploadBuffers;
            std::array<FrameUploadBuffer, kBackBufferCount> uiIndexUploadBuffers;
            std::array<FrameUploadBuffer, kBackBufferCount> debugVertexUploadBuffers;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
            Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil;

            UINT rtvDescriptorSize = 0;
            UINT currentBackBuffer = 0;
            D3D12_VIEWPORT viewport{};
            D3D12_RECT scissorRect{};
            Matrix4 viewProjection = Matrix4::Identity();
        };

        struct DxVertex
        {
            float position[3];
            float normal[3];
            float uv[2];
        };

        struct DxDebugVertex
        {
            float position[4];
            float color[4];
        };

        struct DxUiVertex
        {
            float position[2];
            std::uint8_t color[4];
            float uv[2];
        };

        struct MeshRecord
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
            Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
            D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
            D3D12_INDEX_BUFFER_VIEW indexBufferView{};
            UINT indexCount = 0;
        };

        struct TextureRecord
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> textureResource;
            UINT descriptorIndex = 0;
        };

        struct ShaderRecord
        {
            Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
        };

        bool CreateDevice();
        bool CreateCommandObjects();
        bool CreateFence();
        bool BuildRootSignature();
        bool BuildDebugLinePipeline();
        bool BuildTextureDescriptorHeap();
        bool BuildUiPipeline();
        bool ResetCommandList();
        bool ExecuteCommandListAndWait(const char* contextLabel);
        bool CreateBuffer(const void* data, UINT64 dataSize, D3D12_RESOURCE_STATES finalState, Microsoft::WRL::ComPtr<ID3D12Resource>& outBuffer);
        bool CreateUploadBuffer(const void* data, UINT64 dataSize, Microsoft::WRL::ComPtr<ID3D12Resource>& outBuffer);
        bool CreateTextureResource(const TextureData& textureData, Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture);
        bool CompileShaderBlob(const std::filesystem::path& sourcePath, const std::string& entryPoint, const std::string& profile, Microsoft::WRL::ComPtr<ID3DBlob>& outBlob) const;
        bool EnsureFrameUploadBuffer(FrameUploadBuffer& buffer, UINT64 requiredSize);
        bool AllocateFrameUploadData(FrameUploadBuffer& buffer, const void* data, UINT64 dataSize, UINT64 alignment, D3D12_GPU_VIRTUAL_ADDRESS& outGpuAddress);
        void ReleaseFrameUploadBuffer(FrameUploadBuffer& buffer);

        void RebuildSurfaceBuffers(SurfaceData& surface);
        void WaitForGpu();
        void WaitForFenceValue(UINT64 fenceValue);

        SurfaceData* FindSurface(RenderSurfaceHandle handle);

        core::Logger& logger_;
        Dx12Context context_;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> debugRootSignature_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> debugLinePipelineState_;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> uiRootSignature_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> uiPipelineState_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> textureSrvHeap_;
        UINT textureSrvDescriptorSize_ = 0;
        UINT nextTextureDescriptorIndex_ = 0;

        std::unordered_map<std::uint32_t, SurfaceData> surfaces_;
        std::uint32_t nextSurfaceId_ = 1;

        std::unordered_map<std::uint32_t, MeshRecord> meshes_;
        std::uint32_t nextMeshId_ = 1;

        std::unordered_map<std::uint32_t, TextureRecord> textures_;
        std::uint32_t nextTextureId_ = 1;
        TextureHandle defaultWhiteTexture_{};

        std::unordered_map<std::uint32_t, ShaderRecord> shaders_;
        std::uint32_t nextShaderId_ = 1;

        SurfaceData* activeSurface_ = nullptr;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* activeFrameTransientResources_ = nullptr;
        HANDLE fenceEvent_ = nullptr;
        bool allowTearing_ = false;
        bool vsyncEnabled_ = true;
    };
}