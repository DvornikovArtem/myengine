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

    myengine::game::DoomBrainStats DoomMenuBackdrop::Stats() const
    {
        myengine::game::DoomBrainStats stats;
        if (runner_ != nullptr && connectome_ != nullptr)
        {
            stats.neurons = static_cast<std::uint32_t>(connectome_->NeuronCount());
            stats.spikesPerSecond = static_cast<float>(stats_.spikesPerSecond);
        }
        return stats;
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
                    auto connectome = flybrain::Connectome::Load(path_.u8string(), &error);   // UTF-8, as Load reads it (a path with Russian letters)
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
                    view_->Style().background = {0.0108f, 0.0114f, 0.0125f, 1.0f};   // the ink of the menu
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

        // the picture: a texture of the live views, a frame behind. The brain is in the right 60 % of the window, whole, with a margin around it; on a screen under the
        // main one it is a tenth of the window farther right and dimmer, behind the content.
        const float pictureWidth = width * 0.60f;
        const float pictureX = x + width * 0.40f + width * 0.10f * nested_;
        const float scale = std::min(1.0f, kMaxSide / std::max(pictureWidth, height));
        const std::uint32_t pixelsW = std::max(64u, static_cast<std::uint32_t>(pictureWidth * scale));
        const std::uint32_t pixelsH = std::max(64u, static_cast<std::uint32_t>(height * scale));
        view_->FitCamera(pictureWidth / std::max(height, 1.0f), 0.10f);
        const myengine::render::TextureHandle texture = view_->Submit(*context_.thumbnails, kViewId, pixelsW, pixelsH);
        const float strength = std::clamp(alpha, 0.0f, 1.0f) * shown_ * std::clamp(brightness_, 0.0f, 1.0f);
        if (texture.IsValid() && strength > 0.0f)
        {
            list->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);
            list->AddImage(static_cast<ImTextureID>(static_cast<ImU64>(texture.value)), ImVec2(pictureX, y), ImVec2(pictureX + pictureWidth, y + height), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
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
        UpdateAnchors(x, y, width, height, pictureX, pictureWidth);
    }

    namespace
    {
        // the middle of a set of neurons of the cloud, the median on each axis (a few strays do not move it)
        // `lowFraction` > 0: only the neurons in that lowest part of the cloud (the neck of the descending neurons)
        bool CentreOf(const std::vector<std::uint32_t>& neurons, const std::vector<float>& positions, float out[3], const float lowFraction = 0.0f)
        {
            std::vector<float> axis[3];
            float lowest = -1e30f;
            if (lowFraction > 0.0f)
            {
                std::vector<float> ys;
                for (const std::uint32_t neuron : neurons)
                {
                    if (3u * neuron + 2u < positions.size() && std::fabs(positions[3u * neuron + 1u]) <= 50.0f)
                    {
                        ys.push_back(positions[3u * neuron + 1u]);
                    }
                }
                if (ys.size() >= 8)
                {
                    std::nth_element(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(static_cast<float>(ys.size()) * lowFraction), ys.end());
                    lowest = ys[static_cast<std::size_t>(static_cast<float>(ys.size()) * lowFraction)];
                }
            }
            for (const std::uint32_t neuron : neurons)
            {
                if (3u * neuron + 2u >= positions.size())
                {
                    continue;
                }
                const float* p = positions.data() + 3u * neuron;
                if (std::fabs(p[1]) > 50.0f || (lowFraction > 0.0f && p[1] > lowest))
                {
                    continue;   // not drawn: it has no place in the dataset, or not at the neck
                }
                for (int i = 0; i < 3; ++i)
                {
                    axis[i].push_back(p[i]);
                }
            }
            if (axis[0].size() < 8)
            {
                return false;
            }
            for (int i = 0; i < 3; ++i)
            {
                std::nth_element(axis[i].begin(), axis[i].begin() + static_cast<std::ptrdiff_t>(axis[i].size() / 2), axis[i].end());
                out[i] = axis[i][axis[i].size() / 2];
            }
            return true;
        }
    }

    void DoomMenuBackdrop::UpdateAnchors(const float x, const float y, const float width, const float height, const float pictureX, const float pictureWidth)
    {
        anchors_.clear();
        if (view_ == nullptr || connectome_ == nullptr)
        {
            return;
        }
        const std::vector<float>& positions = view_->Model().positions;
        if (!centresKnown_)
        {
            centresKnown_ = true;   // once: the parts of the brain do not move
            std::vector<std::uint32_t> optic = connectome_->Find("LPLC2").neurons;
            const std::vector<std::uint32_t> lc4 = connectome_->Find("LC4").neurons;
            optic.insert(optic.end(), lc4.begin(), lc4.end());
            const std::vector<std::uint32_t> kc = connectome_->FindPrefix("KC").neurons;
            descending_ = connectome_->FindPrefix("DN").neurons;
            const auto split = [&](const std::vector<std::uint32_t>& all, std::vector<std::uint32_t>* halves)
            {
                for (const std::uint32_t neuron : all)
                {
                    if (3u * neuron < positions.size())
                    {
                        halves[positions[3u * neuron] >= 0.0f ? 0 : 1].push_back(neuron);
                    }
                }
            };
            split(optic, optic_);
            split(kc, mushroom_);
            for (int i = 0; i < 2; ++i)
            {
                if (!CentreOf(optic_[i], positions, opticCentre_[i]))
                {
                    optic_[i].clear();
                }
                if (!CentreOf(mushroom_[i], positions, mushroomCentre_[i]))
                {
                    mushroom_[i].clear();
                }
            }
            if (!CentreOf(descending_, positions, descendingCentre_, 0.15f))
            {
                descending_.clear();
            }
        }
        myengine::render::Matrix4 view;
        myengine::render::Matrix4 projection;
        view_->Camera().Matrices(pictureWidth / std::max(height, 1.0f), view, projection);
        const auto project = [&](const float p[3], myengine::game::DoomBrainAnchor& out) -> bool
        {
            const float v[4] = {p[0], p[1], p[2], 1.0f};
            float eye[4] = {};
            float clip[4] = {};
            for (int c = 0; c < 4; ++c)
            {
                for (int r = 0; r < 4; ++r)
                {
                    eye[c] += v[r] * view.data[static_cast<std::size_t>(r * 4 + c)];
                }
            }
            for (int c = 0; c < 4; ++c)
            {
                for (int r = 0; r < 4; ++r)
                {
                    clip[c] += eye[r] * projection.data[static_cast<std::size_t>(r * 4 + c)];
                }
            }
            if (clip[3] <= 0.01f)
            {
                return false;
            }
            const float nx = clip[0] / clip[3];
            const float ny = clip[1] / clip[3];
            out.x = pictureX + (nx * 0.5f + 0.5f) * pictureWidth;
            out.y = y + (0.5f - ny * 0.5f) * height;
            return out.x >= x && out.x <= x + width && out.y >= y && out.y <= y + height;
        };
        // of two lobes the one that is more to the right (the eyes) or to the left (the mushroom bodies) of the picture carries the label: its shelf runs outwards
        const auto pick = [&](const std::vector<std::uint32_t>* halves, const float (*centres)[3], const bool rightmost, const char* id)
        {
            myengine::game::DoomBrainAnchor best;
            bool found = false;
            for (int i = 0; i < 2; ++i)
            {
                myengine::game::DoomBrainAnchor candidate;
                candidate.id = id;
                if (!halves[i].empty() && project(centres[i], candidate) && (!found || (rightmost ? candidate.x > best.x : candidate.x < best.x)))
                {
                    best = candidate;
                    found = true;
                }
            }
            if (found)
            {
                anchors_.push_back(best);
            }
        };
        pick(optic_, opticCentre_, true, "optic");
        pick(mushroom_, mushroomCentre_, false, "mushroom");
        if (!descending_.empty())
        {
            myengine::game::DoomBrainAnchor neck;
            neck.id = "descending";
            if (project(descendingCentre_, neck))
            {
                anchors_.push_back(neck);
            }
        }
    }
}
