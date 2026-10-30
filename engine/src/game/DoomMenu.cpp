// DoomMenu.cpp

#include "DoomMenu.h"

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

        // the lines of "About": (kind, text); kind 0 a heading, 1 a line, 2 a gap
        struct CreditLine
        {
            int kind;
            const char* text;
        };
        constexpr CreditLine kCredits[] = {
            {0, "THE GAME"},
            {1, "DOOM (1993) by id Software. The source of 1997 is free software under the GNU GPL v2, taken through doomgeneric (ozkl, GPL v2)."},
            {1, "The music is played on an emulated OPL chip as in the original: the routines of Chocolate Doom (GPL v2) and Nuked OPL3 (Nuke.YKT, LGPL v2.1+)."},
            {1, "Game data: Freedoom (BSD style licence, freedoom.github.io), or the shareware DOOM (doom1.wad, id Software, unchanged) or your own DOOM."},
            {1, "FlyDoom is free software: the sources and the licences are in the repository of the project (COPYING, game/doom/third_party)."},
            {2, ""},
            {0, "THE BRAIN"},
            {1, "The connectome: MaleCNS v1.0 (Janelia FlyEM, University of Cambridge, MRC LMB, Google Research), 164,789 traced neurons, licence CC BY 4.0."},
            {1, "Berg S. et al. (2026) Sexual dimorphism in the complete Drosophila male central nervous system connectome. Cell 189(18):5504-5526, doi:10.1016/j.cell.2026.08.015."},
            {1, "Mirror: QuixiAI/MaleCNS on Hugging Face (Hartford E. 2026, doi:10.57967/hf/10410). Changes: only the proofread neurons, the synapses as a sparse matrix, signed by the transmitter."},
            {1, "The model: leaky integrate-and-fire with the constants of Shiu P. et al. (2024) A Drosophila computational brain model reveals sensorimotor processing. Nature 634 (code: MIT)."},
            {1, "The way a fly can play DOOM: DOOMFLY by Alex Wormuth (MIT). It is not a real fly: a network of point neurons wired like the real one."},
            {2, ""},
            {0, "THE LOOK"},
            {1, "Exo 2 (Natanael Gama, SIL Open Font License 1.1), Lucide icons (ISC), Dear ImGui (MIT)."},
            {2, ""},
            {1, "FlyDoom is part of myengine, an educational DirectX 12 engine (ITMO)."},
        };
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

    void DoomMenu::Reset()
    {
        screen_ = Screen::Main;
        quitAsked_ = false;
    }

    DoomMenu::PauseResult DoomMenu::DrawPause(Frame& f, const bool canGoBack, const bool allowBack)
    {
        f.SetSpace(rlui::Space::Screen);
        paint::Dim(f, rlui::Color{0.0f, 0.0f, 0.0f, 0.62f});
        f.SetSpace(rlui::Space::Menu);
        SideShade(f, 760.0f);
        f.SetDefaultFocus(HashId("fd/pause-resume"));
        ScreenTitle(f, "PAUSE", "FLY\nDOOM", {80.0f, 76.0f}, true);
        PauseResult result = PauseResult::None;
        Column column({80.0f, 470.0f, style::kMenuButtonWidth, style::kMenuButtonHeight}, style::kMenuButtonStep);
        if (MenuButton(f, HashId("fd/pause-resume"), column.Next(), "RESUME", Icon::Play))
        {
            result = PauseResult::Resume;
        }
        if (MenuButton(f, HashId("fd/pause-doom"), column.Next(), "DOOM MENU", Icon::Settings))
        {
            result = PauseResult::DoomMenu;
        }
        if (canGoBack && MenuButton(f, HashId("fd/pause-lab"), column.Next(), "LABORATORY", Icon::Home))
        {
            result = PauseResult::Laboratory;
        }
        if (MenuButton(f, HashId("fd/pause-exit"), column.Next(), "EXIT", Icon::Exit))
        {
            result = PauseResult::Quit;
        }
        if (canGoBack)
        {
            paint::Text(f, FontStyle::SemiBold, 20.0f, {84.0f, 900.0f}, style::kTextDim, "LABORATORY starts the program again: DOOM runs once in a process.");
        }
        std::vector<Hint> hints = {{Glyph::UpDown, Glyph::PadDpad, "Select"}, {Glyph::Enter, Glyph::PadA, "Confirm"}, {Glyph::Esc, Glyph::PadB, "Resume"}};
        HintBar(f, Span<Hint>(hints));
        if (allowBack && f.input().Pressed(Nav::Back) && result == PauseResult::None)
        {
            result = PauseResult::Resume;
        }
        return result;
    }

    bool DoomMenu::Nested() const
    {
        return screen_ != Screen::Main;
    }

    void DoomMenu::ShowScreen(const std::string_view name)
    {
        screen_ = name == "play" ? Screen::Play : (name == "settings" ? Screen::Settings : (name == "about" ? Screen::About : Screen::Main));
    }

    void DoomMenu::Go(Frame& f, const Screen next)
    {
        screen_ = next;
        f.ResetFocus();
    }

    void DoomMenu::SideShade(Frame& f, const float width)
    {
        const Vec2 window = f.metrics().designWindow;
        f.SetSpace(Space::Screen);
        paint::FillRectGradient(f, {0.0f, 0.0f, width * window.x / 1920.0f + 400.0f, window.y}, Color::Hex(0x040a18, 0.90f), Color::Hex(0x040a18, 0.0f));
        f.SetSpace(Space::Menu);
    }

    DoomMenu::Result DoomMenu::Draw(Frame& f, DoomSettings& settings, const DoomMenuModel& model, const double timeSeconds, DoomStartRequest& request, bool& settingsChanged)
    {
        (void)timeSeconds;
        Result result = Result::None;
        if (!initialised_)
        {
            initialised_ = true;
            // the choices of the last time
            for (std::size_t i = 0; i < model.modes.size(); ++i)
            {
                if (model.modes[i].id == settings.lastMode && model.modes[i].available)
                {
                    modeIndex_ = static_cast<int>(i);
                }
            }
            for (std::size_t i = 0; i < model.wads.size(); ++i)
            {
                if (!settings.iwad.empty() && model.wads[i].path.string() == settings.iwad)
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
        switch (screen_)
        {
        case Screen::Main: DrawMain(f, settings, model, result); break;
        case Screen::Play: DrawPlay(f, settings, model, result, request); break;
        case Screen::Settings: DrawSettings(f, settings, settingsChanged); break;
        case Screen::About: DrawAbout(f); break;
        }
        return result;
    }

    void DoomMenu::DrawMain(Frame& f, const DoomSettings& settings, const DoomMenuModel& model, Result& result)
    {
        (void)settings;
        SideShade(f, 760.0f);
        f.SetDefaultFocus(HashId("fd/play"));
        ScreenTitle(f, "MYENGINE", "FLY\nDOOM", {80.0f, 76.0f}, true);
        const float versionWidth = Chip(f, {1850.0f, 60.0f}, std::string("V") + MYENGINE_VERSION_STRING);
        if (!model.brainStatus.empty())
        {
            Chip(f, {1850.0f - versionWidth - 12.0f, 60.0f}, "BRAIN: " + model.brainStatus, Icon::Eye);
        }
        Column column({80.0f, 470.0f, style::kMenuButtonWidth, style::kMenuButtonHeight}, style::kMenuButtonStep);
        if (MenuButton(f, HashId("fd/play"), column.Next(), "PLAY", Icon::Play))
        {
            Go(f, Screen::Play);
        }
        if (MenuButton(f, HashId("fd/settings"), column.Next(), "SETTINGS", Icon::Settings))
        {
            Go(f, Screen::Settings);
        }
        if (MenuButton(f, HashId("fd/about"), column.Next(), "ABOUT", Icon::Info))
        {
            Go(f, Screen::About);
        }
        if (MenuButton(f, HashId("fd/exit"), column.Next(), "EXIT", Icon::Exit))
        {
            quitAsked_ = true;
        }
        if (!model.wadProblem.empty())
        {
            const Rect plate{80.0f, 800.0f, 700.0f, 56.0f};
            paint::Parallelogram(f, plate, Color::Hex(0x3a2e0c, 0.86f));
            paint::ParallelogramOutline(f, plate, style::kBoostA.Faded(0.7f), 1.5f);
            paint::IconGlyph(f, Icon::Info, 24.0f, {plate.x + 36.0f, plate.y + plate.h * 0.5f}, style::kBoostA);
            paint::Text(f, FontStyle::SemiBold, 20.0f, {plate.x + 62.0f, plate.y + 14.0f}, style::kBoostA, "Game data not found: PLAY shows how to add DOOM or Freedoom");
        }
        else if (!model.brainProblem.empty())
        {
            paint::Text(f, FontStyle::SemiBold, 22.0f, {84.0f, 840.0f}, style::kTextDim, model.brainProblem);
        }
        std::vector<Hint> hints = {{Glyph::UpDown, Glyph::PadDpad, "Select"}, {Glyph::Enter, Glyph::PadA, "Confirm"}};
        HintBar(f, Span<Hint>(hints));
        if (quitAsked_)
        {
            const DialogResult answer = ConfirmDialog(f, HashId("fd/quit"), {"LEAVE FLYDOOM?", "Your settings are saved.", "Leave", "Stay"});
            if (answer == DialogResult::Confirm)
            {
                result = Result::Quit;
            }
            else if (answer == DialogResult::Cancel)
            {
                quitAsked_ = false;
            }
        }
        else if (f.input().Pressed(Nav::Back))
        {
            quitAsked_ = true;
        }
    }

    void DoomMenu::DrawPlay(Frame& f, DoomSettings& settings, const DoomMenuModel& model, Result& result, DoomStartRequest& request)
    {
        SideShade(f, 1500.0f);
        f.SetDefaultFocus(HashId("fd/start"));
        ScreenTitle(f, "PLAY", "CHOOSE HOW");
        Chip(f, {1850.0f, 60.0f}, model.brainStatus.empty() ? std::string("BRAIN: ...") : "BRAIN: " + model.brainStatus, Icon::Eye);

        // the cards of the modes
        modeIndex_ = std::clamp(modeIndex_, 0, std::max(0, static_cast<int>(model.modes.size()) - 1));
        const float cardTop = 200.0f;
        for (std::size_t i = 0; i < model.modes.size(); ++i)
        {
            const DoomMenuModel::Mode& mode = model.modes[i];
            const Rect rect{80.0f + static_cast<float>(i) * (style::kModeCardWidth + 22.0f), cardTop, style::kModeCardWidth, style::kModeCardHeight};
            const std::string big = mode.id == "human" ? "YOU" : (mode.id == "fly" ? "FLY" : (mode.id == "human-vs-fly" ? "VS" : (mode.id == "fly-vs-fly" ? "2 FLY" : "SWARM")));
            if (mode.available)
            {
                const ItemState state = f.Item(HashId("fd/mode") + static_cast<Id>(i), rect, true);
                if (state.focused)
                {
                    paint::Parallelogram(f, rect, style::kFocus);
                    paint::ParallelogramOutline(f, rect, style::kAccent, 3.0f);
                }
                else
                {
                    paint::Parallelogram(f, rect, state.hovered ? style::kNavy3.Faded(0.9f) : style::kNavy);
                    paint::ParallelogramOutline(f, rect, static_cast<int>(i) == modeIndex_ ? style::kBlue : style::kLine, static_cast<int>(i) == modeIndex_ ? 2.5f : 1.0f);
                }
                const Color ink = state.focused ? style::kFocusText : style::kText;
                paint::TextIn(f, FontStyle::BlackItalic, big.size() > 3 ? 44.0f : 56.0f, {rect.x, rect.y + 70.0f, rect.w, 90.0f}, ink, big, Align::Center);
                paint::TextIn(f, FontStyle::ExtraBoldItalic, 24.0f, {rect.x, rect.y + 190.0f, rect.w, 40.0f}, ink, mode.title, Align::Center);
                paint::TextIn(f, FontStyle::SemiBold, 18.0f, {rect.x, rect.y + 232.0f, rect.w, 30.0f}, state.focused ? Color::Hex(0x4a5d85) : style::kTextDim, mode.caption, Align::Center);
                if (state.activated)
                {
                    modeIndex_ = static_cast<int>(i);
                }
            }
            else
            {
                // a mode that is not made yet: a dim plate, no focus
                paint::Parallelogram(f, rect, Color::Hex(0x0a1630, 0.55f));
                paint::ParallelogramOutline(f, rect, style::kLine, 1.0f);
                paint::TextIn(f, FontStyle::BlackItalic, big.size() > 3 ? 44.0f : 56.0f, {rect.x, rect.y + 70.0f, rect.w, 90.0f}, style::kTextFaint, big, Align::Center);
                paint::TextIn(f, FontStyle::Bold, 24.0f, {rect.x, rect.y + 190.0f, rect.w, 40.0f}, style::kTextFaint, mode.title, Align::Center);
                paint::TextIn(f, FontStyle::SemiBold, 18.0f, {rect.x, rect.y + 232.0f, rect.w, 30.0f}, style::kTextFaint, mode.caption, Align::Center);
                paint::TextIn(f, FontStyle::ExtraBoldItalic, 22.0f, {rect.x, rect.y + 280.0f, rect.w, 30.0f}, style::kBoostA, mode.reason.empty() ? "SOON" : mode.reason, Align::Center);
            }
        }
        const DoomMenuModel::Mode& chosen = model.modes[static_cast<std::size_t>(modeIndex_)];
        paint::Text(f, FontStyle::SemiBold, 22.0f, {82.0f, cardTop + style::kModeCardHeight + 14.0f}, style::kTextDim, chosen.description);
        const bool flyMode = chosen.id == "fly" || chosen.id == "human-vs-fly" || chosen.id == "fly-vs-fly" || chosen.id == "swarm";

        // the rows
        float y = 610.0f;
        const float step = 62.0f;
        const float rowWidth = 1100.0f;
        const float rowHeight = 56.0f;
        const auto next = [&]() { const Rect r{80.0f, y, rowWidth, rowHeight}; y += step; return r; };

        if (model.wads.empty())
        {
            ValueRow(f, HashId("fd/wad"), next(), "Game data", "not found", true, false);
        }
        else
        {
            wadIndex_ = std::clamp(wadIndex_, 0, static_cast<int>(model.wads.size()) - 1);
            const DoomWadEntry& wad = model.wads[static_cast<std::size_t>(wadIndex_)];
            const int stepWad = StepperRow(f, HashId("fd/wad"), next(), "Game data", wad.label, wad.path.filename().string());
            if (stepWad != 0)
            {
                wadIndex_ = (wadIndex_ + stepWad + static_cast<int>(model.wads.size())) % static_cast<int>(model.wads.size());
                levelIndex_ = 0;
            }
            const DoomWadEntry& current = model.wads[static_cast<std::size_t>(wadIndex_)];
            levelIndex_ = std::clamp(levelIndex_, 0, static_cast<int>(current.levels.size()) - 1);
            const int stepLevel = StepperRow(f, HashId("fd/level"), next(), "Start at", current.levels[static_cast<std::size_t>(levelIndex_)].label);
            if (stepLevel != 0)
            {
                levelIndex_ = (levelIndex_ + stepLevel + static_cast<int>(current.levels.size())) % static_cast<int>(current.levels.size());
            }
        }
        {
            const int stepSkill = StepperRow(f, HashId("fd/skill"), next(), "Skill", kSkillNames[std::clamp(settings.skill, 1, 5) - 1]);
            settings.skill = std::clamp(settings.skill + stepSkill, 1, 5);
        }
        if (flyMode)
        {
            const std::string_view sets[] = {"DOOMFLY neurons", "Bio neurons"};
            settings.controlSet = OptionRow(f, HashId("fd/neurons"), next(), "Neurons", Span<std::string_view>(sets), settings.controlSet == "bio" ? 1 : 0) == 1 ? "bio" : "doomfly";
            const std::string_view backends[] = {"Auto", "GPU", "CPU"};
            const int backend = OptionRow(f, HashId("fd/backend"), next(), "Brain on", Span<std::string_view>(backends), settings.backend == "gpu" ? 1 : (settings.backend == "cpu" ? 2 : 0));
            settings.backend = backend == 1 ? "gpu" : (backend == 2 ? "cpu" : "auto");
        }

        // start
        const bool canStart = chosen.available && !model.wads.empty() && (!flyMode || model.flyAvailable);
        if (flyMode && !model.flyAvailable)
        {
            paint::Text(f, FontStyle::SemiBold, 20.0f, {1180.0f, 864.0f}, style::kBad, "The connectome file is not here: tools\\flybrain\\fetch_malecns.ps1");
        }
        if (model.wads.empty())
        {
            paint::Text(f, FontStyle::SemiBold, 20.0f, {1180.0f, 864.0f}, style::kBad, "Put a WAD into %LOCALAPPDATA%\\FlyDoom\\wads (see ABOUT)");
        }
        if (ActionButton(f, HashId("fd/start"), {1180.0f, 900.0f, 560.0f, 72.0f}, "START", Icon::Play, canStart, true) && canStart)
        {
            const DoomWadEntry& wad = model.wads[static_cast<std::size_t>(wadIndex_)];
            const DoomLevelEntry& level = wad.levels[static_cast<std::size_t>(levelIndex_)];
            request = {};
            request.modeId = chosen.id;
            request.choice.controlSet = settings.controlSet;
            request.choice.backend = settings.backend;
            request.choice.episode = level.episode;
            request.choice.map = level.map;
            request.choice.skill = settings.skill;
            request.iwad = wad.path;
            request.doomArgs = WarpArguments(wad.path, level, settings.skill);
            settings.lastMode = chosen.id;
            settings.iwad = wad.path.string();
            settings.episode = level.episode;
            settings.map = level.map;
            result = Result::Start;
        }
        std::vector<Hint> hints = {{Glyph::AllArrows, Glyph::PadDpad, "Choose"}, {Glyph::Enter, Glyph::PadA, "Confirm"}, {Glyph::Esc, Glyph::PadB, "Back"}};
        HintBar(f, Span<Hint>(hints));
        if (f.input().Pressed(Nav::Back))
        {
            Go(f, Screen::Main);
        }
    }

    void DoomMenu::DrawSettings(Frame& f, DoomSettings& settings, bool& changed)
    {
        SideShade(f, 1500.0f);
        f.SetDefaultFocus(HashId("fd/sfx"));
        ScreenTitle(f, "SETTINGS", "SOUND, MOUSE, WINDOW");
        float y = 220.0f;
        const float step = 66.0f;
        const auto next = [&]() { const Rect r{80.0f, y, 1100.0f, 58.0f}; y += step; return r; };
        const auto percent = [](const float value) { return Format("%d%%", static_cast<int>(std::lround(value * 100.0f))); };

        {
            const float value = SliderRow(f, HashId("fd/sfx"), next(), "Sound effects", settings.sfxVolume * 100.0f, 0.0f, 100.0f, 5.0f, percent(settings.sfxVolume)) / 100.0f;
            changed = changed || value != settings.sfxVolume;
            settings.sfxVolume = value;
        }
        {
            const float value = SliderRow(f, HashId("fd/music"), next(), "Music", settings.musicVolume * 100.0f, 0.0f, 100.0f, 5.0f, percent(settings.musicVolume)) / 100.0f;
            changed = changed || value != settings.musicVolume;
            settings.musicVolume = value;
        }
        {
            const bool value = ToggleRow(f, HashId("fd/musicon"), next(), "Music of DOOM", settings.musicOn);
            changed = changed || value != settings.musicOn;
            settings.musicOn = value;
        }
        {
            const float value = SliderRow(f, HashId("fd/mouse"), next(), "Mouse turn", settings.mouseSensitivity * 100.0f, 50.0f, 150.0f, 10.0f, Format("%.2fx", settings.mouseSensitivity)) / 100.0f;
            changed = changed || value != settings.mouseSensitivity;
            settings.mouseSensitivity = value;
        }
        {
            const std::string_view modes[] = {"Windowed", "Borderless", "Fullscreen"};
            const int value = OptionRow(f, HashId("fd/window"), next(), "Window mode", Span<std::string_view>(modes), settings.windowMode);
            changed = changed || value != settings.windowMode;
            settings.windowMode = value;
        }
        {
            const int value = static_cast<int>(SliderRow(f, HashId("fd/uiscale"), next(), "UI scale", static_cast<float>(settings.uiScalePercent), 75.0f, 125.0f, 5.0f,
                                                         Format("%d%%", settings.uiScalePercent)));
            changed = changed || value != settings.uiScalePercent;
            settings.uiScalePercent = value;
        }
        {
            const bool value = ToggleRow(f, HashId("fd/backdrop"), next(), "Brain behind the menu", settings.brainBackdrop);
            changed = changed || value != settings.brainBackdrop;
            settings.brainBackdrop = value;
        }
        Chip(f, {1850.0f, 60.0f}, "SAVED AS YOU CHANGE", Icon::Check);
        std::vector<Hint> hints = {{Glyph::UpDown, Glyph::PadDpad, "Select"}, {Glyph::LeftRight, Glyph::PadDpad, "Change"}, {Glyph::Esc, Glyph::PadB, "Back"}};
        HintBar(f, Span<Hint>(hints));
        if (f.input().Pressed(Nav::Back))
        {
            changed = true;
            Go(f, Screen::Main);
        }
    }

    void DoomMenu::DrawAbout(Frame& f)
    {
        SideShade(f, 1700.0f);
        ScreenTitle(f, "ABOUT", "WHO MADE THIS");
        // the text lies on a plate: the cloud behind the screen does not take its contrast
        const Rect plate{64.0f, 236.0f, 1250.0f, 650.0f};
        paint::Parallelogram(f, plate, style::kNavy);
        paint::ParallelogramOutline(f, plate, style::kLine, 1.0f);
        const Rect view{96.0f, 258.0f, 1150.0f, 608.0f};
        // the lines that are longer than the view go on to the next row
        struct Row
        {
            int kind;
            std::string text;
        };
        std::vector<Row> rows;
        for (const CreditLine& line : kCredits)
        {
            if (line.kind != 1)
            {
                rows.push_back({line.kind, line.text});
                continue;
            }
            std::string current;
            std::string_view rest = line.text;
            while (!rest.empty())
            {
                const std::size_t space = rest.find(' ');
                const std::string word(rest.substr(0, space));
                rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
                const std::string trial = current.empty() ? word : current + " " + word;
                if (!current.empty() && paint::TextWidth(f, FontStyle::SemiBold, 20.0f, trial) > view.w - 40.0f)
                {
                    rows.push_back({1, current});
                    current = "   " + word;
                }
                else
                {
                    current = trial;
                }
            }
            rows.push_back({1, current});
        }
        float height = 0.0f;
        for (const Row& row : rows)
        {
            height += row.kind == 0 ? 52.0f : (row.kind == 1 ? 30.0f : 18.0f);
        }
        {
            ScrollView scroll(f, HashId("fd/about"), view, height);
            float y = view.y - scroll.Offset();
            for (const Row& row : rows)
            {
                if (row.kind == 0)
                {
                    paint::Text(f, FontStyle::ExtraBoldItalic, 30.0f, {view.x, y}, style::kAccent, row.text);
                    y += 52.0f;
                }
                else if (row.kind == 1)
                {
                    paint::Text(f, FontStyle::SemiBold, 20.0f, {view.x + 12.0f, y}, style::kText, row.text);
                    y += 30.0f;
                }
                else
                {
                    y += 18.0f;
                }
            }
        }
        f.SetDefaultFocus(HashId("fd/aboutback"));
        if (ActionButton(f, HashId("fd/aboutback"), {1180.0f, 900.0f, 560.0f, 72.0f}, "BACK", Icon::Home))
        {
            Go(f, Screen::Main);
        }
        std::vector<Hint> hints = {{Glyph::Esc, Glyph::PadB, "Back"}};
        HintBar(f, Span<Hint>(hints));
        if (f.input().Pressed(Nav::Back))
        {
            Go(f, Screen::Main);
        }
    }
}
