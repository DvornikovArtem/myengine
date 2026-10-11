// IRenderAdapter.h

#pragma once

#include <myengine/core/Types.h>
#include <myengine/render/RenderTypes.h>

// If the WIN32_LEAN_AND_MEAN macro is defined before including windows.h, rarely used parts are excluded from the header to speed up compilation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::render
{
    class IRenderAdapter
    {
    public:
        virtual ~IRenderAdapter() = default;

        virtual bool Initialize() = 0;

        virtual MeshHandle UploadMesh(const MeshData& meshData) = 0;
        virtual TextureHandle CreateTexture(const TextureData& textureData) = 0;
        virtual void DestroyTexture(TextureHandle texture) = 0;
        virtual ShaderHandle CreateShaderProgram(const ShaderProgramData& shaderProgram) = 0;

        virtual RenderSurfaceHandle CreateSurface(HWND hwnd, std::uint32_t width, std::uint32_t height) = 0;
        virtual void ResizeSurface(RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height) = 0;

        virtual bool BeginFrame(RenderSurfaceHandle surface, const core::Color& clearColor) = 0;
        virtual void SetRenderRegion(RenderSurfaceHandle surface, const IntRect* region) = 0;
        virtual void SetViewProjection(RenderSurfaceHandle surface, const Matrix4& view, const Matrix4& projection) = 0;
        virtual void Draw(RenderSurfaceHandle surface, const DrawItem& drawItem) = 0;
        virtual void DrawDebugLines(RenderSurfaceHandle surface, const std::vector<DebugLine>& lines) = 0;
        virtual void DrawUiGeometry(RenderSurfaceHandle surface, const UiDrawData& drawData) = 0;
        virtual void EndFrame(RenderSurfaceHandle surface) = 0;

        // Present with vertical sync (true) or immediately (false). Safe to call between frames
        virtual void SetVSync(bool enabled) { (void)enabled; }
        virtual bool IsVSyncEnabled() const { return false; }
        // View mode: draw the meshes of Draw() as wireframe (debug lines and the UI are not affected)
        virtual void SetWireframe(bool enabled) { (void)enabled; }

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

        virtual void Shutdown() = 0;
    };
}