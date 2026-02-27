// InputManager.h

#pragma once

#include <array>
#include <cstdint>

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

		bool IsKeyDown(std::uint32_t key) const;
		bool IsMouseDown(core::MouseButton button) const;

	private:
		std::array<bool, 256> keys_{};
		std::array<bool, 3> mouseButtons_{};
	};
}