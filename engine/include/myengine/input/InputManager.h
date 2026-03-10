// InputManager.h

#pragma once

#include <array>
#include <cstdint>
#include <utility>

#include <myengine/core/Types.h>

namespace myengine::input
{
	class InputManager
	{
	public:
		void OnKeyDown(std::uint32_t key);
		void OnKeyUp(std::uint32_t key);

		void OnMouseDown(core::MouseButton button);
		void OnMouseUp(core::MouseButton button);
		void OnMouseWheel(int delta);
		void OnMouseMove(int x, int y);
		void AddMouseDelta(int deltaX, int deltaY);
		void SetMousePositionReference(int x, int y);

		bool IsKeyDown(std::uint32_t key) const;
		bool IsMouseDown(core::MouseButton button) const;
		int ConsumeMouseWheelSteps();
		std::pair<int, int> ConsumeMouseDelta();
		void ResetMouseTracking();

	private:
		static constexpr int kMouseWheelDelta = 120;

		std::array<bool, 256> keys_{};
		std::array<bool, 3> mouseButtons_{};
		int mouseWheelAccumulated_ = 0;
		bool hasMousePosition_ = false;
		int lastMouseX_ = 0;
		int lastMouseY_ = 0;
		int mouseDeltaX_ = 0;
		int mouseDeltaY_ = 0;
	};
}
