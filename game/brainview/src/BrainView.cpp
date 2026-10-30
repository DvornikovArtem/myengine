#include <brainview/BrainView.h>

#include <myengine/editor/ThumbnailService.h>

#include <unknwn.h>

#include <algorithm>
#include <cmath>
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

    void BrainView::FitCamera(const float aspect, const float margin)
    {
        if (!fitKnown_)
        {
            std::vector<float> ys;
            std::vector<float> radii;
            const std::size_t count = model_.positions.size() / 3;
            ys.reserve(count);
            radii.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const float x = model_.positions[3 * i];
                const float y = model_.positions[3 * i + 1];
                const float z = model_.positions[3 * i + 2];
                if (std::abs(x) >= kHiddenDistance * 0.5f || std::abs(y) >= kHiddenDistance * 0.5f || std::abs(z) >= kHiddenDistance * 0.5f)
                {
                    continue;   // a neuron without a place is drawn far away: not part of the cloud
                }
                ys.push_back(y);
                radii.push_back(std::sqrt(x * x + z * z));
            }
            if (ys.size() > 100)
            {
                // the 0.5 % and the 99.5 % of the places: a few stray neurons do not stretch the frame
                const std::size_t low = ys.size() / 200;
                const std::size_t high = ys.size() - 1 - low;
                std::nth_element(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(low), ys.end());
                fitMinY_ = ys[low];
                std::nth_element(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(high), ys.end());
                fitMaxY_ = ys[high];
                std::nth_element(radii.begin(), radii.begin() + static_cast<std::ptrdiff_t>(high), radii.end());
                fitRadius_ = radii[high];
            }
            fitKnown_ = true;
        }
        const float halfTan = std::tan(camera_.fovY * 0.5f);
        const float byHeight = (fitMaxY_ - fitMinY_) * (1.0f + 2.0f * margin) * 0.5f / halfTan;
        const float byWidth = fitRadius_ * (1.0f + 2.0f * margin) / (halfTan * std::max(aspect, 0.1f));
        camera_.centerY = (fitMinY_ + fitMaxY_) * 0.5f;
        camera_.distance = std::clamp(std::max(byHeight, byWidth), OrbitCamera::kMinDistance, OrbitCamera::kMaxDistance);
    }

    myengine::render::PointCloudDrawParams BrainView::DrawParams(const uint32_t width, const uint32_t height) const
    {
        myengine::render::PointCloudDrawParams params;
        camera_.Matrices(static_cast<float>(width) / static_cast<float>(std::max(height, 1u)), params.view, params.projection);
        params.flashTimes = gpuFlash_.buffer != nullptr ? nullptr : flashes_.Times();
        params.gpuFlash = gpuFlash_;
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

    namespace
    {
        // The references on the buffer and the fence of a GPU brain, held by a request of the live view until it is drawn: the brain may be destroyed in between
        struct GpuFlashHold
        {
            IUnknown* buffer = nullptr;
            IUnknown* fence = nullptr;
            GpuFlashHold(IUnknown* bufferIn, IUnknown* fenceIn) : buffer(bufferIn), fence(fenceIn)
            {
                if (buffer != nullptr)
                {
                    buffer->AddRef();
                }
                if (fence != nullptr)
                {
                    fence->AddRef();
                }
            }
            GpuFlashHold(const GpuFlashHold&) = delete;
            GpuFlashHold& operator=(const GpuFlashHold&) = delete;
            ~GpuFlashHold()
            {
                if (fence != nullptr)
                {
                    fence->Release();
                }
                if (buffer != nullptr)
                {
                    buffer->Release();
                }
            }
        };
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
        if (gpuFlash_.buffer == nullptr)
        {
            request.cloudFlashTimes.assign(flashes_.Times(), flashes_.Times() + flashes_.Count());
        }
        else
        {
            request.keepAlive = std::make_shared<GpuFlashHold>(static_cast<IUnknown*>(gpuFlash_.buffer), static_cast<IUnknown*>(gpuFlash_.fence));
        }
        request.cloudParams.flashTimes = nullptr;   // the service points it at its own copy
        return service.SubmitLiveView(std::move(request));
    }
}
