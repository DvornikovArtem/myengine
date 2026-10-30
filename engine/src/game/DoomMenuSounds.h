// DoomMenuSounds.h
//
// The sounds of the menu of FlyDoom (.team/design/fd-menu-spec.md, §6): they are the sounds of DOOM itself, taken out of the game data the menu has chosen (the lumps DSPSTOP,
// DSPISTOP ... of a DOOM, DOOM 2 or Freedoom WAD), so the cursor of the menu sounds as the cursor of the menu of DOOM. No game data: the menu is silent, it has no sound files of its own
// (nothing to license). The lumps are registered once with the AudioSystem when the WAD is chosen and played on the Sfx bus.

#pragma once

#include "DoomMenuWidgets.h"

#include <myengine/audio/AudioSystem.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace myengine::game
{
    class DoomMenuSounds
    {
    public:
        DoomMenuSounds(audio::AudioSystem& audio, std::function<void(const std::string&)> log);
        ~DoomMenuSounds();
        DoomMenuSounds(const DoomMenuSounds&) = delete;
        DoomMenuSounds& operator=(const DoomMenuSounds&) = delete;

        // The sounds of this WAD; an empty path or a WAD without them: silence. Loading the same WAD again does nothing.
        void Load(const std::filesystem::path& wad);
        void Play(fd::Sfx sfx);
        bool Loaded() const { return !loadedWad_.empty() && loadedCount_ > 0; }
        int LoadedCount() const { return loadedCount_; }

    private:
        audio::AudioSystem& audio_;
        std::function<void(const std::string&)> log_;
        std::filesystem::path loadedWad_;
        std::array<audio::SoundHandle, 9> sounds_{};                 // by fd::Sfx; the quit sounds are in `leave_`
        std::vector<audio::SoundHandle> leave_;                      // the sounds of DOOM when it is left: one of them at random
        std::array<std::chrono::steady_clock::time_point, 9> last_{};
        int loadedCount_ = 0;
        std::uint32_t random_ = 12345u;

        void Unload();
    };
}
