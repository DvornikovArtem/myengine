// FileWatcher.cpp

#include <myengine/core/FileWatcher.h>

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include <tracy/Tracy.hpp>

namespace myengine::core
{
    namespace
    {
        bool IsInside(const std::filesystem::path& file, const std::filesystem::path& directory)
        {
            const auto relative = file.lexically_relative(directory);
            return !relative.empty() && *relative.begin() != "..";
        }

        bool ReadWholeFile(const std::filesystem::path& path, std::string& contents)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return false;
            }
            contents.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            return true;
        }
    }

    FileWatcher::FileWatcher(const std::chrono::milliseconds interval) : interval_(interval)
    {
        context_.priority = jobs::Priority::Streaming;
    }

    FileWatcher::~FileWatcher()
    {
        Stop();
    }

    void FileWatcher::Watch(const std::filesystem::path& file, Callback onChanged)
    {
        watches_.push_back({file.lexically_normal(), std::string(), std::move(onChanged)});
    }

    void FileWatcher::WatchDirectory(const std::filesystem::path& directory, const std::string& extension, Callback onChanged)
    {
        watches_.push_back({directory.lexically_normal(), extension, std::move(onChanged)});
    }

    void FileWatcher::RequestFullRescan()
    {
        fullRescanRequested_ = true;
    }

    void FileWatcher::Poll()
    {
        if (scanRunning_)
        {
            if (jobs::IsBusy(context_))
            {
                return; // the disk is slow: the frame does not wait, the result comes on a later frame
            }

            jobs::Wait(context_); // returns at once; required before the job's data is touched
            scanRunning_ = false;

            const auto changes = std::move(scan_.changes);
            scan_.changes.clear();
            for (const auto& change : changes)
            {
                Deliver(change);
            }
        }

        if (watches_.empty())
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!fullRescanRequested_ && now - lastScanStart_ < interval_)
        {
            return;
        }

        // The job gets its own copy of the watch list, so Watch() during a scan is safe
        scan_.targets.clear();
        for (const auto& watch : watches_)
        {
            scan_.targets.emplace_back(watch.path, watch.extension);
        }
        scan_.reportAll = fullRescanRequested_;
        fullRescanRequested_ = false;

        lastScanStart_ = now;
        scanRunning_ = true;
        // Captures only `this` (job captures are limited to 64 bytes)
        jobs::Execute(context_, [this](jobs::JobArgs) { Scan(scan_); });
    }

    void FileWatcher::Stop()
    {
        if (scanRunning_)
        {
            jobs::Wait(context_);
            scanRunning_ = false;
            scan_.changes.clear();
        }
    }

    // Runs on a Streaming worker. Touches only `state`, never Python or the world
    void FileWatcher::Scan(ScanState& state)
    {
        ZoneScopedN("FileWatcher::Scan");

        try
        {
            std::map<std::filesystem::path, FileState> current;

            for (const auto& [target, extension] : state.targets)
            {
                std::error_code error;
                if (extension.empty())
                {
                    FileState fileState;
                    fileState.exists = std::filesystem::is_regular_file(target, error);
                    if (fileState.exists)
                    {
                        fileState.writeTime = std::filesystem::last_write_time(target, error);
                    }
                    current[target] = fileState;
                    continue;
                }

                if (!std::filesystem::is_directory(target, error))
                {
                    continue;
                }

                for (std::filesystem::recursive_directory_iterator it(target, std::filesystem::directory_options::skip_permission_denied, error), end;
                     !error && it != end; it.increment(error))
                {
                    if (it->path().filename() == "__pycache__")
                    {
                        it.disable_recursion_pending();
                        continue;
                    }
                    std::error_code entryError;
                    if (!it->is_regular_file(entryError) || it->path().extension() != extension)
                    {
                        continue;
                    }

                    FileState fileState;
                    fileState.exists = true;
                    fileState.writeTime = it->last_write_time(entryError);
                    current[it->path().lexically_normal()] = fileState;
                }
            }

            if (state.baselineDone || state.reportAll)
            {
                for (const auto& [path, fileState] : current)
                {
                    const auto known = state.known.find(path);
                    const bool isNew = known == state.known.end() || !known->second.exists;
                    const bool changed = isNew || known->second.writeTime != fileState.writeTime;

                    if (fileState.exists && (changed || state.reportAll))
                    {
                        FileChange change;
                        change.path = path;
                        if (ReadWholeFile(path, change.contents))
                        {
                            state.changes.push_back(std::move(change));
                        }
                    }
                    else if (!fileState.exists && known != state.known.end() && known->second.exists)
                    {
                        state.changes.push_back({path, true, std::string()});
                    }
                }

                // Files of a watched directory that disappeared
                for (const auto& [path, fileState] : state.known)
                {
                    if (fileState.exists && current.find(path) == current.end())
                    {
                        state.changes.push_back({path, true, std::string()});
                    }
                }
            }

            state.known = std::move(current);
            state.baselineDone = true;
        }
        catch (...)
        {
            // Never let an exception leave a job; the next scan tries again
            state.changes.clear();
        }
    }

    void FileWatcher::Deliver(const FileChange& change)
    {
        for (const auto& watch : watches_)
        {
            const bool matches = watch.extension.empty()
                ? change.path == watch.path
                : change.path.extension() == watch.extension && IsInside(change.path, watch.path);
            if (matches && watch.onChanged)
            {
                watch.onChanged(change);
            }
        }
    }
}
