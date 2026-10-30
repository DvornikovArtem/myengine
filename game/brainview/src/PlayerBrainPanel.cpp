#include <brainview/PlayerBrainPanel.h>

#include <myengine/editor/ThumbnailService.h>
#include <myengine/render/IRenderAdapter.h>

#include <imgui/imgui.h>

#include <algorithm>

namespace brainview
{
    namespace
    {
        // The room around the cloud, a part of the height of the picture on each side: the panel shows the whole fly with a little air, the big brain a bit less
        // than three quarters of the window height
        constexpr float kPanelMargin = 0.08f;
        constexpr float kBigMargin = 0.17f;
    }

    PlayerBrainPanel::PlayerBrainPanel(myengine::render::IRenderAdapter* render, myengine::editor::ThumbnailService* thumbnails, std::string id)
        : render_(render), thumbnails_(thumbnails), id_(std::move(id))
    {
    }

    PlayerBrainPanel::~PlayerBrainPanel() = default;

    bool PlayerBrainPanel::EnsureView(const BrainFeedFrame& frame)
    {
        if (view_ != nullptr)
        {
            return true;
        }
        if (failed_ || render_ == nullptr || thumbnails_ == nullptr || frame.connectome == nullptr)
        {
            return false;
        }
        auto view = std::make_unique<BrainView>(*render_);
        if (!view->Create(*frame.connectome, frame.groups))
        {
            failed_ = true;
            return false;
        }
        view->Camera().autoRotateSpeed = 0.10f;
        view_ = std::move(view);
        return true;
    }

    void PlayerBrainPanel::Update(const BrainFeedFrame& frame, const double deltaSeconds)
    {
        if (!EnsureView(frame))
        {
            return;
        }
        view_->Advance(deltaSeconds);
        for (const BrainFeedFrame::Spike& spike : frame.spikes)
        {
            view_->Flash(spike.neuron, std::max(frame.simSecondsNow - spike.simSeconds, 0.0));
        }
        view_->SetGpuFlash(frame.gpu ? frame.gpuFlash : myengine::render::GpuFlashSource{});
        frame_.simSecondsNow = frame.simSecondsNow;
        frame_.gpu = frame.gpu;
        frame_.generation = frame.generation;
        frame_.backend = frame.backend.empty() ? frame_.backend : frame.backend;
        frame_.spikesPerSecond = frame.spikesPerSecond;
        frame_.brainSpeed = frame.brainSpeed;
    }

    void PlayerBrainPanel::DrawPanel(ImDrawList* list, const float x, const float y, const float width, const float height)
    {
        if (view_ == nullptr || big_)
        {
            return;   // the big brain is on: the small picture is not made
        }
        list->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);
        view_->FitCamera(width / std::max(height, 1.0f), kPanelMargin);
        const std::uint32_t pixelsW = std::max(64u, static_cast<std::uint32_t>(std::min(width, 1600.0f)));
        const std::uint32_t pixelsH = std::max(64u, static_cast<std::uint32_t>(std::min(height, 1000.0f)));
        const myengine::render::TextureHandle texture = view_->Submit(*thumbnails_, id_ + "-panel", pixelsW, pixelsH);
        if (texture.IsValid())
        {
            list->AddImage(static_cast<ImTextureID>(static_cast<ImU64>(texture.value)), ImVec2(x, y), ImVec2(x + width, y + height));
        }
        list->PopClipRect();
    }

    void PlayerBrainPanel::DrawOverlay(ImDrawList* list, const float width, const float height)
    {
        if (!big_ || view_ == nullptr)
        {
            return;
        }
        // DOOM stays visible under it, dimmed
        list->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(width, height), IM_COL32(2, 4, 10, 70));
        list->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(width, height), true);
        view_->FitCamera(width / std::max(height, 1.0f), kBigMargin);
        const std::uint32_t pixelsW = std::max(64u, static_cast<std::uint32_t>(std::min(width, 1600.0f)));
        const std::uint32_t pixelsH = std::max(64u, static_cast<std::uint32_t>(std::min(height, 1000.0f)));
        const myengine::render::TextureHandle texture = view_->Submit(*thumbnails_, id_ + "-big", pixelsW, pixelsH);
        if (texture.IsValid())
        {
            list->AddImage(static_cast<ImTextureID>(static_cast<ImU64>(texture.value)), ImVec2(0.0f, 0.0f), ImVec2(width, height), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 175));
        }
        list->PopClipRect();
    }
}
