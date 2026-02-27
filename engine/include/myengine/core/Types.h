// Types.h

#pragma once

#include <cstdint>

namespace myengine::core
{
	using WindowId = std::uint32_t;

	enum class MouseButton : std::uint8_t
	{
		Left = 0,
		Right = 1,
		Middle = 2,
	};

	enum class InputEventType : std::uint8_t
	{
		KeyDown,
		KeyUp,
		MouseDown,
		MouseUp,
		WindowClose,
		WindowResize,
	};

	struct Color
	{
		float r = 0.1f;
		float g = 0.1f;
		float b = 0.1f;
		float a = 1.0f;
	};

	struct InputEvent
	{
		InputEventType type = InputEventType::KeyDown;
		WindowId windowId = 0;
		std::uint32_t key = 0;
		MouseButton mouseButton = MouseButton::Left;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};
}