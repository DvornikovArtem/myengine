// BrainViewWindow.h
// The editor window Window > Fly Brain (docs/flybrain/brain-view.md): loads a .fbrain, runs the brain on a thread of its own with a test stimulus (vision, taste,
// pain or none: a demo that needs no DOOM) and shows it as the 3D cloud with the HUD. Closing the window stops the simulation; opening it starts a new run.
//
// Draw is called every frame inside the ImGui frame of the editor (also when the window is closed: that is how it notices and stops the brain).

#pragma once

#include <brainview/BrainRunner.h>
#include <brainview/BrainView.h>

#include <filesystem>
#include <future>
#include <memory>
#include <string>

namespace myengine::core
{
    class Logger;
}

namespace myengine::editor
{
    class ThumbnailService;
}

namespace brainview
{
    class BrainViewWindow
    {
    public:
        BrainViewWindow(myengine::render::IRenderAdapter& adapter, myengine::editor::ThumbnailService& thumbnails, myengine::core::Logger& logger,
                        std::filesystem::path connectomePath);
        ~BrainViewWindow();
        BrainViewWindow(const BrainViewWindow&) = delete;
        BrainViewWindow& operator=(const BrainViewWindow&) = delete;

        // `open` is the state of the window (the Window menu sets it, the close button clears it)
        void Draw(bool& open, double deltaSeconds);

        // The place where the connectome is looked for when no path was given: MYENGINE_FBRAIN, then assets/flybrain/malecns_v1.fbrain
        static std::filesystem::path DefaultConnectomePath();

        // For tests and automated runs: the state of the load and the view
        bool Ready() const { return view_ != nullptr && view_->Valid() && runner_ != nullptr; }
        BrainView* View() { return view_.get(); }
        BrainRunner* Runner() { return runner_.get(); }

    private:
        enum class LoadState
        {
            NotLoaded,
            Loading,
            Ready,
            Failed
        };
        void StartLoading();
        void FinishLoading();
        void DrawToolbar();

        myengine::render::IRenderAdapter& adapter_;
        myengine::editor::ThumbnailService& thumbnails_;
        myengine::core::Logger& logger_;
        std::filesystem::path path_;
        char pathBuffer_[520] = {};

        LoadState state_ = LoadState::NotLoaded;
        std::string error_;
        std::future<std::shared_ptr<const flybrain::Connectome>> loading_;
        std::string loadingError_;
        std::shared_ptr<const flybrain::Connectome> connectome_;
        std::unique_ptr<BrainView> view_;
        std::unique_ptr<BrainRunner> runner_;
        bool wasOpen_ = false;
        bool showHud_ = true;
        bool showRaster_ = true;
        bool showLook_ = false;
        std::vector<BrainRunner::Spike> spikeScratch_;
        BrainRunner::Stats stats_;
        double statsAge_ = 10.0;
    };
}
