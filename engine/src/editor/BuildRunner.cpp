// BuildRunner.cpp

#include <myengine/editor/BuildRunner.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#include <string_view>
#include <vector>

namespace myengine::editor
{
    namespace
    {
        std::wstring ToWide(const std::string& utf8)
        {
            if (utf8.empty())
            {
                return {};
            }
            const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
            return wide;
        }

        // The console programs write in the OEM code page of the console (cmake, MSBuild, the compilers: in the language of the system), PowerShell passes its
        // own output through in the same page: the lines are read as OEM and given on as UTF-8.
        std::string OemToUtf8(const std::string& text)
        {
            if (text.empty())
            {
                return {};
            }
            const int wideSize = MultiByteToWideChar(CP_OEMCP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (wideSize <= 0)
            {
                return text;
            }
            std::wstring wide(static_cast<std::size_t>(wideSize), L'\0');
            MultiByteToWideChar(CP_OEMCP, 0, text.data(), static_cast<int>(text.size()), wide.data(), wideSize);
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, utf8.data(), size, nullptr, nullptr);
            return utf8;
        }

        // The environment of the child: the one of the editor without the script that drives the editor (MYENGINE_UI_SCRIPT), or the game that the checks of
        // a package start would run that script, and write its path into the log of the package
        std::wstring ChildEnvironment()
        {
            std::wstring block;
            LPWCH strings = GetEnvironmentStringsW();
            if (strings == nullptr)
            {
                return block;
            }
            for (const wchar_t* entry = strings; *entry != L'\0'; entry += wcslen(entry) + 1)
            {
                const std::wstring_view text(entry);
                if (text.rfind(L"MYENGINE_UI_SCRIPT=", 0) == 0)
                {
                    continue;
                }
                block.append(text);
                block.push_back(L'\0');
            }
            FreeEnvironmentStringsW(strings);
            block.push_back(L'\0');
            return block;
        }

        std::string LastError(const char* what)
        {
            const DWORD code = GetLastError();
            return std::string(what) + " (Windows error " + std::to_string(code) + ")";
        }
    }

    BuildRunner::~BuildRunner()
    {
        Cancel();
        JoinReader();
    }

    void BuildRunner::JoinReader()
    {
        if (reader_.joinable())
        {
            reader_.join();
        }
        if (pipeRead_ != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(pipeRead_));
            pipeRead_ = nullptr;
        }
        if (process_ != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(process_));
            process_ = nullptr;
        }
        if (job_ != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(job_));
            job_ = nullptr;
        }
    }

    bool BuildRunner::Start(const std::string& commandLine, const std::filesystem::path& workingDirectory, std::string& error)
    {
        if (running_.load())
        {
            error = "A run is going already";
            return false;
        }
        JoinReader();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.clear();
        }
        finished_ = false;
        exitCode_ = 0;
        reported_ = false;

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &security, 0))
        {
            error = LastError("Cannot create the pipe for the output of the process");
            reported_ = true;
            return false;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);   // the child must not hold the read end

        HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        startup.hStdInput = nullInput != INVALID_HANDLE_VALUE ? nullInput : nullptr;

        std::wstring command = ToWide(commandLine);
        command.push_back(L'\0');
        const std::wstring directory = workingDirectory.wstring();
        std::wstring environment = ChildEnvironment();
        PROCESS_INFORMATION info{};
        // suspended: it goes into the Job Object before it runs, so nothing it starts is outside
        const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                            environment.empty() ? nullptr : environment.data(), directory.empty() ? nullptr : directory.c_str(), &startup, &info);
        const std::string createError = created ? std::string() : LastError("Cannot start the process");
        CloseHandle(writeEnd);   // the child has its copy: the reader sees the end of the pipe when the whole tree is gone
        if (nullInput != INVALID_HANDLE_VALUE && nullInput != nullptr)
        {
            CloseHandle(nullInput);
        }
        if (!created)
        {
            CloseHandle(readEnd);
            error = createError;
            reported_ = true;
            return false;
        }

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;   // the editor goes away: the build goes with it
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            AssignProcessToJobObject(job, info.hProcess);
        }
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);

        process_ = info.hProcess;
        job_ = job;
        pipeRead_ = readEnd;
        running_ = true;
        reader_ = std::thread([this] { ReadLoop(); });
        return true;
    }

    void BuildRunner::ReadLoop()
    {
        std::string pending;
        char buffer[4096];
        const auto push = [this](std::string text)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.push_back(OemToUtf8(text));
        };
        for (;;)
        {
            DWORD read = 0;
            if (!ReadFile(static_cast<HANDLE>(pipeRead_), buffer, sizeof(buffer), &read, nullptr) || read == 0)
            {
                break;   // the pipe is closed: every process of the tree has ended (or was killed)
            }
            pending.append(buffer, read);
            std::size_t start = 0;
            for (std::size_t at = pending.find('\n', start); at != std::string::npos; at = pending.find('\n', start))
            {
                std::string line = pending.substr(start, at - start);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                push(std::move(line));
                start = at + 1;
            }
            pending.erase(0, start);
        }
        if (!pending.empty())
        {
            push(std::move(pending));
        }
        DWORD code = 1;
        WaitForSingleObject(static_cast<HANDLE>(process_), 5000);
        GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
        exitCode_ = static_cast<int>(code);
        finished_ = true;
        running_ = false;
    }

    std::vector<std::string> BuildRunner::TakeLines()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> taken;
        taken.swap(lines_);
        return taken;
    }

    bool BuildRunner::TakeFinished(int& exitCode)
    {
        if (reported_ || !finished_.load())
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!lines_.empty())
            {
                return false;   // the lines go first: the window reads all of them, then the end
            }
        }
        exitCode = exitCode_.load();
        reported_ = true;
        return true;
    }

    void BuildRunner::Cancel()
    {
        if (!running_.load() || job_ == nullptr)
        {
            return;
        }
        TerminateJobObject(static_cast<HANDLE>(job_), 1);
    }
}
