// FileWatcher.h

#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <myengine/jobs/JobSystem.h>

namespace myengine::core
{
    struct FileChange
    {
        std::filesystem::path path;
        bool removed = false;
        std::string contents; // the whole file, read in the job (empty if removed)
    };

    // Watches files by modification time. The scan (exists + last_write_time) and the reading of changed files run
    // as a job in the Streaming pool, so the frame does not wait for the disk. Callbacks run on the main thread in Poll.
    // The first scan only remembers the state, so nothing is reported at startup.
    class FileWatcher
    {
    public:
        using Callback = std::function<void(const FileChange&)>;

        explicit FileWatcher(std::chrono::milliseconds interval = std::chrono::milliseconds(500));
        ~FileWatcher();

        FileWatcher(const FileWatcher&) = delete;
        FileWatcher& operator=(const FileWatcher&) = delete;

        void Watch(const std::filesystem::path& file, Callback onChanged);
        // Every file with this extension inside the directory and its subdirectories, including files created later
        void WatchDirectory(const std::filesystem::path& directory, const std::string& extension, Callback onChanged);

        // Main thread, every frame: delivers the finished scan, starts the next one when the interval has passed
        void Poll();
        // The next scan reports every watched file as changed (explicit "reload all")
        void RequestFullRescan();
        // Waits for the running scan. Call before the job system stops
        void Stop();

    private:
        struct FileState
        {
            bool exists = false;
            std::filesystem::file_time_type writeTime{};
        };

        struct WatchEntry
        {
            std::filesystem::path path; // a file or a directory
            std::string extension; // empty for a single file
            Callback onChanged;
        };

        // Everything the job touches. The main thread reads or writes it only while no job is running
        struct ScanState
        {
            std::vector<std::pair<std::filesystem::path, std::string>> targets; // copy of the watches: path + extension
            std::map<std::filesystem::path, FileState> known;
            std::vector<FileChange> changes;
            bool baselineDone = false;
            bool reportAll = false;
        };

        static void Scan(ScanState& state);
        void Deliver(const FileChange& change);

        std::chrono::milliseconds interval_;
        std::chrono::steady_clock::time_point lastScanStart_{};
        std::vector<WatchEntry> watches_;
        bool scanRunning_ = false;
        bool fullRescanRequested_ = false;

        jobs::Context context_;
        ScanState scan_;
    };
}