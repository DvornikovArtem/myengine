#include <brainview/PlayerBrainPanel.h>

#include <brainview/BrainViewPanel.h>

#include <myengine/editor/ThumbnailService.h>
#include <myengine/render/IRenderAdapter.h>

#include <imgui/imgui.h>

#include <algorithm>

namespace brainview
{
    namespace
    {
        // The camera: the small panel is nearly square and shows the head, the whole window shows the whole fly
        constexpr float kPanelDistance = 2.3f;
        constexpr float kBigDistance = 3.2f;
    }

    PlayerBrainPanel::PlayerBrainPanel(myengine::render::IRenderAdapter* render, myengine::editor::ThumbnailService* thumbnails, std::string id)
        : render_(render), thumbnails_(thumbnails), id_(std::move(id))
    {
    }

    PlayerBrainPanel::~PlayerBrainPanel() = default;

    bool PlayerBrainPanel::EnsureView(const std::shared_ptr<const flybrain::Connectome>& connectome)
    {
        if (view_ != nullptr)
        {
            return true;
        }
        if (failed_ || render_ == nullptr || thumbnails_ == nullptr || connectome == nullptr)
        {
            return false;
        }
        auto view = std::make_unique<BrainView>(*render_);
        if (!view->Create(*connectome))
        {
            failed_ = true;
            return false;
        }
        view->Camera().autoRotateSpeed = 0.10f;
        view->Camera().distance = kBigDistance;
        view_ = std::move(view);
        return true;
    }

    void PlayerBrainPanel::Update(const BrainFeedFrame& frame, const double deltaSeconds)
    {
        if (!EnsureView(frame.connectome))
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

    void PlayerBrainPanel::DrawHud(ImDrawList* list, const float x, const float y, const float width, const float height, const bool big)
    {
        BrainPanelInfo info;
        info.title = big ? "FLY BRAIN" : "BRAIN";
        info.subtitle = big ? "brain on " + (frame_.backend.empty() ? std::string("...") : frame_.backend) + "      Tab: back to the game" : std::string();
        info.simSeconds = frame_.simSecondsNow;
        info.realtimeRatio = frame_.brainSpeed;
        info.spikesPerSecond = frame_.spikesPerSecond;
        info.showRaster = big && !frame_.gpu;
        if (big)
        {
            DrawBrainHud(list, x, y, x + width, y + height, *view_, info);
        }
        else
        {
            // the panel of the controller has its own bars next to the cloud: only a line of text over the picture
            const std::string text = "brain on " + (frame_.backend.empty() ? std::string("...") : frame_.backend);
            list->AddText(ImVec2(x + 6.0f, y + 4.0f), IM_COL32(150, 170, 205, 255), text.c_str());
        }
    }

    void PlayerBrainPanel::DrawPanel(ImDrawList* list, const float x, const float y, const float width, const float height)
    {
        list->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), IM_COL32(4, 7, 14, 255));
        if (view_ == nullptr || big_)
        {
            return;   // the big brain is on: the small picture is not made
        }
        list->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);
        view_->Camera().distance = kPanelDistance;
        const std::uint32_t pixelsW = std::max(64u, static_cast<std::uint32_t>(std::min(width, 1600.0f)));
        const std::uint32_t pixelsH = std::max(64u, static_cast<std::uint32_t>(std::min(height, 1000.0f)));
        const myengine::render::TextureHandle texture = view_->Submit(*thumbnails_, id_ + "-panel", pixelsW, pixelsH);
        if (texture.IsValid())
        {
            list->AddImage(static_cast<ImTextureID>(static_cast<ImU64>(texture.value)), ImVec2(x, y), ImVec2(x + width, y + height));
        }
        DrawHud(list, x, y, width, height, false);
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
        view_->Camera().distance = kBigDistance;
        const std::uint32_t pixelsW = std::max(64u, static_cast<std::uint32_t>(std::min(width, 1600.0f)));
        const std::uint32_t pixelsH = std::max(64u, static_cast<std::uint32_t>(std::min(height, 1000.0f)));
        const myengine::render::TextureHandle texture = view_->Submit(*thumbnails_, id_ + "-big", pixelsW, pixelsH);
        if (texture.IsValid())
        {
            list->AddImage(static_cast<ImTextureID>(static_cast<ImU64>(texture.value)), ImVec2(0.0f, 0.0f), ImVec2(width, height), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 175));
        }
        DrawHud(list, 0.0f, 0.0f, width, height, true);
        list->PopClipRect();
    }
}
