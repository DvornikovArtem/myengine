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

		virtual RenderSurfaceHandle CreateSurface(HWND hwnd, std::uint32_t width, std::uint32_t height) = 0;
		virtual void ResizeSurface(RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height) = 0;

		virtual bool BeginFrame(RenderSurfaceHandle surface, const core::Color& clearColor) = 0;
		virtual void DrawPrimitive(RenderSurfaceHandle surface, const Transform2D& transform) = 0;
		virtual void EndFrame(RenderSurfaceHandle surface) = 0;

		virtual void Shutdown() = 0;
	};
}