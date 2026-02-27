// AppConfig.h

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <myengine/core/Types.h>

namespace myengine::core
{
	class Logger;
}

namespace myengine::config
{
	struct WindowConfig
	{
		std::string title = "myengine";
		std::uint32_t width = 1280;
		std::uint32_t height = 720;
		core::Color clearColor{0.1f, 0.1f, 0.2f, 1.0f};
	};

	struct InputConfig
	{
		float moveSpeed = 0.8f;
		float scaleSpeed = 0.8f;
		float rotateSpeedDeg = 120.0f;
	};

	struct AppConfig
	{
		std::vector<WindowConfig> windows;
		InputConfig input;

		static AppConfig Default();
		static AppConfig LoadFromFile(const std::filesystem::path& path, core::Logger* logger = nullptr);
	};
}