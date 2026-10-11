// CliProcess.cpp

#include "CliProcess.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

#include <windows.h>

namespace myengine::assistant::detail
{
    namespace
    {
        constexpr DWORD kPipeBufferBytes = 1024 * 1024;

        bool HasExtension(const std::filesystem::path& path, const wchar_t* extension)
        {
            std::wstring value = path.extension().wstring();
            std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return value == extension;
        }

        std::size_t Drain(void* pipe, std::string& buffer, const std::size_t budget)
        {
            std::size_t total = 0;
            if (pipe == nullptr)
            {
                return 0;
            }
            char chunk[16384];
            while (total < budget)
            {
                DWORD available = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
                {
                    break;
                }
                const auto wanted = std::min<std::size_t>({static_cast<std::size_t>(available), sizeof(chunk), budget - total});
                DWORD read = 0;
                if (!ReadFile(pipe, chunk, static_cast<DWORD>(wanted), &read, nullptr) || read == 0)
                {
                    break;
                }
                buffer.append(chunk, read);
                total += read;
            }
            return total;
        }
    }

    std::wstring QuoteArgument(const std::wstring& argument)
    {
        // The rules of CommandLineToArgvW: backslashes only matter in front of a quote
        std::wstring quoted = L"\"";
        std::size_t backslashes = 0;
        for (const wchar_t c : argument)
        {
            if (c == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (c == L'"')
            {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted += L'"';
            }
            else
            {
                quoted.append(backslashes, L'\\');
                quoted += c;
            }
            backslashes = 0;
        }
        quoted.append(backslashes * 2, L'\\'); // in front of the closing quote
        quoted += L'"';
        return quoted;
    }

    std::wstring GetEnvironmentText(const wchar_t* name)
    {
        std::wstring value(1024, L'\0');
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
            if (length == 0)
            {
                return {};
            }
            if (length < value.size())
            {
                value.resize(length);
                return value;
            }
            value.assign(static_cast<std::size_t>(length) + 1, L'\0');
        }
        return {};
    }

    bool IsRegularFile(const std::filesystem::path& path)
    {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error);
    }

    std::string SanitizeTail(std::string text)
    {
        for (auto& c : text)
        {
            if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t')
            {
                c = ' ';
            }
        }
        const auto first = text.find_first_not_of(" \r\n\t");
        if (first == std::string::npos)
        {
            return {};
        }
        return text.substr(first);
    }

    void KeepTail(std::string& text, const std::size_t limit)
    {
        if (text.size() > limit)
        {
            text.erase(0, text.size() - limit);
        }
    }

    CliProcess::~CliProcess()
    {
        Close(true);
    }

    bool CliProcess::Start(const std::filesystem::path& executable, std::wstring commandLine,
                           const std::filesystem::path& workingDirectory, std::string& error)
    {
        Close(true);

        const bool viaShell = HasExtension(executable, L".cmd") || HasExtension(executable, L".bat");
        if (viaShell)
        {
            // npm installs a .cmd shim, which only cmd.exe can run
            auto shell = GetEnvironmentText(L"ComSpec");
            if (shell.empty())
            {
                shell = L"cmd.exe";
            }
            commandLine = L"\"" + shell + L"\" /d /s /c \"" + commandLine + L"\"";
        }

        SECURITY_ATTRIBUTES inheritable{};
        inheritable.nLength = sizeof(inheritable);
        inheritable.bInheritHandle = TRUE;

        HANDLE stdinRead = nullptr;
        HANDLE stdinWrite = nullptr;
        HANDLE stdoutRead = nullptr;
        HANDLE stdoutWrite = nullptr;
        HANDLE stderrRead = nullptr;
        HANDLE stderrWrite = nullptr;
        const auto closeAll = [&]()
        {
            for (HANDLE* handle : {&stdinRead, &stdinWrite, &stdoutRead, &stdoutWrite, &stderrRead, &stderrWrite})
            {
                if (*handle != nullptr)
                {
                    CloseHandle(*handle);
                    *handle = nullptr;
                }
            }
        };

        if (!CreatePipe(&stdinRead, &stdinWrite, &inheritable, kPipeBufferBytes) ||
            !CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, kPipeBufferBytes) ||
            !CreatePipe(&stderrRead, &stderrWrite, &inheritable, 64 * 1024))
        {
            error = "Could not create pipes for the CLI process.";
            closeAll();
            return false;
        }
        // The parent's ends must not leak into the child
        SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdinRead;
        startup.hStdOutput = stdoutWrite;
        startup.hStdError = stderrWrite;

        PROCESS_INFORMATION information{};
        const std::wstring directory = workingDirectory.wstring();
        const std::wstring application = executable.wstring();
        const BOOL created = CreateProcessW(
            viaShell ? nullptr : application.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr,
            directory.empty() ? nullptr : directory.c_str(),
            &startup,
            &information);
        if (!created)
        {
            error = "Could not start " + executable.u8string() + " (error " + std::to_string(GetLastError()) + ").";
            closeAll();
            return false;
        }

        process_ = information.hProcess;
        // The job lets Terminate kill the whole tree (the CLIs start helpers) and ends the child if the editor dies
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_ != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            if (!AssignProcessToJobObject(job_, information.hProcess))
            {
                CloseHandle(job_);
                job_ = nullptr;
            }
        }
        ResumeThread(information.hThread);
        CloseHandle(information.hThread);

        CloseHandle(stdinRead);
        CloseHandle(stdoutWrite);
        CloseHandle(stderrWrite);
        stdin_ = stdinWrite;
        stdout_ = stdoutRead;
        stderr_ = stderrRead;
        return true;
    }

    bool CliProcess::WriteInputAndClose(const std::string& bytes)
    {
        if (stdin_ == nullptr)
        {
            return false;
        }
        // The pipe buffer is 1 MB and a prompt is limited well below it, so this does not wait for the child
        std::size_t written = 0;
        bool failed = false;
        while (written < bytes.size())
        {
            DWORD count = 0;
            if (!WriteFile(stdin_, bytes.data() + written, static_cast<DWORD>(bytes.size() - written), &count, nullptr) || count == 0)
            {
                failed = true;
                break;
            }
            written += count;
        }
        CloseHandle(stdin_); // end of input: the CLI answers this one message and exits
        stdin_ = nullptr;
        return !failed;
    }

    std::size_t CliProcess::ReadOutput(std::string& buffer, const std::size_t budget)
    {
        return Drain(stdout_, buffer, budget);
    }

    std::size_t CliProcess::ReadError(std::string& buffer, const std::size_t budget)
    {
        return Drain(stderr_, buffer, budget);
    }

    bool CliProcess::IsRunning() const
    {
        return process_ != nullptr && WaitForSingleObject(process_, 0) != WAIT_OBJECT_0;
    }

    unsigned long CliProcess::ExitCode() const
    {
        DWORD code = 0;
        if (process_ != nullptr)
        {
            GetExitCodeProcess(process_, &code);
        }
        return code;
    }

    void CliProcess::Terminate()
    {
        if (job_ != nullptr)
        {
            TerminateJobObject(job_, 1);
        }
        else if (process_ != nullptr)
        {
            TerminateProcess(process_, 1);
        }
    }

    void CliProcess::Close(const bool terminate)
    {
        if (terminate && process_ != nullptr)
        {
            Terminate();
        }
        for (void** handle : {&process_, &job_, &stdin_, &stdout_, &stderr_})
        {
            if (*handle != nullptr)
            {
                CloseHandle(*handle);
                *handle = nullptr;
            }
        }
    }
}
