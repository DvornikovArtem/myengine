// DoomMusic.h
//
// The music of DOOM: the MIDI file made from a MUS lump is played by an emulation of the OPL chip (the AdLib / Sound Blaster FM chip the
// original used, with the instruments of the GENMIDI lump of the WAD) and rendered to PCM, which the host plays as a looped sound.
// Internal to the library.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <doom/DoomAudio.h>

namespace doom
{
    class DoomMusic
    {
    public:
        explicit DoomMusic(IDoomAudioHost& host);
        ~DoomMusic();

        // The GENMIDI lump of the WAD (the instruments of the chip); it stays valid. Called once, on the thread of DOOM.
        void SetInstruments(const unsigned char* genmidi, int length);
        int Register(const unsigned char* midi, std::size_t length);
        void Unregister(int handle);
        void Play(int handle, bool loop);
        void Stop();
        void Pause();
        void Resume();
        void SetVolume(float volume);
        bool IsPlaying() const;
        // Once per tic: hands over the rendered song when the worker has finished it
        void Update();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
