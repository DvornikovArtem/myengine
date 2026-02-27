// Logger.cpp

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include <myengine/core/Logger.h>

// If the WIN32_LEAN_AND_MEAN macro is defined before including windows.h, rarely used parts are excluded from the header to speed up compilation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::core
{
    namespace
    {
        std::string MakeTimestamp()
        {
            const auto now = std::chrono::system_clock::now();
            const std::time_t currentTime = std::chrono::system_clock::to_time_t(now);

            std::tm localTime{};
            localtime_s(&localTime, &currentTime);

            std::ostringstream stream;
            stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
            return stream.str();
        }
    }

    Logger::~Logger()
    {
        std::scoped_lock lock(mutex_);
        if (file_.is_open())
        {
            file_.flush();
            file_.close();
        }
    }

    bool Logger::Initialize(const std::filesystem::path& path)
    {
        std::scoped_lock lock(mutex_);

        const auto parent = path.parent_path();
        if (!parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }

        file_.open(path, std::ios::out | std::ios::trunc);
        if (!file_.is_open())
        {
            return false;
        }

        const std::string line = "[" + MakeTimestamp() + "] [INFO] Logger initialized";
        file_ << line << '\n';
        file_.flush();
        const std::string debugLine = line + "\n";
        OutputDebugStringA(debugLine.c_str());
        return true;
    }

    void Logger::Debug(const std::string& message)
    {
        Log(LogLevel::Debug, message);
    }

    void Logger::Info(const std::string& message)
    {
        Log(LogLevel::Info, message);
    }

    void Logger::Warning(const std::string& message)
    {
        Log(LogLevel::Warning, message);
    }

    void Logger::Error(const std::string& message)
    {
        Log(LogLevel::Error, message);
    }

    void Logger::Log(LogLevel level, const std::string& message)
    {
        std::scoped_lock lock(mutex_);

        const std::string line = "[" + MakeTimestamp() + "] [" + LevelToString(level) + "] " + message;

        if (file_.is_open())
        {
            file_ << line << '\n';
            file_.flush();
        }

        const std::string debugLine = line + "\n";
        OutputDebugStringA(debugLine.c_str());
    }

    std::string Logger::LevelToString(LogLevel level) const
    {
        switch (level)
        {
            case LogLevel::Debug:
                return "DEBUG";
            case LogLevel::Info:
                return "INFO";
            case LogLevel::Warning:
                return "WARN";
            case LogLevel::Error:
                return "ERROR";
            default:
                return "UNKNOWN";
        }
    }
}