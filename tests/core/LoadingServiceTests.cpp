// LoadingService: when an operation is shown, how long it stays, cancel, polled sources, other threads.

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <myengine/editor/LoadingService.h>

namespace
{
    using namespace myengine::editor;
    using namespace std::chrono_literals;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // A short operation never shows; a long one shows after the threshold and the window stays a little after the end
    void TestThresholds()
    {
        LoadingService service;

        const LoadingId quick = service.Begin("Quick", LoadingMode::Modal);
        Check(service.HasModal(), "A modal operation blocks the editor from the first moment");
        Check(service.Visible().empty(), "A new operation must not be shown yet");
        std::this_thread::sleep_for(40ms);
        service.End(quick);
        Check(!service.HasModal(), "A finished modal operation does not block");
        Check(service.Visible().empty(), "An operation shorter than the threshold must never be shown");
        service.Update();

        const LoadingId slow = service.Begin("Open map", LoadingMode::Modal);
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(LoadingService::kShowAfterSeconds * 1000.0) + 60));
        const auto visible = service.Visible();
        Check(visible.size() == 1 && visible[0].label == "Open map" && visible[0].mode == LoadingMode::Modal,
            "An operation longer than the threshold must be shown");
        Check(visible[0].elapsedSeconds >= LoadingService::kShowAfterSeconds, "The elapsed time must be reported");

        service.End(slow);
        Check(service.Visible().size() == 1, "A finished operation stays for the minimum time so that it does not blink");
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(LoadingService::kMinimumShownSeconds * 1000.0) + 60));
        Check(service.Visible().empty(), "The operation must go after the minimum time");
        service.Update();
        Check(service.ActiveCount() == 0, "Nothing is running");
    }

    void TestProgressAndCancel()
    {
        LoadingService service;
        const LoadingId id = service.Begin("Loading", LoadingMode::Background, true);
        service.SetProgress(id, 0.25f, "Meshes 1/4");
        service.SetProgress(id, 5.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(LoadingService::kShowAfterSeconds * 1000.0) + 60));
        auto visible = service.Visible();
        Check(visible.size() == 1, "Shown");
        Check(visible[0].progress == 1.0f, "Progress is clamped to 1");
        Check(visible[0].detail == "Meshes 1/4", "An empty detail keeps the previous one");

        service.SetProgress(id, 0.5f, "Waiting");
        visible = service.Visible();
        Check(visible[0].progress == 1.0f && visible[0].detail == "Waiting", "The progress never goes back");

        const LoadingId unknown = service.Begin("Unknown length");
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(LoadingService::kShowAfterSeconds * 1000.0) + 60));
        bool indeterminate = false;
        for (const auto& operation : service.Visible())
        {
            indeterminate = indeterminate || (operation.id == unknown && operation.progress < 0.0f);
        }
        Check(indeterminate, "An operation without progress is indeterminate");
        service.SetProgress(unknown, 0.3f);
        service.SetProgress(unknown, -1.0f);
        for (const auto& operation : service.Visible())
        {
            if (operation.id == unknown)
            {
                Check(operation.progress == 0.3f, "A determinate operation does not turn indeterminate again");
            }
        }
        service.End(unknown);

        Check(!service.IsCancelRequested(id), "Not cancelled yet");
        service.RequestCancel(id);
        Check(service.IsCancelRequested(id), "Cancel is remembered");
        service.End(id);
        Check(!service.IsActive(id), "Ended");

        // A cancel of an operation that cannot be cancelled is ignored
        const LoadingId fixed = service.Begin("Fixed");
        service.RequestCancel(fixed);
        Check(!service.IsCancelRequested(fixed), "A non-cancellable operation ignores Cancel");
        service.End(fixed);
    }

    // Nested operations: the title of the outer one, the detail of the innermost
    void TestNesting()
    {
        LoadingService service;
        const LoadingId outer = service.Begin("Opening project", LoadingMode::Modal);
        service.SetSubtitle(outer, "demo");
        const LoadingId inner = service.Begin("Loading map", LoadingMode::Modal, false, outer);
        service.SetProgress(inner, 0.4f, "Adding components");
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(LoadingService::kShowAfterSeconds * 1000.0) + 60));
        const auto visible = service.Visible();
        Check(visible.size() == 1, "A nested operation is not listed on its own");
        Check(visible[0].label == "Opening project" && visible[0].subtitle == "demo", "The title is the outer one");
        Check(visible[0].detail == "Adding components" && visible[0].progress == 0.4f, "The detail is the innermost one");
        service.End(inner);
        service.End(outer);
    }

    void TestSync()
    {
        LoadingService service;
        service.Sync("thumbnails", true, "Generating thumbnails", 0.0f, "3 left");
        service.Sync("thumbnails", true, "Generating thumbnails", 0.5f, "2 left");
        Check(service.ActiveCount() == 1, "Sync of the same key is one operation");
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(LoadingService::kShowAfterSeconds * 1000.0) + 60));
        const auto visible = service.Visible();
        Check(visible.size() == 1 && visible[0].detail == "2 left" && visible[0].progress == 0.5f, "Sync updates the operation");
        service.Sync("thumbnails", false, {});
        Check(service.ActiveCount() == 0, "Sync(false) ends it");

        service.Sync("assets", true, "Loading assets");
        service.Sync("assets", false, {});
        service.Sync("assets", true, "Loading assets");
        Check(service.ActiveCount() == 1, "A key that was ended starts again as a new operation");
    }

    // Workers report progress while the main thread reads the list
    void TestThreads()
    {
        LoadingService service;
        const LoadingId id = service.Begin("Threaded", LoadingMode::Modal);
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker)
        {
            workers.emplace_back([&service, id, worker]()
            {
                for (int step = 0; step < 500; ++step)
                {
                    service.SetProgress(id, static_cast<float>(step) / 500.0f, "worker " + std::to_string(worker));
                }
            });
        }
        for (int step = 0; step < 200; ++step)
        {
            (void)service.Visible();
            (void)service.HasModal();
        }
        for (auto& worker : workers)
        {
            worker.join();
        }
        service.End(id);
        Check(!service.HasModal(), "Ended after the workers");
    }
}

int main()
{
    try
    {
        TestThresholds();
        TestProgressAndCancel();
        TestNesting();
        TestSync();
        TestThreads();
        std::cout << "OK: loading service: thresholds, minimum time, progress, cancel, polled sources, threads\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
