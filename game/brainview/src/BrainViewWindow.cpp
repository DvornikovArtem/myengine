#include <brainview/BrainViewWindow.h>

#include <brainview/BrainPump.h>
#include <brainview/BrainViewPanel.h>

#include <myengine/core/AppPaths.h>
#include <myengine/core/Logger.h>
#include <myengine/editor/ThumbnailService.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace brainview
{
    namespace
    {
        constexpr char kViewId[] = "fly-brain";
        constexpr char kDefaultFile[] = "assets/flybrain/malecns_v1.fbrain";

        std::filesystem::path ExistingOrEmpty(const std::filesystem::path& path)
        {
            std::error_code ec;
            return !path.empty() && std::filesystem::is_regular_file(path, ec) ? path : std::filesystem::path();
        }
    }

    std::filesystem::path BrainViewWindow::DefaultConnectomePath()
    {
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "MYENGINE_FBRAIN") == 0 && value != nullptr)
        {
            const std::filesystem::path fromEnvironment = value;
            std::free(value);
            if (!fromEnvironment.empty())
            {
                return fromEnvironment;
            }
        }
        if (const auto found = ExistingOrEmpty(kDefaultFile); !found.empty())
        {
            return found;
        }
        const std::filesystem::path sourceRoot = myengine::core::apppaths::SourceRoot();
        if (!sourceRoot.empty())
        {
            return sourceRoot / kDefaultFile;
        }
        return kDefaultFile;
    }

    BrainViewWindow::BrainViewWindow(myengine::render::IRenderAdapter& adapter, myengine::editor::ThumbnailService& thumbnails, myengine::core::Logger& logger,
                                     std::filesystem::path connectomePath)
        : adapter_(adapter), thumbnails_(thumbnails), logger_(logger), path_(connectomePath.empty() ? DefaultConnectomePath() : std::move(connectomePath))
    {
        const std::string text = path_.generic_u8string();
        std::snprintf(pathBuffer_, sizeof(pathBuffer_), "%s", text.c_str());
        char* backend = nullptr;
        std::size_t backendLength = 0;
        if (_dupenv_s(&backend, &backendLength, "MYENGINE_FLYBRAIN_BACKEND") == 0 && backend != nullptr)
        {
            flybrain::BackendChoice choice = flybrain::BackendChoice::Auto;
            if (flybrain::ParseBackendChoice(backend, choice))
            {
                backend_ = choice == flybrain::BackendChoice::Auto ? 0 : (choice == flybrain::BackendChoice::Gpu ? 1 : 2);
            }
            std::free(backend);
        }
    }

    void BrainViewWindow::StartRunner()
    {
        BrainRunner::Options options;
        options.backend = backend_ == 0 ? flybrain::BackendChoice::Auto : (backend_ == 1 ? flybrain::BackendChoice::Gpu : flybrain::BackendChoice::Cpu);
        options.renderDevice = adapter_.GetNativeDevice();
        for (const BrainGroup& group : view_->Groups())
        {
            flybrain::Group members;
            members.neurons = group.neurons;
            options.watchGroups.push_back(std::move(members));
        }
        runner_ = std::make_unique<BrainRunner>(connectome_, std::move(options));
        // MYENGINE_FLYBRAIN_PRESET=rest|vision|taste|pain|everything|silence picks the first stimulus (automated runs, the evidence of the task)
        char* preset = nullptr;
        std::size_t presetLength = 0;
        if (_dupenv_s(&preset, &presetLength, "MYENGINE_FLYBRAIN_PRESET") == 0 && preset != nullptr)
        {
            const std::string name = preset;
            std::free(preset);
            for (int i = 0; i <= static_cast<int>(StimulusPreset::Everything); ++i)
            {
                std::string lower = ToString(static_cast<StimulusPreset>(i));
                std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower == name)
                {
                    runner_->SetPreset(static_cast<StimulusPreset>(i));
                }
            }
        }
        runner_->Start();
        view_->SetGpuFlash({});
        statsAge_ = 10.0;
    }

    BrainViewWindow::~BrainViewWindow()
    {
        runner_.reset();   // the thread first: it holds the connectome
        if (loading_.valid())
        {
            loading_.wait();
        }
        view_.reset();
    }

    void BrainViewWindow::StartLoading()
    {
        state_ = LoadState::Loading;
        error_.clear();
        const std::string path = path_.u8string();   // Connectome::Load reads UTF-8: the ACP string of a path with Russian letters would throw in u8path
        logger_.Info("Fly Brain: loading " + path);
        loading_ = std::async(std::launch::async, [path, this]() -> std::shared_ptr<const flybrain::Connectome>
        {
            std::string error;
            auto connectome = flybrain::Connectome::Load(path, &error);
            loadingError_ = connectome != nullptr ? std::string() : error;
            return connectome;
        });
    }

    void BrainViewWindow::FinishLoading()
    {
        connectome_ = loading_.get();
        if (connectome_ == nullptr)
        {
            state_ = LoadState::Failed;
            error_ = loadingError_.empty() ? "the file could not be read" : loadingError_;
            logger_.Warning("Fly Brain: " + error_);
            return;
        }
        view_ = std::make_unique<BrainView>(adapter_);
        if (!view_->Create(*connectome_))
        {
            state_ = LoadState::Failed;
            error_ = "the renderer could not make the point cloud (is the shader assets/shaders/brain_cloud.hlsl there?)";
            logger_.Warning("Fly Brain: " + error_);
            view_.reset();
            connectome_.reset();
            return;
        }
        state_ = LoadState::Ready;
        logger_.Info("Fly Brain: " + std::to_string(connectome_->NeuronCount()) + " neurons in the cloud");
    }

    void BrainViewWindow::DrawToolbar()
    {
        BrainRunner& runner = *runner_;
        BrainView& view = *view_;
        int preset = static_cast<int>(runner.Preset());
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::BeginCombo("Stimulus", ToString(static_cast<StimulusPreset>(preset))))
        {
            for (int i = 0; i <= static_cast<int>(StimulusPreset::Everything); ++i)
            {
                if (ImGui::Selectable(ToString(static_cast<StimulusPreset>(i)), i == preset))
                {
                    runner.SetPreset(static_cast<StimulusPreset>(i));
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        {
            static const char* const kBackends[] = {"Auto", "GPU", "CPU"};
            if (ImGui::BeginCombo("Brain on", kBackends[backend_]))
            {
                for (int i = 0; i < 3; ++i)
                {
                    if (ImGui::Selectable(kBackends[i], i == backend_) && i != backend_)
                    {
                        backend_ = i;
                        restartRunner_ = true;   // a new run on the other backend
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        bool paused = runner.Paused();
        if (ImGui::Checkbox("Pause", &paused))
        {
            runner.SetPaused(paused);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Rotate", &view.Camera().autoRotate);
        ImGui::SameLine();
        ImGui::Checkbox("HUD", &showHud_);
        ImGui::SameLine();
        ImGui::Checkbox("Raster", &showRaster_);
        ImGui::SameLine();
        ImGui::Checkbox("Look", &showLook_);
        if (showLook_)
        {
            ViewStyle& style = view.Style();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Fade", &style.decaySeconds, 0.03f, 0.6f, "%.2f s");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Rest", &style.baseBrightness, 0.0f, 0.2f, "%.3f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Flash", &style.flashBrightness, 0.5f, 16.0f, "%.1f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Size", &style.baseSizePixels, 0.8f, 5.0f, "%.1f px");
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Glow", &style.bloomIntensity, 0.0f, 3.0f, "%.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Exposure", &style.exposure, 0.3f, 3.0f, "%.2f");
            ImGui::SameLine();
            ImGui::Checkbox("Bloom", &style.bloom);
        }
    }

    void BrainViewWindow::Draw(bool& open, const double deltaSeconds)
    {
        // closed: the simulation stops (a closed window does not cost the CPU)
        if (!open)
        {
            if (wasOpen_ && runner_ != nullptr)
            {
                runner_->Stop();
                runner_.reset();
                if (view_ != nullptr)
                {
                    view_->ClearActivity();
                }
            }
            wasOpen_ = false;
            return;
        }
        wasOpen_ = true;

        if (state_ == LoadState::NotLoaded && !path_.empty() && !ExistingOrEmpty(path_).empty())
        {
            StartLoading();
        }
        if (state_ == LoadState::Loading && loading_.valid() && loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            FinishLoading();
        }
        if (state_ == LoadState::Ready && runner_ != nullptr && restartRunner_)
        {
            runner_->Stop();
            runner_.reset();
            view_->ClearActivity();
            restartRunner_ = false;
        }
        if (state_ == LoadState::Ready && runner_ == nullptr)
        {
            StartRunner();
        }

        // the spikes of the brain into the view, before the picture of this frame is described
        if (state_ == LoadState::Ready && runner_ != nullptr)
        {
            PumpBrain(*runner_, *view_, deltaSeconds, pump_);
            statsAge_ += deltaSeconds;
            if (statsAge_ >= 0.25)
            {
                stats_ = runner_->GetStats();
                statsAge_ = 0.0;
            }
        }

        // the first size fits the window of the editor, whatever its size
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 0.06f * viewport->Size.x, viewport->Pos.y + 0.12f * viewport->Size.y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(std::min(1000.0f, 0.78f * viewport->Size.x), std::min(700.0f, 0.78f * viewport->Size.y)), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Fly Brain", &open))
        {
            ImGui::End();
            return;
        }

        if (state_ == LoadState::Loading || state_ == LoadState::NotLoaded)
        {
            if (state_ == LoadState::Loading)
            {
                ImGui::Text("Loading the connectome ...");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "The connectome is not here: %s", path_.string().c_str());
                ImGui::TextWrapped("It is not in the repository (CC BY 4.0, 154 MB). Download and convert it once: tools\\flybrain\\fetch_malecns.ps1, then open this window again, or give the file here.");
                ImGui::InputText("##fbrain", pathBuffer_, sizeof(pathBuffer_));
                ImGui::SameLine();
                if (ImGui::Button("Load"))
                {
                    path_ = std::filesystem::u8path(pathBuffer_);
                    state_ = LoadState::NotLoaded;
                }
            }
        }
        else if (state_ == LoadState::Failed)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "Fly Brain could not start: %s", error_.c_str());
            if (ImGui::Button("Try again"))
            {
                state_ = LoadState::NotLoaded;
            }
        }
        else if (view_ != nullptr && runner_ != nullptr)
        {
            DrawToolbar();
            ImVec2 available = ImGui::GetContentRegionAvail();
            available.x = std::max(available.x, 64.0f);
            available.y = std::max(available.y, 64.0f);
            const myengine::render::TextureHandle texture =
                view_->Submit(thumbnails_, kViewId, static_cast<uint32_t>(available.x), static_cast<uint32_t>(available.y));
            BrainPanelInfo info;
            info.subtitle = connectome_ != nullptr ? "MaleCNS v1.0 (CC BY 4.0), LIF model of Shiu et al. 2024, brain on " + runner_->BackendText() : "";
            info.simSeconds = stats_.simSeconds;
            info.realtimeRatio = stats_.realtimeRatio;
            info.spikesPerSecond = stats_.spikesPerSecond;
            info.showHud = showHud_;
            info.showRaster = showRaster_;
            DrawBrainViewPanel(*view_, texture.IsValid() ? static_cast<std::uint64_t>(texture.value) : 0ull, available.x, available.y, info);
        }
        ImGui::End();
    }
}
