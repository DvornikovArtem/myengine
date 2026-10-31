// DoomGame.cpp
//
// DoomGame over doomgeneric: the callbacks DG_* that the platform part of the port must give, a virtual clock, the queue of keys.

#include <doom/DoomGame.h>
#include <doom/DoomSwarm.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "DoomBridge.h"
#include "DoomMusic.h"

extern "C"
{
#include "doomgeneric.h"
}

namespace doom
{
    namespace
    {
        constexpr std::uint32_t kClockStartMs = 1000;   // DOOM treats 0 as "the clock has not started"
        constexpr int kTicRate = 35;

        struct KeyEvent
        {
            int key;
            bool pressed;
        };
    }

    struct DoomGame::Impl
    {
        DoomGame* owner = nullptr;
        double clockMs = kClockStartMs;              // fractions of a millisecond are kept
        long long lastDueTic = 0;
        std::deque<KeyEvent> keys;
        // The mouse: DOOM keeps only the last mouse event before a tic (mousex is assigned) and its queue holds 64 events, while the host posts at the frame
        // rate. So the movement is added up here and goes to DOOM as one event when a tic is due.
        bool mouseDirty = false;
        int mouseButtons = 0;
        int mouseDx = 0;
        int mouseDy = 0;
        bool frameDrawn = false;
        ITicCmdSource* ticCmdSource = nullptr;
        std::filesystem::path iwad;

        IDoomAudioHost* audio = nullptr;
        std::unique_ptr<DoomMusic> music;
        dgb_audio table{};
        std::vector<std::vector<std::int16_t>> unused;

        std::uint32_t ClockMs() const { return static_cast<std::uint32_t>(clockMs); }
    };

    namespace
    {
        DoomGame::Impl* g_impl = nullptr;   // the callbacks of doomgeneric are C functions without a context: one game per process

        // ---- the sound device, seen from the C modules of DOOM ----
        float ToVolume(int volume127)
        {
            return std::clamp(static_cast<float>(volume127) / 127.0f, 0.0f, 1.0f);
        }

        float ToPan(int sep254)
        {
            return std::clamp((static_cast<float>(sep254) - 127.0f) / 127.0f, -1.0f, 1.0f);
        }

        int RegisterSfx(void* user, const char* name, const short* samples, int frames, int rate)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            return impl->audio->RegisterSfx(name, samples, frames, rate);
        }

        int StartSfx(void* user, int sfx, int channel, int vol, int sep)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            return impl->audio->StartSfx(sfx, channel, ToVolume(vol), ToPan(sep)) ? channel : -1;
        }

        void UpdateSfx(void* user, int channel, int vol, int sep)
        {
            static_cast<DoomGame::Impl*>(user)->audio->UpdateSfx(channel, ToVolume(vol), ToPan(sep));
        }

        void StopSfx(void* user, int channel)
        {
            static_cast<DoomGame::Impl*>(user)->audio->StopSfx(channel);
        }

        int SfxPlaying(void* user, int channel)
        {
            return static_cast<DoomGame::Impl*>(user)->audio->IsSfxPlaying(channel) ? 1 : 0;
        }

        void AudioUpdate(void* user)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            impl->audio->Update();
            if (impl->music)
            {
                impl->music->Update();
            }
        }

        void MusicInstruments(void* user, const unsigned char* genmidi, int length)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            if (impl->music)
            {
                impl->music->SetInstruments(genmidi, length);
            }
        }

        int MusicRegister(void* user, const unsigned char* midi, int length)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            return impl->music ? impl->music->Register(midi, static_cast<std::size_t>(length)) : 0;
        }

        void MusicUnregister(void* user, int handle)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            if (impl->music)
            {
                impl->music->Unregister(handle);
            }
        }

        void MusicPlay(void* user, int handle, int loop)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            if (impl->music)
            {
                impl->music->Play(handle, loop != 0);
            }
        }

        void MusicStop(void* user)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            if (impl->music)
            {
                impl->music->Stop();
            }
        }

        void MusicPause(void* user)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            if (impl->music)
            {
                impl->music->Pause();
            }
        }

        void MusicResume(void* user)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            if (impl->music)
            {
                impl->music->Resume();
            }
        }

        void MusicVolume(void* user, int volume127)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            if (impl->music)
            {
                impl->music->SetVolume(ToVolume(volume127));
            }
        }

        int MusicPlaying(void* user)
        {
            auto* impl = static_cast<DoomGame::Impl*>(user);
            return impl->music && impl->music->IsPlaying() ? 1 : 0;
        }

        // ---- the hook of the tic commands ----
        void TicCmdHook(int player, signed char* forwardmove, signed char* sidemove, short* angleturn, unsigned char* buttons)
        {
            if (g_impl == nullptr || g_impl->ticCmdSource == nullptr)
            {
                return;
            }
            TicCmd cmd;
            cmd.forwardMove = *forwardmove;
            cmd.sideMove = *sidemove;
            cmd.angleTurn = *angleturn;
            cmd.buttons = *buttons;
            g_impl->ticCmdSource->BuildTicCmd(player, cmd);
            *forwardmove = cmd.forwardMove;
            *sidemove = cmd.sideMove;
            *angleturn = cmd.angleTurn;
            *buttons = cmd.buttons;
        }

        // DOOM opens files with fopen and a narrow string (the code page of the system): a path with letters that the page does not have
        // (a Cyrillic user name on an English system) goes in its short 8.3 form
        std::string NarrowPath(const std::filesystem::path& path)
        {
            const std::wstring wide = path.wstring();
            BOOL usedDefault = FALSE;
            const int size = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), -1, nullptr, 0, nullptr, &usedDefault);
            if (size > 0 && !usedDefault)
            {
                std::string narrow(static_cast<std::size_t>(size), '\0');
                WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), -1, narrow.data(), size, nullptr, &usedDefault);
                narrow.resize(static_cast<std::size_t>(size - 1));
                return narrow;
            }
            std::wstring shortPath(MAX_PATH * 2, L'\0');
            const DWORD length = GetShortPathNameW(wide.c_str(), shortPath.data(), static_cast<DWORD>(shortPath.size()));
            if (length > 0 && length < shortPath.size())
            {
                shortPath.resize(length);
                return std::filesystem::path(shortPath).string();
            }
            return path.string();
        }

        bool g_started = false;   // DOOM has been started in this process (it cannot be started again)
    }

    // ---- the callbacks that doomgeneric needs from the platform (the C functions DG_* below call these) ----
    void PlatformDrawFrame()
    {
        if (g_impl != nullptr)
        {
            g_impl->frameDrawn = true;
        }
    }

    // DOOM waits for the next tic with this: the virtual clock moves instead of the thread sleeping
    void PlatformSleepMs(const std::uint32_t ms)
    {
        if (g_impl != nullptr)
        {
            g_impl->clockMs += ms;
        }
    }

    std::uint32_t PlatformTicksMs()
    {
        return g_impl != nullptr ? g_impl->ClockMs() : kClockStartMs;
    }

    int PlatformGetKey(int* pressed, unsigned char* key)
    {
        if (g_impl == nullptr || g_impl->keys.empty())
        {
            return 0;
        }
        const KeyEvent event = g_impl->keys.front();
        g_impl->keys.pop_front();
        *pressed = event.pressed ? 1 : 0;
        *key = static_cast<unsigned char>(event.key);
        return 1;
    }

    void PlatformSetWindowTitle(const char* title)
    {
        if (g_impl != nullptr && title != nullptr && g_impl->owner != nullptr)
        {
            g_impl->owner->OnWindowTitle(title);
        }
    }

    DoomGame::DoomGame() : impl_(std::make_unique<Impl>())
    {
        impl_->owner = this;
    }

    DoomGame::~DoomGame()
    {
        // DOOM itself is left as it is (its globals live until the process ends); only our side goes away
        if (g_impl == impl_.get())
        {
            dgb_SetTicCmdHook(nullptr);
            dgb_SetAudio(nullptr);
            g_impl = nullptr;
        }
    }

    std::unique_ptr<DoomGame> DoomGame::Create(const DoomConfig& config, std::string& error)
    {
        if (g_started)
        {
            error = "DOOM can be started once per process (its state is global); close FlyDoom and start it again";
            return nullptr;
        }
        std::error_code ec;
        if (config.iwad.empty() || !std::filesystem::is_regular_file(config.iwad, ec))
        {
            error = "the game data (WAD) is not found: " + config.iwad.string();
            return nullptr;
        }

        std::unique_ptr<DoomGame> game(new DoomGame());
        Impl& impl = *game->impl_;
        g_impl = &impl;
        impl.iwad = config.iwad;
        g_started = true;

        // The command line of DOOM. The strings live as long as the process: DOOM keeps the pointers.
        static std::vector<std::string> argStrings;
        static std::vector<char*> argv;
        argStrings.clear();
        argStrings.push_back("FlyDoom");
        argStrings.push_back("-iwad");
        argStrings.push_back(NarrowPath(config.iwad));
        argStrings.push_back("-nogui");
        argStrings.push_back("-mb");
        argStrings.push_back("64");
        if (!config.dataDirectory.empty())
        {
            std::filesystem::create_directories(config.dataDirectory, ec);
            std::string dir = NarrowPath(config.dataDirectory);
            if (!dir.empty() && dir.back() != '\\' && dir.back() != '/')
            {
                dir += '\\';
            }
            argStrings.push_back("-configdir");
            argStrings.push_back(dir);
        }
        if (config.audio == nullptr)
        {
            argStrings.push_back("-nosound");
        }
        else if (!config.music)
        {
            argStrings.push_back("-nomusic");
        }
        for (const std::string& arg : config.extraArgs)
        {
            argStrings.push_back(arg);
            if (arg == swarm::kStartArgument)
            {
                // the swarm of flies (D1c): the sprites and the buzz of a fly take the place of those of the Lost Soul; DOOM puts them in after the WADs are loaded
                if (!swarm::PrepareAssets(config.iwad, error))
                {
                    return nullptr;
                }
            }
        }
        argv.clear();
        for (std::string& arg : argStrings)
        {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        impl.audio = config.audio;
        if (config.audio != nullptr)
        {
            if (config.music)
            {
                impl.music = std::make_unique<DoomMusic>(*config.audio);
            }
            impl.table.user = &impl;
            impl.table.register_sfx = RegisterSfx;
            impl.table.start_sfx = StartSfx;
            impl.table.update_sfx = UpdateSfx;
            impl.table.stop_sfx = StopSfx;
            impl.table.sfx_playing = SfxPlaying;
            impl.table.update = AudioUpdate;
            impl.table.music_instruments = MusicInstruments;
            impl.table.music_register = MusicRegister;
            impl.table.music_unregister = MusicUnregister;
            impl.table.music_play = MusicPlay;
            impl.table.music_stop = MusicStop;
            impl.table.music_pause = MusicPause;
            impl.table.music_resume = MusicResume;
            impl.table.music_volume = MusicVolume;
            impl.table.music_playing = MusicPlaying;
            dgb_SetAudio(&impl.table);
        }
        dgb_SetTicCmdHook(TicCmdHook);

        if (dgb_Create(static_cast<int>(argv.size()) - 1, argv.data()) != 0)
        {
            error = dgb_ExitMessage()[0] != '\0' ? std::string(dgb_ExitMessage()) : "DOOM stopped while starting";
            return nullptr;
        }
        // The start up of the original ends with the first tic: the frame of the title screen (or of the level) exists now
        if (impl.frameDrawn)
        {
            game->frameCount_ = 1;
        }
        impl.frameDrawn = false;
        return game;
    }

    bool DoomGame::Advance(const double deltaSeconds)
    {
        if (finished_)
        {
            return false;
        }
        impl_->clockMs += std::max(0.0, deltaSeconds) * 1000.0;
        const long long dueTic = static_cast<long long>((impl_->clockMs - kClockStartMs) * kTicRate / 1000.0);
        if (dueTic <= impl_->lastDueTic)
        {
            return false;
        }
        impl_->lastDueTic = dueTic;

        if (impl_->mouseDirty)
        {
            dgb_PostMouse(impl_->mouseButtons, impl_->mouseDx, impl_->mouseDy);
            impl_->mouseDirty = false;
            impl_->mouseDx = 0;
            impl_->mouseDy = 0;
        }
        impl_->frameDrawn = false;
        if (dgb_Tick() != 0)
        {
            Finish(dgb_ExitMessage(), dgb_ExitCode());
            return false;
        }
        if (impl_->frameDrawn)
        {
            ++frameCount_;
        }
        return impl_->frameDrawn;
    }

    bool DoomGame::StepTic()
    {
        // the clock goes to the first moment of the next tic, so that DOOM finds exactly one tic to run
        const double nextTicMs = kClockStartMs + static_cast<double>(impl_->lastDueTic + 1) * 1000.0 / kTicRate;
        if (impl_->clockMs < nextTicMs)
        {
            impl_->clockMs = nextTicMs + 0.001;
        }
        return Advance(0.0);
    }

    void DoomGame::Finish(const char* message, const int code)
    {
        finished_ = true;
        if (code != 0)
        {
            error_ = (message != nullptr && message[0] != '\0') ? message : "DOOM stopped with an error";
        }
    }

    const std::uint32_t* DoomGame::Pixels() const
    {
        return reinterpret_cast<const std::uint32_t*>(DG_ScreenBuffer);
    }

    std::uint64_t DoomGame::FrameHash() const
    {
        const std::uint32_t* pixels = Pixels();
        if (pixels == nullptr)
        {
            return 0;
        }
        std::uint64_t hash = 1469598103934665603ull;
        for (int i = 0; i < kScreenWidth * kScreenHeight; ++i)
        {
            hash = (hash ^ pixels[i]) * 1099511628211ull;
        }
        return hash;
    }

    bool DoomGame::RenderView(const int player, ViewBuffer& out)
    {
        if (Pixels() == nullptr)
        {
            return false;
        }
        out.width = kScreenWidth;
        out.height = kScreenHeight;
        if (player == Info().consolePlayer)
        {
            out.pixels.assign(Pixels(), Pixels() + kScreenWidth * kScreenHeight);
            return true;
        }
        out.pixels.resize(static_cast<size_t>(kScreenWidth) * kScreenHeight);
        return dgb_RenderPlayerView(player, out.pixels.data()) != 0;
    }

    void DoomGame::RestartLevel()
    {
        dgb_RestartLevel();
    }

    bool DoomGame::RenderEyes(const int player, ViewBuffer& out)
    {
        out.width = kScreenWidth;
        out.height = kScreenHeight;
        out.pixels.resize(static_cast<size_t>(kScreenWidth) * kScreenHeight);
        return dgb_RenderPlayerView(player, out.pixels.data()) != 0;
    }

    void DoomGame::PostKey(const int key, const bool pressed)
    {
        if (!finished_ && key > 0 && key < 256)
        {
            impl_->keys.push_back({key, pressed});
        }
    }

    void DoomGame::PostMouse(const int buttons, const int deltaX, const int deltaY)
    {
        if (!finished_)
        {
            impl_->mouseButtons = buttons;
            impl_->mouseDx += deltaX;
            impl_->mouseDy += deltaY;
            impl_->mouseDirty = true;
        }
    }

    void DoomGame::SetTicCmdSource(ITicCmdSource* source)
    {
        impl_->ticCmdSource = source;
    }

    bool DoomGame::GetTicCmd(const int player, TicCmd& out) const
    {
        return dgb_GetTicCmd(player, &out.forwardMove, &out.sideMove, &out.angleTurn, &out.buttons) != 0;
    }

    const std::filesystem::path& DoomGame::Iwad() const
    {
        return impl_->iwad;
    }

    GameInfo DoomGame::Info() const
    {
        dgb_state state{};
        dgb_GetState(&state);
        GameInfo info;
        info.gameTic = state.gameTic;
        info.state = static_cast<GameState>(std::clamp(state.gameState, 0, 3));
        info.episode = state.episode;
        info.map = state.map;
        info.menuActive = state.menuActive != 0;
        info.paused = state.paused != 0;
        info.demoPlayback = state.demoPlayback != 0;
        info.consolePlayer = state.consolePlayer;
        info.skill = state.skill;
        info.deathmatch = state.deathmatch;
        info.netGame = state.netGame != 0;
        return info;
    }

    PlayerState DoomGame::Player(const int player) const
    {
        PlayerState state;
        dgb_player raw{};
        if (dgb_GetPlayer(player, &raw) == 0)
        {
            return state;
        }
        state.inGame = true;
        state.health = raw.health;
        state.armor = raw.armor;
        state.x = raw.x / 65536.0;
        state.y = raw.y / 65536.0;
        state.z = raw.z / 65536.0;
        state.angleDegrees = static_cast<double>(raw.angle) * (360.0 / 4294967296.0);
        state.readyWeapon = raw.readyWeapon;
        state.kills = raw.kills;
        state.items = raw.items;
        state.secrets = raw.secrets;
        state.frags = raw.frags;
        state.deaths = raw.deaths;
        state.hasAttacker = raw.hasAttacker != 0;
        state.attackerX = raw.attackerX / 65536.0;
        state.attackerY = raw.attackerY / 65536.0;
        state.shots = raw.shots;
        state.hits = raw.hits;
        state.damageDealt = raw.damageDealt;
        state.doorsOpened = raw.doorsOpened;
        state.sector = raw.sector;
        state.inDoorSector = raw.doorSector != 0;
        return state;
    }

    std::vector<double> DoomGame::Whiskers(const int player, const std::vector<double>& anglesDegrees, const double range) const
    {
        std::vector<int> angles(anglesDegrees.size());
        std::vector<int> distances(anglesDegrees.size(), static_cast<int>(range));
        for (std::size_t i = 0; i < angles.size(); ++i)
        {
            angles[i] = static_cast<int>(static_cast<std::int64_t>(std::llround(anglesDegrees[i] * (4294967296.0 / 360.0))));
        }
        if (!angles.empty())
        {
            dgb_GetWhiskers(player, angles.data(), static_cast<int>(angles.size()), static_cast<int>(range), distances.data());
        }
        return std::vector<double>(distances.begin(), distances.end());
    }

    VisibleTarget DoomGame::FindVisibleTarget(const int player, const double angleOffsetDegrees) const
    {
        dgb_visible_target raw{};
        VisibleTarget target;
        const int angleOffset = static_cast<int>(std::llround(angleOffsetDegrees * (4294967296.0 / 360.0)));
        if (dgb_GetVisibleTarget(player, angleOffset, &raw) != 0)
        {
            target.found = true;
            target.angleDegrees = static_cast<double>(raw.angleDelta) * (360.0 / 4294967296.0);
        }
        return target;
    }
}

// The platform functions that doomgeneric.h asks for
extern "C"
{
    void DG_Init()
    {
    }

    void DG_DrawFrame()
    {
        doom::PlatformDrawFrame();
    }

    void DG_SleepMs(uint32_t ms)
    {
        doom::PlatformSleepMs(ms);
    }

    uint32_t DG_GetTicksMs()
    {
        return doom::PlatformTicksMs();
    }

    int DG_GetKey(int* pressed, unsigned char* key)
    {
        return doom::PlatformGetKey(pressed, key);
    }

    void DG_SetWindowTitle(const char* title)
    {
        doom::PlatformSetWindowTitle(title);
    }
}
