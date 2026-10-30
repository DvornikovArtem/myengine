// DoomGameFlow.cpp

#include <myengine/game/DoomGameFlow.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>
#include <cstdlib>
#include <unordered_map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <imgui/imgui.h>

#include <doom/DoomGame.h>

#include "DoomMenu.h"

#include <myengine/audio/AudioSystem.h>
#include <myengine/game/RlGameSettings.h>
#include <myengine/core/AppPaths.h>
#include <myengine/core/CrashHandler.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/ui/UiManager.h>
#include <myengine/ui/rl/RlUiHost.h>
#include <myengine/ui/rl/RlUiPaint.h>

namespace myengine::game
{
    namespace
    {
        constexpr double kMaxFrameSeconds = 0.25;   // a long stall (a window drag, a breakpoint) is not played back as a burst of tics

        bool ProcessIsForeground()
        {
            DWORD pid = 0;
            const HWND foreground = GetForegroundWindow();
            return foreground != nullptr && GetWindowThreadProcessId(foreground, &pid) != 0 && pid == GetCurrentProcessId();
        }

        // ---- the sound of DOOM on the AudioSystem ----
        class DoomAudioHost final : public doom::IDoomAudioHost
        {
        public:
            DoomAudioHost(audio::AudioSystem& audio, const std::function<void(const std::string&)>& log) : audio_(audio), log_(log) {}

            ~DoomAudioHost() override
            {
                audio_.StopBus(audio::Bus::Music);
                for (auto& voice : voices_)
                {
                    audio_.Stop(voice.second);
                }
                UnloadMusic();
                for (const audio::SoundHandle& sound : sounds_)
                {
                    audio_.UnloadSound(sound);
                }
            }

            int RegisterSfx(const char* name, const std::int16_t* samples, const int frames, const int sampleRate) override
            {
                audio::WavData wav;
                wav.sampleRate = static_cast<std::uint32_t>(sampleRate);
                wav.channels = 1;
                wav.samples.assign(samples, samples + frames);
                const audio::SoundHandle sound = audio_.CreateSound(std::string("doom/") + name, wav);
                if (!sound.IsValid())
                {
                    if (!warned_ && log_)
                    {
                        warned_ = true;
                        log_("DOOM: the sound engine does not take raw sounds (Wwise build?): DOOM plays without sound effects");
                    }
                    return 0;
                }
                sounds_.push_back(sound);
                return static_cast<int>(sound.value);
            }

            bool StartSfx(const int sfx, const int channel, const float volume, const float pan) override
            {
                StopSfx(channel);
                audio::PlayParams params;
                params.bus = audio::Bus::Sfx;
                params.volume = volume;
                params.spatial = true;
                params.rolloff = 0.0f;          // DOOM has already made the volume of the distance
                params.dopplerFactor = 0.0f;
                params.minDistance = 0.5f;
                params.position = Position(pan);
                audio::SoundHandle sound;
                sound.value = static_cast<std::uint32_t>(sfx);
                const audio::VoiceHandle voice = audio_.Play(sound, params);
                if (!voice.IsValid())
                {
                    return false;
                }
                voices_[channel] = voice;
                return true;
            }

            void UpdateSfx(const int channel, const float volume, const float pan) override
            {
                const auto it = voices_.find(channel);
                if (it == voices_.end())
                {
                    return;
                }
                audio_.SetVoiceVolume(it->second, volume);
                audio_.SetVoicePosition(it->second, Position(pan));
            }

            void StopSfx(const int channel) override
            {
                const auto it = voices_.find(channel);
                if (it != voices_.end())
                {
                    audio_.Stop(it->second);
                    voices_.erase(it);
                }
            }

            bool IsSfxPlaying(const int channel) override
            {
                const auto it = voices_.find(channel);
                return it != voices_.end() && audio_.IsPlaying(it->second);
            }

            void PlayMusic(const std::int16_t* stereoFrames, const std::size_t frames, const int sampleRate, const bool loop) override
            {
                StopMusic();
                UnloadMusic();
                audio::WavData wav;
                wav.sampleRate = static_cast<std::uint32_t>(sampleRate);
                wav.channels = 2;
                wav.samples.assign(stereoFrames, stereoFrames + frames * 2);
                music_ = audio_.CreateSound("doom/music", wav);
                if (!music_.IsValid())
                {
                    return;
                }
                audio::PlayParams params;
                params.bus = audio::Bus::Music;
                params.loop = loop;
                params.volume = musicVolume_;
                musicVoice_ = audio_.Play(music_, params);
            }

            void StopMusic() override
            {
                if (musicVoice_.IsValid())
                {
                    audio_.Stop(musicVoice_);
                    musicVoice_ = {};
                }
            }

            void PauseMusic() override
            {
                // the AudioSystem has no pause: the volume goes to 0 and comes back, the song goes on (the pause of DOOM is short)
                if (musicVoice_.IsValid())
                {
                    audio_.SetVoiceVolume(musicVoice_, 0.0f);
                }
            }

            void ResumeMusic() override
            {
                if (musicVoice_.IsValid())
                {
                    audio_.SetVoiceVolume(musicVoice_, musicVolume_);
                }
            }

            void SetMusicVolume(const float volume) override
            {
                musicVolume_ = volume;
                if (musicVoice_.IsValid())
                {
                    audio_.SetVoiceVolume(musicVoice_, volume);
                }
            }

            bool IsMusicPlaying() override { return musicVoice_.IsValid() && audio_.IsPlaying(musicVoice_); }

        private:
            // The listener of the AudioSystem stays where it is (the origin, looking along +z): a sound on the unit circle in front of it at
            // the angle that makes `pan` (-1 left .. 1 right)
            static audio::Vec3 Position(const float pan)
            {
                const float side = std::clamp(pan, -1.0f, 1.0f);
                return {side, 0.0f, std::sqrt(std::max(0.0f, 1.0f - side * side)) + 0.001f};
            }

            void UnloadMusic()
            {
                if (music_.IsValid())
                {
                    audio_.UnloadSound(music_);
                    music_ = {};
                }
            }

            audio::AudioSystem& audio_;
            std::function<void(const std::string&)> log_;
            std::vector<audio::SoundHandle> sounds_;
            std::unordered_map<int, audio::VoiceHandle> voices_;
            audio::SoundHandle music_;
            audio::VoiceHandle musicVoice_;
            float musicVolume_ = 1.0f;
            bool warned_ = false;
        };

        // ---- the keys ----
        struct KeyMap
        {
            int vk;
            int doomKey;
        };

        // The keys of the game. WASD and the arrows walk and turn; Ctrl and the left button fire; Space and E use; Shift runs; Alt strafes
        // (the keys of DOOM itself, doomkeys.h, are what the original binds: the arrows, Ctrl, Space, Shift, Alt)
        constexpr KeyMap kGameKeys[] = {
            {'W', doom::key::kUp},        {VK_UP, doom::key::kUp},           {'S', doom::key::kDown},          {VK_DOWN, doom::key::kDown},
            {VK_LEFT, doom::key::kLeft},  {VK_RIGHT, doom::key::kRight},     {'A', doom::key::kStrafeLeft},    {'D', doom::key::kStrafeRight},
            {VK_CONTROL, doom::key::kFire}, {VK_SPACE, doom::key::kUse},     {'E', doom::key::kUse},           {VK_SHIFT, doom::key::kShift},
            {VK_MENU, doom::key::kAlt},   {VK_TAB, doom::key::kTab},
        };

        // Keys that are the same in the game and in the menu
        constexpr KeyMap kCommonKeys[] = {
            {VK_ESCAPE, doom::key::kEscape}, {VK_RETURN, doom::key::kEnter}, {VK_BACK, doom::key::kBackspace}, {VK_PAUSE, doom::key::kPause},
            {VK_OEM_MINUS, doom::key::kMinus}, {VK_OEM_PLUS, doom::key::kEquals}, {VK_F1, doom::key::kF1},        {VK_F2, doom::key::kF1 + 1},
            {VK_F3, doom::key::kF1 + 2},     {VK_F4, doom::key::kF1 + 3},     {VK_F5, doom::key::kF1 + 4},       {VK_F6, doom::key::kF1 + 5},
            {VK_F7, doom::key::kF1 + 6},     {VK_F8, doom::key::kF1 + 7},     {VK_F9, doom::key::kF1 + 8},       {VK_F10, doom::key::kF1 + 9},
            {VK_F11, doom::key::kF11},       {VK_F12, doom::key::kF12},
        };

        // In the menu the arrows move and the letters are letters (Y / N answers, the name of a save)
        constexpr KeyMap kMenuKeys[] = {
            {VK_UP, doom::key::kUp},   {VK_DOWN, doom::key::kDown}, {VK_LEFT, doom::key::kLeft}, {VK_RIGHT, doom::key::kRight},
            {VK_SPACE, ' '},
        };
    }

    struct DoomGameFlow::Impl
    {
        DoomGameServices svc;
        enum class State : std::uint8_t
        {
            Start,
            Menu,        // the laboratory: the menu over the brain
            Launching,   // the player pressed START: the menu fades out, then the game is made
            Game,
            Error,
            Finished,
        } state = State::Start;

        std::string message;                             // what the error screen says
        bool crashChecked = false;                       // the report of a crash of the previous run is looked for once, on the first frame of the window
        std::string crashNote;                           // what the banner says about it (core::crash::PlayerNote); empty: no banner
        float crashNoteLeft = 0.0f;                      // seconds the banner stays
        std::unique_ptr<IDoomController> controller;     // nullptr: the human plays
        int chosenMode = 0;
        DoomLaunchChoice launchChoice;                   // what the menu asked for the mode
        std::vector<std::string> warpArguments;          // "-warp 1 3 -skill 4" of the menu
        std::filesystem::path chosenIwad;                // the WAD of the menu (empty: the search decides)
        bool useChoice = false;                          // the mode was chosen in the menu (its `make` gets the choice)
        // the laboratory
        rlui::Host host;
        DoomMenu menu;
        DoomSettings settings;
        DoomMenuModel menuModel;
        std::unique_ptr<IDoomBrainView> brain;
        double menuTime = 0.0;
        float nestedMix = 0.0f;                          // the brain behind the menu steps back on the screens under the main one (0 .. 1)
        float fade = 0.0f;                               // 0 .. 1: the black over the menu while it leaves; the black over the game while it comes (1 .. 0)
        float fadeIn = 0.0f;                             // 1 -> 0 after the game was made
        bool menuBuilt = false;
        // The scripted input of the tests (MYENGINE_UI_SCRIPT): Tab and Esc of the script reach the game too (the real keys are read with GetAsyncKeyState, a script cannot press them)
        std::function<void(rlui::RawInput&)> scriptHook;
        bool scriptedRaw = false;
        bool rawTab = false;
        bool rawEsc = false;
        bool labPause = false;                           // the pause of Esc is open: the game stands, the player chooses (docs/game/doom.md)
        bool escWas = false;                             // Esc was down in the last frame (the edge opens the pause)
        bool escSwallow = false;                         // Esc was down when the pause opened or closed: DOOM does not get it until it is up
        bool tabHeld = false;                            // the key of the big brain, for the edge
        float mouseRemainder = 0.0f;                     // the part of a unit of the scaled mouse movement that was not sent yet
        std::unique_ptr<DoomAudioHost> audioHost;
        std::unique_ptr<doom::DoomGame> game;
        core::WindowId mainWindow = 0;

        // the picture
        render::TextureHandle texture;
        std::uint32_t textureWidth = 0;
        std::uint32_t textureHeight = 0;
        std::uint64_t uploadedFrame = 0;
        std::uint32_t uploadedScale = 0;
        std::vector<std::uint8_t> rgba;
        render::TextureHandle secondTexture;     // the second picture of the controller (SecondaryView)
        std::uint32_t secondWidth = 0;
        std::uint32_t secondHeight = 0;
        std::uint64_t secondSerial = ~0ull;
        std::uint32_t secondScale = 0;
        std::string title;

        // MYENGINE_DOOM_KEYS="60:esc,64:-esc,90:down": keys pressed (and released with "-") at a frame of the flow, for the captures and the tests
        // that cannot press a key on the screen of a hidden desktop. Names: esc enter up down left right fire use y n.
        struct ScriptedKey
        {
            std::uint64_t frame;
            int key;
            bool pressed;
        };
        std::vector<ScriptedKey> scriptedKeys;
        std::uint64_t flowFrame = 0;

        // MYENGINE_DOOM_MOUSE="60:400,61:-250": the raw movement of the mouse (the units of WM_INPUT) at a frame of the flow, as if the hand moved it; the angle
        // of the player after each one goes to the log. For the tests on the hidden desktop, where no real mouse moves.
        std::vector<std::pair<std::uint64_t, int>> scriptedMouse;
        bool scriptedMouseLog = false;
        double scriptedMouseAngle = -1000.0;
        std::uint64_t scriptedMouseLast = 0;

        void LoadScriptedMouse()
        {
            char* text = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&text, &length, "MYENGINE_DOOM_MOUSE") != 0 || text == nullptr)
            {
                return;
            }
            const std::string list(text);
            std::free(text);
            std::size_t begin = 0;
            while (begin < list.size())
            {
                std::size_t end = list.find(',', begin);
                end = end == std::string::npos ? list.size() : end;
                const std::string item = list.substr(begin, end - begin);
                begin = end + 1;
                const std::size_t colon = item.find(':');
                if (colon != std::string::npos)
                {
                    scriptedMouse.emplace_back(std::strtoull(item.c_str(), nullptr, 10), std::atoi(item.c_str() + colon + 1));
                }
            }
        }

        void LoadScriptedKeys()
        {
            LoadScriptedMouse();
            char* text = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&text, &length, "MYENGINE_DOOM_KEYS") != 0 || text == nullptr)
            {
                return;
            }
            std::string list(text);
            std::free(text);
            static const std::map<std::string, int> names = {
                {"esc", doom::key::kEscape}, {"enter", doom::key::kEnter}, {"up", doom::key::kUp},     {"down", doom::key::kDown}, {"left", doom::key::kLeft},
                {"right", doom::key::kRight}, {"fire", doom::key::kFire},   {"use", doom::key::kUse},   {"y", 'y'},               {"n", 'n'},               {"tab", doom::key::kTab}};
            std::size_t begin = 0;
            while (begin < list.size())
            {
                std::size_t end = list.find(',', begin);
                end = end == std::string::npos ? list.size() : end;
                const std::string item = list.substr(begin, end - begin);
                begin = end + 1;
                const std::size_t colon = item.find(':');
                if (colon == std::string::npos)
                {
                    continue;
                }
                std::string name = item.substr(colon + 1);
                const bool pressed = name.empty() || name[0] != '-';
                if (!pressed)
                {
                    name.erase(0, 1);
                }
                const auto found = names.find(name);
                if (found != names.end())
                {
                    scriptedKeys.push_back({static_cast<std::uint64_t>(std::strtoull(item.c_str(), nullptr, 10)), found->second, pressed});
                }
            }
        }

        // the input
        std::array<int, 256> heldAs{};                   // the DOOM key sent for a virtual key that is down (0: none)
        bool cursorCaptured = false;
        std::uint8_t mouseButtons = 0;

        void Log(const std::string& text) const
        {
            if (svc.log)
            {
                svc.log(text);
            }
        }

        void StartGame()
        {
            if (state != State::Start)
            {
                return;
            }
            std::error_code fileError;
            const std::filesystem::path wadArgument = !chosenIwad.empty() ? chosenIwad : svc.iwad;
            if (!wadArgument.empty() && !std::filesystem::is_regular_file(wadArgument, fileError))
            {
                // the player named a file: another one is not taken instead
                message = "The game data named by --iwad is not found:\n" + wadArgument.string();
                state = State::Error;
                Log("DOOM: " + message);
                return;
            }
            const doom::IwadSearchResult found = doom::FindIwad(wadArgument, svc.exeDirectory, svc.wadDirectories);
            if (found.path.empty())
            {
                message = doom::NoIwadMessage(found);
                state = State::Error;
                Log("DOOM: " + message);
                return;
            }
            Log("DOOM: the game data is " + found.path.string() + " (from " + doom::IwadSourceName(found.source) + (found.isFreedoom ? ", Freedoom" : "") + ")");
            for (const std::filesystem::path& other : found.alternatives)
            {
                Log("DOOM: another WAD found: " + other.string());
            }

            doom::DoomConfig config;
            config.iwad = found.path;
            config.dataDirectory = svc.dataDirectory;
            config.extraArgs = svc.doomArgs;
            config.extraArgs.insert(config.extraArgs.end(), warpArguments.begin(), warpArguments.end());
            if (chosenMode >= 0 && chosenMode < static_cast<int>(svc.modes.size()))
            {
                const std::vector<std::string>& modeArgs = svc.modes[static_cast<std::size_t>(chosenMode)].doomArgs;
                config.extraArgs.insert(config.extraArgs.end(), modeArgs.begin(), modeArgs.end());
            }
            config.music = svc.music && settings.musicOn;
            if (svc.audio != nullptr)
            {
                audioHost = std::make_unique<DoomAudioHost>(*svc.audio, svc.log);
                config.audio = audioHost.get();
            }
            std::string error;
            game = doom::DoomGame::Create(config, error);
            if (!game)
            {
                message = "DOOM did not start:\n" + error;
                state = State::Error;
                Log("DOOM: " + error);
                return;
            }
            state = State::Game;
            LoadScriptedKeys();
            if (chosenMode >= 0 && chosenMode < static_cast<int>(svc.modes.size()) && svc.modes[static_cast<std::size_t>(chosenMode)].make)
            {
                std::string controllerError;
                const DoomMode& mode = svc.modes[static_cast<std::size_t>(chosenMode)];
                DoomLaunchChoice choice = launchChoice;
                choice.render = svc.render;
                choice.thumbnails = svc.thumbnails;
                controller = mode.make(choice, controllerError);
                if (!controller)
                {
                    message = mode.title + " did not start:\n" + controllerError;
                    state = State::Error;
                    Log("DOOM: " + message);
                    return;
                }
                Log("DOOM: " + mode.title);
            }
        }

        // ---- the laboratory ----

        // The six modes of FlyDoom as the menu shows them. The modes the application gave are told by their id.
        void BuildMenuModel()
        {
            menuModel = {};
            struct Slot
            {
                const char* id;
                const char* title;
                const char* caption;
                const char* description;
            };
            static const Slot slots[] = {
                {"human", "HUMAN", "Classic DOOM", "You play DOOM with the keyboard and the mouse."},
                {"fly", "FLY PLAYS", "A brain plays", "The picture goes through the eyes into a fly brain; its descending neurons steer. The game waits for the brain."},
                {"fly-learn", "FLY LEARNS", "A brain learns", "The fly plays and learns from the damage; it keeps what it learned between the runs (--fly-forget starts again)."},
                {"human-vs-fly", "HUMAN VS FLY", "Deathmatch", "You against a fly in one game."},
                {"fly-vs-fly", "FLY VS FLY", "Spectator", "Two flies fight, you watch their brains."},
                {"swarm", "SWARM", "Monsters are flies", "Every monster is a fly with a small brain."},
            };
            for (const Slot& slot : slots)
            {
                DoomMenuModel::Mode mode;
                mode.id = slot.id;
                mode.title = slot.title;
                mode.caption = slot.caption;
                mode.description = slot.description;
                mode.reason = "SOON";
                for (std::size_t i = 0; i < svc.modes.size(); ++i)
                {
                    const DoomMode& entry = svc.modes[i];
                    if (entry.id != slot.id)
                    {
                        continue;
                    }
                    mode.modeIndex = static_cast<int>(i);
                    mode.available = entry.available;
                    mode.reason = entry.unavailableReason.empty() ? "SOON" : entry.unavailableReason;
                    if (!entry.description.empty())
                    {
                        mode.description = entry.description;
                    }
                    break;
                }
                menuModel.modes.push_back(std::move(mode));
            }
            for (const DoomMenuModel::Mode& mode : menuModel.modes)
            {
                menuModel.flyAvailable = menuModel.flyAvailable || (mode.id == "fly" && mode.available);
            }
            // the game data: the best one first, then the others that were found
            const doom::IwadSearchResult found = doom::FindIwad(svc.iwad, svc.exeDirectory, svc.wadDirectories);
            if (found.path.empty())
            {
                menuModel.wadProblem = doom::NoIwadMessage(found);
            }
            else
            {
                DoomWadEntry best;
                best.path = found.path;
                best.label = WadLabel(found.path, found.isFreedoom);
                best.levels = LevelsOfWad(found.path);
                menuModel.wads.push_back(std::move(best));
                for (const std::filesystem::path& other : found.alternatives)
                {
                    DoomWadEntry entry;
                    entry.path = other;
                    entry.label = WadLabel(other, false);
                    entry.levels = LevelsOfWad(other);
                    menuModel.wads.push_back(std::move(entry));
                }
            }
            menuBuilt = true;
        }

        // The volume, the music and the scale of the UI of the settings; the window mode only when it changed
        void ApplySettings(const bool windowToo)
        {
            settings.Clamp();
            if (svc.audio != nullptr)
            {
                svc.audio->SetBusVolume(audio::Bus::Sfx, settings.sfxVolume);
                svc.audio->SetBusVolume(audio::Bus::Music, settings.musicVolume);
                svc.audio->SetBusMuted(audio::Bus::Music, !settings.musicOn);
            }
            host.SetUiScale(static_cast<float>(settings.uiScalePercent) / 100.0f);
            if (windowToo && svc.applyVideo)
            {
                RlVideoSettings video;
                video.windowMode = settings.windowMode;
                const std::pair<std::uint32_t, std::uint32_t> size = svc.windowSize ? svc.windowSize() : std::pair<std::uint32_t, std::uint32_t>{1600u, 900u};
                video.width = static_cast<int>(size.first);
                video.height = static_cast<int>(size.second);
                svc.applyVideo(video);
            }
        }

        void SaveSettings() const
        {
            if (!svc.settingsFile.empty())
            {
                settings.Save(svc.settingsFile);
            }
        }

        // The menu over the brain. The first frames also make the brain view (the connectome is read on its own thread).
        void DrawMenu(rlui::Frame& f, ImDrawList* list, const std::uint32_t width, const std::uint32_t height, const float deltaTime)
        {
            if (svc.ui != nullptr)
            {
                svc.ui->SetGameUiCapturesInput(true);
            }
            if (!menuBuilt)
            {
                BuildMenuModel();
            }
            menuTime += static_cast<double>(deltaTime);
            if (!brain && svc.makeBrainView && settings.brainBackdrop)
            {
                DoomBrainViewContext context;
                context.render = svc.render;
                context.thumbnails = svc.thumbnails;
                context.log = svc.log;
                brain = svc.makeBrainView(context);
            }
            if (brain && !settings.brainBackdrop)
            {
                brain.reset();
            }
            const float leaving = state == State::Launching ? fade : 0.0f;
            if (brain)
            {
                nestedMix += ((menu.Nested() ? 1.0f : 0.0f) - nestedMix) * std::min(1.0f, deltaTime * 7.0f);
                brain->SetNested(nestedMix);
                brain->Draw(list, 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), static_cast<double>(deltaTime), 1.0f - leaving);
                menuModel.brainStatus = brain->Status();
                menuModel.brainProblem = brain->Problem();
            }
            else
            {
                f.SetSpace(rlui::Space::Screen);
                rlui::paint::MenuBackdrop(f);
                f.SetSpace(rlui::Space::Menu);
                menuModel.brainStatus.clear();
                menuModel.brainProblem.clear();
            }

            if (state == State::Menu)
            {
                DoomStartRequest request;
                bool changed = false;
                const DoomSettings before = settings;
                const DoomMenu::Result result = menu.Draw(f, settings, menuModel, menuTime, request, changed);
                if (changed)
                {
                    ApplySettings(settings.windowMode != before.windowMode);
                    SaveSettings();
                }
                if (result == DoomMenu::Result::Quit)
                {
                    SaveSettings();
                    if (svc.quit)
                    {
                        svc.quit();
                    }
                }
                else if (result == DoomMenu::Result::Start)
                {
                    // the mode of the menu: the index in the list of the application
                    chosenMode = 0;
                    useChoice = false;
                    for (const DoomMenuModel::Mode& mode : menuModel.modes)
                    {
                        if (mode.id == request.modeId && mode.modeIndex >= 0)
                        {
                            chosenMode = mode.modeIndex;
                            useChoice = true;
                        }
                    }
                    launchChoice = request.choice;
                    warpArguments = request.doomArgs;
                    chosenIwad = request.iwad;
                    SaveSettings();
                    state = State::Launching;
                    fade = 0.0f;
                }
            }
            else
            {
                // the menu leaves: the black comes over it (and over the brain), then the game is made
                f.SetSpace(rlui::Space::Screen);
                rlui::paint::Dim(f, rlui::Color{0.0f, 0.0f, 0.0f, std::clamp(fade, 0.0f, 1.0f)});
                f.SetSpace(rlui::Space::Menu);
                fade += deltaTime / 0.45f;
                if (fade >= 1.0f)
                {
                    const float size = std::max(18.0f, std::floor(static_cast<float>(height) / 32.0f));
                    list->AddText(ImGui::GetFont(), size, ImVec2(static_cast<float>(width) * 0.5f - size * 3.0f, static_cast<float>(height) * 0.5f), IM_COL32(150, 170, 200, 255), "starting ...");
                    if (fade >= 1.15f)
                    {
                        brain.reset();   // the brain of the menu goes: the game has its own
                        state = State::Start;
                        fadeIn = 1.0f;
                        StartGame();
                    }
                }
            }
        }

        // The panel of a controller in the style of the menu: the title, the picture of the brain, the bars by group, the dim lines of the debugger at the foot. The panel and the
        // picture are on the background list of ImGui (pixels), the text and the bars on the list of the kit above it (the Screen space of the kit is in design units: 1080 high).
        void DrawControllerPanel(rlui::Frame& f, ImDrawList* list, const DoomPanelData& data, const float xPixels, const float yPixels, const float wPixels, const float hPixels)
        {
            using namespace rlui;
            const float px = f.metrics().scale;   // pixels per design unit
            const float x = xPixels / px;
            const float y = yPixels / px;
            const float w = wPixels / px;
            const float h = hPixels / px;
            const float pad = 16.0f;
            list->AddRectFilled(ImVec2(xPixels, yPixels), ImVec2(xPixels + wPixels, yPixels + hPixels), IM_COL32(9, 20, 46, 224));
            list->AddRectFilled(ImVec2(xPixels, yPixels), ImVec2(xPixels + 2.0f, yPixels + hPixels), IM_COL32(53, 214, 255, 255));

            // the picture of the brain
            const float cloudWidth = w - 2.0f * pad;
            const float cloudTop = y + 84.0f;
            const float cloudHeight = std::min(cloudWidth * 1.15f, h * 0.38f);
            list->AddRectFilled(ImVec2((x + pad) * px, cloudTop * px), ImVec2((x + pad + cloudWidth) * px, (cloudTop + cloudHeight) * px), IM_COL32(3, 6, 14, 255));
            controller->DrawBrain(list, (x + pad) * px, cloudTop * px, cloudWidth * px, cloudHeight * px);

            f.SetSpace(Space::Screen);
            float cursor = y + 10.0f;
            paint::Text(f, FontStyle::BlackItalic, 30.0f, {x + pad, cursor}, style::kText, data.title);
            paint::Text(f, FontStyle::Bold, 18.0f, {x + w - pad, cursor + 10.0f}, style::kAccent, data.chip, Align::Right);
            paint::Text(f, FontStyle::SemiBold, 18.0f, {x + pad, cursor + 42.0f}, style::kTextDim, data.subtitle);
            if (!data.brainChip.empty())
            {
                paint::Text(f, FontStyle::Bold, 15.0f, {x + pad + 8.0f, cloudTop + 6.0f}, style::kAccent.Faded(0.9f), data.brainChip, Align::Left, 0.06f);
            }
            cursor = cloudTop + cloudHeight + 14.0f;

            // the bars by group; the rows get what the foot leaves
            const float footHeight = static_cast<float>(data.debug.size()) * 19.0f + 8.0f;
            std::size_t rows = 0;
            for (const DoomPanelGroup& group : data.groups)
            {
                rows += group.bars.size();
            }
            const float titles = static_cast<float>(data.groups.size()) * 26.0f;
            const float room = y + h - footHeight - cursor - titles;
            const float pitch = std::clamp(room / static_cast<float>(std::max<std::size_t>(rows, 1)), 25.0f, 36.0f);
            for (const DoomPanelGroup& group : data.groups)
            {
                paint::Text(f, FontStyle::ExtraBoldItalic, 16.0f, {x + pad, cursor}, style::kTextFaint, group.title, Align::Left, 0.1f);
                cursor += 26.0f;
                for (const DoomPanelBar& bar : group.bars)
                {
                    if (cursor + pitch > y + h - footHeight)
                    {
                        break;
                    }
                    const Color color = Color::Hex(bar.rgb);
                    paint::Text(f, FontStyle::SemiBold, 20.0f, {x + pad, cursor - 2.0f}, style::kText, bar.label);
                    paint::Text(f, FontStyle::Bold, 20.0f, {x + w - pad, cursor - 2.0f}, style::kText, bar.text, Align::Right);
                    const float barY = cursor + 24.0f;
                    const float barHeight = 5.0f;
                    const float barWidth = w - 2.0f * pad;
                    paint::FillRect(f, {x + pad, barY, barWidth, barHeight}, Color::Hex(0x1c3466));
                    const float fraction = std::clamp(bar.value / std::max(bar.limit, 1e-6f), bar.centered ? -1.0f : 0.0f, 1.0f);
                    if (bar.centered)
                    {
                        const float middle = x + pad + barWidth * 0.5f;
                        const float reach = barWidth * 0.5f * fraction;
                        paint::FillRect(f, {std::min(middle, middle + reach), barY, std::fabs(reach), barHeight}, color);
                        paint::FillRect(f, {middle - 0.5f, barY - 3.0f, 1.0f, barHeight + 6.0f}, style::kText);
                    }
                    else
                    {
                        paint::FillRect(f, {x + pad, barY, barWidth * fraction, barHeight}, color);
                    }
                    cursor += pitch;
                }
            }

            // the foot: dim lines for the one who debugs the brain
            float footY = y + h - footHeight + 2.0f;
            for (const std::string& line : data.debug)
            {
                paint::Text(f, FontStyle::SemiBold, 15.0f, {x + pad, footY}, style::kTextFaint, line);
                footY += 19.0f;
            }
            f.SetSpace(Space::Menu);
        }

        // The big brain: the legend on a plate at the left over the picture of the brain (the panel is hidden), the hint at the foot
        void DrawBigBrainLegend(rlui::Frame& f, const DoomPanelData& data, const float widthPixels, const float heightPixels)
        {
            using namespace rlui;
            const float px = f.metrics().scale;
            const float width = widthPixels / px;
            const float height = heightPixels / px;
            const float pad = 20.0f;
            // the rows of the legend: the bars of the groups of neurons, and for each player of a duel (a group that is none of the three of a single brain) all of its bars
            // under its name
            std::vector<const DoomPanelBar*> bars;
            std::vector<const std::string*> headings;   // parallel to `bars`: the name of the player before its first bar, else null
            for (const DoomPanelGroup& group : data.groups)
            {
                const bool single = group.title == "MOTOR" || group.title == "VISION" || group.title == "OUTPUT";
                bool first = true;
                for (const DoomPanelBar& bar : group.bars)
                {
                    if (!single || group.title != "OUTPUT" || bar.label.rfind("dopamine", 0) == 0)
                    {
                        bars.push_back(&bar);
                        headings.push_back(!single && first ? &group.title : nullptr);
                        first = false;
                    }
                }
            }
            float headingRows = 0.0f;
            for (const std::string* heading : headings)
            {
                headingRows += heading != nullptr ? 1.0f : 0.0f;
            }
            const float rowPitch = 34.0f;
            const Rect plate{24.0f, 24.0f, std::min(width * 0.34f, 560.0f), 132.0f + rowPitch * (static_cast<float>(bars.size()) + headingRows)};
            f.SetSpace(Space::Screen);
            paint::Parallelogram(f, plate, style::kNavy);
            paint::ParallelogramOutline(f, plate, style::kLine, 1.0f);
            float cursor = plate.y + 14.0f;
            const float left = plate.x + pad + paint::Bevel(plate.h) * 0.4f;
            paint::Text(f, FontStyle::BlackItalic, 34.0f, {left, cursor}, style::kText, "FLY BRAIN");
            cursor += 50.0f;
            paint::Text(f, FontStyle::SemiBold, 17.0f, {left, cursor}, style::kTextDim, data.brainLine);
            cursor += 46.0f;
            const float inner = plate.w - 2.0f * pad - paint::Bevel(plate.h) * 0.8f;
            for (std::size_t row = 0; row < bars.size(); ++row)
            {
                const DoomPanelBar* bar = bars[row];
                if (headings[row] != nullptr)
                {
                    paint::Text(f, FontStyle::ExtraBoldItalic, 16.0f, {left, cursor + 4.0f}, style::kTextFaint, *headings[row], Align::Left, 0.1f);
                    cursor += rowPitch;
                }
                const Color color = Color::Hex(bar->rgb);
                paint::FillRect(f, {left, cursor + 6.0f, 14.0f, 14.0f}, color);
                paint::Text(f, FontStyle::SemiBold, 20.0f, {left + 24.0f, cursor}, style::kText, bar->label);
                const float barX = left + inner * 0.50f;
                const float barW = inner * 0.30f;
                paint::FillRect(f, {barX, cursor + 11.0f, barW, 5.0f}, Color::Hex(0x1c3466));
                const float fraction = std::clamp(std::fabs(bar->value) / std::max(bar->limit, 1e-6f), 0.0f, 1.0f);
                paint::FillRect(f, {barX, cursor + 11.0f, barW * fraction, 5.0f}, color);
                paint::Text(f, FontStyle::Bold, 20.0f, {left + inner, cursor}, style::kText, bar->text, Align::Right);
                cursor += rowPitch;
            }
            paint::Text(f, FontStyle::SemiBold, 20.0f, {width * 0.5f, height - 46.0f}, style::kTextDim, "Tab: back to the game", Align::Center);
            f.SetSpace(Space::Menu);
        }

        // The keyboard into DOOM: a key that goes down (or up) since the last frame is one event
        void PollKeyboard(const bool active, const bool doomMenu, const bool controlled = false)
        {
            BYTE keys[256] = {};
            const bool haveKeys = active && GetKeyboardState(keys) != FALSE;
            auto down = [&](const int vk) { return haveKeys && (keys[vk] & 0x80) != 0; };

            auto send = [&](const int vk, const int doomKey)
            {
                const bool isDown = down(vk);
                if (isDown && heldAs[static_cast<std::size_t>(vk)] == 0)
                {
                    heldAs[static_cast<std::size_t>(vk)] = doomKey;
                    game->PostKey(doomKey, true);
                }
                else if (!isDown && heldAs[static_cast<std::size_t>(vk)] != 0)
                {
                    game->PostKey(heldAs[static_cast<std::size_t>(vk)], false);
                    heldAs[static_cast<std::size_t>(vk)] = 0;
                }
            };

            for (const KeyMap& entry : kCommonKeys)
            {
                if (entry.vk == VK_ESCAPE && escSwallow)
                {
                    continue;
                }
                send(entry.vk, entry.doomKey);
            }
            if (controlled && !doomMenu)
            {
                // the controller plays: the keyboard only reaches the menu of DOOM (and the keys above), not the game
            }
            else if (doomMenu)
            {
                for (const KeyMap& entry : kMenuKeys)
                {
                    send(entry.vk, entry.doomKey);
                }
                for (int vk = 'A'; vk <= 'Z'; ++vk)
                {
                    send(vk, vk - 'A' + 'a');
                }
                for (int vk = '0'; vk <= '9'; ++vk)
                {
                    send(vk, vk);
                }
            }
            else
            {
                for (const KeyMap& entry : kGameKeys)
                {
                    send(entry.vk, entry.doomKey);
                }
                for (int vk = '1'; vk <= '9'; ++vk)
                {
                    send(vk, vk);
                }
                // the letters the game itself answers to: the cheats of DOOM are typed letters, the map (Tab), Y and N of the questions
                for (int vk = 'A'; vk <= 'Z'; ++vk)
                {
                    if (vk != 'W' && vk != 'S' && vk != 'A' && vk != 'D' && vk != 'E')
                    {
                        send(vk, vk - 'A' + 'a');
                    }
                }
            }
            // a key that is held while the mode changes (W in the menu, 'w' in the game) is released with the key it was sent as
            for (int vk = 0; vk < 256; ++vk)
            {
                if (heldAs[static_cast<std::size_t>(vk)] != 0 && !down(vk))
                {
                    game->PostKey(heldAs[static_cast<std::size_t>(vk)], false);
                    heldAs[static_cast<std::size_t>(vk)] = 0;
                }
            }
        }

        void CaptureCursor(const bool capture)
        {
            if (capture != cursorCaptured)
            {
                cursorCaptured = capture;
                if (svc.captureCursor)
                {
                    svc.captureCursor(capture);
                }
            }
        }

        // The 320x200 frame as the pixels of the texture: an integer scale (so that the pixels stay square blocks, no blur), stretched to 4:3 when asked
        void UploadFrame(const std::uint32_t scale, const bool stretch)
        {
            if (UploadPixels(game->Pixels(), scale, stretch, texture, textureWidth, textureHeight))
            {
                uploadedFrame = game->FrameCount();
                uploadedScale = scale;
            }
        }

        bool UploadPixels(const std::uint32_t* source, const std::uint32_t scale, const bool stretch, render::TextureHandle& target, std::uint32_t& targetWidth, std::uint32_t& targetHeight)
        {
            const std::uint32_t outW = doom::kScreenWidth * scale;
            const std::uint32_t outH = (stretch ? doom::kScreenHeight * 6u / 5u : doom::kScreenHeight) * scale;
            if (source == nullptr)
            {
                return false;
            }
            rgba.resize(static_cast<std::size_t>(outW) * outH * 4u);
            std::vector<std::uint32_t> rowMap(outH);
            for (std::uint32_t y = 0; y < outH; ++y)
            {
                rowMap[y] = std::min<std::uint32_t>(doom::kScreenHeight - 1u, y * doom::kScreenHeight / outH);
            }
            std::uint8_t* out = rgba.data();
            for (std::uint32_t y = 0; y < outH; ++y)
            {
                const std::uint32_t* line = source + static_cast<std::size_t>(rowMap[y]) * doom::kScreenWidth;
                if (y > 0 && rowMap[y] == rowMap[y - 1])
                {
                    std::memcpy(out, out - static_cast<std::size_t>(outW) * 4u, static_cast<std::size_t>(outW) * 4u);
                    out += static_cast<std::size_t>(outW) * 4u;
                    continue;
                }
                for (int x = 0; x < doom::kScreenWidth; ++x)
                {
                    const std::uint32_t p = line[x];
                    const std::uint8_t r = static_cast<std::uint8_t>((p >> 16) & 0xFF);
                    const std::uint8_t g = static_cast<std::uint8_t>((p >> 8) & 0xFF);
                    const std::uint8_t b = static_cast<std::uint8_t>(p & 0xFF);
                    for (std::uint32_t k = 0; k < scale; ++k)
                    {
                        *out++ = r;
                        *out++ = g;
                        *out++ = b;
                        *out++ = 255;
                    }
                }
            }

            render::TextureData data;
            data.width = outW;
            data.height = outH;
            data.channels = 4;
            data.srgb = false;
            data.pixelsRgba8 = rgba;
            if (target.IsValid() && targetWidth == outW && targetHeight == outH)
            {
                svc.render->UpdateTexture(target, data);
            }
            else
            {
                if (target.IsValid())
                {
                    svc.render->DestroyTexture(target);
                }
                target = svc.render->CreateTexture(data);
                targetWidth = outW;
                targetHeight = outH;
            }
            return true;
        }

        void DrawMessage(ImDrawList* list, const std::uint32_t width, const std::uint32_t height)
        {
            const float size = std::max(14.0f, std::floor(static_cast<float>(height) / 36.0f));
            ImFont* font = ImGui::GetFont();
            float y = static_cast<float>(height) * 0.12f;
            const float x = static_cast<float>(width) * 0.08f;
            list->AddText(font, size * 1.6f, ImVec2(x, y), IM_COL32(230, 70, 50, 255), "FlyDoom");
            y += size * 2.6f;
            std::size_t begin = 0;
            while (begin <= message.size())
            {
                std::size_t end = message.find('\n', begin);
                if (end == std::string::npos)
                {
                    end = message.size();
                }
                const std::string line = message.substr(begin, end - begin);
                list->AddText(font, size, ImVec2(x, y), IM_COL32(225, 225, 225, 255), line.c_str());
                y += size * 1.3f;
                begin = end + 1;
            }
            y += size;
            list->AddText(font, size, ImVec2(x, y), IM_COL32(150, 150, 150, 255), "Esc - exit");
        }

        // The banner about the crash of the previous run: over everything, 12 seconds, the last one fades
        void DrawCrashNote(const std::uint32_t width, const std::uint32_t height, const float deltaTime)
        {
            if (crashNoteLeft <= 0.0f)
            {
                return;
            }
            crashNoteLeft -= deltaTime;
            const float size = std::max(14.0f, std::floor(static_cast<float>(height) / 36.0f));
            ImFont* font = ImGui::GetFont();
            ImDrawList* list = ImGui::GetForegroundDrawList();
            const float alpha = std::min(1.0f, std::max(0.0f, crashNoteLeft));
            const float margin = size * 0.8f;
            const float maxWidth = static_cast<float>(width) - 2.0f * margin;
            const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, maxWidth, crashNote.c_str());
            const ImVec2 corner(margin, margin);
            const ImVec2 end(margin + text.x + 2.0f * size, margin + text.y + size);
            list->AddRectFilled(corner, end, IM_COL32(120, 20, 20, static_cast<int>(235.0f * alpha)), size * 0.3f);
            list->AddText(font, size, ImVec2(corner.x + size, corner.y + size * 0.5f), IM_COL32(255, 235, 235, static_cast<int>(255.0f * alpha)), crashNote.c_str(), nullptr, maxWidth);
        }

        bool menuAsked = false;

        void Build(rlui::Frame& frame, const core::WindowId windowId, const std::uint32_t width, const std::uint32_t height, const float deltaTime)
        {
            ImDrawList* list = ImGui::GetBackgroundDrawList();
            list->AddRectFilled(ImVec2(0, 0), ImVec2(static_cast<float>(width), static_cast<float>(height)), IM_COL32(0, 0, 0, 255));
            if (mainWindow == 0)
            {
                mainWindow = windowId;
            }
            if (windowId != mainWindow || width == 0 || height == 0)
            {
                return;
            }
            const bool active = ProcessIsForeground();

            if (!crashChecked)
            {
                // the previous run crashed: the game says so once, with the file the player can send (core/CrashHandler.h)
                crashChecked = true;
                const std::filesystem::path crashDirectory = core::apppaths::UserDataDirectory() / L"crashes";
                if (const auto report = core::crash::TakePending(crashDirectory))
                {
                    Log("the previous run crashed: " + report->summary + "; the report is " + report->text.generic_u8string());
                    crashNote = core::crash::PlayerNote(*report, crashDirectory);
                    crashNoteLeft = 12.0f;
                }
            }
            DrawCrashNote(width, height, deltaTime);

            if (state == State::Start && !menuAsked && svc.showMenu && svc.initialMode < 0)
            {
                menuAsked = true;
                state = State::Menu;
                {
                    char* screen = nullptr;
                    std::size_t screenLength = 0;
                    if (_dupenv_s(&screen, &screenLength, "MYENGINE_DOOM_MENU_SCREEN") == 0 && screen != nullptr)
                    {
                        menu.ShowScreen(screen);
                        std::free(screen);
                    }
                }
            }
            else if (state == State::Start && svc.initialMode >= 0)
            {
                chosenMode = std::min(svc.initialMode, static_cast<int>(svc.modes.size()) - 1);
                launchChoice = svc.choice;
            }
            if (state == State::Menu || state == State::Launching)
            {
                DrawMenu(frame, list, width, height, deltaTime);
                return;
            }
            if (svc.ui != nullptr && state != State::Error)
            {
                svc.ui->SetGameUiCapturesInput(labPause);   // the keys and the mouse are the game's, except in the pause
            }
            if (state == State::Start)
            {
                Log("DOOM: the window is " + std::to_string(width) + "x" + std::to_string(height) + ", ImGui " + std::to_string(ImGui::GetIO().DisplaySize.x) + "x" +
                    std::to_string(ImGui::GetIO().DisplaySize.y) + " scale " + std::to_string(ImGui::GetIO().DisplayFramebufferScale.x));
                StartGame();
            }

            if (state == State::Game && labPause)
            {
                // the pause: the game stands (nor the keys, the mouse, nor the controller move), the picture and the panel are drawn below
                CaptureCursor(false);
                if (svc.takeMouseDelta)
                {
                    svc.takeMouseDelta();
                }
                const bool escDown = scriptedRaw ? rawEsc : (active && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0);
                if (!escDown)
                {
                    escSwallow = false;
                }
                escWas = escDown;
            }
            else if (state == State::Game)
            {
                const doom::GameInfo before = game->Info();
                const bool doomMenu = before.menuActive || before.state != doom::GameState::Level;
                // Esc opens the pause (not when the menu of DOOM is open: there it closes the menu, as in DOOM)
                const bool escDown = scriptedRaw ? rawEsc : (active && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0);
                bool openPause = escDown && !escWas && !before.menuActive;
                escWas = escDown;
                if (!escDown)
                {
                    escSwallow = false;
                }
                for (const ScriptedKey& key : scriptedKeys)
                {
                    if (key.frame == flowFrame && key.key == doom::key::kEscape && key.pressed && !before.menuActive)
                    {
                        openPause = true;   // the scripted Esc of the tests
                    }
                }
                if (openPause)
                {
                    labPause = true;
                    escSwallow = escDown;
                    for (std::size_t vk = 0; vk < heldAs.size(); ++vk)
                    {
                        if (heldAs[vk] != 0)
                        {
                            game->PostKey(heldAs[vk], false);
                            heldAs[vk] = 0;
                        }
                    }
                    if (mouseButtons != 0)
                    {
                        mouseButtons = 0;
                        game->PostMouse(0, 0, 0);
                    }
                    CaptureCursor(false);
                }
                else
                {
                const bool humanPlays = controller == nullptr || controller->HumanPlays();
                PollKeyboard(active, doomMenu, !humanPlays);
                if (controller != nullptr && !humanPlays)
                {
                    // Tab is free when a controller plays (the map of DOOM is the human's): it shows the big brain
                    const bool tab = scriptedRaw ? rawTab : (active && (GetAsyncKeyState(VK_TAB) & 0x8000) != 0);
                    if (tab && !tabHeld)
                    {
                        controller->ToggleBigBrain();
                    }
                    tabHeld = tab;
                }
                for (const ScriptedKey& key : scriptedKeys)
                {
                    if (key.frame == flowFrame)
                    {
                        if (key.key == doom::key::kTab && controller != nullptr)
                        {
                            // the scripted Tab of a controller: the big brain, as the real key does
                            if (key.pressed)
                            {
                                controller->ToggleBigBrain();
                            }
                        }
                        else
                        {
                            game->PostKey(key.key, key.pressed);
                        }
                    }
                }
                ++flowFrame;

                // the mouse: the left button fires, the movement turns; the cursor is the game's only while the level plays
                const bool playing = humanPlays && active && !before.menuActive && !before.paused && before.state == doom::GameState::Level;
                CaptureCursor(playing);
                int buttons = 0;
                int dx = 0;
                // the script of the mouse works without the focus too (the hidden desktop has no foreground window of the process)
                if (playing || (!scriptedMouse.empty() && humanPlays && !before.menuActive && !before.paused && before.state == doom::GameState::Level))
                {
                    buttons = (GetAsyncKeyState(VK_LBUTTON) & 0x8000 ? 1 : 0) | (GetAsyncKeyState(VK_RBUTTON) & 0x8000 ? 2 : 0) |
                              (GetAsyncKeyState(VK_MBUTTON) & 0x8000 ? 4 : 0);
                    // every bit of the movement goes to DOOM: the frames come faster than the tics, and DOOM adds up what came before the next tic
                    int raw = svc.takeMouseDelta ? svc.takeMouseDelta().first : 0;
                    for (const auto& move : scriptedMouse)
                    {
                        if (move.first + 1 == flowFrame)   // flowFrame has moved on above: the frame of the script is the one that just ran
                        {
                            raw += move.second;
                            scriptedMouseLog = true;
                            scriptedMouseLast = flowFrame;
                        }
                    }
                    const float scaled = static_cast<float>(raw) * settings.mouseSensitivity + mouseRemainder;
                    dx = static_cast<int>(std::trunc(scaled));
                    mouseRemainder = scaled - static_cast<float>(dx);
                }
                else if (svc.takeMouseDelta)
                {
                    svc.takeMouseDelta();   // the movement of the cursor in the menu is not a turn
                }
                if (buttons != mouseButtons || dx != 0)
                {
                    mouseButtons = static_cast<std::uint8_t>(buttons);
                    game->PostMouse(buttons, dx, 0);
                }

                if (controller != nullptr)
                {
                    controller->Advance(*game, std::min(static_cast<double>(deltaTime), kMaxFrameSeconds));
                }
                else
                {
                    game->Advance(std::min(static_cast<double>(deltaTime), kMaxFrameSeconds));
                }
                if (scriptedMouseLog && flowFrame < scriptedMouseLast + 120)
                {
                    const double angle = game->Player(game->Info().consolePlayer).angleDegrees;
                    if (angle != scriptedMouseAngle)
                    {
                        scriptedMouseAngle = angle;
                        Log("DOOM mouse script: frame " + std::to_string(flowFrame) + ", angle " + std::to_string(angle) + " degrees");
                    }
                }
                if (game->WindowTitle() != title)
                {
                    title = game->WindowTitle();
                    if (svc.setTitle)
                    {
                        svc.setTitle(title);
                    }
                }
                if (game->IsFinished())
                {
                    CaptureCursor(false);
                    if (!game->ErrorMessage().empty())
                    {
                        message = "DOOM stopped:\n" + game->ErrorMessage();
                        state = State::Error;
                        Log("DOOM: " + game->ErrorMessage());
                    }
                    else
                    {
                        state = State::Finished;
                        if (svc.quit)
                        {
                            svc.quit();
                        }
                    }
                }
                }
            }

            if (state == State::Game)
            {
                // the largest whole scale that fits; the picture is drawn at the size the window gives, so a window smaller than one scale shrinks it
                const float viewHeightUnits = svc.squarePixels ? static_cast<float>(doom::kScreenHeight) : static_cast<float>(doom::kScreenHeight * 6 / 5);   // 240: the 4:3 of the monitors of 1993, in whole numbers
                // a controller has a panel at the right of the picture (the brain of the fly); the human has the whole window
                const float heightFitPicture = std::floor(static_cast<float>(doom::kScreenWidth) * static_cast<float>(height) / viewHeightUnits);
                const bool panelled = controller != nullptr && controller->HasPanel();   // the swarm leaves the whole window to the game
                const bool bigBrain = panelled && controller->BigBrainOpen();             // the whole window is the brain's: DOOM is centred under it, no strip of the panel
                const float panelWidth = (panelled && !bigBrain) ? std::max(240.0f, static_cast<float>(width) - heightFitPicture) : 0.0f;
                const float pictureWidth = static_cast<float>(width) - panelWidth;
                // a second picture (the eyes of the fly) shares the place of the picture: two views side by side
                const SecondaryView* second = controller != nullptr ? controller->Secondary() : nullptr;
                const float viewsAcross = second != nullptr ? 2.0f : 1.0f;
                const float fit = std::min(pictureWidth / viewsAcross / doom::kScreenWidth, static_cast<float>(height) / viewHeightUnits);
                const std::uint32_t scale = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(panelled ? std::ceil(fit) : std::floor(fit)), 1u, 4u);
                if (uploadedScale != scale)
                {
                    Log("DOOM: the picture is drawn at x" + std::to_string(scale) + " in a window of " + std::to_string(width) + "x" + std::to_string(height));
                }
                if (!texture.IsValid() || uploadedFrame != game->FrameCount() || uploadedScale != scale)
                {
                    UploadFrame(scale, !svc.squarePixels);
                }
                if (second != nullptr && second->width == doom::kScreenWidth && second->height == doom::kScreenHeight &&
                    (!secondTexture.IsValid() || secondSerial != second->serial || secondScale != scale))
                {
                    if (UploadPixels(second->pixels.data(), scale, !svc.squarePixels, secondTexture, secondWidth, secondHeight))
                    {
                        secondSerial = second->serial;
                        secondScale = scale;
                    }
                }
                if (texture.IsValid())
                {
                    // an integer factor on the screen when it fits, so that the sharp pixels of the texture stay sharp
                    const float whole = (fit >= 1.0f && !panelled) ? std::floor(fit) : fit;   // with a panel the picture fills the rest of the window
                    const float finalW = doom::kScreenWidth * whole;
                    const float finalH = viewHeightUnits * whole;
                    const float cell = pictureWidth / viewsAcross;
                    const float left = std::floor((cell - finalW) * 0.5f);
                    const float top = std::floor((static_cast<float>(height) - finalH) * 0.5f);
                    list->AddImage(static_cast<ImTextureID>(texture.value), ImVec2(left, top), ImVec2(left + finalW, top + finalH));
                    const float captionSize = std::clamp(std::floor(finalH / 14.0f), 11.0f, 26.0f);
                    auto caption = [&](const std::string& text, const float x0, const ImU32 color)
                    {
                        if (text.empty())
                        {
                            return;
                        }
                        ImFont* font = ImGui::GetFont();
                        const ImVec2 box = font->CalcTextSizeA(captionSize, 1.0e9f, 0.0f, text.c_str());
                        list->AddRectFilled(ImVec2(x0 + 6.0f, top + 6.0f), ImVec2(x0 + 6.0f + box.x + 12.0f, top + 6.0f + box.y + 6.0f), IM_COL32(0, 0, 0, 170));
                        list->AddText(font, captionSize, ImVec2(x0 + 12.0f, top + 9.0f), color, text.c_str());
                    };
                    if (second != nullptr)
                    {
                        caption(second->mainLabel, left, IM_COL32(120, 220, 140, 255));
                        if (secondTexture.IsValid())
                        {
                            const float left2 = cell + left;
                            list->AddImage(static_cast<ImTextureID>(secondTexture.value), ImVec2(left2, top), ImVec2(left2 + finalW, top + finalH));
                            caption(second->label, left2, IM_COL32(240, 170, 80, 255));
                        }
                    }
                }
                if (controller != nullptr)
                {
                    DoomPanelData data;
                    const bool described = panelled && controller->DescribePanel(data);
                    const bool big = panelled && controller->BigBrainOpen();
                    if (panelled && !described)
                    {
                        controller->DrawPanel(list, pictureWidth, 0.0f, panelWidth, static_cast<float>(height));
                    }
                    else if (described && !big)
                    {
                        DrawControllerPanel(frame, list, data, pictureWidth, 0.0f, panelWidth, static_cast<float>(height));
                    }
                    controller->DrawOverlay(list, static_cast<float>(width), static_cast<float>(height));
                    if (described && big)
                    {
                        DrawBigBrainLegend(frame, data, static_cast<float>(width), static_cast<float>(height));
                    }
                }
                if (labPause)
                {
                    const DoomMenu::PauseResult choice = menu.DrawPause(frame, static_cast<bool>(svc.backToMenu), !escSwallow);
                    if (choice == DoomMenu::PauseResult::Resume)
                    {
                        labPause = false;
                        escSwallow = escWas;
                    }
                    else if (choice == DoomMenu::PauseResult::DoomMenu)
                    {
                        labPause = false;
                        escSwallow = escWas;
                        game->PostKey(doom::key::kEscape, true);
                        game->PostKey(doom::key::kEscape, false);
                    }
                    else if (choice == DoomMenu::PauseResult::Laboratory)
                    {
                        if (svc.backToMenu)
                        {
                            svc.backToMenu();
                        }
                    }
                    else if (choice == DoomMenu::PauseResult::Quit)
                    {
                        if (svc.quit)
                        {
                            svc.quit();
                        }
                    }
                }
                // the black of the start goes away over half a second
                if (fadeIn > 0.0f)
                {
                    list->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(static_cast<float>(width), static_cast<float>(height)), IM_COL32(0, 0, 0, static_cast<int>(std::clamp(fadeIn, 0.0f, 1.0f) * 255.0f)));
                    fadeIn = std::max(0.0f, fadeIn - deltaTime / 0.5f);
                }
            }
            else if (state == State::Error)
            {
                DrawMessage(list, width, height);
                if (active && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 && svc.quit)
                {
                    svc.quit();
                }
            }
        }
    };

    DoomGameFlow::DoomGameFlow(const DoomGameServices& services) : impl_(std::make_unique<Impl>())
    {
        impl_->svc = services;
    }

    DoomGameFlow::~DoomGameFlow()
    {
        if (impl_->cursorCaptured && impl_->svc.captureCursor)
        {
            impl_->svc.captureCursor(false);
        }
        if (impl_->texture.IsValid() && impl_->svc.render != nullptr)
        {
            impl_->svc.render->DestroyTexture(impl_->texture);
        }
        if (impl_->secondTexture.IsValid() && impl_->svc.render != nullptr)
        {
            impl_->svc.render->DestroyTexture(impl_->secondTexture);
        }
        // the controller first (it uses the game), then the game (it still talks to the audio host), then the host
        impl_->controller.reset();
        impl_->game.reset();
        impl_->audioHost.reset();
        impl_->brain.reset();
        impl_->SaveSettings();
        impl_->host.Uninstall();
    }

    void DoomGameFlow::SetRawInputHook(std::function<void(rlui::RawInput&)> hook)
    {
        Impl* impl = impl_.get();
        impl->scriptHook = std::move(hook);
        impl->scriptedRaw = static_cast<bool>(impl->scriptHook);
        impl->host.SetRawInputHook([impl](rlui::RawInput& input)
                                   {
                                       if (impl->scriptHook)
                                       {
                                           impl->scriptHook(input);
                                           impl->rawTab = input.keys[VK_TAB];
                                           impl->rawEsc = input.keys[VK_ESCAPE];
                                       }
                                   });
    }

    void DoomGameFlow::Install()
    {
        Impl* impl = impl_.get();
        // the settings of the player, then the host of the UI kit (Exo 2 and the plates of Rocket Arena): the menu is drawn with it, the game with the draw list of ImGui
        if (!impl->svc.settingsFile.empty())
        {
            impl->settings.Load(impl->svc.settingsFile);
        }
        rlui::HostConfig config;
        config.uiScale = static_cast<float>(impl->settings.uiScalePercent) / 100.0f;
        impl->host.Install(*impl_->svc.ui, config);
        impl->host.SetScreen([impl](rlui::Frame& frame)
                             {
                                 const rlui::Vec2 window = frame.metrics().window;
                                 impl->Build(frame, 1, static_cast<std::uint32_t>(window.x), static_cast<std::uint32_t>(window.y), frame.DeltaTime());
                             });
        impl->ApplySettings(impl->settings.windowMode != 0);
    }

    const char* DoomGameFlow::StateName() const
    {
        switch (impl_->state)
        {
            case Impl::State::Start: return "start";
            case Impl::State::Menu: return "menu";
            case Impl::State::Launching: return "launching";
            case Impl::State::Game: return "game";
            case Impl::State::Error: return "error";
            case Impl::State::Finished: return "finished";
        }
        return "start";
    }

    std::string DoomGameFlow::ControllerStatus() const
    {
        return impl_->controller ? impl_->controller->Status() : std::string();
    }

    std::uint64_t DoomGameFlow::FrameCount() const
    {
        return impl_->game ? impl_->game->FrameCount() : 0;
    }
}
