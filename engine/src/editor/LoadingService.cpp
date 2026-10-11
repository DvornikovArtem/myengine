// LoadingService.cpp

#include <myengine/editor/LoadingService.h>

#include <algorithm>
#include <utility>

namespace myengine::editor
{
    namespace
    {
        double SecondsBetween(const LoadingService::Clock::time_point from, const LoadingService::Clock::time_point to)
        {
            return std::chrono::duration<double>(to - from).count();
        }
    }

    LoadingOperation LoadingService::Describe(const Entry& entry, const Clock::time_point now) const
    {
        LoadingOperation operation = entry.operation;
        operation.elapsedSeconds = SecondsBetween(entry.started, entry.finished ? entry.ended : now);
        return operation;
    }

    // Visible when it has run for the threshold, and for a finished one while the minimum time since it was shown has
    // not passed (the first time the UI asks it becomes "shown": see Visible)
    bool LoadingService::IsVisible(const Entry& entry, const Clock::time_point now) const
    {
        if (!entry.finished)
        {
            return SecondsBetween(entry.started, now) >= kShowAfterSeconds;
        }

        // It ended: it deserved a window only if it was longer than the threshold, and it stays until the minimum time
        const double duration = SecondsBetween(entry.started, entry.ended);
        if (duration < kShowAfterSeconds)
        {
            return false;
        }
        const Clock::time_point shownAt = entry.started + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(kShowAfterSeconds));
        return SecondsBetween(shownAt, now) < kMinimumShownSeconds;
    }

    LoadingId LoadingService::Begin(std::string label, const LoadingMode mode, const bool cancellable, const LoadingId parent)
    {
        std::scoped_lock lock(mutex_);
        Entry entry;
        entry.parent = parent;
        entry.operation.id = nextId_++;
        entry.operation.label = std::move(label);
        entry.operation.mode = mode;
        entry.operation.cancellable = cancellable;
        entry.started = Clock::now();
        entries_.push_back(std::move(entry));
        return entries_.back().operation.id;
    }

    void LoadingService::SetProgress(const LoadingId id, const float progress, std::string detail)
    {
        std::scoped_lock lock(mutex_);
        for (Entry& entry : entries_)
        {
            if (entry.operation.id == id && !entry.finished)
            {
                // Only grows: indeterminate becomes determinate, but never the other way, and never backwards
                if (progress >= 0.0f)
                {
                    entry.operation.progress = std::max(entry.operation.progress, std::min(progress, 1.0f));
                }
                if (!detail.empty())
                {
                    entry.operation.detail = std::move(detail);
                }
                return;
            }
        }
    }

    void LoadingService::SetLabel(const LoadingId id, std::string label)
    {
        std::scoped_lock lock(mutex_);
        for (Entry& entry : entries_)
        {
            if (entry.operation.id == id && !entry.finished)
            {
                entry.operation.label = std::move(label);
                return;
            }
        }
    }

    void LoadingService::SetSubtitle(const LoadingId id, std::string subtitle)
    {
        std::scoped_lock lock(mutex_);
        for (Entry& entry : entries_)
        {
            if (entry.operation.id == id && !entry.finished)
            {
                entry.operation.subtitle = std::move(subtitle);
                return;
            }
        }
    }

    void LoadingService::End(const LoadingId id)
    {
        std::scoped_lock lock(mutex_);
        for (Entry& entry : entries_)
        {
            if (entry.operation.id == id && !entry.finished)
            {
                entry.finished = true;
                entry.ended = Clock::now();
                entry.operation.progress = 1.0f;
                return;
            }
        }
    }

    void LoadingService::RequestCancel(const LoadingId id)
    {
        std::scoped_lock lock(mutex_);
        for (Entry& entry : entries_)
        {
            if (entry.operation.id == id && !entry.finished && entry.operation.cancellable)
            {
                entry.operation.cancelRequested = true;
                return;
            }
        }
    }

    bool LoadingService::IsCancelRequested(const LoadingId id) const
    {
        std::scoped_lock lock(mutex_);
        for (const Entry& entry : entries_)
        {
            if (entry.operation.id == id)
            {
                return entry.operation.cancelRequested;
            }
        }
        return false;
    }

    bool LoadingService::IsActive(const LoadingId id) const
    {
        std::scoped_lock lock(mutex_);
        for (const Entry& entry : entries_)
        {
            if (entry.operation.id == id)
            {
                return !entry.finished;
            }
        }
        return false;
    }

    void LoadingService::Sync(
        const std::string& key,
        const bool active,
        const std::string& label,
        const float progress,
        const std::string& detail,
        const LoadingMode mode)
    {
        std::scoped_lock lock(mutex_);
        Entry* found = nullptr;
        for (Entry& entry : entries_)
        {
            if (!entry.finished && entry.key == key)
            {
                found = &entry;
                break;
            }
        }

        if (!active)
        {
            if (found != nullptr)
            {
                found->finished = true;
                found->ended = Clock::now();
                found->operation.progress = 1.0f;
            }
            return;
        }

        if (found == nullptr)
        {
            Entry entry;
            entry.operation.id = nextId_++;
            entry.operation.mode = mode;
            entry.key = key;
            entry.started = Clock::now();
            entries_.push_back(std::move(entry));
            found = &entries_.back();
        }
        found->operation.label = label;
        found->operation.detail = detail;
        if (progress >= 0.0f)
        {
            found->operation.progress = std::max(found->operation.progress, std::min(progress, 1.0f));
        }
    }

    std::vector<LoadingOperation> LoadingService::Visible() const
    {
        return Visible(Clock::now());
    }

    std::vector<LoadingOperation> LoadingService::Visible(const Clock::time_point now) const
    {
        std::scoped_lock lock(mutex_);
        std::vector<LoadingOperation> result;
        for (const Entry& entry : entries_)
        {
            // A nested operation is shown through its outer one
            if (entry.parent != 0 || !IsVisible(entry, now))
            {
                continue;
            }

            LoadingOperation operation = Describe(entry, now);
            if (!entry.finished)
            {
                // The innermost active child gives the detail (its own, or its title when it has none)
                LoadingId current = entry.operation.id;
                bool descended = true;
                while (descended)
                {
                    descended = false;
                    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
                    {
                        if (it->parent == current && !it->finished)
                        {
                            current = it->operation.id;
                            operation.detail = !it->operation.detail.empty() ? it->operation.detail : it->operation.label;
                            if (operation.progress < 0.0f && it->operation.progress >= 0.0f)
                            {
                                operation.progress = it->operation.progress;
                            }
                            descended = true;
                            break;
                        }
                    }
                }
            }
            result.push_back(std::move(operation));
        }
        return result;
    }

    bool LoadingService::HasModal() const
    {
        std::scoped_lock lock(mutex_);
        return std::any_of(entries_.begin(), entries_.end(), [](const Entry& entry)
        {
            return !entry.finished && entry.operation.mode != LoadingMode::Background;
        });
    }

    bool LoadingService::HasSplash() const
    {
        std::scoped_lock lock(mutex_);
        return std::any_of(entries_.begin(), entries_.end(), [](const Entry& entry)
        {
            return !entry.finished && entry.operation.mode == LoadingMode::Splash;
        });
    }

    std::size_t LoadingService::ActiveCount() const
    {
        std::scoped_lock lock(mutex_);
        return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(), [](const Entry& entry)
        {
            return !entry.finished;
        }));
    }

    void LoadingService::Update()
    {
        Update(Clock::now());
    }

    void LoadingService::Update(const Clock::time_point now)
    {
        std::scoped_lock lock(mutex_);
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& entry)
            {
                if (!entry.finished)
                {
                    return false;
                }
                // Finished: it is kept only while it still has to be shown
                return !IsVisible(entry, now);
            }),
            entries_.end());
    }
}
