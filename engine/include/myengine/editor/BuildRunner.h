// BuildRunner.h
// Runs a command line (the build script of a game, the check of a package) as a child process and hands its output to the editor line by line, without
// blocking it: the output of the process goes through a pipe to a thread that reads it, the editor takes the lines from a queue once a frame.
// Cancel stops the whole tree of processes (powershell, cmake, msbuild, the compilers): the process lives in a Job Object that is terminated.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace myengine::editor
{
    class BuildRunner
    {
    public:
        BuildRunner() = default;
        ~BuildRunner();
        BuildRunner(const BuildRunner&) = delete;
        BuildRunner& operator=(const BuildRunner&) = delete;

        // Starts the process (UTF-8 command line, a working directory). false: `error` says why (the editor shows it in the log). Not while one runs.
        bool Start(const std::string& commandLine, const std::filesystem::path& workingDirectory, std::string& error);
        // The lines that came since the last call (UTF-8, without the line break). Call it every frame.
        std::vector<std::string> TakeLines();
        // true once the process has ended and its last line was taken: `exitCode` is the code of the process. Gives the answer once per run.
        bool TakeFinished(int& exitCode);
        bool IsRunning() const { return running_.load(); }
        // Kills the process and everything it started. Returns at once; the run ends (TakeFinished) when the pipe is closed.
        void Cancel();

    private:
        void ReadLoop();
        void JoinReader();

        std::thread reader_;
        std::atomic<bool> running_{false};
        std::atomic<bool> finished_{false};   // the reader has read everything and has the exit code
        std::atomic<int> exitCode_{0};
        std::mutex mutex_;
        std::vector<std::string> lines_;
        bool reported_ = true;                // the finish of the last run was given out
        void* process_ = nullptr;             // HANDLE
        void* job_ = nullptr;                 // HANDLE of the Job Object
        void* pipeRead_ = nullptr;            // HANDLE
    };
}
