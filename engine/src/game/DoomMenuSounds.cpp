// DoomMenuSounds.cpp

#include "DoomMenuSounds.h"

#include <myengine/game/DoomLevels.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace myengine::game
{
    namespace
    {
        // The lump of each event (§6) and its minimum gap in ms (the repeat of a held arrow plays the cursor not more than 20 times a second)
        struct SoundSpec
        {
            fd::Sfx sfx;
            const char* lump;
            int gapMs;
        };
        constexpr SoundSpec kSpecs[] = {
            {fd::Sfx::Focus, "DSPSTOP", 50},
            {fd::Sfx::Select, "DSPISTOL", 0},
            {fd::Sfx::Back, "DSSWTCHX", 0},
            {fd::Sfx::Change, "DSSTNMOV", 60},
            {fd::Sfx::Deny, "DSOOF", 150},
            {fd::Sfx::Start, "DSTELEPT", 0},
            {fd::Sfx::PauseOpen, "DSSWTCHN", 0},
            {fd::Sfx::PauseClose, "DSSWTCHX", 0},
        };
        // what DOOM says when it is left
        constexpr const char* kLeaveLumps[] = {"DSPLDETH", "DSDMPAIN", "DSPOPAIN", "DSSLOP", "DSTELEPT", "DSPOSIT1", "DSPOSIT3", "DSPOSIT2", "DSBGACT"};

        std::uint32_t ReadU32(std::ifstream& stream)
        {
            unsigned char b[4] = {};
            stream.read(reinterpret_cast<char*>(b), 4);
            return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) | (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        }
    }

    std::vector<std::int16_t> DecodeDmxSound(const std::vector<unsigned char>& lump, std::uint32_t& sampleRate)
    {
        std::vector<std::int16_t> samples;
        if (lump.size() < 8 + 32)
        {
            return samples;
        }
        const unsigned format = lump[0] | (lump[1] << 8);
        const unsigned rate = lump[2] | (lump[3] << 8);
        const std::uint32_t count = static_cast<std::uint32_t>(lump[4]) | (static_cast<std::uint32_t>(lump[5]) << 8) | (static_cast<std::uint32_t>(lump[6]) << 16) | (static_cast<std::uint32_t>(lump[7]) << 24);
        if (format != 3 || rate < 4000 || rate > 48000 || count <= 32 || static_cast<std::size_t>(count) + 8 > lump.size())
        {
            return samples;
        }
        sampleRate = rate;
        // `count` includes 16 bytes of padding in front and 16 behind
        const std::size_t first = 8 + 16;
        const std::size_t length = count - 32;
        samples.reserve(length);
        for (std::size_t i = 0; i < length; ++i)
        {
            samples.push_back(static_cast<std::int16_t>((static_cast<int>(lump[first + i]) - 128) << 8));
        }
        return samples;
    }

    std::vector<std::pair<std::string, std::vector<unsigned char>>> ReadWadLumps(const std::filesystem::path& wad, const std::vector<std::string>& names)
    {
        std::vector<std::pair<std::string, std::vector<unsigned char>>> lumps;
        std::ifstream stream(wad, std::ios::binary);
        if (!stream)
        {
            return lumps;
        }
        char magic[4] = {};
        stream.read(magic, 4);
        if (!stream || (std::memcmp(magic, "IWAD", 4) != 0 && std::memcmp(magic, "PWAD", 4) != 0))
        {
            return lumps;
        }
        const std::uint32_t count = ReadU32(stream);
        const std::uint32_t directory = ReadU32(stream);
        if (!stream || count == 0 || count > 1000000u)
        {
            return lumps;
        }
        stream.seekg(directory, std::ios::beg);
        struct Entry
        {
            std::uint32_t offset;
            std::uint32_t size;
            char name[9];
        };
        std::vector<Entry> wanted;
        for (std::uint32_t i = 0; i < count && stream; ++i)
        {
            Entry entry{};
            entry.offset = ReadU32(stream);
            entry.size = ReadU32(stream);
            stream.read(entry.name, 8);
            entry.name[8] = 0;
            for (const std::string& name : names)
            {
                if (name == entry.name)
                {
                    wanted.push_back(entry);
                    break;
                }
            }
        }
        for (const Entry& entry : wanted)
        {
            if (entry.size == 0 || entry.size > 4 * 1024 * 1024)
            {
                continue;
            }
            stream.clear();
            stream.seekg(entry.offset, std::ios::beg);
            std::vector<unsigned char> bytes(entry.size);
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (stream)
            {
                lumps.emplace_back(entry.name, std::move(bytes));
            }
        }
        return lumps;
    }

    DoomMenuSounds::DoomMenuSounds(audio::AudioSystem& audio, std::function<void(const std::string&)> log) : audio_(audio), log_(std::move(log)) {}

    DoomMenuSounds::~DoomMenuSounds() { Unload(); }

    void DoomMenuSounds::Unload()
    {
        for (audio::SoundHandle& sound : sounds_)
        {
            if (sound.IsValid())
            {
                audio_.UnloadSound(sound);
            }
            sound = {};
        }
        for (const audio::SoundHandle& sound : leave_)
        {
            if (sound.IsValid())
            {
                audio_.UnloadSound(sound);
            }
        }
        leave_.clear();
        loadedCount_ = 0;
    }

    void DoomMenuSounds::Load(const std::filesystem::path& wad)
    {
        if (wad == loadedWad_)
        {
            return;
        }
        Unload();
        loadedWad_ = wad;
        if (wad.empty())
        {
            return;
        }
        std::vector<std::string> names;
        for (const SoundSpec& spec : kSpecs)
        {
            names.push_back(spec.lump);
        }
        for (const char* name : kLeaveLumps)
        {
            names.push_back(name);
        }
        const auto lumps = ReadWadLumps(wad, names);
        const auto make = [&](const std::string& lumpName) -> audio::SoundHandle
        {
            for (const auto& lump : lumps)
            {
                if (lump.first != lumpName)
                {
                    continue;
                }
                audio::WavData wav;
                std::uint32_t rate = 0;
                wav.samples = DecodeDmxSound(lump.second, rate);
                if (wav.samples.empty())
                {
                    return {};
                }
                wav.sampleRate = rate;
                wav.channels = 1;
                // two menus never ask for one name together, but the same lump is made once per use ("DSSWTCHX" is Back and the closing of the pause)
                return audio_.CreateSound("fdmenu/" + lumpName + "/" + std::to_string(loadedCount_), wav);
            }
            return {};
        };
        for (const SoundSpec& spec : kSpecs)
        {
            audio::SoundHandle sound = make(spec.lump);
            if (sound.IsValid())
            {
                ++loadedCount_;
            }
            sounds_[static_cast<std::size_t>(spec.sfx)] = sound;
        }
        for (const char* name : kLeaveLumps)
        {
            const audio::SoundHandle sound = make(name);
            if (sound.IsValid())
            {
                leave_.push_back(sound);
                ++loadedCount_;
            }
        }
        if (log_)
        {
            log_("FlyDoom menu: " + std::to_string(loadedCount_) + " sounds of the game data " + wad.filename().string());
        }
    }

    void DoomMenuSounds::Play(const fd::Sfx sfx)
    {
        const std::size_t index = static_cast<std::size_t>(sfx);
        audio::SoundHandle sound = index < sounds_.size() ? sounds_[index] : audio::SoundHandle{};
        if (sfx == fd::Sfx::Leave && !leave_.empty())
        {
            random_ = random_ * 1664525u + 1013904223u;
            sound = leave_[(random_ >> 16) % leave_.size()];
        }
        if (!sound.IsValid() || index >= last_.size())
        {
            return;
        }
        int gap = 0;
        for (const SoundSpec& spec : kSpecs)
        {
            if (spec.sfx == sfx)
            {
                gap = spec.gapMs;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (gap > 0 && now - last_[index] < std::chrono::milliseconds(gap))
        {
            return;
        }
        last_[index] = now;
        audio::PlayParams params;
        params.bus = audio::Bus::Sfx;
        params.volume = 0.7f;
        audio_.Play(sound, params);
    }
}
