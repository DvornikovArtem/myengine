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

#include <myengine/audio/AudioSystem.h>
#include <myengine/core/AppPaths.h>
#include <myengine/core/CrashHandler.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/ui/UiManager.h>

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
            Choose,
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
        bool modeAsked = false;
        std::array<bool, 10> chooseHeld{};               // the digit keys of the menu of the modes, for the edge
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

        void LoadScriptedKeys()
        {
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
                {"right", doom::key::kRight}, {"fire", doom::key::kFire},   {"use", doom::key::kUse},   {"y", 'y'},               {"n", 'n'}};
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
            if (!svc.iwad.empty() && !std::filesystem::is_regular_file(svc.iwad, fileError))
            {
                // the player named a file: another one is not taken instead
                message = "The game data named by --iwad is not found:\n" + svc.iwad.string();
                state = State::Error;
                Log("DOOM: " + message);
                return;
            }
            const doom::IwadSearchResult found = doom::FindIwad(svc.iwad, svc.exeDirectory, svc.wadDirectories);
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
            if (chosenMode >= 0 && chosenMode < static_cast<int>(svc.modes.size()))
            {
                const auto& modeArgs = svc.modes[static_cast<std::size_t>(chosenMode)].doomArgs;
                config.extraArgs.insert(config.extraArgs.end(), modeArgs.begin(), modeArgs.end());
            }
            config.music = svc.music;
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
                controller = svc.modes[static_cast<std::size_t>(chosenMode)].make(svc.choice, controllerError);
                if (!controller)
                {
                    message = svc.modes[static_cast<std::size_t>(chosenMode)].title + " did not start:\n" + controllerError;
                    state = State::Error;
                    Log("DOOM: " + message);
                    return;
                }
                Log("DOOM: " + svc.modes[static_cast<std::size_t>(chosenMode)].title);
            }
        }

        // The menu in front of the game: which player plays. Digit keys, Esc leaves.
        void DrawChoose(ImDrawList* list, const std::uint32_t width, const std::uint32_t height, const bool active)
        {
            const float size = std::max(16.0f, std::floor(static_cast<float>(height) / 30.0f));
            ImFont* font = ImGui::GetFont();
            float y = static_cast<float>(height) * 0.2f;
            const float x = static_cast<float>(width) * 0.1f;
            list->AddText(font, size * 2.2f, ImVec2(x, y), IM_COL32(230, 70, 50, 255), "FlyDoom");
            y += size * 3.4f;
            for (std::size_t i = 0; i < svc.modes.size() && i < 9; ++i)
            {
                const DoomMode& mode = svc.modes[i];
                const std::string line = std::to_string(i + 1) + "  " + mode.title + (mode.available ? "" : "  (" + mode.unavailableReason + ")");
                list->AddText(font, size * 1.3f, ImVec2(x, y), mode.available ? IM_COL32(235, 235, 235, 255) : IM_COL32(110, 110, 120, 255), line.c_str());
                y += size * 2.0f;
            }
            y += size;
            list->AddText(font, size, ImVec2(x, y), IM_COL32(150, 150, 150, 255), "press the number, Esc - exit");
            if (!active)
            {
                return;
            }
            for (int digit = 1; digit <= 9 && digit <= static_cast<int>(svc.modes.size()); ++digit)
            {
                const bool down = (GetAsyncKeyState('0' + digit) & 0x8000) != 0 || (GetAsyncKeyState(VK_NUMPAD0 + digit) & 0x8000) != 0;
                if (down && !chooseHeld[static_cast<std::size_t>(digit)] && svc.modes[static_cast<std::size_t>(digit - 1)].available)
                {
                    chosenMode = digit - 1;
                    state = State::Start;
                    StartGame();
                }
                chooseHeld[static_cast<std::size_t>(digit)] = down;
            }
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 && svc.quit)
            {
                svc.quit();
            }
        }

        // The keyboard into DOOM: a key that goes down (or up) since the last frame is one event
        void PollKeyboard(const bool active, const bool menu, const bool controlled = false)
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
                send(entry.vk, entry.doomKey);
            }
            if (controlled && !menu)
            {
                // the controller plays: the keyboard only reaches the menu of DOOM (and the keys above), not the game
            }
            else if (menu)
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

        void Build(const core::WindowId windowId, const std::uint32_t width, const std::uint32_t height, const float deltaTime)
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

            if (state == State::Start && std::count_if(svc.modes.begin(), svc.modes.end(), [](const DoomMode& m) { return m.available; }) > 1 && svc.initialMode < 0 && !modeAsked)
            {
                modeAsked = true;
                state = State::Choose;
            }
            else if (state == State::Start && svc.initialMode >= 0)
            {
                chosenMode = std::min(svc.initialMode, static_cast<int>(svc.modes.size()) - 1);
            }
            if (state == State::Choose)
            {
                DrawChoose(list, width, height, active);
                return;
            }
            if (state == State::Start)
            {
                Log("DOOM: the window is " + std::to_string(width) + "x" + std::to_string(height) + ", ImGui " + std::to_string(ImGui::GetIO().DisplaySize.x) + "x" +
                    std::to_string(ImGui::GetIO().DisplaySize.y) + " scale " + std::to_string(ImGui::GetIO().DisplayFramebufferScale.x));
                StartGame();
            }

            if (state == State::Game)
            {
                const doom::GameInfo before = game->Info();
                const bool menu = before.menuActive || before.state != doom::GameState::Level;
                const bool humanPlays = controller == nullptr || controller->HumanPlays();
                PollKeyboard(active, menu, !humanPlays);
                for (const ScriptedKey& key : scriptedKeys)
                {
                    if (key.frame == flowFrame)
                    {
                        game->PostKey(key.key, key.pressed);
                    }
                }
                ++flowFrame;

                // the mouse: the left button fires, the movement turns; the cursor is the game's only while the level plays
                const bool playing = humanPlays && active && !before.menuActive && !before.paused && before.state == doom::GameState::Level;
                CaptureCursor(playing);
                int buttons = 0;
                int dx = 0;
                if (playing)
                {
                    buttons = (GetAsyncKeyState(VK_LBUTTON) & 0x8000 ? 1 : 0) | (GetAsyncKeyState(VK_RBUTTON) & 0x8000 ? 2 : 0) |
                              (GetAsyncKeyState(VK_MBUTTON) & 0x8000 ? 4 : 0);
                    if (svc.takeMouseDelta)
                    {
                        dx = svc.takeMouseDelta().first;
                    }
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

            if (state == State::Game)
            {
                // the largest whole scale that fits; the picture is drawn at the size the window gives, so a window smaller than one scale shrinks it
                const float viewHeightUnits = svc.squarePixels ? static_cast<float>(doom::kScreenHeight) : static_cast<float>(doom::kScreenHeight * 6 / 5);   // 240: the 4:3 of the monitors of 1993, in whole numbers
                // a controller has a panel at the right of the picture (the brain of the fly); the human has the whole window
                const bool panelled = controller != nullptr && controller->HasPanel();   // the swarm leaves the whole window to the game
                const float panelWidth = panelled ? std::max(240.0f, std::floor(static_cast<float>(width) * 0.26f)) : 0.0f;
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
                    if (panelled)
                    {
                        controller->DrawPanel(list, pictureWidth, 0.0f, panelWidth, static_cast<float>(height));
                    }
                    controller->DrawOverlay(list, static_cast<float>(width), static_cast<float>(height));
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
    }

    void DoomGameFlow::Install()
    {
        ui::GameUiHooks hooks;
        hooks.bakeFonts = [](ImFontAtlas& atlas, std::uint32_t, std::uint32_t) { atlas.AddFontDefault(); };
        Impl* impl = impl_.get();
        hooks.build = [impl](const core::WindowId windowId, const std::uint32_t width, const std::uint32_t height, const float deltaTime)
        {
            impl->Build(windowId, width, height, deltaTime);
        };
        impl_->svc.ui->SetGameUi(std::move(hooks));
        // the keys and the mouse are the game's (the UI kit does not take them)
        impl_->svc.ui->SetGameUiCapturesInput(false);
    }

    const char* DoomGameFlow::StateName() const
    {
        switch (impl_->state)
        {
            case Impl::State::Start: return "start";
            case Impl::State::Choose: return "choose";
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
