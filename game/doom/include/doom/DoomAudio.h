// DoomAudio.h
//
// The sound device behind DOOM (docs/game/doom.md): the port decodes the sound effects of the WAD and renders the music, the host
// (the engine's AudioSystem, or a test) plays what it is given. No engine type is visible here.

#pragma once

#include <cstddef>
#include <cstdint>

namespace doom
{
    class IDoomAudioHost
    {
    public:
        virtual ~IDoomAudioHost() = default;

        // A sound effect of the WAD as 16 bit mono; returns a handle > 0 (0: the host cannot keep it)
        virtual int RegisterSfx(const char* name, const std::int16_t* samples, int frames, int sampleRate) = 0;
        // DOOM's channel is the key: a new sound on a channel replaces the old one. volume 0..1, pan -1 (left) .. +1 (right).
        // false: not played (no free voice)
        virtual bool StartSfx(int sfx, int channel, float volume, float pan) = 0;
        virtual void UpdateSfx(int channel, float volume, float pan) = 0;
        virtual void StopSfx(int channel) = 0;
        virtual bool IsSfxPlaying(int channel) = 0;
        // Once per tick of DOOM
        virtual void Update() {}

        // The music of a level, rendered by the OPL emulation: stereo 16 bit frames, a copy is made by the host before it returns.
        // Replaces the music that plays.
        virtual void PlayMusic(const std::int16_t* stereoFrames, std::size_t frames, int sampleRate, bool loop)
        {
            (void)stereoFrames;
            (void)frames;
            (void)sampleRate;
            (void)loop;
        }
        virtual void StopMusic() {}
        virtual void PauseMusic() {}
        virtual void ResumeMusic() {}
        virtual void SetMusicVolume(float volume) { (void)volume; }
        virtual bool IsMusicPlaying() { return false; }
    };
}
