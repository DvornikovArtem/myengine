// FlyModes.cpp

#include <doomfly/FlyModes.h>

#include <doomfly/FlyControls.h>
#include <doomfly/FlyPlayer.h>

#include <imgui/imgui.h>

#include <algorithm>
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
            FlyController(std::unique_ptr<FlyPlayer> player, std::string title, std::function<void(const std::string&)> log)
                : player_(std::move(player)), title_(std::move(title)), log_(std::move(log))
            {
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
                const float size = std::clamp(std::floor(height / 40.0f), 11.0f, 22.0f);
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
                    cursor += size * 1.25f;
                    const float barHeight = size * 0.5f;
                    list->AddRectFilled(ImVec2(left, cursor), ImVec2(left + inner, cursor + barHeight), IM_COL32(40, 40, 52, 255));
                    const float fraction = std::clamp(value / maxValue, 0.0f, 1.0f);
                    list->AddRectFilled(ImVec2(left, cursor), ImVec2(left + inner * fraction, cursor + barHeight), color);
                    cursor += barHeight + size * 0.55f;
                };
                // a bar that goes both ways (turn): the middle is zero
                auto signedBar = [&](const std::string& label, const float value, const float limit, const std::string& unit)
                {
                    char number[64];
                    std::snprintf(number, sizeof(number), "%+.2f %s", value, unit.c_str());
                    list->AddText(font, size, ImVec2(left, cursor), dim, label.c_str());
                    list->AddText(font, size, ImVec2(left + inner - size * 6.5f, cursor), text, number);
                    cursor += size * 1.25f;
                    const float barHeight = size * 0.5f;
                    const float middle = left + inner * 0.5f;
                    list->AddRectFilled(ImVec2(left, cursor), ImVec2(left + inner, cursor + barHeight), IM_COL32(40, 40, 52, 255));
                    const float fraction = std::clamp(value / limit, -1.0f, 1.0f);
                    list->AddRectFilled(ImVec2(std::min(middle, middle + inner * 0.5f * fraction), cursor), ImVec2(std::max(middle, middle + inner * 0.5f * fraction), cursor + barHeight),
                                        fraction < 0 ? amber : green);
                    list->AddLine(ImVec2(middle, cursor - 2.0f), ImVec2(middle, cursor + barHeight + 2.0f), text, 1.0f);
                    cursor += barHeight + size * 0.55f;
                };

                line("FLY PLAYS", red, 1.5f);
                line(status.controlsName + " neurons", dim);
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
                cursor += size * 0.3f;

                // the reinforcement and the health of the brain
                bar("dopamine PPL101 (mV)", status.dopamineMv, 4.0f, status.dopamineMv > 0.0f ? red : dim);
                char brain[128];
                std::snprintf(brain, sizeof(brain), "spikes %.0f k/s   tics %llu", status.spikesPerSecond / 1000.0f, static_cast<unsigned long long>(status.tics));
                line(brain, text);
                std::snprintf(brain, sizeof(brain), "resets %u   runaway %u   hurt %u", status.resets, status.seizures, status.damages);
                line(brain, status.seizures > 0 ? amber : dim);

                // the place of the view of the brain (task V0): a square at the bottom of the panel; the spikes of the last tic are in player.Snapshot()
                const float square = std::min(inner, std::max(0.0f, y + height - cursor - pad));
                if (square > size * 4.0f)
                {
                    const float top = y + height - pad - square;
                    list->AddRect(ImVec2(left, top), ImVec2(left + square, top + square), IM_COL32(70, 70, 90, 255));
                    list->AddText(font, size * 0.9f, ImVec2(left + size * 0.5f, top + size * 0.5f), IM_COL32(90, 90, 110, 255), "brain view");
                    const BrainSnapshot snapshot = player_->Snapshot();
                    // a raster of the last spikes by the index of the neuron: a first sign of life until the view comes
                    for (const flybrain::SpikeEvent& spike : snapshot.spikes)
                    {
                        const float u = static_cast<float>(spike.neuron % 997) / 997.0f;
                        const float v = static_cast<float>(spike.neuron / 997 % 167) / 167.0f;
                        list->AddRectFilled(ImVec2(left + u * square, top + v * square), ImVec2(left + u * square + 2.0f, top + v * square + 2.0f), IM_COL32(120, 200, 255, 160));
                    }
                }
            }

        private:
            std::unique_ptr<FlyPlayer> player_;
            std::string title_;
            std::function<void(const std::string&)> log_;
            std::uint64_t nextLogTic_ = 0;
            doom::DoomGame* attached_ = nullptr;
        };

        std::unique_ptr<myengine::game::IDoomController> MakeFly(const std::shared_ptr<Shared>& shared, const char* controlsFile, const char* title, std::string& error)
        {
            const FlyModeOptions& options = shared->options;
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
            config.seed = options.seed;
            config.reward = options.reward;
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
            return std::make_unique<FlyController>(std::move(player), title, options.log);
        }
    }

    bool ConnectomeAvailable(const FlyModeOptions& options)
    {
        std::error_code ec;
        return fs::is_regular_file(FindFile(options, options.connectome, "malecns_v1.fbrain"), ec);
    }

    std::vector<myengine::game::DoomMode> MakeModes(const FlyModeOptions& options, const bool withFly)
    {
        std::vector<myengine::game::DoomMode> modes;
        modes.push_back({"Human plays", {}});
        if (withFly)
        {
            auto shared = std::make_shared<Shared>();
            shared->options = options;
            modes.push_back({"Fly plays (DOOMFLY neurons: DNp20, DNpe017)", [shared](std::string& error) { return MakeFly(shared, "controls_doomfly.json", "Fly plays (DOOMFLY neurons)", error); }});
            modes.push_back({"Fly plays (Bio neurons: DNa02, DNp09: stands still)", [shared](std::string& error) { return MakeFly(shared, "controls_bio.json", "Fly plays (Bio neurons)", error); }});
        }
        return modes;
    }
}
