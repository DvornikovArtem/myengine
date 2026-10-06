// FileWatcher.h

#pragma once

#include <filesystem>
#include <functional>
#include <vector>

namespace myengine::core
{
    // Polls file modification time and calls a callback for changed files.
    // The polling and the file reads move to a Streaming job, callbacks run on the main thread
    class FileWatcher
    {
    public:
        using Callback = std::function<void(const std::filesystem::path&)>;

        void Watch(const std::filesystem::path& file, Callback onChanged);
        void Poll(); // checks exists + last_write_time once per interval

    private:
        struct Entry
        {
            std::filesystem::path file;
            Callback onChanged;
        };

        std::vector<Entry> entries_;
    };
}