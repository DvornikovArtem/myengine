// DoomLevels.h
//
// The levels a WAD has and how to start one (the menu of FlyDoom, docs/game/doom.md): the names by the file (doom1.wad is episode 1, doom2.wad and freedoom2.wad are MAP01 to
// MAP32 ...) and the arguments of DOOM ("-warp 1 3 -skill 4") that start it. Pure functions, tested without a window.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace myengine::game
{
    // One level that can be started: the title screen of DOOM (episode 0) or ExMy / MAPnn
    struct DoomLevelEntry
    {
        std::string label;     // "E1M1" / "MAP07" / "Title screen"
        int episode = 0;       // 0: no warp (the title screen of DOOM decides); in a WAD with MAPnn the episode is 1 and `map` is the number
        int map = 1;
    };

    // The lump of a DMX sound as it lies in a WAD: format 3, the rate, the number of samples (with 16 bytes of padding at each end), 8 bit unsigned. Returns the samples as 16 bit
    // (empty when the bytes are not a sound of this kind); `sampleRate` is set then.
    std::vector<std::int16_t> DecodeDmxSound(const std::vector<unsigned char>& lump, std::uint32_t& sampleRate);

    // The lumps of a WAD (IWAD or PWAD) by name (upper case), read from its directory. Names that are not there are left out; the lumps come in the order of the WAD.
    std::vector<std::pair<std::string, std::vector<unsigned char>>> ReadWadLumps(const std::filesystem::path& wad, const std::vector<std::string>& names);

    // The levels of a WAD by its name (doom1.wad: E1M1 .. E1M9; doom.wad and freedoom1: four episodes; doom2, freedoom2, plutonia, tnt: MAP01 .. MAP32). The first is the title screen.
    std::vector<DoomLevelEntry> LevelsOfWad(const std::filesystem::path& wad);
    // "Freedoom: Phase 1", "DOOM (shareware, episode 1)", "DOOM 2" ...
    std::string WadLabel(const std::filesystem::path& wad, bool freedoom);
    // The arguments of DOOM that start the chosen level: "-warp 1 3 -skill 4"; nothing for the title screen. A WAD with MAPnn takes one number: "-warp 7 -skill 4".
    std::vector<std::string> WarpArguments(const std::filesystem::path& wad, const DoomLevelEntry& level, int skill);
}
