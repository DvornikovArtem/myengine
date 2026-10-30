// IRenderAdapter.h

#pragma once

#include <string>

#include <myengine/core/Types.h>
#include <myengine/render/GpuStats.h>
#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderLayout.h>

// If the WIN32_LEAN_AND_MEAN macro is defined before including windows.h, rarely used parts are excluded from the header to speed up compilation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::render
{
    // One shader program as the renderer knows it now (the Shader Layouts window)
    struct ShaderProgramInfo
    {
        ShaderHandle handle;
        std::string name;               // the file of the program
        std::uint32_t version = 1;      // 1 at the creation, +1 for every successful reload
        std::string lastError;          // the first error of the last reload that failed; empty after a good one
    };

    // How many GPU objects the shaders hold: they must not grow with the number of reloads
    struct ShaderStatistics
    {
        std::uint32_t programs = 0;          // live programs, the renderer's own (debug lines, UI) included
        std::uint32_t pipelineStates = 0;    // live PSOs (a scene program has a normal and a wireframe one)
        std::uint32_t rootSignatures = 0;    // root signatures in the cache
        std::uint32_t retiredObjects = 0;    // replaced PSOs / root signatures that wait for the fence
        std::uint64_t reloads = 0;           // successful reloads
        std::uint64_t reloadFailures = 0;    // reloads that left the old program
    };

    // The quality of the scene passes of a surface (not of render targets: the previews and thumbnails keep drawing directly).
    // Changeable between frames; the menu of the game sets it. docs/render/scene-passes.md.
    struct RenderSettings
    {
        std::uint8_t shadowQuality = 2;   // 0 off, 1 low (1024 map, one tap), 2 high (2048 map, PCF 3x3)
        std::uint8_t msaaSamples = 4;     // 1 (off) or 4
        bool bloom = true;                // HDR scene target with a bloom of the emissive / over-bright parts
        bool instancing = true;           // draws of one mesh and material become one instanced draw
        float bloomThreshold = 1.0f;      // brightness above which a pixel glows
        float bloomIntensity = 0.55f;     // how much of the blurred glow is added
    };

    // The light of the scene surface (the shaders get it as the engine variables gLightDirection, gLightColor, gAmbientSky,
    // gAmbientGround). The defaults are the old fixed light: direction (0.35, -1, 0.25), 25 % ambient.
    struct SceneLighting
    {
        Float3 direction{0.35f, -1.0f, 0.25f};  // the way the light travels
        Float3 color{0.75f, 0.75f, 0.75f};      // directional light, already multiplied by its intensity
        Float3 skyAmbient{0.30f, 0.30f, 0.32f};   // hemisphere ambient: the colour of a surface that looks up
        Float3 groundAmbient{0.16f, 0.16f, 0.15f}; // ... and of one that looks down
    };

    class IRenderAdapter
    {
    public:
        virtual ~IRenderAdapter() = default;

        virtual bool Initialize() = 0;

        virtual MeshHandle UploadMesh(const MeshData& meshData) = 0;
        virtual TextureHandle CreateTexture(const TextureData& textureData) = 0;
        virtual void DestroyTexture(TextureHandle texture) = 0;
        // Replaces the pixels of a texture (an image that changes every frame: the screen of DOOM) under the same handle. The size may change.
        // Synchronous like CreateTexture: call it between frames. false: no such texture, or the data is not valid.
        virtual bool UpdateTexture(TextureHandle texture, const TextureData& textureData)
        {
            (void)texture;
            (void)textureData;
            return false;
        }
        virtual ShaderHandle CreateShaderProgram(const ShaderProgramData& shaderProgram) = 0;

        // ---- shader reflection ----
        // What the program expects (resources, constant buffers with offsets, vertex inputs), read from the compiled
        // shaders. false: no such program, or the adapter does not reflect.
        virtual bool GetShaderLayout(ShaderHandle shader, ShaderLayout& outLayout, ShaderVariant variant = ShaderVariant::Normal) const
        {
            (void)shader;
            (void)variant;
            outLayout = {};
            return false;
        }
        // The same for the programs that the renderer builds for itself (debug lines, the UI)
        virtual bool GetBuiltinShaderLayout(BuiltinShader shader, ShaderLayout& outLayout) const
        {
            (void)shader;
            outLayout = {};
            return false;
        }
        // The errors and warnings (file, line, column) of the last CreateShaderProgram / ReloadShaderProgram call; empty when
        // it succeeded without messages
        virtual std::vector<ShaderDiagnostic> GetLastShaderDiagnostics() const { return {}; }

        // ---- shader reload ----
        // Frees the program. Its pipeline states stay until the GPU has finished the frames that may use them.
        virtual void DestroyShader(ShaderHandle shader) { (void)shader; }
        // Builds a new version of the program beside the old one and replaces it under the same handle only when all of it
        // succeeded (compile, reflection, root signature, input layout, pipeline states). false: the old program keeps
        // running as it is, GetLastShaderDiagnostics says why.
        virtual bool ReloadShaderProgram(ShaderHandle shader, const ShaderProgramData& shaderProgram)
        {
            (void)shader;
            (void)shaderProgram;
            return false;
        }
        // 1 after the creation, +1 for every successful reload (materials re-check themselves against a new layout); 0: no such program
        virtual std::uint32_t GetShaderVersion(ShaderHandle shader) const
        {
            (void)shader;
            return 0;
        }
        virtual std::vector<ShaderProgramInfo> GetShaderPrograms() const { return {}; }
        virtual ShaderStatistics GetShaderStatistics() const { return {}; }

        virtual RenderSurfaceHandle CreateSurface(HWND hwnd, std::uint32_t width, std::uint32_t height) = 0;
        virtual void ResizeSurface(RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height) = 0;

        virtual bool BeginFrame(RenderSurfaceHandle surface, const core::Color& clearColor) = 0;
        virtual void SetRenderRegion(RenderSurfaceHandle surface, const IntRect* region) = 0;
        virtual void SetViewProjection(RenderSurfaceHandle surface, const Matrix4& view, const Matrix4& projection) = 0;
        virtual void Draw(RenderSurfaceHandle surface, const DrawItem& drawItem) = 0;
        // A batch of instances of one mesh with one material, one draw (particles): instanceData has 16 floats per instance, what
        // the instance stream of the program reads as INSTANCE0..3 (a model matrix for the lit shader, the particle data for
        // particle.hlsl). Only for programs that read the instance stream; only on the surface of the frame (not in render targets).
        // drawItem.model is not used; its bounds (centre, radius) sort a transparent batch among the other transparent draws.
        virtual void DrawInstances(RenderSurfaceHandle surface, const DrawItem& drawItem, const float* instanceData, std::uint32_t instanceCount)
        {
            (void)surface;
            (void)drawItem;
            (void)instanceData;
            (void)instanceCount;
        }
        virtual void DrawDebugLines(RenderSurfaceHandle surface, const std::vector<DebugLine>& lines) = 0;
        virtual void DrawUiGeometry(RenderSurfaceHandle surface, const UiDrawData& drawData) = 0;
        virtual void EndFrame(RenderSurfaceHandle surface) = 0;

        // Present with vertical sync (true) or immediately (false). Safe to call between frames
        virtual void SetVSync(bool enabled) { (void)enabled; }
        virtual bool IsVSyncEnabled() const { return false; }
        // View mode: draw the meshes of Draw() as wireframe (debug lines and the UI are not affected)
        virtual void SetWireframe(bool enabled) { (void)enabled; }
        // Light of the meshes drawn into a render target (previews): the way the light travels in world space and
        // the share of the colour that the dark side keeps (0..1). Applies until the next call; the lighting of
        // the scene surface is not affected.
        virtual void SetTargetLighting(const Float3& direction, float ambient)
        {
            (void)direction;
            (void)ambient;
        }
        // Settings and light of the scene surface (shadows, MSAA, bloom, instancing; the sun and the ambient)
        virtual void SetRenderSettings(const RenderSettings& settings) { (void)settings; }
        virtual RenderSettings GetRenderSettings() const { return {}; }
        virtual void SetSceneLighting(const SceneLighting& lighting) { (void)lighting; }
        // Saves the next presented frame of the surface as a PNG (diagnostics: --screenshot). false: not available.
        virtual bool RequestScreenshot(const std::string& path) { (void)path; return false; }

        // ---- render to texture ----
        // An RGBA8 colour target with its own depth. Create and destroy it between frames, never between
        // BeginFrame and EndFrame. 0 when the adapter has no offscreen support or the limit is reached.
        virtual RenderTargetHandle CreateRenderTarget(std::uint32_t width, std::uint32_t height)
        {
            (void)width;
            (void)height;
            return {};
        }
        virtual void DestroyRenderTarget(RenderTargetHandle target) { (void)target; }
        // The texture of the target, to draw with ImGui::Image / DrawItem::texture. Its content is undefined
        // until the target has been rendered once; the texture handle stays the same for the target's lifetime.
        virtual TextureHandle GetRenderTargetTexture(RenderTargetHandle target) const
        {
            (void)target;
            return {};
        }
        // Inside a frame (after BeginFrame, before EndFrame), on the main thread. Between BeginRenderTarget and
        // EndRenderTarget, SetViewProjection / Draw / DrawDebugLines of the frame's surface go to the target
        // (its own viewport, view-projection and depth); SetRenderRegion and DrawUiGeometry are ignored.
        // EndRenderTarget makes the texture readable again and restores the surface as the output.
        virtual bool BeginRenderTarget(RenderTargetHandle target, const core::Color& clearColor)
        {
            (void)target;
            (void)clearColor;
            return false;
        }
        virtual void EndRenderTarget(RenderTargetHandle target) { (void)target; }
        // Copies the target to the CPU as RGBA8 rows (width * height * 4 bytes). Synchronous: it waits for the
        // GPU, so call it between frames and rarely (the thumbnail disk cache).
        virtual bool ReadRenderTargetPixels(RenderTargetHandle target, std::vector<std::uint8_t>& outRgba8)
        {
            (void)target;
            outRgba8.clear();
            return false;
        }
        virtual bool GetRenderTargetSize(RenderTargetHandle target, std::uint32_t& outWidth, std::uint32_t& outHeight) const
        {
            (void)target;
            outWidth = 0;
            outHeight = 0;
            return false;
        }

        // ---- point clouds ----
        // A cloud of glowing points (the 3D view of the fly brain): the positions go to the GPU now (between frames, like UploadMesh), the flashes with every
        // DrawPointCloud. Drawn additively with HDR, a bloom and a tone map into the render target that is being rendered; 0: no support.
        virtual PointCloudHandle CreatePointCloud(const PointCloudData& data)
        {
            (void)data;
            return {};
        }
        virtual void DestroyPointCloud(PointCloudHandle cloud) { (void)cloud; }
        // Inside BeginRenderTarget / EndRenderTarget of the frame's surface. The target is overwritten (the background is in `params`).
        virtual bool DrawPointCloud(RenderSurfaceHandle surface, PointCloudHandle cloud, const PointCloudDrawParams& params)
        {
            (void)surface;
            (void)cloud;
            (void)params;
            return false;
        }

        // ---- GPU diagnostics (capture tools, statistics) ----
        // A named group of commands: a marker that RenderDoc / PIX show as a tree node, and a timed pass of the frame
        // statistics. Inside a frame only; groups nest, EndDebugEvent closes the innermost.
        virtual void BeginDebugEvent(const std::string& name) { (void)name; }
        virtual void EndDebugEvent() {}
        // The name of the next BeginRenderTarget pass in the capture ("Thumbnail: coin.prefab")
        virtual void SetRenderTargetLabel(const std::string& label) { (void)label; }
        // What the last finished frame cost (draws, triangles, state switches, GPU time per pass). waitForGpu waits for
        // the frame in flight first: a diagnostics call between frames, not something for every frame.
        virtual bool GetLastFrameStats(GpuFrameStats& out, bool waitForGpu)
        {
            (void)waitForGpu;
            out = {};
            return false;
        }
        virtual std::string GetAdapterName() const { return {}; }
        // The GPU device was removed (a hang, a page fault, a driver reset): nothing is drawn any more
        virtual bool IsDeviceRemoved() const { return false; }

        virtual void Shutdown() = 0;
    };
}