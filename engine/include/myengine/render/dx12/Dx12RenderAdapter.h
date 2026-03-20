// Dx12RenderAdapter.h

#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>

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
        ShaderHandle CreateShaderProgram(const ShaderProgramData& shaderProgram) override;

        RenderSurfaceHandle CreateSurface(HWND hwnd, std::uint32_t width, std::uint32_t height) override;
        void ResizeSurface(RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height) override;

        bool BeginFrame(RenderSurfaceHandle surface, const core::Color& clearColor) override;
        void SetViewProjection(RenderSurfaceHandle surface, const Matrix4& view, const Matrix4& projection) override;
        void Draw(RenderSurfaceHandle surface, const DrawItem& drawItem) override;
        void EndFrame(RenderSurfaceHandle surface) override;

        void Shutdown() override;

    private:
        static constexpr UINT kBackBufferCount = 2;
        static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
        static constexpr UINT kMaxTextureDescriptors = 512;

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

        struct DxVertex
        {
            float position[3];
            float normal[3];
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
        bool BuildTextureDescriptorHeap();
        bool ResetCommandList();
        bool ExecuteCommandListAndWait(const char* contextLabel);
        bool CreateBuffer(const void* data, UINT64 dataSize, D3D12_RESOURCE_STATES finalState, Microsoft::WRL::ComPtr<ID3D12Resource>& outBuffer);
        bool CreateTextureResource(const TextureData& textureData, Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture);
        bool CompileShaderBlob(const std::filesystem::path& sourcePath, const std::string& entryPoint, const std::string& profile, Microsoft::WRL::ComPtr<ID3DBlob>& outBlob) const;

        void RebuildSurfaceBuffers(SurfaceData& surface);
        void WaitForGpu();

        SurfaceData* FindSurface(RenderSurfaceHandle handle);

        core::Logger& logger_;
        Dx12Context context_;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> textureSrvHeap_;
        UINT textureSrvDescriptorSize_ = 0;
        UINT nextTextureDescriptorIndex_ = 0;

        std::unordered_map<std::uint32_t, SurfaceData> surfaces_;
        std::uint32_t nextSurfaceId_ = 1;

        std::unordered_map<std::uint32_t, MeshRecord> meshes_;
        std::uint32_t nextMeshId_ = 1;

        std::unordered_map<std::uint32_t, TextureRecord> textures_;
        std::uint32_t nextTextureId_ = 1;

        std::unordered_map<std::uint32_t, ShaderRecord> shaders_;
        std::uint32_t nextShaderId_ = 1;

        SurfaceData* activeSurface_ = nullptr;
        HANDLE fenceEvent_ = nullptr;
    };
}