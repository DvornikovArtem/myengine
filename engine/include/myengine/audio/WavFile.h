// WavFile.h
//
// RIFF/WAVE reading without a device: PCM 8 / 16 / 24 / 32 bit and float 32 bit, mono or stereo, any sample rate; the samples
// are converted to 16 bit. Parsing from memory is what the tests use (a file is read and handed to it).

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <myengine/audio/AudioTypes.h>

namespace myengine::audio
{
    // false: `error` says what is wrong (not RIFF/WAVE, an unsupported format, a truncated chunk, more than two channels...)
    bool ParseWav(const std::uint8_t* bytes, std::size_t size, WavData& out, std::string& error);
    bool LoadWavFile(const std::filesystem::path& path, WavData& out, std::string& error);

    // A 16 bit PCM WAV in memory (the tests and the tools build files with it)
    std::vector<std::uint8_t> BuildWav(const WavData& data);
}
