// LoadingService.h

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace myengine::editor
{
    enum class LoadingMode : std::uint8_t
    {
        Background, // a small indicator (status bar); the editor stays usable
        Modal,      // a window that blocks the editor: open a map, open or create a project
        Splash,     // the whole window instead of the editor: the project is starting up
    };

    using LoadingId = std::uint32_t; // 0 is "no operation"

    struct LoadingOperation
    {
        LoadingId id = 0;
        std::string label;    // the title: "Loading map..."
        std::string subtitle; // what exactly: "benchmark"
        std::string detail;   // the stage: "Adding components"; the one of the innermost nested operation
        float progress = -1.0f; // 0..1; negative: the length is not known
        LoadingMode mode = LoadingMode::Background;
        bool cancellable = false;
        bool cancelRequested = false;
        double elapsedSeconds = 0.0;
    };

    // The long operations of the editor that the user should see: a modal window for a map or a project, an indicator
    // in the status bar for the background ones. The operation itself runs wherever it likes (a worker thread, spread
    // over frames); this service only holds what the UI shows. Every method may be called from any thread.
    //
    // An operation is shown only when it has been running for kShowAfterSeconds, so that the quick ones do not
    // flash, and once shown it stays for kMinimumShownSeconds, so that a window does not blink either. The progress
    // of an operation never goes back. An operation begun with a parent is part of it: the title shown is the
    // outer one, the detail comes from the innermost.
    class LoadingService
    {
    public:
        using Clock = std::chrono::steady_clock;

        static constexpr double kShowAfterSeconds = 0.2;
        static constexpr double kMinimumShownSeconds = 0.5;

        LoadingId Begin(
            std::string label,
            LoadingMode mode = LoadingMode::Background,
            bool cancellable = false,
            LoadingId parent = 0);
        // progress: 0..1 or a negative value (unknown length); detail replaces the second line when it is not empty
        void SetProgress(LoadingId id, float progress, std::string detail = {});
        void SetLabel(LoadingId id, std::string label);
        void SetSubtitle(LoadingId id, std::string subtitle);
        void End(LoadingId id);

        void RequestCancel(LoadingId id);
        bool IsCancelRequested(LoadingId id) const;
        bool IsActive(LoadingId id) const;

        // For sources that are only polled (thumbnails left, assets still streaming): starts an operation named `key`
        // when `active` becomes true, updates it while it stays true, and ends it when it turns false.
        void Sync(const std::string& key, bool active, const std::string& label, float progress = -1.0f,
            const std::string& detail = {}, LoadingMode mode = LoadingMode::Background);

        // The operations to draw now: the active ones older than the threshold, and the ones that ended less than the
        // minimum time after they were first shown. Oldest first.
        std::vector<LoadingOperation> Visible() const;
        std::vector<LoadingOperation> Visible(Clock::time_point now) const;

        // A modal (or splash) operation is running, shown or not: the editor ignores input until it ends
        bool HasModal() const;
        // The startup splash is active: the panels are not drawn at all until it ends
        bool HasSplash() const;
        std::size_t ActiveCount() const;

        // Forgets the finished operations that no longer need to be shown. Once per frame.
        void Update();
        void Update(Clock::time_point now);

    private:
        struct Entry
        {
            LoadingOperation operation;
            LoadingId parent = 0;
            std::string key; // Sync
            Clock::time_point started;
            Clock::time_point ended;
            bool finished = false;
        };

        LoadingOperation Describe(const Entry& entry, Clock::time_point now) const;
        bool IsVisible(const Entry& entry, Clock::time_point now) const;

        mutable std::mutex mutex_;
        std::vector<Entry> entries_;
        LoadingId nextId_ = 1;
    };
}
