#include <brainview/DoomMenuBackdrop.h>

#include <myengine/editor/ThumbnailService.h>
#include <myengine/render/IRenderAdapter.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace brainview
{
    namespace
    {
        constexpr char kViewId[] = "flydoom-menu";
        constexpr float kMaxSide = 1600.0f;

        std::filesystem::path IfFile(const std::filesystem::path& path)
        {
            std::error_code ec;
            return !path.empty() && std::filesystem::is_regular_file(path, ec) ? path : std::filesystem::path();
        }
    }

    std::filesystem::path DoomMenuBackdrop::FindConnectome(const std::filesystem::path& explicitPath, const std::filesystem::path& exeDirectory, const std::filesystem::path& sourceDirectory)
    {
        if (!explicitPath.empty())
        {
            return explicitPath;
        }
        const std::filesystem::path relative = std::filesystem::path("assets") / "flybrain" / "malecns_v1.fbrain";
        for (const std::filesystem::path& root : {exeDirectory, sourceDirectory})
        {
            if (!root.empty())
            {
                if (const auto found = IfFile(root / relative); !found.empty())
                {
                    return found;
                }
            }
        }
        return exeDirectory.empty() ? relative : exeDirectory / relative;
    }

    DoomMenuBackdrop::DoomMenuBackdrop(const myengine::game::DoomBrainViewContext& context, std::filesystem::path connectomePath) : context_(context), path_(std::move(connectomePath)) {}

    DoomMenuBackdrop::~DoomMenuBackdrop()
    {
        runner_.reset();   // the thread holds the connectome
        if (loading_.valid())
        {
            loading_.wait();
        }
        view_.reset();
    }

    std::string DoomMenuBackdrop::Status() const
    {
        if (runner_ == nullptr)
        {
            return problem_.empty() ? "LOADING" : "";
        }
        return runner_->BackendText().substr(0, runner_->BackendText().find(" ("));   // "GPU" / "CPU": the name of the adapter is for the log
    }

    void DoomMenuBackdrop::Draw(ImDrawList* list, const float x, const float y, const float width, const float height, const double deltaSeconds, const float alpha)
    {
        clock_ += deltaSeconds;
        if (!loadStarted_)
        {
            loadStarted_ = true;
            if (IfFile(path_).empty())
            {
                problem_ = "The connectome is not here, the menu has no brain: " + path_.string();
            }
            else
            {
                loading_ = std::async(std::launch::async, [this]() -> std::shared_ptr<const flybrain::Connectome>
                {
                    std::string error;
                    auto connectome = flybrain::Connectome::Load(path_.string(), &error);
                    loadError_ = connectome != nullptr ? std::string() : error;
                    return connectome;
                });
            }
        }
        if (loading_.valid() && loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            connectome_ = loading_.get();
            if (connectome_ == nullptr)
            {
                problem_ = "The connectome could not be read: " + loadError_;
            }
            else if (context_.render != nullptr && context_.thumbnails != nullptr)
            {
                view_ = std::make_unique<BrainView>(*context_.render);
                if (!view_->Create(*connectome_))
                {
                    problem_ = "The renderer could not make the cloud of the brain.";
                    view_.reset();
                }
                else
                {
                    // slow and wide: a hologram behind the text
                    view_->Camera().autoRotateSpeed = 0.07f;
                    view_->Camera().distance = 3.1f;
                    view_->Camera().pitch = 0.12f;
                    view_->Style().flashBrightness = 0.9f;
                    view_->Style().baseBrightness = 0.045f;
                    BrainRunner::Options options;
                    options.backend = flybrain::BackendChoice::Auto;
                    options.renderDevice = context_.render->GetNativeDevice();
                    for (const BrainGroup& group : view_->Groups())
                    {
                        flybrain::Group members;
                        members.neurons = group.neurons;
                        options.watchGroups.push_back(std::move(members));
                    }
                    runner_ = std::make_unique<BrainRunner>(connectome_, std::move(options));
                    runner_->SetPreset(StimulusPreset::Rest);
                    runner_->Start();
                    if (context_.log)
                    {
                        context_.log("FlyDoom menu: the brain behind the menu runs (" + std::to_string(connectome_->NeuronCount()) + " neurons)");
                    }
                }
            }
        }
        if (view_ == nullptr || runner_ == nullptr)
        {
            return;
        }
        PumpBrain(*runner_, *view_, deltaSeconds, scratch_);
        statsAge_ += deltaSeconds;
        if (statsAge_ >= 0.5)
        {
            stats_ = runner_->GetStats();
            statsAge_ = 0.0;
        }
        shown_ = std::min(1.0f, shown_ + static_cast<float>(deltaSeconds) / 1.5f);

        // the picture: a texture of the live views, a frame behind; not larger than 1600 on a side
        const float scale = std::min(1.0f, kMaxSide / std::max(width, height));
        const std::uint32_t pixelsW = std::max(64u, static_cast<std::uint32_t>(width * scale));
        const std::uint32_t pixelsH = std::max(64u, static_cast<std::uint32_t>(height * scale));
        const myengine::render::TextureHandle texture = view_->Submit(*context_.thumbnails, kViewId, pixelsW, pixelsH);
        const float strength = std::clamp(alpha, 0.0f, 1.0f) * shown_;
        if (texture.IsValid() && strength > 0.0f)
        {
            // the brain sits right of the text of the menu: the picture is moved by a fifth of the window
            list->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);
            const float shift = width * 0.17f;
            list->AddImage(static_cast<ImTextureID>(static_cast<ImU64>(texture.value)), ImVec2(x + shift, y), ImVec2(x + width + shift, y + height), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                           IM_COL32(255, 255, 255, static_cast<int>(strength * 255.0f)));
            // scan lines, as on the monitor of a laboratory: a dark line every fourth pixel at a few percent
            const float step = std::max(3.0f, std::floor(height / 270.0f));
            const ImU32 line = IM_COL32(0, 0, 0, static_cast<int>(34.0f * strength));
            for (float row = y; row < y + height; row += step)
            {
                list->AddLine(ImVec2(x, row), ImVec2(x + width, row), line, 1.0f);
            }
            // a vignette: the corners go dark
            const ImU32 clear = IM_COL32(0, 0, 0, 0);
            const ImU32 dark = IM_COL32(0, 0, 0, static_cast<int>(150.0f * strength));
            const float edge = std::min(width, height) * 0.35f;
            list->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + width, y + edge), dark, dark, clear, clear);
            list->AddRectFilledMultiColor(ImVec2(x, y + height - edge), ImVec2(x + width, y + height), clear, clear, dark, dark);
            list->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + edge, y + height), dark, clear, clear, dark);
            list->AddRectFilledMultiColor(ImVec2(x + width - edge, y), ImVec2(x + width, y + height), clear, dark, dark, clear);
            list->PopClipRect();
        }
    }
}
