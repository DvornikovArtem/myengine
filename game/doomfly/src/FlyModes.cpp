// FlyModes.cpp

#include <doomfly/FlyModes.h>

#include <doomfly/FlyControls.h>
#include <doomfly/FlyPlayer.h>

#include <brainview/PlayerBrainPanel.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>

namespace doomfly
{
    namespace
    {
        namespace fs = std::filesystem;

        fs::path FindFile(const FlyModeOptions& options, const fs::path& explicitPath, const char* name)
        {
            std::error_code ec;
            if (!explicitPath.empty())
            {
                return explicitPath;
            }
            std::vector<fs::path> candidates;
            if (!options.exeDirectory.empty())
            {
                candidates.push_back(options.exeDirectory / "assets" / "flybrain" / name);
            }
            if (!options.sourceDirectory.empty())
            {
                candidates.push_back(options.sourceDirectory / "assets" / "flybrain" / name);
            }
            for (const fs::path& candidate : candidates)
            {
                if (fs::is_regular_file(candidate, ec))
                {
                    return candidate;
                }
            }
            return candidates.empty() ? fs::path(name) : candidates.front();
        }

        // One connectome for all the modes of a run: reading it takes a fraction of a second, its memory is 154 MB
        struct Shared
        {
            FlyModeOptions options;
            std::shared_ptr<const flybrain::Connectome> connectome;
        };

        class FlyController final : public myengine::game::IDoomController
        {
        public:
            FlyController(std::unique_ptr<FlyPlayer> player, std::string title, std::function<void(const std::string&)> log, myengine::render::IRenderAdapter* render,
                          myengine::editor::ThumbnailService* thumbnails)
                : player_(std::move(player)), title_(std::move(title)), log_(std::move(log))
            {
                if (render != nullptr && thumbnails != nullptr)
                {
                    panel_ = std::make_unique<brainview::PlayerBrainPanel>(render, thumbnails, "fly-brain");
                }
            }

            ~FlyController() override
            {
                if (attached_ != nullptr)
                {
                    player_->Detach(*attached_);
                }
            }

            bool Advance(doom::DoomGame& game, const double deltaSeconds) override
            {
                if (attached_ != &game)
                {
                    attached_ = &game;
                    player_->Attach(game);
                }
                const bool frame = player_->Pump(game, deltaSeconds);
                UpdateBrainView(deltaSeconds);
                // a line of the log every 10 seconds of the game: where the fly is and how the brain keeps up
                const FlyStatus status = player_->Status();
                if (log_ && status.tics >= nextLogTic_)
                {
                    nextLogTic_ = status.tics + 350;
                    const doom::PlayerState player = game.Player(game.Info().consolePlayer);
                    char text[256];
                    std::snprintf(text, sizeof(text), "fly: tic %llu, brain %.2fx, game %.2fx, at %.0f %.0f angle %.0f, health %d, ammo-shots %s, spikes %.0f k/s, resets %u, runaway %u, hurt %u",
                                  static_cast<unsigned long long>(status.tics), status.brainSpeed, status.gameSpeed, player.x, player.y, player.angleDegrees, player.health,
                                  status.fire ? "firing" : "-", status.spikesPerSecond / 1000.0f, status.resets, status.seizures, status.damages);
                    log_(text);
                }
                return frame;
            }

            std::string Status() const override
            {
                char text[160];
                const FlyStatus status = player_->Status();
                std::snprintf(text, sizeof(text), "%s, brain speed %.2fx", title_.c_str(), status.brainSpeed);
                return text;
            }

            void DrawPanel(ImDrawList* list, const float x, const float y, const float width, const float height) override
            {
                const FlyStatus status = player_->Status();
                ImFont* font = ImGui::GetFont();
                // the cloud of the brain (V1) keeps the lower part of the panel; the text above it is set to what is left
                const float cloudHeight = panel_ != nullptr ? std::min(width * 0.95f, height * 0.40f) : 0.0f;
                const float size = std::clamp(std::floor((height - cloudHeight) / 40.0f), 9.0f, 22.0f);
                const float pad = size * 0.8f;
                const ImU32 text = IM_COL32(225, 225, 225, 255);
                const ImU32 dim = IM_COL32(140, 140, 150, 255);
                const ImU32 red = IM_COL32(230, 70, 50, 255);
                const ImU32 green = IM_COL32(90, 200, 110, 255);
                const ImU32 amber = IM_COL32(240, 180, 60, 255);

                list->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), IM_COL32(14, 14, 20, 255));
                list->AddLine(ImVec2(x, y), ImVec2(x, y + height), IM_COL32(60, 60, 75, 255), 2.0f);
                float cursor = y + pad;
                const float left = x + pad;
                const float inner = width - 2.0f * pad;
                auto line = [&](const std::string& value, const ImU32 color, const float scale = 1.0f)
                {
                    list->AddText(font, size * scale, ImVec2(left, cursor), color, value.c_str());
                    cursor += size * scale * 1.35f;
                };
                auto bar = [&](const std::string& label, const float value, const float maxValue, const ImU32 color)
                {
                    char number[48];
                    std::snprintf(number, sizeof(number), "%5.1f", value);
                    list->AddText(font, size, ImVec2(left, cursor), dim, label.c_str());
                    list->AddText(font, size, ImVec2(left + inner - size * 3.2f, cursor), text, number);
                    cursor += size * 1.1f;
                    const float barHeight = size * 0.3f;
                    list->AddRectFilled(ImVec2(left, cursor), ImVec2(left + inner, cursor + barHeight), IM_COL32(40, 40, 52, 255));
                    const float fraction = std::clamp(value / maxValue, 0.0f, 1.0f);
                    list->AddRectFilled(ImVec2(left, cursor), ImVec2(left + inner * fraction, cursor + barHeight), color);
                    cursor += barHeight + size * 0.3f;
                };
                // a bar that goes both ways (turn): the middle is zero
                auto signedBar = [&](const std::string& label, const float value, const float limit, const std::string& unit)
                {
                    char number[64];
                    std::snprintf(number, sizeof(number), "%+.2f %s", value, unit.c_str());
                    list->AddText(font, size, ImVec2(left, cursor), dim, label.c_str());
                    list->AddText(font, size, ImVec2(left + inner - size * 6.5f, cursor), text, number);
                    cursor += size * 1.1f;
                    const float barHeight = size * 0.3f;
                    const float middle = left + inner * 0.5f;
                    list->AddRectFilled(ImVec2(left, cursor), ImVec2(left + inner, cursor + barHeight), IM_COL32(40, 40, 52, 255));
                    const float fraction = std::clamp(value / limit, -1.0f, 1.0f);
                    list->AddRectFilled(ImVec2(std::min(middle, middle + inner * 0.5f * fraction), cursor), ImVec2(std::max(middle, middle + inner * 0.5f * fraction), cursor + barHeight),
                                        fraction < 0 ? amber : green);
                    list->AddLine(ImVec2(middle, cursor - 2.0f), ImVec2(middle, cursor + barHeight + 2.0f), text, 1.0f);
                    cursor += barHeight + size * 0.3f;
                };

                line("FLY PLAYS", red, 1.5f);
                line(status.controlsName + " neurons", dim);
                line("brain on the " + status.backend, dim);
                cursor += size * 0.4f;

                // the speed of the brain: 1.0 is real time
                char speed[96];
                std::snprintf(speed, sizeof(speed), "brain speed %.2fx   game %.2fx", status.brainSpeed, status.gameSpeed);
                line(speed, status.brainSpeed >= 0.95 ? green : amber);
                if (status.waiting)
                {
                    line("the game waits for the brain", amber);
                }
                cursor += size * 0.3f;

                // the neurons the controls read
                for (const RateReading& reading : status.rates)
                {
                    bar(reading.name + " (Hz)", reading.hz, 60.0f, IM_COL32(100, 160, 240, 255));
                }
                cursor += size * 0.2f;

                // what the player does
                signedBar("turn", status.turnDegrees, 6.0f, "deg/tic");
                bar("forward", std::fabs(status.forwardUnits), 50.0f, green);
                line(std::string("fire ") + (status.fire ? "ON" : "off") + "   use " + (status.use ? "ON" : "off"), status.fire ? red : dim);
                line(status.reflex ? "REFLEX of the legs, not the brain" : "the brain steers", status.reflex ? amber : dim);
                cursor += size * 0.3f;

                // the reinforcement and the health of the brain
                bar("dopamine PPL101 (mV)", status.dopamineMv, 4.0f, status.dopamineMv > 0.0f ? red : dim);
                char brain[128];
                std::snprintf(brain, sizeof(brain), "spikes %.0f k/s   tics %llu", status.spikesPerSecond / 1000.0f, static_cast<unsigned long long>(status.tics));
                line(brain, text);
                std::snprintf(brain, sizeof(brain), "resets %u   runaway %u   hurt %u", status.resets, status.seizures, status.damages);
                line(brain, status.seizures > 0 ? amber : dim);

                // the brain in 3D (V1, docs/flybrain/brain-view.md) in what is left of the panel; Tab shows it over the whole window
                const float room = cloudHeight > 0.0f ? cloudHeight - pad : std::max(0.0f, y + height - cursor - pad);
                if (room > size * 4.0f)
                {
                    const float top = y + height - pad - room;
                    if (panel_ != nullptr)
                    {
                        panel_->DrawPanel(list, left, top, inner, room);
                    }
                    else
                    {
                        list->AddRect(ImVec2(left, top), ImVec2(left + inner, top + room), IM_COL32(70, 70, 90, 255));
                    }
                }
            }

            void ToggleBigBrain() override
            {
                if (panel_ != nullptr)
                {
                    panel_->ToggleBig();
                }
            }

            void DrawOverlay(ImDrawList* list, const float width, const float height) override
            {
                if (panel_ != nullptr)
                {
                    panel_->DrawOverlay(list, width, height);
                }
            }

        private:
            // What the brain of the player did since the last frame, for the picture of the cloud: the spikes of the last tic on the CPU, or the buffer of the last spike steps
            // of the brain itself on the GPU (it lives on the device of the renderer; the ring of a feed is good until the end of the frame, so it is not kept)
            void UpdateBrainView(const double deltaSeconds)
            {
                if (panel_ == nullptr)
                {
                    return;
                }
                const BrainFeedSnapshot feed = player_->BrainFeed();
                if (feed.connectome == nullptr)
                {
                    return;
                }
                const double dtSeconds = flybrain::ModelParams{}.dtMs / 1000.0;
                const auto now = std::chrono::steady_clock::now();
                if (feed.step != lastFeedStep_)
                {
                    lastFeedStep_ = feed.step;
                    stepSeen_ = now;
                }
                brainview::BrainFeedFrame frame;
                frame.connectome = feed.connectome;
                frame.simSecondsNow = static_cast<double>(feed.step) * dtSeconds;
                frame.generation = feed.generation;
                const FlyStatus status = player_->Status();
                frame.spikesPerSecond = status.spikesPerSecond;
                frame.brainSpeed = status.brainSpeed;
                if (feed.backend == flybrain::Backend::Gpu && feed.ring.lastSpikeStep != nullptr)
                {
                    frame.gpu = true;
                    frame.backend = "GPU";
                    frame.gpuFlash.buffer = feed.ring.lastSpikeStep;
                    frame.gpuFlash.fence = feed.ring.fence;
                    frame.gpuFlash.fenceValue = feed.ring.fenceValue;
                    frame.gpuFlash.stepWhole = static_cast<std::uint32_t>(feed.step);
                    frame.gpuFlash.stepSeconds = static_cast<float>(dtSeconds);
                    // the step was known `since` seconds ago: the flashes go on fading between two tics of the game
                    const double since = std::min(std::chrono::duration<double>(now - stepSeen_).count(), 0.25);
                    frame.gpuFlash.stepFraction = static_cast<float>(since / dtSeconds);
                }
                else
                {
                    frame.backend = "CPU";
                    frame.spikes.reserve(feed.spikes.size());
                    for (const flybrain::SpikeEvent& spike : feed.spikes)
                    {
                        frame.spikes.push_back({spike.neuron, static_cast<double>(spike.step) * dtSeconds});
                    }
                }
                panel_->Update(frame, deltaSeconds);
            }

            std::unique_ptr<FlyPlayer> player_;
            std::unique_ptr<brainview::PlayerBrainPanel> panel_;
            std::uint64_t lastFeedStep_ = ~0ull;
            std::chrono::steady_clock::time_point stepSeen_ = std::chrono::steady_clock::now();
            std::string title_;
            std::function<void(const std::string&)> log_;
            std::uint64_t nextLogTic_ = 0;
            doom::DoomGame* attached_ = nullptr;
        };

        std::unique_ptr<myengine::game::IDoomController> MakeFly(const std::shared_ptr<Shared>& shared, const myengine::game::DoomLaunchChoice& choice, std::string& error)
        {
            const FlyModeOptions& options = shared->options;
            const bool bio = choice.controlSet == "bio";
            const char* controlsFile = bio ? "controls_bio.json" : "controls_doomfly.json";
            const char* title = bio ? "Fly plays (Bio neurons)" : "Fly plays (DOOMFLY neurons)";
            if (!shared->connectome)
            {
                const fs::path path = FindFile(options, options.connectome, "malecns_v1.fbrain");
                std::error_code ec;
                if (!fs::is_regular_file(path, ec))
                {
                    error = "the connectome is not found: " + path.string() + "\nrun tools\\flybrain\\fetch_malecns.ps1 (docs/flybrain/flybrain.md)";
                    return nullptr;
                }
                if (options.log)
                {
                    options.log("fly: reading the connectome " + path.string());
                }
                shared->connectome = flybrain::Connectome::Load(path.u8string(), &error);
                if (!shared->connectome)
                {
                    return nullptr;
                }
            }
            FlyPlayerConfig config;
            config.connectome = shared->connectome;
            config.seed = choice.seed != 1 ? choice.seed : options.seed;
            config.reward = options.reward;
            config.log = options.log;
            if (!flybrain::ParseBackendChoice(choice.backend, config.backend))
            {
                error = "unknown backend \"" + choice.backend + "\" (auto, cpu, gpu)";
                return nullptr;
            }
            config.renderDevice = options.renderDevice ? options.renderDevice() : nullptr;
            const fs::path controls = FindFile(options, {}, controlsFile);
            if (!FlyControls::Load(controls, config.controls, error))
            {
                return nullptr;
            }
            auto player = FlyPlayer::Create(config, error);
            if (!player)
            {
                return nullptr;
            }
            if (options.log)
            {
                const EyesInfo& info = player->Eyes().Info();
                options.log(std::string("fly: ") + title + ", controls " + controls.string() + "; photoreceptors " + std::to_string(info.receptors[0]) + " left / " +
                            std::to_string(info.receptors[1]) + " right, columns covered " + std::to_string(info.columnsCovered[0]) + " of " + std::to_string(info.columns[0]) + " / " +
                            std::to_string(info.columnsCovered[1]) + " of " + std::to_string(info.columns[1]));
            }
            return std::make_unique<FlyController>(std::move(player), title, options.log, choice.render, choice.thumbnails);
        }
    }

    bool ConnectomeAvailable(const FlyModeOptions& options)
    {
        std::error_code ec;
        return fs::is_regular_file(FindFile(options, options.connectome, "malecns_v1.fbrain"), ec);
    }

    std::vector<myengine::game::DoomMode> MakeModes(const FlyModeOptions& options, const bool withFly)
    {
        using myengine::game::DoomLaunchChoice;
        using myengine::game::DoomMode;
        std::vector<DoomMode> modes;
        const std::string noBrain = "no connectome: tools\\flybrain\\fetch_malecns.ps1";

        DoomMode human;
        human.id = "human";
        human.title = "Human plays";
        human.description = "You play DOOM with the keyboard and the mouse.";
        modes.push_back(std::move(human));

        auto shared = std::make_shared<Shared>();
        shared->options = options;
        DoomMode fly;
        fly.id = "fly";
        fly.title = "Fly plays";
        fly.description = "The brain of a fly (164 789 neurons of the MaleCNS connectome) plays: the picture goes in through the eyes, the descending neurons steer.";
        fly.available = withFly;
        fly.unavailableReason = withFly ? "" : noBrain;
        fly.make = [shared](const DoomLaunchChoice& choice, std::string& error) { return MakeFly(shared, choice, error); };
        modes.push_back(std::move(fly));

        for (const char* id : {"human-vs-fly", "fly-vs-fly", "swarm"})
        {
            DoomMode later;
            later.id = id;
            later.title = std::string(id) == "human-vs-fly" ? "Human vs Fly" : std::string(id) == "fly-vs-fly" ? "Fly vs Fly" : "Swarm";
            later.description = std::string(id) == "human-vs-fly" ? "A deathmatch against a fly." : std::string(id) == "fly-vs-fly" ? "Two brains play against each other, you watch." : "8-32 flies hunt you.";
            later.available = false;
            later.unavailableReason = "soon";
            modes.push_back(std::move(later));
        }
        return modes;
    }
}
