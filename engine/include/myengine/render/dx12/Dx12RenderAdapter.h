// Dx12RenderAdapter.h

#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <myengine/render/IRenderAdapter.h>
#include <myengine/render/dx12/Dx12Context.h>
#include <myengine/render/dx12/InputLayoutBuilder.h>
#include <myengine/render/dx12/RootSignatureBuilder.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::render::dx12
{
    class Dx12GpuProfiler;
    class ShaderReflector;
    struct CompiledShaderProgram;

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
        bool GetShaderLayout(ShaderHandle shader, ShaderLayout& outLayout, ShaderVariant variant = ShaderVariant::Normal) const override;
        bool GetBuiltinShaderLayout(BuiltinShader shader, ShaderLayout& outLayout) const override;
        std::vector<ShaderDiagnostic> GetLastShaderDiagnostics() const override { return lastShaderDiagnostics_; }
        void DestroyShader(ShaderHandle shader) override;
        bool ReloadShaderProgram(ShaderHandle shader, const ShaderProgramData& shaderProgram) override;
        std::uint32_t GetShaderVersion(ShaderHandle shader) const override;
        std::vector<ShaderProgramInfo> GetShaderPrograms() const override;
        ShaderStatistics GetShaderStatistics() const override;

        // The debug layer (--dx12-debug / MYENGINE_DX12_DEBUG) is on. The report below exists only with it.
        bool IsDebugLayerEnabled() const { return debugLayerEnabled_; }

        // What Shutdown found alive in the device when everything of the adapter was released: the objects other than the device
        // itself. `checked` is false when the debug layer is off (or Shutdown has not run). The objects are also in the log.
        struct LiveObjectReport
        {
            bool checked = false;
            std::uint32_t liveObjects = 0;
            std::vector<std::string> objects;   // "Live ID3D12Resource at 0x..., Refcount: 1"
        };
        const LiveObjectReport& GetLiveObjectReport() const { return liveObjectReport_; }

        RenderSurfaceHandle CreateSurface(HWND hwnd, std::uint32_t width, std::uint32_t height) override;
        void ResizeSurface(RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height) override;

        bool BeginFrame(RenderSurfaceHandle surface, const core::Color& clearColor) override;
        void SetRenderRegion(RenderSurfaceHandle surface, const IntRect* region) override;
        void SetViewProjection(RenderSurfaceHandle surface, const Matrix4& view, const Matrix4& projection) override;
        void Draw(RenderSurfaceHandle surface, const DrawItem& drawItem) override;
        void DrawDebugLines(RenderSurfaceHandle surface, const std::vector<DebugLine>& lines) override;
        void DrawUiGeometry(RenderSurfaceHandle surface, const UiDrawData& drawData) override;
        void EndFrame(RenderSurfaceHandle surface) override;

        void SetVSync(bool enabled) override { vsyncEnabled_ = enabled; }
        bool IsVSyncEnabled() const override { return vsyncEnabled_; }
        void SetWireframe(bool enabled) override { wireframe_ = enabled; }
        void SetTargetLighting(const Float3& direction, float ambient) override
        {
            targetLightDirection_ = direction;
            targetAmbient_ = ambient;
        }
        void SetRenderSettings(const RenderSettings& settings) override;
        RenderSettings GetRenderSettings() const override { return settings_; }
        void SetSceneLighting(const SceneLighting& lighting) override { lighting_ = lighting; }
        bool RequestScreenshot(const std::string& path) override;

        RenderTargetHandle CreateRenderTarget(std::uint32_t width, std::uint32_t height) override;
        void DestroyRenderTarget(RenderTargetHandle target) override;
        TextureHandle GetRenderTargetTexture(RenderTargetHandle target) const override;
        bool BeginRenderTarget(RenderTargetHandle target, const core::Color& clearColor) override;
        void EndRenderTarget(RenderTargetHandle target) override;
        bool ReadRenderTargetPixels(RenderTargetHandle target, std::vector<std::uint8_t>& outRgba8) override;
        bool GetRenderTargetSize(RenderTargetHandle target, std::uint32_t& outWidth, std::uint32_t& outHeight) const override;

        void BeginDebugEvent(const std::string& name) override;
        void EndDebugEvent() override;
        void SetRenderTargetLabel(const std::string& label) override { pendingTargetLabel_ = label; }
        bool GetLastFrameStats(GpuFrameStats& out, bool waitForGpu) override;
        std::string GetAdapterName() const override { return adapterName_; }
        bool IsDeviceRemoved() const override { return deviceRemovedLogged_; }

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
        static constexpr UINT kMaxTextureDescriptors = 2048;
        static constexpr UINT kMaxRenderTargets = 64;

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
            std::array<FrameUploadBuffer, kBackBufferCount> constantUploadBuffers; // the data of root CBVs
            std::array<FrameUploadBuffer, kBackBufferCount> instanceUploadBuffers; // the model matrices of the instanced draws
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
            Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil;

            UINT rtvDescriptorSize = 0;
            UINT currentBackBuffer = 0;
            D3D12_VIEWPORT viewport{};
            D3D12_RECT scissorRect{};
            Matrix4 viewProjection = Matrix4::Identity();
            Float3 cameraPosition{};
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

        // The tables of the vertex streams (InputLayoutBuilder) describe these structs
        static_assert(sizeof(DxVertex) == kMeshVertexStride, "the mesh vertex stream table is out of date");
        static_assert(sizeof(DxDebugVertex) == kDebugLineVertexStride, "the debug line vertex stream table is out of date");
        static_assert(sizeof(DxUiVertex) == kUiVertexStride, "the UI vertex stream table is out of date");

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
            bool ownedByRenderTarget = false; // DestroyTexture leaves it to DestroyRenderTarget
        };

        struct RenderTargetRecord
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> color;
            Microsoft::WRL::ComPtr<ID3D12Resource> depth;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            UINT slot = 0; // index in the RTV and DSV heaps
            TextureHandle texture{};
            Matrix4 viewProjection = Matrix4::Identity();
            Float3 cameraPosition{};
        };

        // A resource that the GPU may still read: released once the fence reaches `fenceValue`
        struct RetiredResource
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            Microsoft::WRL::ComPtr<ID3D12Resource> secondResource;
            std::vector<Microsoft::WRL::ComPtr<IUnknown>> objects; // pipeline states, root signatures of a replaced program
            UINT64 fenceValue = 0;
            UINT descriptorIndex = 0;
            bool hasDescriptor = false;
            bool hasSlot = false;
            UINT slot = 0;
        };

        // The kind of pass a program is for: the states of its pipeline
        enum class ProgramKind
        {
            Scene,
            DebugLines,
            Ui,
            Post,   // a fullscreen triangle (bloom, composite): no depth, no culling, no vertex data
        };

        // The key of a pipeline state variant of one program: the same shaders for another blend, culling, pass or target format
        struct PipelineKey
        {
            BlendMode blend = BlendMode::Opaque;
            bool doubleSided = false;
            bool depthOnly = false;           // the shadow pass: no pixel shader, no colour target
            DXGI_FORMAT colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            std::uint8_t samples = 1;
            std::uint64_t Hash() const
            {
                return static_cast<std::uint64_t>(blend) | (doubleSided ? 4ull : 0ull) | (depthOnly ? 8ull : 0ull) |
                    (static_cast<std::uint64_t>(samples) << 8) | (static_cast<std::uint64_t>(colorFormat) << 16);
            }
        };

        // Where the renderer writes its own values in a program: found by name once, when the program is created
        constexpr static std::uint32_t kNoOffset = 0xFFFFFFFFu;
        struct ConstantBufferSlot
        {
            std::uint32_t rootParameter = kNoRootParameter;
            std::uint32_t layoutIndex = 0; // the index of the buffer in ShaderLayout::constantBuffers (and in MaterialParams)
            bool rootConstants = true;   // root constants or a root CBV
            std::uint32_t sizeBytes = 0;
            std::uint32_t sizeDwords = 0;
            // The "engine semantics" (docs/render/architecture.md, 3.5): offsets of the variables by name
            std::uint32_t model = kNoOffset;          // gModel
            std::uint32_t viewProjection = kNoOffset; // gViewProjection
            std::uint32_t color = kNoOffset;          // gColor (a draw item without a material only; with one it is the material's)
            std::uint32_t lightDirection = kNoOffset; // gLightDirection (xyz: direction, w: ambient)
            std::uint32_t screenSize = kNoOffset;     // screenSize (the UI)
            std::uint32_t translation = kNoOffset;    // translation (the UI)
            std::uint32_t lightViewProjection = kNoOffset; // gLightViewProjection: the shadow map's view-projection
            std::uint32_t cameraPosition = kNoOffset;      // gCameraPosition
            std::uint32_t lightColor = kNoOffset;          // gLightColor (rgb)
            std::uint32_t ambientSky = kNoOffset;          // gAmbientSky
            std::uint32_t ambientGround = kNoOffset;       // gAmbientGround
            std::uint32_t shadowParams = kNoOffset;        // gShadowParams: x 1/size, y depth bias, z normal offset, w enabled
            std::uint32_t postParams = kNoOffset;          // gPostParams (post passes)
            std::uint32_t texelSize = kNoOffset;           // gTexelSize (post passes)
        };
        // A texture of the program: where its descriptor table is and which texture of MaterialParams it takes
        struct TextureSlot
        {
            std::uint32_t rootParameter = kNoRootParameter;
            std::uint32_t materialIndex = 0; // the index among the Texture resources of the layout (MaterialParams::Textures())
        };
        struct DrawSlots
        {
            std::vector<ConstantBufferSlot> constantBuffers;
            std::vector<TextureSlot> textures;                 // every texture of the shader, found once when the program is built
            std::uint32_t textureParameter = kNoRootParameter; // the first texture (the smallest register): where DrawItem::texture goes
            std::uint32_t shadowMapParameter = kNoRootParameter; // the texture named gShadowMap: the renderer binds the shadow map there
        };

        struct ShaderRecord
        {
            std::uint32_t version = 1;                                 // +1 for every successful reload
            std::string sourceName;                                    // the file of the program
            std::string lastError;                                     // the first error of the last reload that failed
            Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature; // from the layout, shared by programs of one shape
            std::uint64_t rootSignatureShape = 0;
            ProgramBinding binding;                                    // name -> root parameter
            DrawSlots slots;
            Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
            Microsoft::WRL::ComPtr<ID3D12PipelineState> wireframePipelineState; // same shaders, fill mode wireframe (may be empty)
            std::uint64_t materialLayoutHash = 0;                      // MaterialLayoutHash of `layout`: the parameters of a material must match it
            mutable bool materialMismatchLogged = false;               // the warning about parameters built for another layout is given once
            ShaderLayout layout;          // from the reflection of the compiled shaders
            ShaderLayout wireframeLayout; // the vertex shader with the constant pixel shader of the wireframe variant
            // What the pipeline state variants are built from (blend modes, the shadow pass, the HDR / MSAA scene target)
            ProgramKind kind = ProgramKind::Scene;
            Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> pixelBlob;
            InputLayoutBuild inputLayout;
            bool instanced = false;       // the vertex shader reads INSTANCE0..3: the model matrix comes from slot 1
            mutable std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> variants;
        };

        // The marker group that the draw calls outside render targets fall into: switched when the kind of calls changes
        enum class AutoGroup
        {
            None,
            Scene,
            SceneTransparent,
            Shadow,
            Bloom,
            Composite,
            DebugLines,
            Ui,
        };

        void EnterAutoGroup(AutoGroup group);
        void CloseAutoGroup();
        // DRED: the breadcrumbs and the page fault of a device that was removed (once)
        void LogDeviceRemoved(const char* where);

        bool CreateDevice();
        bool CreateCommandObjects();
        bool CreateFence();
        bool BuildDebugLinePipeline();
        bool BuildTextureDescriptorHeap();
        bool BuildRenderTargetHeaps();
        bool AllocateTextureDescriptor(UINT& outIndex);
        void RetireResources(RetiredResource&& retired);
        void CollectRetiredResources();
        const Matrix4& CurrentViewProjection(const SurfaceData& surface) const;
        bool BuildUiPipeline();
        bool ResetCommandList();
        bool ExecuteCommandListAndWait(const char* contextLabel);
        bool CreateBuffer(const void* data, UINT64 dataSize, D3D12_RESOURCE_STATES finalState, Microsoft::WRL::ComPtr<ID3D12Resource>& outBuffer);
        bool CreateUploadBuffer(const void* data, UINT64 dataSize, Microsoft::WRL::ComPtr<ID3D12Resource>& outBuffer);
        bool CreateTextureResource(const TextureData& textureData, Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture);
        // Compiles and reflects one of the renderer's own programs (the file is searched like the other assets).
        // The messages go to the log.
        bool BuildBuiltinProgram(const char* relativeDescriptorPath, ProgramKind kind, ShaderRecord& record);
        void LogShaderDiagnostics(const std::vector<ShaderDiagnostic>& diagnostics) const;
        // The root signature (from the layout, cached by shape), the pipeline states and the draw slots of a program
        bool BuildProgram(const CompiledShaderProgram& compiled, const ShaderProgramData& data, ProgramKind kind, ShaderRecord& record);
        bool BuildProgramImpl(const CompiledShaderProgram& compiled, const ShaderProgramData& data, ProgramKind kind, ShaderRecord& record);
        void RetireShaderRecord(ShaderRecord&& record);
        void ReleaseRootSignature(std::uint64_t shape, RetiredResource& retired);
        bool GetRootSignature(const RootSignatureDescription& description, Microsoft::WRL::ComPtr<ID3D12RootSignature>& outRootSignature);
        void SetRootSignature(ID3D12RootSignature* rootSignature);
        void SetConstants(const ConstantBufferSlot& slot, SurfaceData& surface);
        bool EnsureFrameUploadBuffer(FrameUploadBuffer& buffer, UINT64 requiredSize);
        bool AllocateFrameUploadData(FrameUploadBuffer& buffer, const void* data, UINT64 dataSize, UINT64 alignment, D3D12_GPU_VIRTUAL_ADDRESS& outGpuAddress);
        void ReleaseFrameUploadBuffer(FrameUploadBuffer& buffer);

        // ---- scene passes (Dx12ScenePass.cpp): the draws of a surface are collected, then drawn as one shadow pass, the opaque
        // instanced groups, the transparent ones, and (with MSAA or bloom) resolved, blurred and composited into the back buffer ----
        struct PendingDraw
        {
            MeshHandle mesh;
            ShaderHandle shader;
            const MaterialParams* material = nullptr;
            TextureHandle texture;
            Matrix4 model;
            core::Color color;
            BlendMode blend = BlendMode::Opaque;
            bool doubleSided = false;
            bool castShadow = true;
            Float3 boundsCenter{};
            float boundsRadius = 0.0f;
        };
        // How a pass writes the engine values (the light, the camera, the shadow map) into the constant buffers
        struct PassParams
        {
            Matrix4 viewProjection;
            Matrix4 lightViewProjection;
            Float3 cameraPosition{};
            float shadowParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            bool depthOnly = false;
            PipelineKey key;
        };
        static constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        static constexpr UINT kBloomLevels = 3;
        struct SceneTargets
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint8_t samples = 1;
            bool bloom = false;
            Microsoft::WRL::ComPtr<ID3D12Resource> color;     // HDR, MSAA when samples > 1
            Microsoft::WRL::ComPtr<ID3D12Resource> depth;
            Microsoft::WRL::ComPtr<ID3D12Resource> resolved;  // single sample copy of `color` (only with MSAA)
            std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBloomLevels> bloomLevels;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap; // 0 colour, 1 resolved, 2.. bloom levels
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
            UINT srvResolved = 0;                                  // the descriptors in the texture heap
            std::array<UINT, kBloomLevels> srvBloom{};
            bool descriptorsAllocated = false;
            D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        };
        bool EnsureSceneTargets(const SurfaceData& surface);
        bool EnsureShadowMap();
        ID3D12PipelineState* GetPipelineVariant(const ShaderRecord& record, const PipelineKey& key);
        void FlushScene();
        void BeginSceneOutput(SurfaceData& surface);
        void EndSceneOutput(SurfaceData& surface);
        void SetSurfaceOutput(SurfaceData& surface); // the render target the commands of the surface go to now (scene or back buffer)
        void RenderShadowPass(SurfaceData& surface, const std::vector<std::size_t>& casters, PassParams& mainPass);
        void RecordDraws(SurfaceData& surface, const std::vector<std::size_t>& order, const PassParams& pass);
        void RunBloomAndComposite(SurfaceData& surface);
        void DrawFullscreen(float mode, UINT sourceA, UINT sourceB, float texelX, float texelY, const PipelineKey& key);
        void WriteEngineConstants(const ConstantBufferSlot& slot, const PendingDraw& draw, const PassParams& pass);
        void RetireSceneTargets();
        void RetireShadowMap();
        void ApplyEnvironmentSettings();
        void WriteScreenshot(ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, std::uint32_t width, std::uint32_t height);

        void RebuildSurfaceBuffers(SurfaceData& surface);
        void WaitForGpu();
        void WaitForFenceValue(UINT64 fenceValue);

        SurfaceData* FindSurface(RenderSurfaceHandle handle);

        core::Logger& logger_;
        Dx12Context context_;

        // Root signatures by the shape of the layout: programs with the same structure share one
        std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12RootSignature>> rootSignatures_;
        std::unordered_map<std::uint64_t, std::uint32_t> rootSignatureUsers_; // programs that use the root signature of a shape
        std::uint64_t shaderReloads_ = 0;
        std::uint64_t shaderReloadFailures_ = 0;
        ID3D12RootSignature* boundRootSignature_ = nullptr; // on the command list now (SetGraphicsRootSignature only when it changes)
        std::vector<std::uint8_t> constantScratch_;         // the bytes of one cbuffer while a draw is prepared
        bool forceRootCbv_ = false;                         // MYENGINE_DX12_ROOT_CBV=1: cbuffers as root CBVs, for checking that path
        ShaderRecord debugProgram_;                         // the programs that the renderer builds itself
        ShaderRecord uiProgram_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> textureSrvHeap_;
        UINT textureSrvDescriptorSize_ = 0;
        UINT nextTextureDescriptorIndex_ = 0;

        std::unordered_map<std::uint32_t, SurfaceData> surfaces_;
        std::uint32_t nextSurfaceId_ = 1;

        std::unordered_map<std::uint32_t, MeshRecord> meshes_;
        std::uint32_t nextMeshId_ = 1;

        std::unordered_map<std::uint32_t, TextureRecord> textures_;
        std::uint32_t nextTextureId_ = 1;
        std::vector<UINT> freeTextureDescriptors_;

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> targetRtvHeap_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> targetDsvHeap_;
        UINT targetRtvDescriptorSize_ = 0;
        UINT targetDsvDescriptorSize_ = 0;
        std::vector<UINT> freeTargetSlots_;
        std::unordered_map<std::uint32_t, RenderTargetRecord> renderTargets_;
        std::uint32_t nextRenderTargetId_ = 1;
        RenderTargetRecord* activeTarget_ = nullptr; // between BeginRenderTarget and EndRenderTarget
        std::vector<RetiredResource> retiredResources_;
        TextureHandle defaultWhiteTexture_{};

        std::unordered_map<std::uint32_t, ShaderRecord> shaders_;
        std::uint32_t nextShaderId_ = 1;

        SurfaceData* activeSurface_ = nullptr;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* activeFrameTransientResources_ = nullptr;
        HANDLE fenceEvent_ = nullptr;
        bool allowTearing_ = false;
        bool vsyncEnabled_ = true;
        bool wireframe_ = false;
        bool wireframeBeforeTarget_ = false; // the view mode is off while a render target is drawn (thumbnails)
        Float3 targetLightDirection_{0.35f, -1.0f, 0.25f}; // light of the meshes drawn into a render target
        float targetAmbient_ = 0.25f;
        Microsoft::WRL::ComPtr<ID3DBlob> wireframePixelShader_; // constant colour, shared by every wireframe PSO
        ShaderLayout wireframePixelLayout_;                      // its reflection (no resources)

        // scene passes
        RenderSettings settings_;
        SceneLighting lighting_;
        std::vector<PendingDraw> pendingDraws_;
        bool sceneOutputActive_ = false;       // the commands of the surface go to the HDR scene targets
        bool useSceneTargets_ = false;         // this frame uses them (MSAA or bloom on)
        core::Color sceneClearColor_{0.0f, 0.0f, 0.0f, 1.0f};
        SceneTargets sceneTargets_;
        ShaderRecord postProgram_;             // the fullscreen passes: bloom, composite
        Microsoft::WRL::ComPtr<ID3D12Resource> shadowMap_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shadowDsvHeap_;
        UINT shadowSrv_ = 0;
        std::uint32_t shadowMapSize_ = 0;
        bool shadowMapReady_ = false;          // a map was rendered this frame and holds depth
        std::string screenshotPath_;           // RequestScreenshot: the next presented frame is saved here
        std::uint64_t screenshotFrame_ = 1;    // ... from this frame on
        std::uint32_t frameDraws_ = 0;         // diagnostic counters of the last flush
        std::uint32_t frameInstanced_ = 0;

        std::unique_ptr<ShaderReflector> shaderReflector_;       // compiles the programs, keeps the cache
        std::vector<ShaderDiagnostic> lastShaderDiagnostics_;

        std::unique_ptr<Dx12GpuProfiler> profiler_;
        std::uint64_t frameCounter_ = 0; // frames begun so far; the first one is 1
        AutoGroup autoGroup_ = AutoGroup::None;
        std::string pendingTargetLabel_;
        std::string adapterName_;
        bool dredEnabled_ = false;
        bool debugLayerEnabled_ = false;
        LiveObjectReport liveObjectReport_;
        bool deviceRemovedLogged_ = false;
        std::uint64_t testRemoveDeviceFrame_ = 0; // MYENGINE_DX12_TEST_REMOVE_DEVICE=<frame>: removes the device on purpose
    };
}