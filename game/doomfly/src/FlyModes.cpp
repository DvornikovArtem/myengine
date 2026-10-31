// FlyModes.cpp

#include <doomfly/FlyModes.h>

#include <doomfly/FlyControls.h>
#include <doomfly/FlyPlayer.h>
#include <doomfly/FlySwarm.h>

#include <brainview/BrainLayout.h>
#include <brainview/PlayerBrainPanel.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <tuple>
#include <chrono>
#include <iterator>

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

        // The panel of one brain: the neurons the controls read, what the player does, the health of the brain. `header` is the big title.
        void DrawFlyPanel(const FlyPlayer& fly, const std::string& header, ImDrawList* list, const float x, const float y, const float width, const float height)
        {
            const FlyStatus status = fly.Status();
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

            line(header, red, 1.5f);
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
            bar(fly.Learns() ? "dopamine PPL1 (mV)" : "dopamine PPL101 (mV)", status.dopamineMv, fly.Learns() ? 15.0f : 4.0f, status.dopamineMv > 0.0f ? red : dim);
            if (fly.Learns())
            {
                char learned[128];
                std::snprintf(learned, sizeof(learned), "KC %.0f spikes/s   learned weight %.4f", status.kcSpikesPerSecond, status.learnedScale);
                line(learned, text);
                line("our bridge: picture -> visual inputs of KCs; MBON in the turn: our weight", dim);
                line("the KC -> MBON synapses learn, but do not change the walls", dim);
            }
            if (status.wallSense)
            {
                char wall[128];
                std::snprintf(wall, sizeof(wall), "OUR LAYER: wall ahead %.0f %%   brake %.0f %%   learned from %u", status.wallChance * 100.0f, status.wallBrake * 100.0f, status.wallPositives);
                line(wall, amber);
                line("WallSense: our layer (a memory of pictures), not the connectome", dim);
            }
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
                const BrainSnapshot snapshot = fly.Snapshot();
                // a raster of the last spikes by the index of the neuron: a first sign of life until the view comes
                for (const flybrain::SpikeEvent& spike : snapshot.spikes)
                {
                    const float u = static_cast<float>(spike.neuron % 997) / 997.0f;
                    const float v = static_cast<float>(spike.neuron / 997 % 167) / 167.0f;
                    list->AddRectFilled(ImVec2(left + u * square, top + v * square), ImVec2(left + u * square + 2.0f, top + v * square + 2.0f), IM_COL32(120, 200, 255, 160));
                }
            }
        }

        // The 3D brain of one player (V1, docs/flybrain/brain-view.md): the cloud, the groups of neurons that are coloured in it, what the panel says about the brain. The
        // controller of a single fly owns one, the duel one per brain.
        class FlyBrainView
        {
        public:
            FlyBrainView(FlyPlayer& player, myengine::render::IRenderAdapter* render, myengine::editor::ThumbnailService* thumbnails, const std::string& id) : player_(player)
            {
                if (render != nullptr && thumbnails != nullptr)
                {
                    panel_ = std::make_unique<brainview::PlayerBrainPanel>(render, thumbnails, id);
                    BuildGroups();
                }
            }

            bool Valid() const { return panel_ != nullptr; }

            // What the brain did since the last frame, for the picture of the cloud: the spikes of the last tic on the CPU, or the buffer of the last spike steps of the brain
            // itself on the GPU (it lives on the device of the renderer; the ring of a feed is good until the end of the frame, so it is not kept)
            void Update(const double deltaSeconds)
            {
                if (panel_ == nullptr)
                {
                    return;
                }
                const BrainFeedSnapshot feed = player_.BrainFeed();
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
                if (panel_->View() == nullptr)
                {
                    frame.groups = groups_;   // until the view is made
                }
                frame.simSecondsNow = static_cast<double>(feed.step) * dtSeconds;
                frame.generation = feed.generation;
                const FlyStatus status = player_.Status();
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

            // The panel of a single brain: the groups of neurons by what they do, with the colours of the cloud
            void Describe(myengine::game::DoomPanelData& out, const std::string& title) const
            {
                const FlyStatus status = player_.Status();
                char text[160];
                out.title = title;
                out.subtitle = status.controlsName + " neurons";
                std::snprintf(text, sizeof(text), "%.1fx real time", status.brainSpeed);
                out.chip = text;
                out.brainChip = "BRAIN " + status.backend.substr(0, status.backend.find(' '));
                myengine::game::DoomPanelGroup motor;
                motor.title = "MOTOR";
                myengine::game::DoomPanelGroup vision;
                vision.title = "VISION";
                for (const Entry& entry : entries_)
                {
                    float hz = 0.0f;
                    for (const RateReading& reading : status.rates)
                    {
                        if (reading.name == entry.name)
                        {
                            hz = reading.hz;
                        }
                    }
                    myengine::game::DoomPanelBar bar;
                    bar.label = entry.name;
                    std::snprintf(text, sizeof(text), "%.0f Hz", hz);
                    bar.text = text;
                    bar.value = hz;
                    bar.limit = 60.0f;
                    bar.rgb = Rgb(entry.color);
                    (entry.vision ? vision : motor).bars.push_back(std::move(bar));
                }
                if (!motor.bars.empty())
                {
                    out.groups.push_back(std::move(motor));
                }
                out.groups.push_back(Output("OUTPUT"));
                // N4: what learns goes first (the panel is cut at the foot when it is long), the eyes last
                std::vector<myengine::game::DoomPanelGroup> learned;
                if (player_.Learns())
                {
                    // N3b: what the mushroom body sees and what it has learned (the bridge into the KCs and the MBON turn are ours, docs/flybrain/flybrain.md)
                    myengine::game::DoomPanelGroup learning;
                    learning.title = "MUSHROOM BODY";
                    myengine::game::DoomPanelBar kc;
                    kc.label = "Kenyon cells";
                    std::snprintf(text, sizeof(text), "%.0f spikes/s", status.kcSpikesPerSecond);
                    kc.text = text;
                    kc.value = status.kcSpikesPerSecond;
                    kc.limit = 2000.0f;
                    learning.bars.push_back(std::move(kc));
                    myengine::game::DoomPanelBar scale;
                    scale.label = "KC -> MBON weight";
                    std::snprintf(text, sizeof(text), "%.4f", status.learnedScale);
                    scale.text = text;
                    scale.value = static_cast<float>(status.learnedScale);
                    scale.limit = 1.0f;
                    learning.bars.push_back(std::move(scale));
                    learned.push_back(std::move(learning));
                    out.debug.push_back("our bridge: picture -> visual inputs of KCs; MBON in the turn: our weight");
                    out.debug.push_back("the KC -> MBON synapses learn, but do not change the walls");
                }
                if (status.wallSense)
                {
                    // N4: our layer (WallSense), not the connectome: what it sees coming and how much it brakes
                    myengine::game::DoomPanelGroup layer;
                    layer.title = "WALLSENSE: OUR LAYER (PICTURE MEMORY), NOT THE CONNECTOME";
                    myengine::game::DoomPanelBar chance;
                    chance.label = "wall ahead";
                    std::snprintf(text, sizeof(text), "%.0f %%", status.wallChance * 100.0f);
                    chance.text = text;
                    chance.value = status.wallChance;
                    chance.limit = 1.0f;
                    chance.rgb = 0xe65a3c;
                    layer.bars.push_back(std::move(chance));
                    myengine::game::DoomPanelBar brake;
                    brake.label = "brake";
                    std::snprintf(text, sizeof(text), "%.0f %%", status.wallBrake * 100.0f);
                    brake.text = text;
                    brake.value = status.wallBrake;
                    brake.limit = 1.0f;
                    brake.rgb = 0xf0b43c;
                    layer.bars.push_back(std::move(brake));
                    myengine::game::DoomPanelBar seen;
                    seen.label = "before a wall";
                    std::snprintf(text, sizeof(text), "%u pictures", status.wallPositives);
                    seen.text = text;
                    seen.value = static_cast<float>(status.wallPositives);
                    seen.limit = 2000.0f;
                    seen.rgb = 0x5ac86e;
                    layer.bars.push_back(std::move(seen));
                    learned.insert(learned.begin(), std::move(layer));
                }
                out.groups.insert(out.groups.begin(), std::make_move_iterator(learned.begin()), std::make_move_iterator(learned.end()));
                if (!vision.bars.empty())
                {
                    out.groups.push_back(std::move(vision));
                }
                // the dim lines at the foot (they are for the one who debugs the brain, not for the player)
                std::snprintf(text, sizeof(text), "brain %.2fx   game %.2fx   tic %llu", status.brainSpeed, status.gameSpeed, static_cast<unsigned long long>(status.tics));
                out.debug.push_back(text);
                std::snprintf(text, sizeof(text), "%.0f k spikes/s   resets %u   runaway %u   hurt %u", status.spikesPerSecond / 1000.0f, status.resets, status.seizures, status.damages);
                out.debug.push_back(text);
                if (status.waiting)
                {
                    out.debug.push_back("the game waits for the brain");
                }
                if (status.reflex)
                {
                    out.debug.push_back("a reflex of the legs, not the brain");
                }
                // the line of the big brain: the neurons, the spikes, the speed
                std::string neurons = std::to_string(player_.GetConnectome().NeuronCount());
                for (std::size_t at = neurons.size(); at > 3; at -= 3)
                {
                    neurons.insert(at - 3, " ");
                }
                std::snprintf(text, sizeof(text), "%s neurons   %.2f M spikes/s   %.2fx real time", neurons.c_str(), status.spikesPerSecond / 1.0e6f, status.brainSpeed);
                out.brainLine = text;
            }

            // What the player does and the pain, as a group of the panel
            myengine::game::DoomPanelGroup Output(const std::string& title) const
            {
                const FlyStatus status = player_.Status();
                char text[160];
                myengine::game::DoomPanelGroup action;
                action.title = title;
                myengine::game::DoomPanelBar turn;
                turn.label = "turn";
                std::snprintf(text, sizeof(text), "%+.2f deg/tic", status.turnDegrees);
                turn.text = text;
                turn.value = status.turnDegrees;
                turn.limit = 6.0f;
                turn.centered = true;
                turn.rgb = 0x46e08a;
                action.bars.push_back(std::move(turn));
                myengine::game::DoomPanelBar forward;
                forward.label = "forward";
                std::snprintf(text, sizeof(text), "%.0f", std::fabs(status.forwardUnits));
                forward.text = text;
                forward.value = std::fabs(status.forwardUnits);
                forward.limit = 50.0f;
                forward.rgb = 0x46e08a;
                action.bars.push_back(std::move(forward));
                myengine::game::DoomPanelBar fire;
                fire.label = "fire";
                fire.text = status.fire ? "ON" : "off";
                fire.value = status.fire ? 1.0f : 0.0f;
                fire.limit = 1.0f;
                fire.rgb = Rgb(brainview::kColorFire);
                action.bars.push_back(std::move(fire));
                myengine::game::DoomPanelBar dopamine;
                dopamine.label = "dopamine (pain)";
                std::snprintf(text, sizeof(text), "%.1f mV", status.dopamineMv);
                dopamine.text = text;
                dopamine.value = status.dopamineMv;
                dopamine.limit = 4.0f;
                dopamine.rgb = Rgb(brainview::kColorDopamine);
                action.bars.push_back(std::move(dopamine));
                return action;
            }

            void DrawCloud(ImDrawList* list, const float x, const float y, const float width, const float height)
            {
                if (panel_ != nullptr)
                {
                    panel_->DrawPanel(list, x, y, width, height);
                }
            }
            bool Big() const { return panel_ != nullptr && panel_->Big(); }
            void ToggleBig()
            {
                if (panel_ != nullptr)
                {
                    panel_->ToggleBig();
                }
            }
            void DrawOverlay(ImDrawList* list, const float width, const float height, const int count, const int slot)
            {
                if (panel_ != nullptr)
                {
                    panel_->DrawOverlay(list, width, height, count, slot);
                }
            }
            std::string Backend() const
            {
                const std::string backend = player_.Status().backend;
                return backend.substr(0, backend.find(' '));
            }

        private:
            struct Entry
            {
                std::string name;      // as the rates name it: "DNp20 R", "DNpe017"
                std::uint8_t color = 0;
                bool vision = false;
            };

            // The 0xRRGGBB of a colour of the cloud for a bar of the panel: the palette is HDR (the groups glow), the panel wants the colour
            static std::uint32_t Rgb(const std::uint8_t colorIndex)
            {
                float palette[brainview::kPaletteSize][4];
                brainview::FillPalette(palette);
                const float* c = palette[colorIndex];
                const float peak = std::max({c[0], c[1], c[2], 1.0f});
                auto channel = [&](const float v) { return static_cast<std::uint32_t>(std::clamp(v / peak, 0.0f, 1.0f) * 255.0f + 0.5f); };
                return (channel(c[0]) << 16) | (channel(c[1]) << 8) | channel(c[2]);
            }

            // The neurons that the controls read, grouped as the panel lists them, and the groups that colour them in the cloud (with the photoreceptors and the dopamine neurons)
            void BuildGroups()
            {
                const flybrain::Connectome& connectome = player_.GetConnectome();
                const FlyControls& controls = player_.Controls();
                auto add = [&](const ControlTerm& term, const std::uint8_t color, const bool vision)
                {
                    const std::string name = term.type + (term.side == flybrain::Side::Left ? " L" : term.side == flybrain::Side::Right ? " R" : "");
                    for (const Entry& entry : entries_)
                    {
                        if (entry.name == name)
                        {
                            return;
                        }
                    }
                    brainview::BrainGroup group;
                    group.id = name;
                    group.label = name;
                    group.colorIndex = color;
                    group.neurons = connectome.Find(term.type, term.side).neurons;
                    if (group.neurons.empty())
                    {
                        return;
                    }
                    groups_.push_back(std::move(group));
                    entries_.push_back({name, color, vision});
                };
                auto side = [](const ControlTerm& term, const std::uint8_t fallback)
                {
                    return term.side == flybrain::Side::Left ? static_cast<std::uint8_t>(brainview::kColorTurnLeft)
                                                              : term.side == flybrain::Side::Right ? static_cast<std::uint8_t>(brainview::kColorTurnRight) : fallback;
                };
                for (const ControlTerm& term : controls.turn.terms)
                {
                    add(term, side(term, brainview::kColorTurnLeft), false);
                }
                for (const ControlTerm& term : controls.forward.terms)
                {
                    add(term, brainview::kColorForward, false);
                }
                for (const ControlTerm& term : controls.fire.terms)
                {
                    add(term, brainview::kColorFire, false);
                }
                for (const ControlTerm& term : controls.fire.spikeTerms)
                {
                    add(term, brainview::kColorFire, false);
                }
                for (const ControlTerm& term : controls.show)
                {
                    add(term, term.type == "LC4" ? brainview::kColorVision : brainview::kColorEyes, true);
                }
                for (const auto& [id, label, color, neurons] : {std::make_tuple("dopamine", "Dopamine (pain)", brainview::kColorDopamine, connectome.FindPrefix("PPL1").neurons),
                                                                 std::make_tuple("eyes", "Eyes (R1-R6)", brainview::kColorEyes, connectome.Find("R1-R6").neurons)})
                {
                    if (!neurons.empty())
                    {
                        groups_.push_back({id, label, static_cast<std::uint8_t>(color), neurons});
                    }
                }
            }

            FlyPlayer& player_;
            std::unique_ptr<brainview::PlayerBrainPanel> panel_;
            std::vector<brainview::BrainGroup> groups_;
            std::vector<Entry> entries_;
            std::uint64_t lastFeedStep_ = ~0ull;
            std::chrono::steady_clock::time_point stepSeen_ = std::chrono::steady_clock::now();
        };

        class FlyController final : public myengine::game::IDoomController
        {
        public:
            // `fastest` (N4, Fly learns, the speed MAX): the game runs as fast as the brain can (still one tic of the brain per tic of the game), the speed it reaches
            // is shown; `mark`: the line over the curve of learning on the picture ("LEARNS from scratch", "TRAINED"), empty: no curve
            FlyController(std::unique_ptr<FlyPlayer> player, std::string title, std::function<void(const std::string&)> log, myengine::render::IRenderAdapter* render,
                          myengine::editor::ThumbnailService* thumbnails, const bool fastest = false, std::string mark = {})
                : player_(std::move(player)), title_(std::move(title)), log_(std::move(log)), fastest_(fastest), mark_(std::move(mark))
            {
                view_ = std::make_unique<FlyBrainView>(*player_, render, thumbnails, "fly-brain");
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
                bool frame = false;
                if (fastest_)
                {
                    // as fast as the brain: tic after tic for 40 ms of the frame (the window keeps drawing), at least one
                    const auto begin = std::chrono::steady_clock::now();
                    do
                    {
                        frame = player_->StepBlocking(game) || frame;
                        Curve(game);
                    } while (std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() < 0.040);
                }
                else
                {
                    frame = player_->Pump(game, deltaSeconds);
                    if (frame)
                    {
                        Curve(game);
                    }
                }
                view_->Update(deltaSeconds);
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
                DrawFlyPanel(*player_, "FLY PLAYS", list, x, y, width, height);
            }

            // with a renderer the window draws the panel in the style of the menu from this data (the cloud of the brain in it); without one DrawPanel is the panel
            bool DescribePanel(myengine::game::DoomPanelData& out) const override
            {
                if (!view_->Valid())
                {
                    return false;
                }
                view_->Describe(out, player_->Learns() ? "FLY LEARNS" : "FLY PLAYS");
                return true;
            }
            void DrawBrain(ImDrawList* list, const float x, const float y, const float width, const float height) override { view_->DrawCloud(list, x, y, width, height); }
            bool BigBrainOpen() const override { return view_->Big(); }
            void ToggleBigBrain() override { view_->ToggleBig(); }
            void DrawOverlay(ImDrawList* list, const float width, const float height) override
            {
                if (!mark_.empty() && !view_->Big())
                {
                    DrawCurve(list, height);
                }
                view_->DrawOverlay(list, width, height, 1, 0);
            }

        private:
            static constexpr int kCurveBinTics = 35 * 15;   // a point of the curve: 15 s of the game
            static constexpr std::size_t kCurvePoints = 48;

            // N4: the curve of learning: the share of the time pushing against a wall in every 15 s of the game, and how long every life lasted
            void Curve(doom::DoomGame& game)
            {
                const FlyStatus status = player_->Status();
                const doom::PlayerState player = game.Player(game.Info().consolePlayer);
                if (player.inGame && player.health > 0)
                {
                    ++lifeTics_;
                }
                else if (lifeTics_ > 0)
                {
                    lives_.push_back(lifeTics_ / 35.0f);
                    lifeTics_ = 0;
                }
                if (status.levelTics >= curveLevelTics_ + kCurveBinTics)
                {
                    atWall_.push_back(100.0f * static_cast<float>(status.wallTics - curveWallTics_) / static_cast<float>(status.levelTics - curveLevelTics_));
                    curveWallTics_ = status.wallTics;
                    curveLevelTics_ = status.levelTics;
                }
                if (atWall_.size() > kCurvePoints)
                {
                    atWall_.erase(atWall_.begin());
                }
                if (lives_.size() > kCurvePoints)
                {
                    lives_.erase(lives_.begin());
                }
            }

            // Over the top left of the picture: the speed, what the fly is (learning / trained), and the two curves
            void DrawCurve(ImDrawList* list, const float height) const
            {
                ImFont* font = ImGui::GetFont();
                const float size = std::clamp(std::floor(height / 52.0f), 12.0f, 20.0f);
                const float w = size * 22.0f;
                const float graph = size * 3.6f;
                const float x0 = size * 0.8f;
                const float y0 = size * 0.8f;
                const float h = size * 4.2f + 2.0f * (graph + size * 1.6f);
                const ImU32 text = IM_COL32(230, 230, 230, 255);
                const ImU32 dim = IM_COL32(150, 150, 160, 255);
                const ImU32 amber = IM_COL32(240, 180, 60, 255);
                list->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(8, 8, 12, 200), size * 0.3f);
                float cursor = y0 + size * 0.5f;
                char line[160];
                std::snprintf(line, sizeof(line), "x%.1f", player_->Status().gameSpeed);
                list->AddText(font, size * 2.0f, ImVec2(x0 + size * 0.6f, cursor), amber, line);
                list->AddText(font, size * 0.8f, ImVec2(x0 + size * 5.0f, cursor + size * 0.6f), dim,
                              fastest_ ? "speed MAX: as fast as the brain thinks" : "speed x1: real time (when the brain keeps up)");
                cursor += size * 2.2f;
                const FlyStatus status = player_->Status();
                std::snprintf(line, sizeof(line), "%s   %.1f min of the game", mark_.c_str(), status.levelTics / 35.0 / 60.0);
                list->AddText(font, size, ImVec2(x0 + size * 0.6f, cursor), text, line);
                cursor += size * 1.5f;
                auto plot = [&](const char* title, const std::vector<float>& values, const ImU32 color)
                {
                    const float left = x0 + size * 0.6f;
                    const float right = x0 + w - size * 0.6f;
                    float top = 1.0f;
                    for (const float v : values)
                    {
                        top = std::max(top, v);
                    }
                    std::snprintf(line, sizeof(line), "%s (max %.0f)", title, top);
                    list->AddText(font, size * 0.85f, ImVec2(left, cursor), dim, line);
                    cursor += size * 1.1f;
                    list->AddRectFilled(ImVec2(left, cursor), ImVec2(right, cursor + graph), IM_COL32(30, 30, 40, 220));
                    const float step = (right - left) / static_cast<float>(kCurvePoints);
                    for (std::size_t i = 0; i < values.size(); ++i)
                    {
                        const float bar = graph * std::clamp(values[i] / top, 0.0f, 1.0f);
                        list->AddRectFilled(ImVec2(left + step * i + 1.0f, cursor + graph - bar), ImVec2(left + step * (i + 1), cursor + graph), color);
                    }
                    cursor += graph + size * 0.5f;
                };
                plot("% of the time at a wall, every 15 s", atWall_, IM_COL32(230, 90, 60, 255));
                plot("life, s, every life", lives_, IM_COL32(90, 200, 110, 255));
            }

            std::unique_ptr<FlyPlayer> player_;
            std::unique_ptr<FlyBrainView> view_;
            std::string title_;
            std::function<void(const std::string&)> log_;
            std::uint64_t nextLogTic_ = 0;
            doom::DoomGame* attached_ = nullptr;
            bool fastest_ = false;
            std::string mark_;
            std::vector<float> atWall_;
            std::vector<float> lives_;
            std::uint64_t curveWallTics_ = 0;
            std::uint64_t curveLevelTics_ = 0;
            int lifeTics_ = 0;
        };

        // The connectome is read once for all the modes of a run; "" when it is there, otherwise the text of the error
        bool EnsureConnectome(const std::shared_ptr<Shared>& shared, std::string& error)
        {
            const FlyModeOptions& options = shared->options;
            if (shared->connectome)
            {
                return true;
            }
            const fs::path path = FindFile(options, options.connectome, "malecns_v1.fbrain");
            std::error_code ec;
            if (!fs::is_regular_file(path, ec))
            {
                error = "the connectome is not found: " + path.string() + "\nrun tools\\flybrain\\fetch_malecns.ps1 (docs/flybrain/flybrain.md)";
                return false;
            }
            if (options.log)
            {
                options.log("fly: reading the connectome " + path.string());
            }
            shared->connectome = flybrain::Connectome::Load(path.u8string(), &error);
            return shared->connectome != nullptr;
        }

        // One brain: `index` is its player in the game (-1: the console player), `seedOffset` makes the noise of two brains different
        std::unique_ptr<FlyPlayer> CreateFly(const std::shared_ptr<Shared>& shared, const myengine::game::DoomLaunchChoice& choice, const int index, const bool ownView,
                                             const bool drivesScreens, const std::uint64_t seedOffset, std::string& error, const bool learns = false)
        {
            const FlyModeOptions& options = shared->options;
            const bool bio = choice.controlSet == "bio" && !learns;
            const char* controlsFile = learns ? "controls_learn.json" : bio ? "controls_bio.json" : "controls_doomfly.json";
            if (!EnsureConnectome(shared, error))
            {
                return nullptr;
            }
            FlyPlayerConfig config;
            config.connectome = shared->connectome;
            config.seed = (choice.seed != 1 ? choice.seed : options.seed) + seedOffset;
            config.reward = options.reward;
            config.log = options.log;
            config.playerIndex = index;
            config.ownView = ownView;
            config.drivesScreens = drivesScreens;
            if (!flybrain::ParseBackendChoice(choice.backend, config.backend))
            {
                error = "unknown backend \"" + choice.backend + "\" (auto, cpu, gpu)";
                return nullptr;
            }
            config.renderDevice = options.renderDevice ? options.renderDevice() : nullptr;
            if (learns)
            {
                // N3b: the KC -> MBON synapses learn; N4: Fly learns always starts from scratch (no memory file) and our layer WallSense learns next to them
                config.learning.enabled = true;
                config.wallSense.enabled = true;
                config.wallSense.learning = true;
            }
            else if (!options.untrained && choice.trainedFly)
            {
                // N4: the other modes play with the trained fly: our layer learned offline (tests/doomfly/FlyLearnMeasure.cpp --wall-out), it only plays here
                std::error_code ec;
                const fs::path trained = FindFile(options, {}, "fly_trained.bin");
                if (fs::is_regular_file(trained, ec))
                {
                    // it goes on learning in the session (a new map in tens of seconds), the file is not written
                    config.wallSense.enabled = true;
                    config.wallSense.learning = true;
                    config.wallSenseLoad = {trained.u8string()};
                }
                else if (options.log)
                {
                    options.log("fly: no trained fly (" + trained.string() + "), the fly plays untrained");
                }
            }
            const fs::path controls = FindFile(options, {}, controlsFile);
            if (!FlyControls::Load(controls, config.controls, error))
            {
                return nullptr;
            }
            auto player = FlyPlayer::Create(config, error);
            if (player && options.log)
            {
                const EyesInfo& info = player->Eyes().Info();
                options.log(std::string("fly: ") + (learns ? "learning DOOMFLY + MBON" : bio ? "Bio" : "DOOMFLY") + " neurons, controls " + controls.string() + "; photoreceptors " + std::to_string(info.receptors[0]) +
                            " left / " + std::to_string(info.receptors[1]) + " right, columns covered " + std::to_string(info.columnsCovered[0]) + " of " + std::to_string(info.columns[0]) +
                            " / " + std::to_string(info.columnsCovered[1]) + " of " + std::to_string(info.columns[1]));
            }
            return player;
        }

        std::unique_ptr<myengine::game::IDoomController> MakeFly(const std::shared_ptr<Shared>& shared, const myengine::game::DoomLaunchChoice& choice, std::string& error,
                                                                 const bool learns = false)
        {
            const bool bio = choice.controlSet == "bio";
            auto player = CreateFly(shared, choice, -1, false, true, 0, error, learns);
            if (!player)
            {
                return nullptr;
            }
            const char* title = learns ? "Fly learns" : bio ? "Fly plays (Bio neurons)" : "Fly plays (DOOMFLY neurons)";
            // N4: the curve of learning over the picture: Fly learns (from scratch, MAX speed with --fly-fast), and the trained fly
            const bool trained = player->WallSenseOn() && !learns;
            const std::string mark = learns ? "LEARNS from scratch (our layer + KC -> MBON)" : trained ? "BUNDLED MEMORY (per map, WallSense)" : "";
            return std::make_unique<FlyController>(std::move(player), title, shared->options.log, choice.render, choice.thumbnails, learns && (shared->options.fastest || choice.fastest), mark);
        }

        // ---------------------------------------------------------------------------------------------------------------------------------------
        // Human vs Fly and Fly vs Fly: a deathmatch of two in one game ("-deathmatch -players 2")

        constexpr int kFragLimit = 10;                 // the round ends at this many frags ...
        constexpr int kRoundTicsDefault = 35 * 180;    // ... or after three minutes: the one with more frags wins, equal is a draw
        constexpr int kBannerTics = 35 * 3;

        // MYENGINE_DUEL_ROUND_TICS=<tics> shortens the round (the captures and the checks of the end of a round)
        int RoundTics()
        {
            char* text = nullptr;
            std::size_t length = 0;
            int value = kRoundTicsDefault;
            if (_dupenv_s(&text, &length, "MYENGINE_DUEL_ROUND_TICS") == 0 && text != nullptr)
            {
                value = std::max(35, std::atoi(text));
                std::free(text);
            }
            return value;
        }

        class DuelController final : public myengine::game::IDoomController, private doom::ITicCmdSource
        {
        public:
            // `flies`: the brains with their players (1 for Human vs Fly, 0 and 1 for Fly vs Fly)
            DuelController(std::vector<std::unique_ptr<FlyPlayer>> flies, std::vector<int> players, const bool humanPlays, std::function<void(const std::string&)> log,
                           myengine::render::IRenderAdapter* render, myengine::editor::ThumbnailService* thumbnails)
                : flies_(std::move(flies)), players_(std::move(players)), humanPlays_(humanPlays), log_(std::move(log))
            {
                for (std::size_t i = 0; i < flies_.size(); ++i)
                {
                    views_.push_back(std::make_unique<FlyBrainView>(*flies_[i], render, thumbnails, "duel-brain-" + std::to_string(i)));
                }
            }

            ~DuelController() override
            {
                if (attached_ != nullptr)
                {
                    attached_->SetTicCmdSource(nullptr);
                }
            }

            bool HumanPlays() const override { return humanPlays_; }

            const myengine::game::SecondaryView* Secondary() const override { return secondary_.pixels.empty() ? nullptr : &secondary_; }

            bool Advance(doom::DoomGame& game, const double deltaSeconds) override
            {
                if (attached_ != &game)
                {
                    attached_ = &game;
                    game.SetTicCmdSource(this);
                }
                for (const auto& fly : flies_)
                {
                    fly->Begin(game, deltaSeconds);
                }
                bool ready = true;
                for (const auto& fly : flies_)
                {
                    ready = fly->Poll(game) && ready;
                }
                for (const auto& view : views_)
                {
                    view->Update(deltaSeconds);
                }
                bool frame = false;
                if (ready)
                {
                    frame = game.StepTic();
                    for (const auto& fly : flies_)
                    {
                        fly->EndTic(game);
                    }
                    UpdateRound(game);
                    UpdateSecondary();
                    Log(game);
                }
                return frame;
            }

            std::string Status() const override
            {
                char text[200];
                std::snprintf(text, sizeof(text), "%s %d : %d, brain speed %.2fx", Title(), score_[0], score_[1], flies_.empty() ? 0.0 : flies_.back()->Status().brainSpeed);
                return text;
            }

            void DrawPanel(ImDrawList* list, const float x, const float y, const float width, const float height) override
            {
                ImFont* font = ImGui::GetFont();
                const float size = std::clamp(std::floor(height / 40.0f), 11.0f, 22.0f);
                const float pad = size * 0.8f;
                const float headHeight = size * 8.2f;
                list->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + headHeight), IM_COL32(14, 14, 20, 255));
                list->AddLine(ImVec2(x, y), ImVec2(x, y + headHeight), IM_COL32(60, 60, 75, 255), 2.0f);
                float cursor = y + pad;
                const float left = x + pad;
                list->AddText(font, size * 1.5f, ImVec2(left, cursor), IM_COL32(230, 70, 50, 255), humanPlays_ ? "HUMAN VS FLY" : "FLY VS FLY");
                cursor += size * 2.1f;
                char text[160];
                std::snprintf(text, sizeof(text), "%s  %d : %d  %s", humanPlays_ ? "YOU" : "FLY 1", frags_[0], frags_[1], humanPlays_ ? "FLY" : "FLY 2");
                list->AddText(font, size * 1.7f, ImVec2(left, cursor), IM_COL32(240, 240, 240, 255), text);
                cursor += size * 2.2f;
                std::snprintf(text, sizeof(text), "to %d frags or 3 min   rounds won %d : %d", kFragLimit, wins_[0], wins_[1]);
                list->AddText(font, size * 0.85f, ImVec2(left, cursor), IM_COL32(150, 150, 160, 255), text);
                cursor += size * 1.3f;
                if (winner_ >= 0)
                {
                    const char* name = winner_ == 2 ? "DRAW" : (winner_ == 0 ? (humanPlays_ ? "YOU WIN" : "FLY 1 WINS") : (humanPlays_ ? "THE FLY WINS" : "FLY 2 WINS"));
                    list->AddText(font, size * 1.4f, ImVec2(left, cursor), IM_COL32(240, 180, 60, 255), name);
                }
                else
                {
                    std::snprintf(text, sizeof(text), "time %d:%02d", (roundTicsLimit_ - roundTics_) / 35 / 60, (roundTicsLimit_ - roundTics_) / 35 % 60);
                    list->AddText(font, size * 1.1f, ImVec2(left, cursor), IM_COL32(200, 200, 210, 255), text);
                }
                const float rest = height - headHeight;
                const float each = flies_.empty() ? 0.0f : rest / static_cast<float>(flies_.size());
                for (size_t i = 0; i < flies_.size(); ++i)
                {
                    std::string name = humanPlays_ ? "FLY PLAYS" : (i == 0 ? "FLY 1" : "FLY 2");
                    DrawFlyPanel(*flies_[i], name, list, x, y + headHeight + each * static_cast<float>(i), width, each);
                }
            }

            // with a renderer the window draws the panel in the style of the menu: the score and the time, the clouds of both brains side by side, what each player does
            bool DescribePanel(myengine::game::DoomPanelData& out) const override
            {
                for (const auto& view : views_)
                {
                    if (!view->Valid())
                    {
                        return false;
                    }
                }
                if (views_.empty())
                {
                    return false;
                }
                char text[200];
                out.title = humanPlays_ ? "HUMAN VS FLY" : "FLY VS FLY";
                std::snprintf(text, sizeof(text), "%s  %d : %d  %s", humanPlays_ ? "YOU" : "FLY 1", frags_[0], frags_[1], humanPlays_ ? "FLY" : "FLY 2");
                out.subtitle = text;
                if (winner_ >= 0)
                {
                    out.chip = winner_ == 2 ? "DRAW" : (winner_ == 0 ? (humanPlays_ ? "YOU WIN" : "FLY 1 WINS") : (humanPlays_ ? "THE FLY WINS" : "FLY 2 WINS"));
                }
                else
                {
                    std::snprintf(text, sizeof(text), "%d:%02d", (roundTicsLimit_ - roundTics_) / 35 / 60, (roundTicsLimit_ - roundTics_) / 35 % 60);
                    out.chip = text;
                }
                out.brainChip = "BRAIN " + views_.front()->Backend();
                for (std::size_t i = 0; i < views_.size(); ++i)
                {
                    out.groups.push_back(views_[i]->Output(humanPlays_ ? "THE FLY" : (i == 0 ? "FLY 1" : "FLY 2")));
                }
                std::snprintf(text, sizeof(text), "to %d frags or 3 min   rounds won %d : %d", kFragLimit, wins_[0], wins_[1]);
                out.debug.push_back(text);
                std::snprintf(text, sizeof(text), "brain %.2fx", flies_.back()->Status().brainSpeed);
                out.debug.push_back(text);
                std::snprintf(text, sizeof(text), "%zu brains", views_.size());
                out.brainLine = text;
                return true;
            }
            // the clouds of the brains side by side in the rect
            void DrawBrain(ImDrawList* list, const float x, const float y, const float width, const float height) override
            {
                const float each = width / static_cast<float>(std::max<std::size_t>(views_.size(), 1));
                for (std::size_t i = 0; i < views_.size(); ++i)
                {
                    views_[i]->DrawCloud(list, x + each * static_cast<float>(i), y, each, height);
                }
            }
            bool BigBrainOpen() const override { return !views_.empty() && views_.front()->Big(); }
            void ToggleBigBrain() override
            {
                for (const auto& view : views_)
                {
                    view->ToggleBig();
                }
            }
            void DrawOverlay(ImDrawList* list, const float width, const float height) override
            {
                for (std::size_t i = 0; i < views_.size(); ++i)
                {
                    views_[i]->DrawOverlay(list, width, height, static_cast<int>(views_.size()), static_cast<int>(i));
                }
            }

        private:
            const char* Title() const { return humanPlays_ ? "Human vs Fly" : "Fly vs Fly"; }

            // doom::ITicCmdSource: the brains steer their players, the others (the human) keep what DOOM built from the keyboard
            void BuildTicCmd(const int player, doom::TicCmd& cmd) override
            {
                for (size_t i = 0; i < flies_.size(); ++i)
                {
                    if (players_[i] == player)
                    {
                        cmd = flies_[i]->Command();
                    }
                }
            }

            // The round is over: the winner by the frags (equal: a draw)
            void FinishRound(const char* why)
            {
                winner_ = frags_[0] == frags_[1] ? 2 : (frags_[0] > frags_[1] ? 0 : 1);
                if (winner_ < 2)
                {
                    ++wins_[winner_];
                }
                ++rounds_;
                bannerLeft_ = kBannerTics;
                if (log_)
                {
                    char text[200];
                    std::snprintf(text, sizeof(text), "duel: round %d over (%s), frags %d : %d, %s; rounds won %d : %d", rounds_, why, frags_[0], frags_[1],
                                  winner_ == 2 ? "draw" : (winner_ == 0 ? "player 0 wins" : "player 1 wins"), wins_[0], wins_[1]);
                    log_(text);
                }
            }

            void NewRound(doom::DoomGame& game)
            {
                game.RestartLevel();
                winner_ = -1;
                roundTics_ = 0;
                frags_[0] = frags_[1] = 0;
            }

            void UpdateRound(doom::DoomGame& game)
            {
                const doom::GameInfo info = game.Info();
                // somebody used the exit of the level: that is the end of the round too, DOOM shows the intermission, the same level starts again after it
                if (info.state == doom::GameState::Intermission || info.state == doom::GameState::Finale)
                {
                    if (!inIntermission_)
                    {
                        inIntermission_ = true;
                        bannerLeft_ = kBannerTics;
                        if (winner_ < 0)
                        {
                            FinishRound("the exit of the level");
                        }
                    }
                    if (--bannerLeft_ <= 0)
                    {
                        inIntermission_ = false;
                        NewRound(game);
                    }
                    return;
                }
                inIntermission_ = false;
                const doom::PlayerState p0 = game.Player(0);
                const doom::PlayerState p1 = game.Player(1);
                if (info.state != doom::GameState::Level || !p0.inGame || !p1.inGame)
                {
                    return;
                }
                frags_[0] = p0.frags;
                frags_[1] = p1.frags;
                if (winner_ < 0)
                {
                    ++roundTics_;
                    if (frags_[0] >= kFragLimit || frags_[1] >= kFragLimit)
                    {
                        FinishRound("frag limit");
                    }
                    else if (roundTics_ >= roundTicsLimit_)
                    {
                        FinishRound("time");
                    }
                }
                else if (--bannerLeft_ <= 0)
                {
                    NewRound(game);
                }
            }

            void UpdateSecondary()
            {
                const FlyPlayer& fly = *flies_.back();     // Human vs Fly: the fly; Fly vs Fly: the second fly
                const std::vector<std::uint32_t>& picture = fly.EyesPicture();
                if (picture.size() != static_cast<size_t>(doom::kScreenWidth) * doom::kScreenHeight)
                {
                    return;
                }
                secondary_.width = doom::kScreenWidth;
                secondary_.height = doom::kScreenHeight;
                secondary_.pixels = picture;
                ++secondary_.serial;
                secondary_.mainLabel = humanPlays_ ? "YOU" : "FLY 1";
                secondary_.label = humanPlays_ ? "THE EYES OF THE FLY" : "FLY 2";
            }

            void Log(doom::DoomGame& game)
            {
                const FlyStatus status = flies_.back()->Status();
                if (!log_ || status.tics < nextLogTic_)
                {
                    return;
                }
                nextLogTic_ = status.tics + 350;
                const doom::PlayerState p0 = game.Player(0);
                const doom::PlayerState p1 = game.Player(1);
                const doom::GameInfo info = game.Info();
                char text[340];
                std::snprintf(text, sizeof(text), "duel: tic %llu (game tic %d, state %d%s), brain %.2fx, game %.2fx, p0 health %d frags %d at %.0f %.0f, p1 health %d frags %d at %.0f %.0f, spikes %.0f k/s, resets %u",
                              static_cast<unsigned long long>(status.tics), info.gameTic, static_cast<int>(info.state), info.menuActive ? ", menu" : "", status.brainSpeed, status.gameSpeed, p0.health,
                              p0.frags, p0.x, p0.y, p1.health, p1.frags, p1.x, p1.y, status.spikesPerSecond / 1000.0f, status.resets);
                log_(text);
            }

            std::vector<std::unique_ptr<FlyPlayer>> flies_;
            std::vector<std::unique_ptr<FlyBrainView>> views_;   // one cloud per brain (empty cloud without a renderer)
            std::vector<int> players_;
            bool humanPlays_ = false;
            std::function<void(const std::string&)> log_;
            doom::DoomGame* attached_ = nullptr;
            myengine::game::SecondaryView secondary_;
            std::uint64_t nextLogTic_ = 0;
            int frags_[2] = {0, 0};
            int score_[2] = {0, 0};
            int wins_[2] = {0, 0};
            int rounds_ = 0;
            int roundTics_ = 0;
            int roundTicsLimit_ = RoundTics();
            int winner_ = -1;                          // -1: playing, 0 / 1: the player, 2: a draw
            int bannerLeft_ = 0;
            bool inIntermission_ = false;
        };

        std::unique_ptr<myengine::game::IDoomController> MakeDuel(const std::shared_ptr<Shared>& shared, const myengine::game::DoomLaunchChoice& choice, const bool humanVsFly,
                                                                  std::string& error)
        {
            std::vector<std::unique_ptr<FlyPlayer>> flies;
            std::vector<int> players;
            if (humanVsFly)
            {
                flies.push_back(CreateFly(shared, choice, 1, true, false, 0, error));
                players.push_back(1);
            }
            else
            {
                flies.push_back(CreateFly(shared, choice, 0, true, true, 0, error));
                players.push_back(0);
                if (flies.back())
                {
                    flies.push_back(CreateFly(shared, choice, 1, true, false, 1000, error));
                    players.push_back(1);
                }
            }
            for (const auto& fly : flies)
            {
                if (!fly)
                {
                    return nullptr;
                }
            }
            return std::make_unique<DuelController>(std::move(flies), std::move(players), humanVsFly, shared->options.log, choice.render, choice.thumbnails);
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

        DoomMode learn;
        learn.id = "fly-learn";
        learn.title = "Fly learns";
        learn.description = "The fly starts from scratch: WallSense, our picture-memory layer, learns from wall contacts. "
                            "KC -> MBON synapses also learn, but have not been shown to improve wall avoidance.";
        learn.available = withFly;
        learn.unavailableReason = withFly ? "" : noBrain;
        learn.make = [shared](const DoomLaunchChoice& choice, std::string& error) { return MakeFly(shared, choice, error, true); };
        modes.push_back(std::move(learn));

        DoomMode versus;
        versus.id = "human-vs-fly";
        versus.title = "Human vs Fly";
        versus.description = "A deathmatch of two: you against a fly. Its eyes are on the right of your picture.";
        versus.available = withFly;
        versus.unavailableReason = withFly ? "" : noBrain;
        versus.doomArgs = {"-deathmatch", "-nomonsters", "-players", "2", "-warp", "1", "1"};
        versus.make = [shared](const DoomLaunchChoice& choice, std::string& error) { return MakeDuel(shared, choice, true, error); };
        modes.push_back(std::move(versus));

        DoomMode duel;
        duel.id = "fly-vs-fly";
        duel.title = "Fly vs Fly";
        duel.description = "Two brains play a deathmatch against each other, you watch.";
        duel.available = withFly;
        duel.unavailableReason = withFly ? "" : noBrain;
        duel.doomArgs = {"-deathmatch", "-nomonsters", "-players", "2", "-warp", "1", "1"};
        duel.make = [shared](const DoomLaunchChoice& choice, std::string& error) { return MakeDuel(shared, choice, false, error); };
        modes.push_back(std::move(duel));

        DoomMode swarm;
        swarm.id = "swarm";
        swarm.title = "Swarm";
        swarm.description = "You play, 8-32 flies attack in waves; the brain of every fly is a piece of the connectome.";
        std::string swarmReason;
        swarm.available = SwarmAvailable(options, swarmReason);
        swarm.unavailableReason = swarmReason;
        swarm.doomArgs = SwarmDoomArgs();
        swarm.make = [options](const DoomLaunchChoice& choice, std::string& error) { return MakeSwarmController(options, choice.flies, choice.backend, choice.seed, error); };
        modes.push_back(std::move(swarm));
        return modes;
    }
}
