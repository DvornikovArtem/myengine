// InputManager.h

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <myengine/core/Types.h>

namespace myengine::input
{
	class InputManager
	{
	public:
		void BeginFrame();
		void SetActiveWindow(core::WindowId windowId);

		void OnKeyDown(std::uint32_t key);
		void OnKeyUp(std::uint32_t key);

		void OnMouseDown(core::MouseButton button);
		void OnMouseUp(core::MouseButton button);
		void OnMouseWheel(int delta);
		void OnMouseMove(int x, int y);
		void AddMouseDelta(int deltaX, int deltaY);
		void SetMousePositionReference(int x, int y);

		bool IsKeyDown(std::uint32_t key) const;
		bool WasKeyPressed(std::uint32_t key) const;
		bool WasKeyReleased(std::uint32_t key) const;
		bool IsMouseDown(core::MouseButton button) const;
		bool WasMousePressed(core::MouseButton button) const;
		bool WasMouseReleased(core::MouseButton button) const;
		int ConsumeMouseWheelSteps();
		std::pair<int, int> ConsumeMouseDelta();
		void ResetMouseTracking();

		void BindAction(std::string action, std::uint32_t key);
		void BindMouseAction(std::string action, core::MouseButton button);
		bool IsActionDown(std::string_view action) const;
		bool WasActionPressed(std::string_view action) const;
		bool WasActionReleased(std::string_view action) const;

		core::WindowId GetActiveWindowId() const;

	private:
		enum class BindingDevice : std::uint8_t
		{
			Key = 0,
			MouseButton = 1,
		};

		struct ActionBinding
		{
			BindingDevice device = BindingDevice::Key;
			std::uint32_t code = 0;
		};

		bool MatchesBindingPressed(const ActionBinding& binding) const;
		bool MatchesBindingReleased(const ActionBinding& binding) const;
		bool MatchesBindingDown(const ActionBinding& binding) const;

		static constexpr int kMouseWheelDelta = 120;

		std::array<bool, 256> keys_{};
		std::array<bool, 256> keyPressed_{};
		std::array<bool, 256> keyReleased_{};
		std::array<bool, 3> mouseButtons_{};
		std::array<bool, 3> mouseButtonsPressed_{};
		std::array<bool, 3> mouseButtonsReleased_{};
		int mouseWheelAccumulated_ = 0;
		bool hasMousePosition_ = false;
		int lastMouseX_ = 0;
		int lastMouseY_ = 0;
		int mouseDeltaX_ = 0;
		int mouseDeltaY_ = 0;
		core::WindowId activeWindowId_ = 0;
		std::unordered_map<std::string, std::vector<ActionBinding>> actionBindings_;
	};
}