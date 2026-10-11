// CliProcess.h
//
// What the CLI backends (Claude Code, Codex) share: a child process with its three pipes, started without a window,
// killed with its whole tree, read without blocking. Internal to the assistant module.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace myengine::assistant::detail
{
    // One argument of a Windows command line, quoted the way the CRT of the child parses it
    std::wstring QuoteArgument(const std::wstring& argument);
    std::wstring GetEnvironmentText(const wchar_t* name);
    bool IsRegularFile(const std::filesystem::path& path);
    // The text of a pipe as one readable piece: control characters become spaces, leading blanks are cut
    std::string SanitizeTail(std::string text);
    // Keeps the last `limit` bytes of `text`
    void KeepTail(std::string& text, std::size_t limit);

    class CliProcess
    {
    public:
        CliProcess() = default;
        ~CliProcess();
        CliProcess(const CliProcess&) = delete;
        CliProcess& operator=(const CliProcess&) = delete;

        // `commandLine` is the whole line, the quoted executable included. A .cmd / .bat shim (npm installs these) is
        // started through cmd.exe. The child runs in `workingDirectory` (empty: the editor's own)
        bool Start(const std::filesystem::path& executable, std::wstring commandLine,
                   const std::filesystem::path& workingDirectory, std::string& error);

        // Writes everything to the child's stdin and closes it. False when the child went away
        bool WriteInputAndClose(const std::string& bytes);

        // Appends what is available in the pipe to `buffer`, at most `budget` bytes. Never blocks
        std::size_t ReadOutput(std::string& buffer, std::size_t budget);
        std::size_t ReadError(std::string& buffer, std::size_t budget);

        bool IsRunning() const; // started and not exited
        bool HasStarted() const { return process_ != nullptr; }
        unsigned long ExitCode() const;

        // Kills the child and its helpers (a no-op after it exited)
        void Terminate();
        // Releases the handles; the child is killed first when `terminate`
        void Close(bool terminate);

    private:
        void* process_ = nullptr;
        void* job_ = nullptr;
        void* stdin_ = nullptr;
        void* stdout_ = nullptr;
        void* stderr_ = nullptr;
    };
}
