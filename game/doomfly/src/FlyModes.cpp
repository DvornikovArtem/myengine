// FlyModes.cpp

#include <doomfly/FlyModes.h>

#include <doomfly/FlyControls.h>
#include <doomfly/FlyPlayer.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
                DrawFlyPanel(*player_, "FLY PLAYS", list, x, y, width, height);
            }

        private:
            std::unique_ptr<FlyPlayer> player_;
            std::string title_;
            std::function<void(const std::string&)> log_;
            std::uint64_t nextLogTic_ = 0;
            doom::DoomGame* attached_ = nullptr;
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
                                             const bool drivesScreens, const std::uint64_t seedOffset, std::string& error)
        {
            const FlyModeOptions& options = shared->options;
            const bool bio = choice.controlSet == "bio";
            const char* controlsFile = bio ? "controls_bio.json" : "controls_doomfly.json";
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
            const fs::path controls = FindFile(options, {}, controlsFile);
            if (!FlyControls::Load(controls, config.controls, error))
            {
                return nullptr;
            }
            auto player = FlyPlayer::Create(config, error);
            if (player && options.log)
            {
                const EyesInfo& info = player->Eyes().Info();
                options.log(std::string("fly: ") + (bio ? "Bio" : "DOOMFLY") + " neurons, controls " + controls.string() + "; photoreceptors " + std::to_string(info.receptors[0]) +
                            " left / " + std::to_string(info.receptors[1]) + " right, columns covered " + std::to_string(info.columnsCovered[0]) + " of " + std::to_string(info.columns[0]) +
                            " / " + std::to_string(info.columnsCovered[1]) + " of " + std::to_string(info.columns[1]));
            }
            return player;
        }

        std::unique_ptr<myengine::game::IDoomController> MakeFly(const std::shared_ptr<Shared>& shared, const myengine::game::DoomLaunchChoice& choice, std::string& error)
        {
            const bool bio = choice.controlSet == "bio";
            auto player = CreateFly(shared, choice, -1, false, true, 0, error);
            if (!player)
            {
                return nullptr;
            }
            return std::make_unique<FlyController>(std::move(player), bio ? "Fly plays (Bio neurons)" : "Fly plays (DOOMFLY neurons)", shared->options.log);
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
            DuelController(std::vector<std::unique_ptr<FlyPlayer>> flies, std::vector<int> players, const bool humanPlays, std::function<void(const std::string&)> log)
                : flies_(std::move(flies)), players_(std::move(players)), humanPlays_(humanPlays), log_(std::move(log))
            {
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
            return std::make_unique<DuelController>(std::move(flies), std::move(players), humanVsFly, shared->options.log);
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
        swarm.description = "8-32 flies hunt you.";
        swarm.available = false;
        swarm.unavailableReason = "soon";
        modes.push_back(std::move(swarm));
        return modes;
    }
}
