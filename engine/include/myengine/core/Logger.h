// Logger.h

#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace myengine::core {
	enum class LogLevel
	{
		Debug,
		Info,
		Warning,
		Error,
	};

	class Logger
	{
	public:
		Logger() = default;
		~Logger();

		bool Initialize(const std::filesystem::path& path);

		void Debug(const std::string& message);
		void Info(const std::string& message);
		void Warning(const std::string& message);
		void Error(const std::string& message);

		void Log(LogLevel level, const std::string& message);

	private:
		std::string LevelToString(LogLevel level) const;

		std::ofstream file_;
		std::mutex mutex_;
	};
}