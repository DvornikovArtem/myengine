#include <brainview/BrainView.h>

#include <myengine/editor/ThumbnailService.h>

#include <algorithm>
#include <cstring>

namespace brainview
{
    BrainView::BrainView(myengine::render::IRenderAdapter& adapter) : adapter_(adapter) {}

    BrainView::~BrainView()
    {
        Destroy();
    }

    bool BrainView::Create(const flybrain::Connectome& connectome, std::vector<BrainGroup> groups)
    {
        Destroy();
        groups_ = groups.empty() ? DefaultGroups(connectome) : std::move(groups);
        model_ = BuildCloudModel(connectome, groups_);
        const uint32_t count = connectome.NeuronCount();

        indexInGroup_.assign(count, 0);
        std::vector<uint32_t> sizes;
        for (std::size_t g = 0; g < groups_.size(); ++g)
        {
            uint32_t next = 0;
            for (const uint32_t neuron : groups_[g].neurons)
            {
                if (neuron < count && model_.groupOf[neuron] == static_cast<uint8_t>(g))
                {
                    indexInGroup_[neuron] = next++;
                }
            }
            sizes.push_back(next);
        }
        activity_.Configure(sizes);
        flashes_.Reset(count);
        clock_ = 0.0;
        flashCount_ = 0;

        myengine::render::PointCloudData data;
        data.positions = model_.positions;
        data.colorIndex = model_.colorIndex;
        cloud_ = adapter_.CreatePointCloud(data);
        return cloud_.IsValid();
    }

    void BrainView::Destroy()
    {
        if (cloud_.IsValid())
        {
            adapter_.DestroyPointCloud(cloud_);
            cloud_ = {};
        }
        model_ = {};
        groups_.clear();
        indexInGroup_.clear();
        flashes_.Reset(0);
        activity_.Configure({});
    }

    void BrainView::Advance(const double deltaSeconds)
    {
        const double step = std::max(deltaSeconds, 0.0);
        clock_ += step;
        flashes_.SetNow(clock_);
        camera_.Advance(step);
        activity_.Advance(clock_);
    }

    void BrainView::Flash(const uint32_t neuron, const double ageSeconds)
    {
        if (neuron >= flashes_.Count())
        {
            return;
        }
        const double time = clock_ - std::max(ageSeconds, 0.0);
        flashes_.Mark(neuron, time);
        ++flashCount_;
        const uint8_t group = model_.groupOf[neuron];
        if (group != kNoGroup)
        {
            activity_.Record(group, indexInGroup_[neuron], time);
        }
    }

    void BrainView::ClearActivity()
    {
        flashes_.Reset(flashes_.Count());
        std::vector<uint32_t> sizes;
        for (std::size_t g = 0; g < activity_.GroupCount(); ++g)
        {
            sizes.push_back(activity_.GroupSize(g));
        }
        activity_.Configure(sizes);
        activity_.Advance(clock_);
        flashCount_ = 0;
    }

    myengine::render::PointCloudDrawParams BrainView::DrawParams(const uint32_t width, const uint32_t height) const
    {
        myengine::render::PointCloudDrawParams params;
        camera_.Matrices(static_cast<float>(width) / static_cast<float>(std::max(height, 1u)), params.view, params.projection);
        params.flashTimes = flashes_.Times();
        params.time = flashes_.Now();
        params.decaySeconds = style_.decaySeconds;
        params.baseBrightness = style_.baseBrightness;
        params.flashBrightness = style_.flashBrightness;
        params.baseSizePixels = style_.baseSizePixels * (static_cast<float>(height) / 540.0f);
        params.flashSizePixels = style_.flashSizePixels * (static_cast<float>(height) / 540.0f);
        params.referenceDepth = camera_.distance;
        params.depthRange = 1.1f;
        FillPalette(params.palette);
        params.bloom = style_.bloom;
        params.bloomThreshold = style_.bloomThreshold;
        params.bloomIntensity = style_.bloomIntensity;
        params.exposure = style_.exposure;
        params.background = style_.background;
        return params;
    }

    bool BrainView::Render(const myengine::render::RenderSurfaceHandle surface, const myengine::render::RenderTargetHandle target, const uint32_t width, const uint32_t height)
    {
        if (!cloud_.IsValid() || !adapter_.BeginRenderTarget(target, style_.background))
        {
            return false;
        }
        const bool drawn = adapter_.DrawPointCloud(surface, cloud_, DrawParams(width, height));
        adapter_.EndRenderTarget(target);
        return drawn;
    }

    myengine::render::TextureHandle BrainView::Submit(myengine::editor::ThumbnailService& service, const std::string& id, const uint32_t width, const uint32_t height)
    {
        if (!cloud_.IsValid())
        {
            return {};
        }
        myengine::editor::LiveViewRequest request;
        request.id = id;
        request.width = width;
        request.height = height;
        request.clearColor = style_.background;
        request.pointCloud = cloud_;
        request.cloudParams = DrawParams(width, height);
        request.view = request.cloudParams.view;
        request.projection = request.cloudParams.projection;
        request.cloudFlashTimes.assign(flashes_.Times(), flashes_.Times() + flashes_.Count());
        request.cloudParams.flashTimes = nullptr;   // the service points it at its own copy
        return service.SubmitLiveView(std::move(request));
    }
}
