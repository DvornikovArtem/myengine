// FileWatcher.cpp

#include <myengine/core/FileWatcher.h>

#include <utility>

namespace myengine::core
{
    void FileWatcher::Watch(const std::filesystem::path& file, Callback onChanged)
    {
        entries_.push_back({file, std::move(onChanged)});
    }

    void FileWatcher::Poll()
    {
        // T5: compare exists + last_write_time with the stored state, call onChanged for changed files
    }
}