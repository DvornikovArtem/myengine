// DoomMenu.cpp

#include "DoomMenu.h"

#include "DoomMenuSounds.h"

#include <myengine/core/Version.h>
#include <myengine/ui/rl/RlUiPaint.h>
#include <myengine/ui/rl/RlUiWidgets.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace myengine::game
{
    namespace
    {
        using namespace rlui;
        namespace tone = fd::tone;

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string Format(const char* format, ...)
        {
            char buffer[256];
            va_list arguments;
            va_start(arguments, format);
            std::vsnprintf(buffer, sizeof(buffer), format, arguments);
            va_end(arguments);
            return buffer;
        }

        const char* const kSkillNames[] = {"I'm too young to die", "Hey, not too rough", "Hurt me plenty", "Ultra-Violence", "Nightmare!"};

        float Clamp01(const float v) { return std::clamp(v, 0.0f, 1.0f); }
        float Ease(const float t) { const float u = 1.0f - Clamp01(t); return 1.0f - u * u * u; }
        float Phase(const double t, const double start, const double duration) { return Clamp01(static_cast<float>((t - start) / duration)); }

        // 164789 -> "164 789"
        std::string GroupDigits(const std::uint32_t value)
        {
            std::string digits = std::to_string(value);
            std::string out;
            for (std::size_t i = 0; i < digits.size(); ++i)
            {
                if (i > 0 && (digits.size() - i) % 3 == 0)
                {
                    out.push_back(' ');
                }
                out.push_back(digits[i]);
            }
            return out;
        }

        // A lamp that flickers on: 0 -> 1 -> 0.3 -> 1 (the letters of the title at the entry, the title of a screen that has come)
        float Lamp(const float u)
        {
            if (u <= 0.0f)
            {
                return 0.0f;
            }
            if (u >= 1.0f)
            {
                return 1.0f;
            }
            if (u < 1.0f / 3.0f)
            {
                return u * 3.0f;
            }
            if (u < 2.0f / 3.0f)
            {
                return 1.0f - (u - 1.0f / 3.0f) * 3.0f * 0.7f;
            }
            return 0.3f + (u - 2.0f / 3.0f) * 3.0f * 0.7f;
        }

        bool IsFlyMode(const std::string& id) { return id != "human"; }

        // ---- the words of the modes (§3.2): by the id of the mode, the model of the flow gives only the id and whether it can be played
        struct ModeInfo
        {
            const char* id;
            const char* name;
            const char* caption;
            const char* description;
            const char* hint;     // above START
        };
        constexpr ModeInfo kModes[] = {
            {"human", "HUMAN", "Classic DOOM", "You play DOOM with the keyboard and the mouse. Nothing stands between you and the game.", "Esc pauses. Tab opens the DOOM map."},
            {"fly", "FLY PLAYS", "A brain plays",
             "The frame of DOOM goes into the eyes of a fly brain. Its descending neurons press the keys. The game waits for the brain.", "Esc in the game pauses. Tab shows the brain big."},
            {"fly-learn", "FLY LEARNS", "Learning from walls",
             "The fly starts from scratch. WallSense learns from pictures seen before wall contact; mushroom-body activity is shown separately.",
             "Shown on screen: actual speed, time against walls, and lifetime."},
            {"human-vs-fly", "HUMAN VS FLY", "Deathmatch", "You against a fly in one deathmatch. The fly sees its own screen and plays with its own brain.", "Esc pauses. Tab shows the fly brain; M opens the DOOM map."},
            {"fly-vs-fly", "FLY VS FLY", "You watch", "Two flies fight each other. You watch both brains at work; Tab shows them big.", "Esc in the game pauses. Tab shows both brains big."},
            {"swarm", "SWARM", "Monsters are flies", "Every monster gets a small fly brain of its own. You play against the swarm.", "Esc in the game pauses."},
        };

        const ModeInfo* InfoOf(const std::string& id)
        {
            for (const ModeInfo& info : kModes)
            {
                if (id == info.id)
                {
                    return &info;
                }
            }
            return nullptr;
        }

        std::vector<fd::Tag> TagsOf(const std::string& id)
        {
            const Color you = tone::kRed;
            const Color fly = tone::kBrain;
            if (id == "human")
            {
                return {{"YOU", you}};
            }
            if (id == "fly")
            {
                return {{"FLY", fly}};
            }
            if (id == "fly-learn")
            {
                return {{"FLY", fly}, {"LEARNS", fly}};
            }
            if (id == "human-vs-fly")
            {
                return {{"YOU", you}, {"FLY", fly}};
            }
            if (id == "fly-vs-fly")
            {
                return {{"FLY", fly}, {"FLY", fly}};
            }
            return {{"FLY \xC3\x97N", fly}};
        }

        std::vector<std::vector<fd::ChainNode>> ChainsOf(const std::string& id, const std::string& neurons)
        {
            using fd::ChainNode;
            using fd::NodeKind;
            const auto fly = [&]() -> std::vector<ChainNode>
            {
                return {{"DOOM FRAME", "", NodeKind::Red, false},
                        {"EYES", "LPLC2 LC4", NodeKind::Eye, false},
                        {"BRAIN", neurons, NodeKind::Brain, false},
                        {"DESCENDING", "DNP70 DNA02", NodeKind::Eye, false},
                        {"KEYS", "", NodeKind::Red, false}};
            };
            if (id == "human")
            {
                return {{{"YOU", "", NodeKind::Red, false}, {"KEYS \xC2\xB7 MOUSE", "", NodeKind::Red, false}, {"DOOM", "", NodeKind::Red, false}}};
            }
            if (id == "fly")
            {
                return {fly()};
            }
            if (id == "fly-learn")
            {
                return {fly(), {{"PAIN", "", NodeKind::Red, false}, {"DOPAMINE", "PPL1", NodeKind::Eye, false}, {"MUSHROOM BODY", "KC \xE2\x86\x92 MBON", NodeKind::Brain, false}}};
            }
            if (id == "human-vs-fly")
            {
                return {{{"YOU", "", NodeKind::Red, true}, {"DOOM", "", NodeKind::Red, true}, {"FLY BRAIN", "", NodeKind::Brain, false}}};
            }
            if (id == "fly-vs-fly")
            {
                return {{{"FLY BRAIN A", "", NodeKind::Brain, true}, {"DOOM", "", NodeKind::Red, true}, {"FLY BRAIN B", "", NodeKind::Brain, false}}};
            }
            return {{{"YOU", "", NodeKind::Red, true}, {"DOOM", "", NodeKind::Red, true}, {"MONSTER \xC3\x97 N", "", NodeKind::Red, true}, {"SMALL BRAIN \xC3\x97 N", "", NodeKind::Brain, false}}};
        }

        // what the sheet of "About" says: (the title, the paragraphs); {braces} mark the names of the data and the DOI (the colour of the brain)
        struct CreditSection
        {
            const char* title;
            std::vector<const char*> paragraphs;
        };
        const std::vector<CreditSection>& Credits()
        {
            static const std::vector<CreditSection> sections = {
                {"THE GAME",
                 {"DOOM (1993) by id Software. The source of 1997 is free software under the GNU GPL v2, taken through doomgeneric (ozkl, GPL v2).",
                  "The music is played on an emulated OPL chip, as in the original: the routines of Chocolate Doom (GPL v2) and Nuked OPL3 (Nuke.YKT, LGPL v2.1+).",
                  "Game data: Freedoom (BSD-style licence, freedoom.github.io), the shareware DOOM (doom1.wad, id Software, unchanged) or your own DOOM. The sounds of this menu come from that data too."}},
                {"THE BRAIN",
                 {"The connectome: {MaleCNS v1.0} (Janelia FlyEM, University of Cambridge, MRC LMB, Google Research), 164,789 traced neurons, licence CC BY 4.0.",
                  "Berg S. et al. (2026) Sexual dimorphism in the complete Drosophila male central nervous system connectome. Cell 189(18):5504-5526, {doi:10.1016/j.cell.2026.08.015}.",
                  "Mirror: QuixiAI/MaleCNS on Hugging Face (Hartford E. 2026, {doi:10.57967/hf/10410}). Changes: only the proofread neurons, the synapses as a sparse matrix, signed by the transmitter.",
                  "The model: leaky integrate-and-fire with the constants of Shiu P. et al. (2024) A Drosophila computational brain model reveals sensorimotor processing. Nature 634 (code: MIT).",
                  "The way a fly can play DOOM: DOOMFLY by Alex Wormuth (MIT). It is not a real fly: a network of point neurons wired like the real one."}},
                {"THE LOOK",
                 {"Big Shoulders Display (The Big Shoulders Project Authors) and JetBrains Mono (JetBrains), both SIL Open Font License 1.1; Dear ImGui (MIT)."}},
                {"THIS PROJECT", {"FlyDoom is part of myengine, an educational DirectX 12 engine (ITMO)."}},
            };
            return sections;
        }

        struct Word
        {
            std::string text;
            bool highlight = false;
        };

        // the words of a paragraph with their colour: {a name of data} is cyan
        std::vector<Word> WordsOf(const char* text)
        {
            std::vector<Word> words;
            bool highlight = false;
            std::string_view rest = text;
            while (!rest.empty())
            {
                const std::size_t space = rest.find(' ');
                std::string word(rest.substr(0, space));
                rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
                bool closes = false;
                if (!word.empty() && word.front() == '{')
                {
                    word.erase(word.begin());
                    highlight = true;
                }
                while (!word.empty() && word.find('}') != std::string::npos)
                {
                    word.erase(word.find('}'), 1);
                    closes = true;
                }
                words.push_back({word, highlight});
                if (closes)
                {
                    highlight = false;
                }
            }
            return words;
        }
    }

    std::vector<DoomLevelEntry> LevelsOfWad(const std::filesystem::path& wad)
    {
        const std::string name = Lower(wad.filename().string());
        std::vector<DoomLevelEntry> levels;
        levels.push_back({"Title screen (the menu of DOOM)", 0, 1});
        const auto episodes = [&](const int count, const int maps)
        {
            for (int e = 1; e <= count; ++e)
            {
                for (int m = 1; m <= maps; ++m)
                {
                    levels.push_back({Format("E%dM%d", e, m), e, m});
                }
            }
        };
        const auto flat = [&](const int count)
        {
            for (int m = 1; m <= count; ++m)
            {
                levels.push_back({Format("MAP%02d", m), 0 + 1, m});   // episode 1 is the marker "flat numbering": WarpArguments writes one number
            }
        };
        if (name == "doom1.wad")
        {
            episodes(1, 9);   // the shareware episode
        }
        else if (name == "doom.wad" || name == "freedoom1.wad" || name == "chex.wad")
        {
            episodes(4, 9);
        }
        else if (name.find("doom2") != std::string::npos || name.find("freedoom2") != std::string::npos || name == "plutonia.wad" || name == "tnt.wad")
        {
            flat(32);
        }
        else
        {
            episodes(1, 9);
        }
        return levels;
    }

    std::string WadLabel(const std::filesystem::path& wad, const bool freedoom)
    {
        const std::string name = Lower(wad.filename().string());
        if (name == "freedoom1.wad")
        {
            return "Freedoom: Phase 1";
        }
        if (name == "freedoom2.wad")
        {
            return "Freedoom: Phase 2";
        }
        if (name == "doom1.wad")
        {
            return "DOOM (shareware, episode 1)";
        }
        if (name == "doom.wad")
        {
            return "DOOM";
        }
        if (name == "doom2.wad")
        {
            return "DOOM 2";
        }
        if (name == "plutonia.wad")
        {
            return "Final DOOM: The Plutonia Experiment";
        }
        if (name == "tnt.wad")
        {
            return "Final DOOM: TNT Evilution";
        }
        return (freedoom ? "Freedoom: " : "") + wad.filename().string();
    }

    std::vector<std::string> WarpArguments(const std::filesystem::path& wad, const DoomLevelEntry& level, const int skill)
    {
        std::vector<std::string> arguments;
        if (level.episode <= 0)
        {
            return arguments;
        }
        const std::string name = Lower(wad.filename().string());
        const bool flat = name.find("doom2") != std::string::npos || name.find("freedoom2") != std::string::npos || name == "plutonia.wad" || name == "tnt.wad";
        arguments.push_back("-warp");
        if (!flat)
        {
            arguments.push_back(std::to_string(level.episode));
        }
        arguments.push_back(std::to_string(level.map));
        arguments.push_back("-skill");
        arguments.push_back(std::to_string(std::clamp(skill, 1, 5)));
        return arguments;
    }


    DoomMenu::DoomMenu()
    {
        fx_.play = [this](const fd::Sfx sfx) { Sound(sfx); };
    }

    DoomMenu::~DoomMenu() = default;

    void DoomMenu::Sound(const fd::Sfx sfx)
    {
        if (sounds_ != nullptr)
        {
            sounds_->Play(sfx);
        }
    }

    void DoomMenu::Reset()
    {
        screen_ = Screen::Main;
        quitAsked_ = false;
        quitAppear_ = 0.0f;
        introDone_ = true;   // back from a game: no entry again
        enterTime_ = time_;
        lastFocus_ = kNoId;
        fx_.bar = {};
    }

    bool DoomMenu::Nested() const
    {
        return screen_ != Screen::Main;
    }

    float DoomMenu::BackdropBrightness() const
    {
        switch (screen_)
        {
        case Screen::Main: return 1.0f;
        case Screen::Play: return 0.32f;
        case Screen::Settings: return 0.25f;
        case Screen::About: return 0.22f;
        }
        return 1.0f;
    }

    void DoomMenu::ShowScreen(const std::string_view name)
    {
        screen_ = name == "play" ? Screen::Play : (name == "settings" ? Screen::Settings : (name == "about" ? Screen::About : Screen::Main));
        introDone_ = true;   // a screen asked for by name (the captures, the laboratory coming back from a game): no entry
    }

    void DoomMenu::Go(Frame& f, const Screen next)
    {
        if (screen_ == Screen::Main)
        {
            mainFocus_ = f.Focus();
        }
        screen_ = next;
        enterTime_ = time_;
        lastFocus_ = kNoId;
        fx_.bar.alpha = 0.0f;
        fx_.bar.placed = false;
        if (fx_.reduced)
        {
            f.SetFocus(kNoId);   // the kit would slide the new screen in: with reduced motion it only fades
        }
        else
        {
            f.ResetFocus();
        }
    }

    // the focus of a chain of rows moves one step with the up and down arrows (the kit moves it by the geometry, which does not know the order of two columns)
    namespace
    {
        bool NavChain(Frame& f, const std::vector<Id>& ids, const bool wrapStop)
        {
            const auto it = std::find(ids.begin(), ids.end(), f.Focus());
            if (it == ids.end())
            {
                return false;
            }
            const std::ptrdiff_t index = it - ids.begin();
            if (f.input().Pressed(Nav::Down))
            {
                f.TakeNav(Nav::Down);
                if (index + 1 < static_cast<std::ptrdiff_t>(ids.size()))
                {
                    f.SetFocus(ids[static_cast<std::size_t>(index + 1)]);
                }
                return true;
            }
            if (f.input().Pressed(Nav::Up))
            {
                f.TakeNav(Nav::Up);
                if (index > 0)
                {
                    f.SetFocus(ids[static_cast<std::size_t>(index - 1)]);
                }
                else if (!wrapStop)
                {
                    return false;
                }
                return true;
            }
            return false;
        }

        std::string LevelLabelOf(const DoomMenuModel& model, const DoomSettings& settings)
        {
            for (const DoomWadEntry& wad : model.wads)
            {
                if (!settings.iwad.empty() && wad.path.u8string() != settings.iwad)
                {
                    continue;
                }
                for (const DoomLevelEntry& level : wad.levels)
                {
                    if (level.episode == settings.episode && (settings.episode == 0 || level.map == settings.map))
                    {
                        return level.episode == 0 ? "TITLE" : level.label;
                    }
                }
            }
            return settings.episode == 0 ? "TITLE" : Format("E%dM%d", std::max(settings.episode, 1), settings.map);
        }
    }

    DoomMenu::Result DoomMenu::Draw(Frame& f, DoomSettings& settings, const DoomMenuModel& model, const double timeSeconds, DoomStartRequest& request, bool& settingsChanged)
    {
        Result result = Result::None;
        time_ = timeSeconds;
        fx_.reduced = settings.menuMotion != 0;
        if (!initialised_)
        {
            initialised_ = true;
            enterTime_ = timeSeconds;
            // the choices of the last time
            for (std::size_t i = 0; i < model.modes.size(); ++i)
            {
                if (model.modes[i].id == settings.lastMode)
                {
                    modeIndex_ = static_cast<int>(i);
                }
            }
            for (std::size_t i = 0; i < model.wads.size(); ++i)
            {
                if (!settings.iwad.empty() && model.wads[i].path.u8string() == settings.iwad)
                {
                    wadIndex_ = static_cast<int>(i);
                }
            }
            if (wadIndex_ < static_cast<int>(model.wads.size()))
            {
                const auto& levels = model.wads[static_cast<std::size_t>(wadIndex_)].levels;
                for (std::size_t i = 0; i < levels.size(); ++i)
                {
                    if (levels[i].episode == settings.episode && (settings.episode == 0 || levels[i].map == settings.map))
                    {
                        levelIndex_ = static_cast<int>(i);
                    }
                }
            }
        }
        if (sounds_ != nullptr)
        {
            sounds_->Load(wadIndex_ < static_cast<int>(model.wads.size()) ? model.wads[static_cast<std::size_t>(wadIndex_)].path : std::filesystem::path());
        }

        // the entry of the first show is over at once with any key, click or button
        if (!introDone_)
        {
            introTime_ += std::min(f.DeltaTime(), 1.0f / 30.0f);   // a stall of the first frames (the fonts, the window) does not eat the entry
            const bool skip = f.input().MousePressed() || f.input().Pressed(Nav::Confirm) || f.input().Pressed(Nav::Back) || f.input().Pressed(Nav::Up) || f.input().Pressed(Nav::Down) ||
                              f.input().Pressed(Nav::Left) || f.input().Pressed(Nav::Right);
            if (skip || introTime_ > (fx_.reduced ? 0.3f : 1.4f))
            {
                introDone_ = true;
            }
        }
        const float intro = introDone_ ? 10.0f : introTime_;
        fd::PaintFrame(f, screen_ == Screen::Main && !introDone_ && !fx_.reduced ? Phase(intro, 0.0, 0.24) : 1.0f);
        if (fx_.reduced)
        {
            // everything of a screen only fades in
            const float fade = !introDone_ ? Phase(intro, 0.0, 0.3) : Phase(time_, enterTime_, 0.12);
            f.SetAlpha(fade);
        }

        switch (screen_)
        {
        case Screen::Main: DrawMain(f, settings, model, result); break;
        case Screen::Play: DrawPlay(f, settings, model, result, request); break;
        case Screen::Settings: DrawSettings(f, settings, settingsChanged); break;
        case Screen::About: DrawAbout(f); break;
        }
        if (!quitAsked_)
        {
            // the sound of the cursor: the focus moved (not the first one of a screen)
            const Id focus = f.Focus();
            if (lastFocus_ != kNoId && focus != kNoId && focus != lastFocus_)
            {
                Sound(fd::Sfx::Focus);
            }
            lastFocus_ = focus;
        }
        return result;
    }

    // ---- the main screen (§3.1)

    void DoomMenu::DrawMain(Frame& f, DoomSettings& settings, const DoomMenuModel& model, Result& result)
    {
        const float t = introDone_ ? 10.0f : introTime_;
        const bool reduced = fx_.reduced;
        const bool entering = !introDone_ && !reduced;
        f.SetDefaultFocus(mainFocus_ != kNoId ? mainFocus_ : HashId("fd/play"));
        {
            // the bar of the focus comes in with the item that has it (the first one at the entry): the same alpha and the same shift
            const float first = entering ? Ease(Phase(t, 0.52, 0.18)) : 1.0f;
            fx_.bar.gate = first;
            fx_.bar.shift = reduced ? 0.0f : (1.0f - first) * -24.0f;
        }
        fd::DrawBar(f, fx_);

        // the top line: the name, then the numbers of the brain
        {
            std::vector<fd::Run> left = {{"MYENGINE \xC2\xB7 ITMO", false, tone::kDim}};
            if (entering)
            {
                // typed by symbol, with a block of bone at the end (240 ms)
                const std::size_t shown = static_cast<std::size_t>(Phase(t, 0.0, 0.24) * static_cast<float>(left[0].text.size()));
                std::string text;
                for (std::size_t i = 0, count = 0; i < left[0].text.size() && count < shown; ++i)
                {
                    text.push_back(left[0].text[i]);
                    count += (static_cast<unsigned char>(left[0].text[i]) & 0xC0) != 0x80 ? 1 : 0;
                }
                left[0].text = text;
            }
            fd::TopLeft(f, left);
            const float counters = entering ? Ease(Phase(t, 0.9, 0.4)) : 1.0f;
            std::vector<std::vector<fd::Run>> right;
            if (!model.brainStatus.empty())
            {
                const double rate = std::clamp(static_cast<double>(model.spikesPerSecond) / 1.0e6 * 6.0, 1.0, 6.0);
                const float pulse = 0.75f + 0.25f * static_cast<float>(std::sin(time_ * rate * 2.0 * 3.14159265));
                fd::Run dot;
                dot.dot = true;
                dot.color = tone::kBrain.Faded(pulse);
                if (model.brainStatus == "LOADING")
                {
                    right.push_back({dot, {"BRAIN LOADING", false, tone::kDim}});
                }
                else
                {
                    right.push_back({dot, {"BRAIN ON ", false, tone::kDim}, {model.brainStatus, true, tone::kBone}});
                }
            }
            if (model.neuronCount > 0)
            {
                right.push_back({{GroupDigits(static_cast<std::uint32_t>(static_cast<float>(model.neuronCount) * counters)), true, tone::kBone}, {" NEURONS", false, tone::kDim}});
            }
            if (model.spikesPerSecond > 0.0f)
            {
                const float rate = model.spikesPerSecond * counters;
                const std::string value = rate >= 1.0e6f ? Format("%.2f M", rate / 1.0e6f) : (rate >= 1.0e3f ? Format("%.0f K", rate / 1.0e3f) : Format("%.0f", rate));
                right.push_back({{value, true, tone::kBone}, {" SPIKES/S", false, tone::kDim}});
            }
            right.push_back({{std::string("V") + MYENGINE_VERSION_STRING, false, tone::kDim}});
            fd::TopRight(f, right);
        }

        // FLYDOOM: the letters light up like lamps at the entry
        {
            const char* letters = "FLYDOOM";
            const float delays[7] = {0.12f, 0.19f, 0.15f, 0.30f, 0.24f, 0.36f, 0.28f};
            const float size = 220.0f;
            const float spacing = 0.02f;
            if (entering)
            {
                float x = 92.0f;
                for (int i = 0; i < 7; ++i)
                {
                    const std::string letter(1, letters[i]);
                    const float alpha = Lamp((t - delays[i]) / 0.16f);
                    fd::TextTop(f, fd::kDisplay, size, {x, 118.0f}, (i < 3 ? tone::kBrain : tone::kRed).Faded(alpha), letter);
                    x += paint::TextWidth(f, fd::kDisplay, size, letter) + spacing * size;
                }
            }
            else
            {
                // the browser kerns the pairs of the poster face (FL, LY ...), the text of the kit does not: a tighter step makes the same width
                const float width = paint::TextWidth(f, fd::kDisplay, size, "FLY", -0.012f);
                fd::TextTop(f, fd::kDisplay, size, {92.0f, 118.0f}, tone::kBrain, "FLY", Align::Left, -0.012f);
                fd::TextTop(f, fd::kDisplay, size, {92.0f + width + -0.012f * size + 2.0f, 118.0f}, tone::kRed, "DOOM", Align::Left, -0.012f);
            }
        }
        {
            const std::string neurons = model.neuronCount > 0 ? GroupDigits(model.neuronCount) : "164 789";
            const float alpha = entering ? Ease(Phase(t, 0.52, 0.18)) : 1.0f;
            fd::TextMid(f, fd::kMono, 18.0f, {96.0f, 333.0f}, tone::kDim.Faded(alpha), "A fruit fly's brain plays DOOM.");
            fd::TextMid(f, fd::kMono, 18.0f, {96.0f, 361.0f}, tone::kDim.Faded(alpha), neurons + " neurons, wired like the real one.");
        }

        // the items
        std::string last;
        if (!settings.iwad.empty())
        {
            std::string mode = settings.lastMode;
            if (const ModeInfo* info = InfoOf(settings.lastMode))
            {
                mode = info->name;
            }
            last = "LAST: " + fd::Upper(mode) + " \xC2\xB7 " + LevelLabelOf(model, settings) + " \xC2\xB7 " + fd::Upper(kSkillNames[std::clamp(settings.skill, 1, 5) - 1]);
        }
        struct Entry
        {
            const char* id;
            const char* index;
            const char* word;
            std::string sub;
        };
        const Entry entries[] = {{"fd/play", "01", "PLAY", last}, {"fd/settings", "02", "SETTINGS", ""}, {"fd/about", "03", "ABOUT", ""}, {"fd/exit", "04", "EXIT", ""}};
        float y = 500.0f;
        for (int i = 0; i < 4; ++i)
        {
            const float local = entering ? Phase(t, 0.52 + 0.06 * static_cast<double>(i), 0.18) : 1.0f;
            const float dx = reduced ? 0.0f : (1.0f - Ease(local)) * -24.0f;
            fd::MenuItemSpec spec{entries[i].index, entries[i].word, entries[i].sub, true};
            const rlui::ItemState state = fd::MenuItem(f, fx_, HashId(entries[i].id), y, spec, entering ? Ease(local) : 1.0f, dx);
            y += entries[i].sub.empty() ? 76.0f : 104.0f;
            if (state.activated && !quitAsked_)
            {
                Sound(fd::Sfx::Select);
                if (i == 0)
                {
                    Go(f, Screen::Play);
                }
                else if (i == 1)
                {
                    Go(f, Screen::Settings);
                }
                else if (i == 2)
                {
                    Go(f, Screen::About);
                }
                else
                {
                    quitAsked_ = true;
                    quitAppear_ = 0.0f;
                }
                if (i < 3)
                {
                    return;
                }
            }
        }

        // what is missing
        float noteY = 868.0f;
        if (!model.wadProblem.empty())
        {
            fd::Note(f, {96.0f, noteY}, "NO GAME DATA YET. PLAY shows where to put DOOM or Freedoom.", 760.0f);
            noteY += 30.0f;
        }
        if (!model.flyAvailable && !model.modes.empty())
        {
            fd::Note(f, {96.0f, noteY}, "NO FLY BRAIN YET. Fly modes need tools\\flybrain\\fetch_malecns.ps1.", 760.0f);
        }

        // the labels on the cloud (Should): the three parts of the brain and what each one does in the game
        DrawCallouts(f, model, entering ? Ease(Phase(t, 0.7, 0.4)) : 1.0f);
        fd::CapsMid(f, fd::kMono, 13.0f, {1824.0f, 1017.0f}, tone::kFaint, "MaleCNS v1.0 \xC2\xB7 Janelia FlyEM \xC2\xB7 CC BY 4.0", Align::Right);

        fd::Hints(f, {{{"UP", "DOWN"}, "SELECT"}, {{"ENTER"}, "CONFIRM"}, {{"ESC"}, "EXIT"}});

        if (quitAsked_)
        {
            quitAppear_ = std::min(1.0f, quitAppear_ + f.DeltaTime() / 0.12f);
            const int answer = fd::ConfirmQuit(f, fx_, "LEAVE FLYDOOM?", "Your settings are saved. The fly will wait.", fx_.reduced ? 1.0f : quitAppear_);
            if (answer > 0)
            {
                Sound(fd::Sfx::Leave);
                result = Result::Quit;
            }
            else if (answer < 0)
            {
                Sound(fd::Sfx::Back);
                quitAsked_ = false;
            }
        }
        else if (f.TakeNav(Nav::Back))
        {
            Sound(fd::Sfx::Back);
            quitAsked_ = true;
            quitAppear_ = 0.0f;
        }
    }

    void DoomMenu::DrawCallouts(Frame& f, const DoomMenuModel& model, const float alpha)
    {
        if (alpha <= 0.01f || model.anchors.empty())
        {
            return;
        }
        struct Label
        {
            const char* id;
            const char* title;
            const char* text;
            float dy;      // the line goes up (-) or down (+) to its shelf
            float side;    // the shelf runs to the right (+1) or to the left (-1)
        };
        const Label labels[] = {{"optic", "OPTIC LOBES", "see the frame of DOOM", -75.0f, 1.0f},
                                {"mushroom", "MUSHROOM BODIES", "remember what hurt", 118.0f, -1.0f},
                                {"descending", "DESCENDING NEURONS", "press the keys", 95.0f, 1.0f}};
        for (const Label& label : labels)
        {
            for (const DoomBrainAnchor& anchor : model.anchors)
            {
                if (anchor.id != label.id)
                {
                    continue;
                }
                const Vec2 point = f.ToDesign({anchor.x, anchor.y});
                const float side = label.side;
                const float reach = 70.0f + std::fabs(label.dy) * 0.6f;
                const Vec2 elbow{point.x + side * reach, point.y + label.dy};
                const Vec2 end{elbow.x + side * 40.0f, elbow.y};
                const float textX = end.x + side * 12.0f;
                const std::string title = label.title;
                const float width = std::max(fd::CapsWidth(f, fd::kMonoBold, 14.0f, title), paint::TextWidth(f, fd::kMono, 13.0f, label.text));
                const float left = side > 0.0f ? textX : textX - width;
                // hidden when the point left the picture or the text would lie on the column of the menu
                if (point.x < 0.0f || point.x > 1920.0f || point.y < 90.0f || point.y > 980.0f || left < 760.0f || left + width > 1900.0f)
                {
                    continue;
                }
                const Color brain = tone::kBrain.Faded(alpha);
                // a plate of the ink under the words: the cloud is bright there (fields 8 x 4)
                {
                    const float plateLeft = (side > 0.0f ? textX : textX - width) - 8.0f;
                    paint::FillRect(f, {plateLeft, elbow.y - 8.0f - 9.0f - 4.0f, width + 16.0f, 18.0f + 20.0f + 8.0f - 2.0f}, tone::kInk.Faded(0.75f * alpha));
                }
                paint::CircleOutline(f, point, 4.5f, brain, 1.5f);
                paint::Line(f, point, elbow, tone::kLine2.Faded(alpha), 1.0f);
                paint::Line(f, elbow, end, tone::kLine2.Faded(alpha), 1.0f);
                fd::CapsMid(f, fd::kMonoBold, 14.0f, {textX, elbow.y - 8.0f}, brain, title, side > 0.0f ? Align::Left : Align::Right);
                fd::TextMid(f, fd::kMono, 13.0f, {textX, elbow.y + 10.0f}, tone::kDim.Faded(alpha), label.text, side > 0.0f ? Align::Left : Align::Right);
            }
        }
    }

    // ---- who plays (§3.2)

    void DoomMenu::DrawPlay(Frame& f, DoomSettings& settings, const DoomMenuModel& model, Result& result, DoomStartRequest& request)
    {
        // the heading and the lists come first: the bar of the focus under them
        const float flicker = fx_.reduced ? 1.0f : Lamp(static_cast<float>((time_ - enterTime_ - 0.06) / 0.14));
        modeIndex_ = std::clamp(modeIndex_, 0, std::max(0, static_cast<int>(model.modes.size()) - 1));
        const Id startId = HashId("fd/start");
        f.SetDefaultFocus(HashId("fd/mode") + static_cast<Id>(modeIndex_));

        // the sheet of the dossier first (the bar is drawn over it)
        const Rect sheet{736.0f, 112.0f, 1088.0f, 870.0f};
        fd::Sheet(f, sheet);
        fd::DrawBar(f, fx_);

        fd::TopLeft(f, {{"FLYDOOM / ", false, tone::kDim}, {"PLAY", true, tone::kBone}});
        {
            std::vector<std::vector<fd::Run>> right;
            if (!model.brainStatus.empty())
            {
                fd::Run dot;
                dot.dot = true;
                dot.color = tone::kBrain;
                if (model.brainStatus == "LOADING")
                {
                    right.push_back({dot, {"BRAIN LOADING", false, tone::kDim}});
                }
                else
                {
                    right.push_back({dot, {"BRAIN ON ", false, tone::kDim}, {model.brainStatus, true, tone::kBone}});
                }
            }
            if (model.neuronCount > 0)
            {
                right.push_back({{GroupDigits(model.neuronCount), true, tone::kBone}, {" NEURONS", false, tone::kDim}});
            }
            right.push_back({{std::string("V") + MYENGINE_VERSION_STRING, false, tone::kDim}});
            fd::TopRight(f, right);
        }
        fd::TextTop(f, fd::kDisplay, 104.0f, {92.0f, 112.0f}, tone::kBone.Faded(flicker), "WHO PLAYS", Align::Left, 0.02f);

        // the list of the modes: the focus is the choice
        paint::FillRect(f, {72.0f, 252.0f, 600.0f, 1.0f}, tone::kLine);
        std::vector<Id> chain;
        std::vector<int> rowKinds;
        const DoomMenuModel::Mode* chosen = model.modes.empty() ? nullptr : &model.modes[static_cast<std::size_t>(modeIndex_)];
        const std::string chosenId = chosen != nullptr ? chosen->id : "human";
        const bool flyMode = IsFlyMode(chosenId);
        enum Row : int { kGameData, kStartAt, kSkill, kNeurons, kFlyBrain, kSpeed, kStartFrom, kFlies, kBrainOn };
        {
            rowKinds = {kGameData, kStartAt, kSkill};
            if (chosenId == "fly" || chosenId == "human-vs-fly" || chosenId == "fly-vs-fly")
            {
                rowKinds.push_back(kNeurons);
                if (model.learnOptions.flyBrain)
                {
                    rowKinds.push_back(kFlyBrain);
                }
            }
            else if (chosenId == "fly-learn")
            {
                if (model.learnOptions.speed)
                {
                    rowKinds.push_back(kSpeed);
                }
                if (model.learnOptions.startFrom)
                {
                    rowKinds.push_back(kStartFrom);
                }
            }
            else if (chosenId == "swarm")
            {
                rowKinds.push_back(kFlies);
            }
            if (flyMode)
            {
                rowKinds.push_back(kBrainOn);
            }
        }
        const auto rowId = [](const int kind) { return HashId("fd/row") + static_cast<Id>(kind); };
        for (const int kind : rowKinds)
        {
            chain.push_back(rowId(kind));
        }
        chain.push_back(startId);

        for (std::size_t i = 0; i < model.modes.size(); ++i)
        {
            const DoomMenuModel::Mode& mode = model.modes[i];
            const ModeInfo* info = InfoOf(mode.id);
            fd::ModeRowSpec spec;
            spec.index = Format("%02d", static_cast<int>(i) + 1);
            spec.name = info != nullptr ? info->name : mode.title;
            spec.caption = info != nullptr ? info->caption : mode.caption;
            spec.tags = TagsOf(mode.id);
            spec.available = mode.available;
            const std::string reason = mode.reason;
            spec.soon = reason.empty() || Lower(reason) == "soon" ? "SOON" : (reason.size() <= 10 ? fd::Upper(reason) : "N/A");
            spec.chosen = static_cast<int>(i) == modeIndex_;
            const rlui::ItemState state = fd::ModeRow(f, fx_, HashId("fd/mode") + static_cast<Id>(i), {72.0f, 253.0f + 104.0f * static_cast<float>(i), 600.0f, 104.0f}, spec);
            if (state.focused)
            {
                if (i + 1 == model.modes.size() && f.input().Pressed(Nav::Down))
                {
                    f.TakeNav(Nav::Down);   // the last mode is the end of the list: the kit would take the focus to START
                }
                if (modeIndex_ != static_cast<int>(i))
                {
                    modeIndex_ = static_cast<int>(i);
                    dossierTime_ = time_;
                }
                if ((f.TakeNav(Nav::Right) || state.activated) && !chain.empty())
                {
                    if (mode.available)
                    {
                        Sound(fd::Sfx::Select);
                        f.SetFocus(chain.front());
                    }
                    else
                    {
                        Sound(fd::Sfx::Deny);
                    }
                }
            }
        }

        // the dossier
        const ModeInfo* info = InfoOf(chosenId);
        const float swap = Ease(static_cast<float>((time_ - dossierTime_) / 0.12));
        float baseAlpha = 1.0f;
        const std::string neurons = model.neuronCount > 0 ? GroupDigits(model.neuronCount) : "164 789";
        {
            baseAlpha = f.Alpha();
            f.SetAlpha(baseAlpha * (fx_.reduced ? 1.0f : swap));
            fd::DrawRuns(f, {{Format("MODE %02d", modeIndex_ + 1), true, tone::kRed}, {Format(" / %02d", static_cast<int>(model.modes.size())), false, tone::kFaint}}, 14.0f, {776.0f, 155.0f}, Align::Left);
            fd::TextTop(f, fd::kDisplay, 96.0f, {772.0f, 177.0f}, tone::kBone, fd::Upper(info != nullptr ? info->name : (chosen != nullptr ? chosen->title : "")), Align::Left, 0.02f);
            const std::string description = info != nullptr ? info->description : (chosen != nullptr ? chosen->description : "");
            float lineY = 286.0f;
            for (const std::string& line : fd::Wrap(f, fd::kMono, 18.0f, description, 860.0f))
            {
                fd::TextMid(f, fd::kMono, 18.0f, {776.0f, lineY}, tone::kText, line);
                lineY += 28.0f;
            }
            f.SetAlpha(baseAlpha);
        }
        fd::Section(f, 776.0f, 390.0f, 1008.0f, "A", "SIGNAL PATH");
        {
            const std::vector<std::vector<fd::ChainNode>> chains = ChainsOf(chosenId, neurons);
            const float grow = fx_.reduced ? 1.0f : Clamp01(static_cast<float>((time_ - dossierTime_ - 0.06) / 0.5));
            float chainY = chains.size() > 1 ? 410.0f : 418.0f;   // two paths (Fly learns) sit closer, so that both fit above SETUP
            for (std::size_t c = 0; c < chains.size(); ++c)
            {
                float chainX = 776.0f;
                if (c > 0)
                {
                    // the second path runs back from the game to the brain: it says so
                    fd::CapsMid(f, fd::kMono, 12.0f, {776.0f, chainY + 20.0f}, tone::kDim, "AND BACK:");
                    chainX += fd::CapsWidth(f, fd::kMono, 12.0f, "AND BACK:") + 14.0f;
                }
                fd::Chain(f, chainX, chainY, chains[c], grow);
                chainY += chains.size() > 1 ? 46.0f : 52.0f;
            }
        }
        fd::Section(f, 776.0f, 520.0f, 1008.0f, "B", "SETUP");

        // the setup: the rows of the mode
        const bool wadsFound = !model.wads.empty();
        const float rowsTop = 541.0f;
        const float rowStep = 52.0f;
        int rowNumber = 0;
        bool anyChange = false;
        for (const int kind : rowKinds)
        {
            const Rect rect{760.0f, rowsTop + rowStep * static_cast<float>(rowNumber++), 600.0f, 52.0f};
            const Id id = rowId(kind);
            fd::Edit edit;
            switch (kind)
            {
            case kGameData:
            {
                if (!wadsFound)
                {
                    fd::SetupRow(f, fx_, id, rect, "Game data");
                    fd::CapsMid(f, fd::kMonoBold, 17.0f, {rect.x + rect.w - 20.0f, rect.y + 26.0f}, tone::kAmber, "NOT FOUND", Align::Right, 0.06f);
                    break;
                }
                wadIndex_ = std::clamp(wadIndex_, 0, static_cast<int>(model.wads.size()) - 1);
                const DoomWadEntry& wad = model.wads[static_cast<std::size_t>(wadIndex_)];
                edit = fd::Stepper(f, fx_, id, rect, "Game data", wad.label, wad.path.filename().string());
                if (edit.delta != 0)
                {
                    wadIndex_ = (wadIndex_ + edit.delta + static_cast<int>(model.wads.size())) % static_cast<int>(model.wads.size());
                    levelIndex_ = 0;
                }
                break;
            }
            case kStartAt:
            {
                if (!wadsFound)
                {
                    fd::SetupRow(f, fx_, id, rect, "Start at");
                    break;
                }
                const DoomWadEntry& current = model.wads[static_cast<std::size_t>(std::clamp(wadIndex_, 0, static_cast<int>(model.wads.size()) - 1))];
                levelIndex_ = std::clamp(levelIndex_, 0, static_cast<int>(current.levels.size()) - 1);
                edit = fd::Stepper(f, fx_, id, rect, "Start at", current.levels[static_cast<std::size_t>(levelIndex_)].label);
                if (edit.delta != 0)
                {
                    levelIndex_ = (levelIndex_ + edit.delta + static_cast<int>(current.levels.size())) % static_cast<int>(current.levels.size());
                }
                break;
            }
            case kSkill:
            {
                edit = fd::Stepper(f, fx_, id, rect, "Skill", kSkillNames[std::clamp(settings.skill, 1, 5) - 1]);
                settings.skill = std::clamp(settings.skill + edit.delta, 1, 5);
                edit.changed = edit.delta != 0 && settings.skill != std::clamp(settings.skill - edit.delta, 1, 5);
                break;
            }
            case kNeurons:
            {
                int selected = settings.controlSet == "bio" ? 1 : 0;
                edit = fd::Segments(f, fx_, id, rect, "Neurons", {"DOOMFLY", "BIO"}, selected);
                settings.controlSet = selected == 1 ? "bio" : "doomfly";
                break;
            }
            case kFlyBrain:
            {
                int selected = settings.flyBrain == "scratch" ? 1 : 0;
                edit = fd::Segments(f, fx_, id, rect, "Fly brain", {"TRAINED", "FROM SCRATCH"}, selected);
                settings.flyBrain = selected == 1 ? "scratch" : "trained";
                break;
            }
            case kSpeed:
            {
                // ×1 (real time) or MAX (as fast as the brain can go: the overlay of the game shows what it is)
                int selected = settings.learnSpeed == 1 ? 0 : 1;
                edit = fd::Segments(f, fx_, id, rect, "Speed", {"\xC3\x97" "1", "MAX"}, selected);
                settings.learnSpeed = selected == 0 ? 1 : 0;
                break;
            }
            case kStartFrom:
            {
                int selected = settings.learnStart == "trained" ? 1 : 0;
                edit = fd::Segments(f, fx_, id, rect, "Start from", {"SCRATCH", "TRAINED"}, selected);
                settings.learnStart = selected == 1 ? "trained" : "scratch";
                break;
            }
            case kFlies:
            {
                edit = fd::Stepper(f, fx_, id, rect, "Flies", std::to_string(settings.flies), "small brains");
                if (edit.delta != 0)
                {
                    const int before = settings.flies;
                    settings.flies = std::clamp(settings.flies + 4 * edit.delta, 8, 32);
                    edit.changed = settings.flies != before;
                    edit.delta = edit.changed ? edit.delta : 0;
                }
                break;
            }
            case kBrainOn:
            {
                int selected = settings.backend == "gpu" ? 1 : (settings.backend == "cpu" ? 2 : 0);
                edit = fd::Segments(f, fx_, id, rect, "Brain on", {"AUTO", "GPU", "CPU"}, selected);
                settings.backend = selected == 1 ? "gpu" : (selected == 2 ? "cpu" : "auto");
                break;
            }
            default: break;
            }
            if (edit.changed || edit.delta != 0)
            {
                anyChange = true;
            }
            if (edit.activated)
            {
                Sound(fd::Sfx::Select);
                f.SetFocus(startId);
            }
        }
        if (anyChange)
        {
            Sound(fd::Sfx::Change);
        }

        // what stands in the way, and what the mode says about the game
        const bool canStart = chosen != nullptr && chosen->available && wadsFound && (!flyMode || model.flyAvailable);
        {
            float noteY = 570.0f;
            if (chosen != nullptr && !chosen->available)
            {
                const std::string reason = Lower(chosen->reason);
                const bool noBrain = reason.rfind("no connectome", 0) == 0;
                fd::Note(f, {1404.0f, noteY}, reason.empty() || reason == "soon" ? "Not made yet." : (noBrain ? "No fly brain yet. Fly modes need tools\\flybrain\\fetch_malecns.ps1." : chosen->reason), 380.0f);
            }
            else if (!wadsFound)
            {
                fd::Note(f, {1404.0f, noteY}, "No game data. Put a WAD into", 380.0f);
                fd::TextMid(f, fd::kMono, 14.0f, {1426.0f, noteY + 26.0f}, tone::kBone, "%LOCALAPPDATA%\\FlyDoom\\wads");
                fd::TextMid(f, fd::kMono, 14.0f, {1426.0f, noteY + 52.0f}, tone::kDim, "DOOM of Steam and Freedoom");
                fd::TextMid(f, fd::kMono, 14.0f, {1426.0f, noteY + 72.0f}, tone::kDim, "are found by themselves.");
            }
            else if (flyMode && !model.flyAvailable)
            {
                fd::Note(f, {1404.0f, noteY}, "No fly brain yet. Fly modes need tools\\flybrain\\fetch_malecns.ps1.", 380.0f);
            }
        }
        if (info != nullptr && canStart)
        {
            const std::string hint = info->hint;
            float hintY = 806.0f;
            for (const std::string& line : fd::Wrap(f, fd::kMono, 14.0f, hint, 380.0f))
            {
                fd::TextMid(f, fd::kMono, 14.0f, {1404.0f, hintY}, tone::kDim, line);
                hintY += 24.0f;
            }
        }
        const rlui::ItemState start = fd::Button(f, fx_, startId, {1404.0f, 866.0f, 380.0f, 76.0f}, "START", fd::ButtonKind::Primary, canStart, "ENTER", true);
        if (start.activated)
        {
            if (canStart)
            {
                const DoomWadEntry& wad = model.wads[static_cast<std::size_t>(wadIndex_)];
                const DoomLevelEntry& level = wad.levels[static_cast<std::size_t>(levelIndex_)];
                request = {};
                request.modeId = chosen->id;
                request.choice.controlSet = settings.controlSet;
                request.choice.backend = settings.backend;
                request.choice.episode = level.episode;
                request.choice.map = level.map;
                request.choice.skill = settings.skill;
                request.choice.flies = settings.flies;
                request.choice.fastest = settings.learnSpeed == 0;
                request.choice.trainedFly = settings.flyBrain != "scratch";
                request.iwad = wad.path;
                request.doomArgs = WarpArguments(wad.path, level, settings.skill);
                settings.lastMode = chosen->id;
                settings.iwad = wad.path.u8string();   // UTF-8: the JSON of the settings takes nothing else (a WAD in a folder with a Russian name made the save throw)
                settings.episode = level.episode;
                settings.map = level.map;
                Sound(fd::Sfx::Start);
                result = Result::Start;
            }
            else
            {
                Sound(fd::Sfx::Deny);
            }
        }

        // the order of the rows: up and down walk the setup and START, they do not leave it
        NavChain(f, chain, true);
        const bool inSetup = std::find(chain.begin(), chain.end(), f.Focus()) != chain.end();
        std::vector<fd::Hint> hints;
        if (inSetup)
        {
            hints = {{{"UP", "DOWN"}, "ROW"}, {{"LEFT", "RIGHT"}, "CHANGE"}, {{"ENTER"}, "NEXT"}, {{"ESC"}, "MODES"}};
        }
        else
        {
            hints = {{{"UP", "DOWN"}, "MODE"}, {{"RIGHT", "ENTER"}, "SET UP"}, {{"ESC"}, "BACK"}};
        }
        fd::Hints(f, hints);
        if (f.TakeNav(Nav::Back))
        {
            Sound(fd::Sfx::Back);
            if (inSetup)
            {
                f.SetFocus(HashId("fd/mode") + static_cast<Id>(modeIndex_));
            }
            else
            {
                Go(f, Screen::Main);
            }
        }
    }

    // ---- settings (§3.3)

    void DoomMenu::DrawSettings(Frame& f, DoomSettings& settings, bool& changed)
    {
        const float flicker = fx_.reduced ? 1.0f : Lamp(static_cast<float>((time_ - enterTime_ - 0.06) / 0.14));
        f.SetDefaultFocus(HashId("fd/sfx"));
        const Rect left{72.0f, 238.0f, 868.0f, 600.0f};
        const Rect right{980.0f, 238.0f, 868.0f, 600.0f};
        fd::Sheet(f, left);
        fd::Sheet(f, right);
        fd::DrawBar(f, fx_);

        fd::TopLeft(f, {{"FLYDOOM / ", false, tone::kDim}, {"SETTINGS", true, tone::kBone}});
        {
            const float saved = static_cast<float>(std::clamp(1.0 - (time_ - savedTime_) / 1.2, 0.0, 1.0));
            fd::Run check;
            check.check = true;
            check.color = tone::kBrain.Faded(0.45f + 0.55f * saved);
            fd::TopRight(f, {{check, {"SAVED AS YOU CHANGE", false, tone::kDim}}, {{std::string("V") + MYENGINE_VERSION_STRING, false, tone::kDim}}});
        }
        fd::TextTop(f, fd::kDisplay, 104.0f, {92.0f, 112.0f}, tone::kBone.Faded(flicker), "SETTINGS", Align::Left, 0.02f);

        std::vector<Id> chain;
        bool any = false;
        const auto mark = [&](const bool edited)
        {
            if (edited)
            {
                any = true;
            }
        };
        const auto percent = [](const float value) { return Format("%d%%", static_cast<int>(std::lround(value * 100.0f))); };
        const auto row = [](const float x, const int number) { return Rect{x, 289.0f + 52.0f * static_cast<float>(number), 840.0f, 52.0f}; };

        // 01 SOUND
        fd::Section(f, 96.0f, 270.0f, 820.0f, "01", "SOUND");
        {
            const Id id = HashId("fd/sfx");
            chain.push_back(id);
            float t = settings.sfxVolume;
            const fd::Edit edit = fd::Slider(f, fx_, id, row(76.0f, 0), "Effects", t, [](const float v, const int d) { return std::round((v + 0.05f * static_cast<float>(d)) * 20.0f) / 20.0f; }, percent(t));
            t = std::round(t * 20.0f) / 20.0f;
            settings.sfxVolume = t;
            mark(edit.changed);
        }
        {
            const Id id = HashId("fd/music");
            chain.push_back(id);
            float t = settings.musicVolume;
            const fd::Edit edit = fd::Slider(f, fx_, id, row(76.0f, 1), "Music", t, [](const float v, const int d) { return std::round((v + 0.05f * static_cast<float>(d)) * 20.0f) / 20.0f; }, percent(t));
            t = std::round(t * 20.0f) / 20.0f;
            settings.musicVolume = t;
            mark(edit.changed);
        }
        {
            const Id id = HashId("fd/musicon");
            chain.push_back(id);
            int selected = settings.musicOn ? 0 : 1;
            const fd::Edit edit = fd::Segments(f, fx_, id, row(76.0f, 2), "Music of DOOM", {"ON", "OFF"}, selected);
            settings.musicOn = selected == 0;
            mark(edit.changed);
        }

        // 02 MOUSE: a log scale, 1.00x in the middle
        fd::Section(f, 96.0f, 496.0f, 820.0f, "02", "MOUSE");
        const Rect mouseRow{76.0f, 515.0f, 840.0f, 52.0f};
        bool mouseFocused = false;
        {
            const Id id = HashId("fd/mouse");
            chain.push_back(id);
            const float lo = std::log(0.25f);
            const float hi = std::log(4.0f);
            float t = (std::log(std::clamp(settings.mouseSensitivity, 0.25f, 4.0f)) - lo) / (hi - lo);
            const float before = t;
            const auto snap = [](const float v) { return std::round(v * 20.0f) / 20.0f; };
            const fd::Edit edit = fd::Slider(f, fx_, id, mouseRow, "Turn speed", t,
                                             [&](const float v, const int d)
                                             {
                                                 const float value = std::exp(lo + (hi - lo) * v);
                                                 float next = snap(value * std::pow(1.1f, static_cast<float>(d)));
                                                 if (next == snap(value))
                                                 {
                                                     next = snap(value) + 0.05f * static_cast<float>(d);
                                                 }
                                                 return (std::log(std::clamp(next, 0.25f, 4.0f)) - lo) / (hi - lo);
                                             },
                                             Format("%.2f\xC3\x97", snap(std::exp(lo + (hi - lo) * t))), 10, true);
            if (t != before)
            {
                settings.mouseSensitivity = std::clamp(snap(std::exp(lo + (hi - lo) * t)), 0.25f, 4.0f);
            }
            mark(edit.changed);
            mouseFocused = edit.focused;
            const float trackX = mouseRow.x + mouseRow.w - 16.0f - 104.0f - 300.0f;
            fd::TextMid(f, fd::kMono, 12.0f, {trackX, 581.0f}, tone::kFaint, "0.25\xC3\x97", Align::Center);
            fd::TextMid(f, fd::kMono, 12.0f, {trackX + 150.0f, 581.0f}, tone::kFaint, "1\xC3\x97", Align::Center);
            fd::TextMid(f, fd::kMono, 12.0f, {trackX + 300.0f, 581.0f}, tone::kFaint, "4\xC3\x97", Align::Center);
        }
        // The test dial uses DOOM's native 8 / 65536 circle per count, the human baseline gain and the saved setting.
        {
            const Vec2 mouse = f.input().MousePosition();
            if (mouseFocused && f.input().MouseMoved() && time_ - dialRest_ < 10.0)
            {
                const float dx = mouse.x - lastMouse_.x;
                const float counts = dx;   // a pixel of the pointer is a unit of the movement of the mouse, whatever the scale of the menu is
                if (std::isfinite(counts) && std::fabs(counts) < 800.0f)   // the first position of the pointer is not a movement
                {
                    const float degrees = counts * kDoomMouseBaselineGain * settings.mouseSensitivity * (8.0f * 360.0f / 65536.0f);
                    dialAngle_ += degrees;
                    dialSweep_ += degrees;
                }
                dialRest_ = time_;
            }
            lastMouse_ = mouse;
            if (time_ - dialRest_ > 1.5)
            {
                dialAngle_ *= std::max(0.0f, 1.0f - f.DeltaTime() * 6.0f);
                if (std::fabs(dialAngle_) < 0.5f)
                {
                    dialAngle_ = 0.0f;
                    dialSweep_ = 0.0f;
                }
            }
            if (mouseFocused || dialAngle_ != 0.0f)
            {
                fd::Dial(f, {166.0f, 662.0f}, 52.0f, dialAngle_);
                fd::TextMid(f, fd::kMonoBold, 17.0f, {246.0f, 630.0f}, tone::kBone, "Move the mouse: the needle turns as DOOM will.");
                fd::TextMid(f, fd::kMono, 16.0f, {246.0f, 657.0f}, tone::kDim, "At 1.00\xC3\x97 a sweep of 4096 counts turns 270\xC2\xB0.");
                fd::TextMid(f, fd::kMono, 16.0f, {246.0f, 683.0f}, tone::kFaint, Format("%.0f\xC2\xB0 \xC2\xB7 this sweep", std::fabs(dialSweep_)));
            }
        }

        // 03 SCREEN
        fd::Section(f, 1004.0f, 270.0f, 820.0f, "03", "SCREEN");
        {
            const Id id = HashId("fd/window");
            chain.push_back(id);
            int selected = std::clamp(settings.windowMode, 0, 2);
            const fd::Edit edit = fd::Segments(f, fx_, id, row(984.0f, 0), "Window", {"WINDOWED", "BORDERLESS", "FULLSCREEN"}, selected);
            settings.windowMode = selected;
            mark(edit.changed);
        }
        {
            const Id id = HashId("fd/uiscale");
            chain.push_back(id);
            float t = static_cast<float>(settings.uiScalePercent - 75) / 50.0f;
            const fd::Edit edit = fd::Slider(f, fx_, id, row(984.0f, 1), "Menu scale", t, [](const float v, const int d) { return std::round((v + 0.1f * static_cast<float>(d)) * 10.0f) / 10.0f; },
                                             Format("%d%%", 75 + static_cast<int>(std::lround(t * 10.0f)) * 5), 10, true);
            settings.uiScalePercent = 75 + static_cast<int>(std::lround(t * 10.0f)) * 5;
            mark(edit.changed);
        }
        {
            const Id id = HashId("fd/backdrop");
            chain.push_back(id);
            int selected = settings.brainBackdrop ? 0 : 1;
            const fd::Edit edit = fd::Segments(f, fx_, id, row(984.0f, 2), "Brain behind the menu", {"ON", "OFF"}, selected);
            settings.brainBackdrop = selected == 0;
            mark(edit.changed);
        }
        {
            const Id id = HashId("fd/motion");
            chain.push_back(id);
            int selected = settings.menuMotion != 0 ? 1 : 0;
            const fd::Edit edit = fd::Segments(f, fx_, id, row(984.0f, 3), "Menu motion", {"FULL", "REDUCED"}, selected);
            settings.menuMotion = selected;
            mark(edit.changed);
            fd::TextMid(f, fd::kMono, 14.0f, {1004.0f, 523.0f}, tone::kFaint, "Reduced: no flicker and no slide, the screens only fade.");
        }

        // 04 CONTROLS: what the keys do (the keys of DOOM itself)
        fd::Section(f, 1004.0f, 570.0f, 820.0f, "04", "CONTROLS");
        {
            struct Control
            {
                std::vector<std::string> keys;
                const char* text;
            };
            const std::vector<Control> first = {{{"W", "UP"}, "forward"}, {{"A", "D"}, "step aside"}, {{"CTRL", "LMB"}, "fire"}, {{"SHIFT"}, "run"}, {{"ALT"}, "strafe with the turn"}, {{"TAB"}, "map / big brain in fly modes"}};
            const std::vector<Control> second = {{{"S", "DOWN"}, "back"}, {{"LEFT", "RIGHT", "MOUSE"}, "turn"}, {{"SPACE", "E"}, "use, open"}, {{"1", "-", "7"}, "weapon"}, {{"ESC"}, "pause"}, {{"M"}, "map in Human vs Fly"}};
            for (int column = 0; column < 2; ++column)
            {
                const std::vector<Control>& controls = column == 0 ? first : second;
                for (std::size_t i = 0; i < controls.size(); ++i)
                {
                    float x = column == 0 ? 1004.0f : 1434.0f;
                    const float y = 608.0f + 32.0f * static_cast<float>(i);
                    for (const std::string& key : controls[i].keys)
                    {
                        x += fd::KeyCapDraw(f, {x, y}, key) + 6.0f;
                    }
                    fd::TextMid(f, fd::kMono, 15.0f, {x + 6.0f, y}, tone::kDim, controls[i].text);
                }
            }
            fd::TextMid(f, fd::kMono, 14.0f, {1004.0f, 806.0f}, tone::kFaint, "The keys are DOOM's own. Game options, saving and loading: DOOM MENU in the pause.");
        }

        if (any)
        {
            changed = true;
            savedTime_ = time_;
            Sound(fd::Sfx::Change);
        }
        NavChain(f, chain, true);
        fd::Hints(f, {{{"UP", "DOWN"}, "SELECT"}, {{"LEFT", "RIGHT"}, "CHANGE"}, {{"ESC"}, "BACK"}});
        if (f.TakeNav(Nav::Back))
        {
            changed = true;
            Sound(fd::Sfx::Back);
            Go(f, Screen::Main);
        }
    }

    // ---- about (§3.4)

    void DoomMenu::DrawAbout(Frame& f)
    {
        const float flicker = fx_.reduced ? 1.0f : Lamp(static_cast<float>((time_ - enterTime_ - 0.06) / 0.14));
        const Id tocId = HashId("fd/toc");
        f.SetDefaultFocus(tocId);
        const Rect sheet{488.0f, 238.0f, 940.0f, 742.0f};
        fd::Sheet(f, sheet);
        fd::DrawBar(f, fx_);

        fd::TopLeft(f, {{"FLYDOOM / ", false, tone::kDim}, {"ABOUT", true, tone::kBone}});
        fd::TopRight(f, {{{std::string("V") + MYENGINE_VERSION_STRING, false, tone::kDim}}});
        fd::TextTop(f, fd::kDisplay, 104.0f, {92.0f, 112.0f}, tone::kBone.Faded(flicker), "ABOUT", Align::Left, 0.02f);

        const std::vector<CreditSection>& sections = Credits();
        // where the sheet is now: the offset of the list is kept by the kit under this id
        const Id scrollId = HashId("fd/about");
        float& offset = f.FloatState(scrollId);
        const Rect view{536.0f, 262.0f, 860.0f, 690.0f};
        int active = 0;
        for (std::size_t i = 0; i < sectionTops_.size(); ++i)
        {
            if (sectionTops_[i] <= offset + 40.0f)
            {
                active = static_cast<int>(i);
            }
        }

        // the table of contents
        std::vector<Id> tocIds;
        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            const Id id = tocId + static_cast<Id>(i);
            tocIds.push_back(id);
            const Rect rect{72.0f, 262.0f + 56.0f * static_cast<float>(i), 340.0f, 56.0f};
            const rlui::ItemState state = f.Item(id, rect, true);
            if (state.focused)
            {
                fd::BarTarget(fx_, rect, 6.0f);
            }
            fd::CapsMid(f, fd::kMono, 15.0f, {96.0f, rect.y + 28.0f}, state.focused || static_cast<int>(i) == active ? tone::kRed : tone::kFaint, Format("%02d", static_cast<int>(i) + 1));
            fd::CapsMid(f, fd::kItem, 30.0f, {132.0f, rect.y + 28.0f}, state.focused ? tone::kBone : (static_cast<int>(i) == active ? tone::kText : tone::kDim), sections[i].title, Align::Left, 0.02f);
            if (state.activated && i < sectionTops_.size())
            {
                offset = sectionTops_[i];
                Sound(fd::Sfx::Select);
            }
            if (state.focused && f.TakeNav(Nav::Right))
            {
                f.SetFocus(HashId("fd/aboutsheet"));
            }
        }
        {
            float y = 531.0f;
            for (const std::string& line : fd::Wrap(f, fd::kMono, 14.0f, "FlyDoom is free software. The sources and the licences are in the repository (COPYING, game/doom/third_party).", 300.0f))
            {
                fd::TextMid(f, fd::kMono, 14.0f, {96.0f, y}, tone::kFaint, line);
                y += 22.0f;
            }
        }

        // the sheet: it takes the focus to be read with the arrows
        const Id sheetId = HashId("fd/aboutsheet");
        const rlui::ItemState reading = f.Item(sheetId, sheet, true);
        if (reading.focused)
        {
            if (f.TakeNav(Nav::Down))
            {
                offset += 56.0f;
            }
            else if (f.TakeNav(Nav::Up))
            {
                offset -= 56.0f;
            }
            else if (f.TakeNav(Nav::Left))
            {
                f.SetFocus(tocIds[static_cast<std::size_t>(active)]);
            }
        }

        // the text
        struct Placed
        {
            float y;
            int kind;   // 0 heading, 1 text line
            std::string index;
            std::string text;
            std::vector<Word> words;
        };
        std::vector<Placed> placed;
        sectionTops_.clear();
        float cursor = 0.0f;
        const float wrapWidth = 844.0f;
        const float spaceWidth = paint::TextWidth(f, fd::kMono, 17.0f, " ");
        for (std::size_t s = 0; s < sections.size(); ++s)
        {
            sectionTops_.push_back(cursor);
            placed.push_back({cursor, 0, Format("%02d", static_cast<int>(s) + 1), sections[s].title, {}});
            cursor += 62.0f;
            for (const char* paragraph : sections[s].paragraphs)
            {
                // the lines of a paragraph
                std::vector<Word> line;
                float width = 0.0f;
                const auto flush = [&]()
                {
                    if (!line.empty())
                    {
                        placed.push_back({cursor, 1, {}, {}, line});
                        cursor += 28.0f;
                        line.clear();
                        width = 0.0f;
                    }
                };
                for (const Word& word : WordsOf(paragraph))
                {
                    const float w = paint::TextWidth(f, fd::kMono, 17.0f, word.text);
                    if (!line.empty() && width + spaceWidth + w > wrapWidth)
                    {
                        flush();
                    }
                    width += (line.empty() ? 0.0f : spaceWidth) + w;
                    line.push_back(word);
                }
                flush();
                cursor += 12.0f;
            }
            cursor += 14.0f;
        }
        const float contentHeight = cursor;
        offset = std::clamp(offset, 0.0f, std::max(contentHeight - view.h, 0.0f));
        {
            ScrollView scroll(f, scrollId, view, contentHeight);
            for (const Placed& item : placed)
            {
                const float y = view.y + item.y;
                if (item.kind == 0)
                {
                    fd::TextMid(f, fd::kMonoBold, 15.0f, {view.x, y + 18.0f}, tone::kRed, item.index);
                    fd::CapsMid(f, fd::kItem, 34.0f, {view.x + 36.0f, y + 18.0f}, tone::kBone, item.text, Align::Left, 0.02f);
                }
                else
                {
                    float x = view.x;
                    for (const Word& word : item.words)
                    {
                        fd::TextMid(f, fd::kMono, 17.0f, {x, y + 14.0f}, word.highlight ? tone::kBrain : tone::kText, word.text);
                        x += paint::TextWidth(f, fd::kMono, 17.0f, word.text) + spaceWidth;
                    }
                }
            }
        }
        // the end of the visible text fades into the sheet, a mark says that there is more
        const bool more = contentHeight > view.h && offset < contentHeight - view.h - 2.0f;
        if (more)
        {
            const Color sheetColour = Color::Hex(0x0E0F11);
            paint::FillRectGradientV(f, {view.x - 8.0f, view.y + view.h - 100.0f, view.w + 16.0f, 100.0f}, sheetColour.Faded(0.0f), sheetColour.Faded(0.97f));
            fd::DrawRuns(f, {{"\xE2\x86\x93 MORE", true, tone::kRed}, {Format(" \xC2\xB7 %d OF %d \xC2\xB7 WHEEL OR \xE2\x86\x93", active + 1, static_cast<int>(sections.size())), false, tone::kFaint}}, 13.0f, {view.x, 962.0f}, Align::Left);
        }
        // the bar at the right
        {
            const float track = 690.0f;
            paint::FillRect(f, {1418.0f, view.y, 2.0f, track}, tone::kLine);
            if (contentHeight > view.h)
            {
                const float thumb = std::max(40.0f, track * view.h / contentHeight);
                const float top = (track - thumb) * offset / std::max(contentHeight - view.h, 1.0f);
                paint::FillRect(f, {1418.0f, view.y + top, 2.0f, thumb}, tone::kBone);
            }
        }

        NavChain(f, tocIds, true);
        fd::Hints(f, {{{"UP", "DOWN"}, "READ"}, {{"ESC"}, "BACK"}});
        if (f.TakeNav(Nav::Back))
        {
            Sound(fd::Sfx::Back);
            Go(f, Screen::Main);
        }
    }

    // ---- the pause (§3.5)

    DoomMenu::PauseResult DoomMenu::DrawPause(Frame& f, const bool canGoBack, const bool allowBack, const PauseInfo& info)
    {
        const double now = f.Time();
        if (now - pauseDrawn_ > 0.5)
        {
            // the pause has just opened
            pauseOpened_ = now;
            fd::Fx fresh;
            fresh.play = fx_.play;
            fx_ = fresh;
            f.SetFocus(kNoId);
            Sound(fd::Sfx::PauseOpen);
            lastFocus_ = kNoId;
        }
        pauseDrawn_ = now;
        fx_.reduced = info.reduceMotion;
        const double t = now - pauseOpened_;
        f.SetSpace(Space::Screen);
        paint::Dim(f, Color::Hex(0x060607, 0.62f * (fx_.reduced ? 1.0f : Ease(static_cast<float>(t / 0.15)))));
        {
            // the left is darker, so that the panel of the brain and the HUD under it do not read
            const Vec2 window = f.metrics().designWindow;
            const float width = 1100.0f;
            paint::FillRectGradient(f, {0.0f, 0.0f, width * 0.55f, window.y}, tone::kInk.Faded(0.97f), tone::kInk.Faded(0.95f));
            paint::FillRectGradient(f, {width * 0.55f, 0.0f, width * 0.45f, window.y}, tone::kInk.Faded(0.95f), tone::kInk.Faded(0.0f));
        }
        f.SetSpace(Space::Menu);
        fd::PaintFrame(f, 1.0f, false, info.frameRight);
        f.SetDefaultFocus(HashId("fd/pause-resume"));
        fx_.bar.gate = 1.0f;
        fx_.bar.shift = 0.0f;
        fd::DrawBar(f, fx_);

        if (!info.line.empty())
        {
            fd::TopLeft(f, {{info.line, false, tone::kDim}});
        }
        if (!info.backend.empty())
        {
            fd::Run dot;
            dot.dot = true;
            dot.color = tone::kBrain;
            std::vector<std::vector<fd::Run>> right = {{dot, {"BRAIN ON ", false, tone::kDim}, {info.backend, true, tone::kBone}}};
            if (!info.speed.empty())
            {
                const std::size_t space = info.speed.find(' ');
                right.push_back({{info.speed.substr(0, space), true, tone::kBone}, {space == std::string::npos ? std::string() : " " + info.speed.substr(space + 1), false, tone::kDim}});
            }
            fd::TopRight(f, right, info.rightEdge);
        }
        const float flicker = fx_.reduced ? 1.0f : Lamp(static_cast<float>(t / 0.06));
        fd::TextTop(f, fd::kDisplay, 200.0f, {92.0f, 150.0f}, tone::kBone.Faded(flicker), "PAUSED", Align::Left, 0.02f);

        PauseResult result = PauseResult::None;
        struct Item
        {
            const char* id;
            const char* index;
            const char* word;
            const char* sub;
            PauseResult action;
        };
        std::vector<Item> items;
        if (info.progressScreen && info.canContinue)
        {
            items.push_back({"fd/pause-continue", "01", "CONTINUE", "Advance the DOOM results", PauseResult::Continue});
        }
        else
        {
            items.push_back({"fd/pause-resume", "01", "RESUME", "", PauseResult::Resume});
        }
        items.push_back({"fd/pause-doom", "02", "DOOM MENU", "Save, load, options of DOOM", PauseResult::DoomMenu});
        if (canGoBack)
        {
            items.push_back({"fd/pause-lab", "03", info.progressScreen ? "MAIN MENU" : "LABORATORY",
                             info.progressScreen ? "Return to the FlyDoom main menu" : "Starts FlyDoom again: DOOM runs once per process", PauseResult::Laboratory});
        }
        items.push_back({"fd/pause-exit", canGoBack ? "04" : "03", "EXIT", "", PauseResult::Quit});
        float y = 420.0f;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const float local = Phase(t, 0.1 + 0.04 * static_cast<double>(i), 0.18);
            const float dx = fx_.reduced ? 0.0f : (1.0f - Ease(local)) * -24.0f;
            fd::MenuItemSpec spec{items[i].index, items[i].word, items[i].sub, true};
            const rlui::ItemState state = fd::MenuItem(f, fx_, HashId(items[i].id), y, spec, fx_.reduced ? 1.0f : Ease(local), dx);
            y += items[i].sub[0] == 0 ? 76.0f : 104.0f;
            if (state.activated)
            {
                result = items[i].action;
            }
        }
        fd::Hints(f, {{{"UP", "DOWN"}, "SELECT"}, {{"ENTER"}, "CONFIRM"}, {{"ESC"}, "RESUME"}});
        if (allowBack && f.TakeNav(Nav::Back) && result == PauseResult::None)
        {
            result = PauseResult::Resume;
        }
        const Id focus = f.Focus();
        if (lastFocus_ != kNoId && focus != kNoId && focus != lastFocus_)
        {
            Sound(fd::Sfx::Focus);
        }
        lastFocus_ = focus;
        if (result == PauseResult::Resume)
        {
            Sound(fd::Sfx::PauseClose);
        }
        else if (result != PauseResult::None)
        {
            Sound(fd::Sfx::Select);
        }
        return result;
    }
}
