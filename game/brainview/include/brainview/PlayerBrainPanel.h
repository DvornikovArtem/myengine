// PlayerBrainPanel.h
// The brain of the fly that plays DOOM, in the window of the game (docs/flybrain/brain-view.md, "In the game"): the cloud in the panel at the right of the picture, and
// "the big brain" (Tab): the same cloud over the whole window above a dimmed DOOM. The owner of the brain of the game (game/doomfly: the controller of the fly) gives the
// panel one BrainFeedFrame per frame, what its brain did since the last one; the panel owns the view and draws it. A brain on the GPU is drawn from its own buffer, no copy.

#pragma once

#include <brainview/BrainView.h>

#include <flybrain/Connectome.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ImDrawList;

namespace myengine::editor
{
    class ThumbnailService;
}

namespace brainview
{
    // One look at the brain of the game: what happened since the last one
    struct BrainFeedFrame
    {
        struct Spike
        {
            std::uint32_t neuron = 0;
            double simSeconds = 0.0;      // when it spiked, on the clock of the brain
        };
        std::shared_ptr<const flybrain::Connectome> connectome;   // the positions of the cloud; the same for the whole game
        std::vector<Spike> spikes;        // a brain on the CPU: the spikes since the last look (oldest first)
        double simSecondsNow = 0.0;       // the clock of the brain at this look
        bool gpu = false;                 // a brain on the GPU (the render device): `gpuFlash` is the source of the flashes and `spikes` stay empty
        myengine::render::GpuFlashSource gpuFlash;
        std::uint32_t generation = 0;     // grows when the brain is made again (death, a new level): `gpuFlash` of an older one is not used
        std::string backend;              // "GPU" / "CPU" for the HUD
        double spikesPerSecond = 0.0;     // for the HUD (0: unknown)
        double brainSpeed = 0.0;          // simulated seconds per wall second (0: unknown)
    };

    class PlayerBrainPanel
    {
    public:
        PlayerBrainPanel(myengine::render::IRenderAdapter* render, myengine::editor::ThumbnailService* thumbnails, std::string id);
        ~PlayerBrainPanel();
        PlayerBrainPanel(const PlayerBrainPanel&) = delete;
        PlayerBrainPanel& operator=(const PlayerBrainPanel&) = delete;

        // Once per frame, before the drawing: the brain of this frame (the flashes of the spikes) and the real time of the frame
        void Update(const BrainFeedFrame& frame, double deltaSeconds);

        // The picture in the rect (window pixels) with a small HUD over it. A dark box while the cloud is made, and while the big brain is on.
        void DrawPanel(ImDrawList* list, float x, float y, float width, float height);
        // Tab: the cloud over the whole window; DrawOverlay draws it (after the picture of the game and the panel) while it is on
        void ToggleBig() { big_ = !big_; }
        bool Big() const { return big_; }
        void DrawOverlay(ImDrawList* list, float width, float height);

        BrainView* View() { return view_.get(); }

    private:
        bool EnsureView(const std::shared_ptr<const flybrain::Connectome>& connectome);
        void DrawHud(ImDrawList* list, float x, float y, float width, float height, bool big);

        myengine::render::IRenderAdapter* render_;
        myengine::editor::ThumbnailService* thumbnails_;
        std::string id_;
        std::unique_ptr<BrainView> view_;
        BrainFeedFrame frame_;                // the last one, without the spikes
        bool big_ = false;
        bool failed_ = false;
    };
}
