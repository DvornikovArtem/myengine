// DoomMenuBackdrop.h
// The brain behind the menu of FlyDoom (docs/game/doom.md, "The laboratory"): a hologram of the real brain of the fly that turns slowly and is alive, because a fly at rest is
// (the lamina hums: the "Rest" stimulus of the brain view). It loads the connectome on its own thread while the menu is already there, runs the brain on the GPU when it
// can (on the device of the renderer, so the cloud reads the spikes without a copy) and on the CPU when it cannot, draws the cloud into a texture of the live-view service
// and puts it behind the menu with scan lines and a vignette over it. The engine knows it as myengine::game::IDoomBrainView.

#pragma once

#include <brainview/BrainPump.h>
#include <brainview/BrainRunner.h>
#include <brainview/BrainView.h>

#include <myengine/game/DoomFrontEnd.h>

#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace brainview
{
    class DoomMenuBackdrop final : public myengine::game::IDoomBrainView
    {
    public:
        DoomMenuBackdrop(const myengine::game::DoomBrainViewContext& context, std::filesystem::path connectomePath);
        ~DoomMenuBackdrop() override;

        void Draw(ImDrawList* list, float x, float y, float width, float height, double deltaSeconds, float alpha) override;
        std::string Status() const override;
        void SetNested(const float mix) override { nested_ = mix; }
        std::string Problem() const override { return problem_; }
        void SetBrightness(const float level) override { brightness_ = level; }
        myengine::game::DoomBrainStats Stats() const override;
        std::vector<myengine::game::DoomBrainAnchor> Anchors() const override { return anchors_; }

        // The connectome file of the game: `--fbrain`, then assets/flybrain/malecns_v1.fbrain next to the exe, then in the sources
        static std::filesystem::path FindConnectome(const std::filesystem::path& explicitPath, const std::filesystem::path& exeDirectory, const std::filesystem::path& sourceDirectory);

        // The test and the evidence read these
        bool Ready() const { return view_ != nullptr && runner_ != nullptr; }
        BrainView* View() { return view_.get(); }

    private:
        void UpdateAnchors(float x, float y, float width, float height, float pictureX, float pictureWidth);

        myengine::game::DoomBrainViewContext context_;
        std::filesystem::path path_;
        std::future<std::shared_ptr<const flybrain::Connectome>> loading_;
        bool loadStarted_ = false;
        std::string loadError_;
        std::string problem_;
        float nested_ = 0.0f;   // 0 main screen, 1 a screen under it
        float brightness_ = 1.0f;
        // the parts of the brain the labels of the menu point at (indices of neurons): the lobes of the eyes, the mushroom bodies, the descending neurons
        std::vector<std::uint32_t> optic_[2];       // [0]: the neurons right of the middle of the head, [1]: left of it
        std::vector<std::uint32_t> mushroom_[2];
        std::vector<std::uint32_t> descending_;
        float opticCentre_[2][3] = {};
        float mushroomCentre_[2][3] = {};
        float descendingCentre_[3] = {};
        bool centresKnown_ = false;
        std::vector<myengine::game::DoomBrainAnchor> anchors_;
        std::shared_ptr<const flybrain::Connectome> connectome_;
        std::unique_ptr<BrainView> view_;
        std::unique_ptr<BrainRunner> runner_;
        BrainPumpScratch scratch_;
        BrainRunner::Stats stats_;
        double statsAge_ = 10.0;
        float shown_ = 0.0f;          // the picture fades in when it is ready
        double clock_ = 0.0;
    };
}
