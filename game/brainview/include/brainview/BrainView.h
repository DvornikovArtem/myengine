// BrainView.h
// The 3D view of a fly brain (docs/flybrain/brain-view.md): ~165 000 neurons as a glowing cloud in their real places, each one flashes when it spikes and fades.
// It knows nothing of DOOM and does not run the simulation: whoever runs it tells the view which neuron spiked how long ago (Flash). The cloud is drawn into a
// render target of the live-view service, so the texture can be put in an ImGui window, a corner of the game or a "monitor" in a scene.
//
// The GPU side is IRenderAdapter::CreatePointCloud / DrawPointCloud: the positions go up once, per frame only the time of the last spike of every neuron (one float).
// A GPU brain that keeps those times in its own buffer would bind that buffer instead of the copy: the interface of the view (Flash) stays.

#pragma once

#include <brainview/BrainActivity.h>
#include <brainview/BrainLayout.h>
#include <brainview/OrbitCamera.h>

#include <myengine/core/Types.h>
#include <myengine/render/IRenderAdapter.h>

#include <memory>
#include <string>
#include <vector>

namespace myengine::editor
{
    class ThumbnailService;
}

namespace brainview
{
    // How the cloud looks
    struct ViewStyle
    {
        float decaySeconds = 0.12f;       // a flash fades to 1/e in this time
        float baseBrightness = 0.05f;     // a neuron at rest
        float flashBrightness = 1.1f;     // added by a flash (a few thousand flash at once in a busy brain: more than this turns the dense parts white)
        float baseSizePixels = 1.4f;      // at a picture 540 pixels high; the sizes grow with the height
        float flashSizePixels = 1.8f;
        bool bloom = true;
        float bloomThreshold = 0.7f;
        float bloomIntensity = 0.7f;
        float exposure = 0.9f;
        myengine::core::Color background{0.010f, 0.012f, 0.024f, 1.0f};
    };

    class BrainView
    {
    public:
        explicit BrainView(myengine::render::IRenderAdapter& adapter);
        ~BrainView();
        BrainView(const BrainView&) = delete;
        BrainView& operator=(const BrainView&) = delete;

        // Builds the cloud of a connectome (between frames: the points go to the GPU). `groups` empty: DefaultGroups.
        bool Create(const flybrain::Connectome& connectome, std::vector<BrainGroup> groups = {});
        void Destroy();
        bool Valid() const { return cloud_.IsValid(); }
        uint32_t NeuronCount() const { return static_cast<uint32_t>(flashes_.Count()); }

        // The clock of the view, seconds of wall time since Create. Advance moves it, turns the camera and moves the window of the activity.
        void Advance(double deltaSeconds);
        double Clock() const { return clock_; }

        // The neuron spiked `ageSeconds` ago (0: just now); the group it belongs to counts it too
        void Flash(uint32_t neuron, double ageSeconds);
        // The flashes come from a buffer of the GPU brain (the steps of the last spikes, no copy to the CPU) until it is cleared with a default source. Call it every
        // frame before Submit / Render: the step and the fence change. The buffer must belong to the device of the renderer.
        void SetGpuFlash(const myengine::render::GpuFlashSource& source) { gpuFlash_ = source; }
        bool UsesGpuFlash() const { return gpuFlash_.buffer != nullptr; }
        // Forgets the flashes and the activity (a new run)
        void ClearActivity();
        uint64_t FlashCount() const { return flashCount_; }

        // Puts the camera where the whole cloud (the brain and the nerve cord) fits a picture of this aspect (width / height) with a margin around it (a part of the
        // height: 0.08 leaves 8 % on each side); the cloud turns around its long axis, so the width of the turn is counted too. For the views that are not orbited by hand.
        void FitCamera(float aspect, float margin = 0.08f);

        OrbitCamera& Camera() { return camera_; }
        ViewStyle& Style() { return style_; }
        const std::vector<BrainGroup>& Groups() const { return groups_; }
        GroupActivity& Activity() { return activity_; }
        const GroupActivity& Activity() const { return activity_; }
        const CloudModel& Model() const { return model_; }

        // Describes the picture of this frame to the live-view service and returns the texture it was last drawn into (the service draws it before the scene, so the
        // image is a frame behind; invalid until the first render). `id` names the view (one id, one render target).
        myengine::render::TextureHandle Submit(myengine::editor::ThumbnailService& service, const std::string& id, uint32_t width, uint32_t height);

        // Draws the cloud into `target` now: inside a frame of `surface` (after BeginFrame), not inside another render target. For a caller that has no live-view
        // service (the demo, a game that draws its own frame); the picture is ready when EndFrame has been called.
        bool Render(myengine::render::RenderSurfaceHandle surface, myengine::render::RenderTargetHandle target, uint32_t width, uint32_t height);

        // The parameters of the cloud draw for a target of this size (for a caller that draws the cloud itself)
        myengine::render::PointCloudDrawParams DrawParams(uint32_t width, uint32_t height) const;

    private:
        myengine::render::IRenderAdapter& adapter_;
        myengine::render::PointCloudHandle cloud_{};
        CloudModel model_;
        std::vector<BrainGroup> groups_;
        std::vector<uint32_t> indexInGroup_;   // per neuron: its position among the neurons of its group
        SpikeFlashes flashes_;
        GroupActivity activity_;
        OrbitCamera camera_;
        ViewStyle style_;
        myengine::render::GpuFlashSource gpuFlash_;
        double clock_ = 0.0;
        uint64_t flashCount_ = 0;
        // the extent of the cloud for FitCamera (the outliers cut): along the long axis and the radius of the turn
        bool fitKnown_ = false;
        float fitMinY_ = -1.0f;
        float fitMaxY_ = 1.0f;
        float fitRadius_ = 1.0f;
    };
}
