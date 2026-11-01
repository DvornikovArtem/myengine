// DoomFrontEndTests.cpp
// V1 (FlyDoom): what the menu of FlyDoom does without a window: the settings in their file (a round trip, a file that was edited by hand or is broken, the limits), the levels
// of a WAD by its name, the arguments of DOOM that start a level, the label of a WAD, and the sounds of the menu (a DMX lump out of a WAD).

#include <myengine/game/DoomFrontEnd.h>
#include <myengine/game/DoomLevels.h>

#include <cstdint>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    using namespace myengine::game;
    namespace fs = std::filesystem;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void Write(const fs::path& file, const std::string& text)
    {
        fs::create_directories(file.parent_path());
        std::ofstream stream(file, std::ios::trunc);
        stream << text;
    }

    void TestSettings(const fs::path& directory)
    {
        const fs::path file = directory / "FlyDoom" / "settings.json";
        DoomSettings none;
        Check(!none.Load(file), "no file: the defaults stay and Load says so");
        Check(none.sfxVolume == 1.0f && none.musicOn && none.lastMode == "human" && none.windowMode == 0 && none.brainBackdrop, "the defaults");

        DoomSettings settings;
        settings.sfxVolume = 0.35f;
        settings.musicVolume = 0.0f;
        settings.musicOn = false;
        settings.mouseSensitivity = 1.75f;
        settings.windowMode = 2;
        settings.uiScalePercent = 110;
        settings.brainBackdrop = false;
        settings.lastMode = "fly";
        settings.controlSet = "bio";
        settings.backend = "gpu";
        settings.iwad = "C:\\games\\doom\\freedoom1.wad";
        settings.episode = 2;
        settings.map = 5;
        settings.skill = 4;
        settings.menuMotion = 1;
        settings.flies = 24;
        settings.learnSpeed = 0;
        settings.learnStart = "trained";
        settings.flyBrain = "scratch";
        settings.mouseSensitivity = 3.5f;
        Check(settings.Save(file) && fs::exists(file), "the settings are written");
        DoomSettings back;
        Check(back.Load(file), "and read");
        Check(back.sfxVolume == 0.35f && back.musicVolume == 0.0f && !back.musicOn && back.mouseSensitivity == 3.5f && back.windowMode == 2 && back.uiScalePercent == 110 && !back.brainBackdrop,
              "the numbers and the switches come back");
        Check(back.menuMotion == 1 && back.flies == 24 && back.learnSpeed == 0 && back.learnStart == "trained" && back.flyBrain == "scratch" && back.mouseSensitivity == 3.5f,
              "reduced motion, the flies of the swarm, the rows of the fly that learns and a mouse speed up to 4x come back");
        Check(none.menuMotion == 0 && none.flies == 16 && none.learnSpeed == 1 && none.learnStart == "scratch" && none.flyBrain == "trained", "their defaults");
        Check(back.lastMode == "fly" && back.controlSet == "bio" && back.backend == "gpu" && back.iwad == settings.iwad && back.episode == 2 && back.map == 5 && back.skill == 4,
              "the choices of the menu come back (also a path with a backslash)");

        // edited by hand: values out of the range are brought into it, a value of the wrong kind keeps the default, an unknown key is ignored
        Write(file, R"({"sfxVolume": 7.5, "musicVolume": -2, "mouseSensitivity": 0.0, "windowMode": 9, "uiScalePercent": 500, "skill": 0, "episode": 99, "map": 0,
                        "controlSet": "elephant", "backend": "quantum", "lastMode": "", "musicOn": "yes", "windowTitle": "x",
                        "menuMotion": 5, "flies": 100, "learnSpeed": 3, "learnStart": "later", "flyBrain": "wise"})");
        DoomSettings edited;
        Check(edited.Load(file), "a file edited by hand is read");
        Check(edited.sfxVolume == 1.0f && edited.musicVolume == 0.0f && edited.mouseSensitivity == 0.25f && edited.windowMode == 2 && edited.uiScalePercent == 125, "numbers go into their range");
        Check(edited.skill == 1 && edited.episode == 9 && edited.map == 1, "the level and the skill too");
        Check(edited.controlSet == "doomfly" && edited.backend == "auto" && edited.lastMode == "human", "a name that the game does not know is the default");
        Check(edited.musicOn, "a text where a switch is expected keeps the default");
        Check(edited.menuMotion == 1 && edited.flies == 32 && edited.learnSpeed == 0 && edited.learnStart == "scratch" && edited.flyBrain == "trained", "the new choices are brought into their range too");
        Write(file, R"({"mouseSensitivity": 9.0, "learnSpeed": 100, "flies": 1})");
        DoomSettings limits;
        Check(limits.Load(file) && limits.mouseSensitivity == 4.0f && limits.learnSpeed == 0 && limits.flies == 8, "the mouse goes up to 4x, any speed but 1 is MAX (0), the swarm from 8 flies");

        // a broken file does not stop the game: the defaults and false
        Write(file, "{ this is not json");
        DoomSettings broken;
        Check(!broken.Load(file) && broken.sfxVolume == 1.0f, "a broken file: the defaults");
        Write(file, "[1, 2, 3]");
        Check(!broken.Load(file), "JSON that is not an object: the defaults");

        // the file that is written whole or not at all: no .tmp is left
        Check(settings.Save(file) && !fs::exists(file.wstring() + L".tmp"), "no temporary file is left");
        // a place that cannot be written: false, not a crash
        Check(!settings.Save(directory / "FlyDoom" / "settings.json" / "inside-a-file.json"), "a path inside a file cannot be written");

        // the game in a folder with a Russian name ("C:\Users\...\Рабочий стол\FlyDoom"): the menu keeps the WAD in UTF-8 and it comes back as it was
        const fs::path russian = fs::path(L"C:\\Users\\ivan\\OneDrive\\\u0420\u0430\u0431\u043e\u0447\u0438\u0439 \u0441\u0442\u043e\u043b\\FlyDoom\\assets\\doom\\freedoom1.wad");
        DoomSettings utf8 = settings;
        utf8.iwad = russian.u8string();
        Check(utf8.Save(file), "a WAD under a Russian folder is saved");
        DoomSettings utf8Back;
        Check(utf8Back.Load(file) && fs::u8path(utf8Back.iwad) == russian, "and read back as the same path");
        // the same path in the code page of Windows (what path.string() gave before): the save does not throw and writes the rest
        DoomSettings ansi = settings;
        ansi.iwad = "C:\\Users\\ivan\\OneDrive\\\xD0\xE0\xE1\xEE\xF7\xE8\xE9 \xF1\xF2\xEE\xEB\\FlyDoom\\assets\\doom\\freedoom1.wad";
        ansi.skill = 2;
        Check(ansi.Save(file), "a path that is not UTF-8 does not stop the save (it used to throw json type_error 316)");
        DoomSettings ansiBack;
        Check(ansiBack.Load(file) && ansiBack.skill == 2, "and the other settings are there");
    }

    void TestLevels()
    {
        const auto labels = [](const std::vector<DoomLevelEntry>& levels)
        {
            std::string text;
            for (const DoomLevelEntry& level : levels)
            {
                text += level.label + " ";
            }
            return text;
        };
        const auto shareware = LevelsOfWad("C:/x/doom1.wad");
        Check(shareware.size() == 10 && shareware[0].episode == 0 && shareware[1].label == "E1M1" && shareware[9].label == "E1M9", "doom1.wad: the title screen and E1M1 .. E1M9 (" + labels(shareware) + ")");
        const auto doom = LevelsOfWad("C:/x/DOOM.WAD");
        Check(doom.size() == 1 + 4 * 9 && doom[10].label == "E2M1" && doom.back().label == "E4M9", "doom.wad: four episodes, the case of the name does not matter");
        const auto freedoom1 = LevelsOfWad("freedoom1.wad");
        Check(freedoom1.size() == 1 + 4 * 9, "freedoom1.wad has four episodes");
        const auto doom2 = LevelsOfWad("C:/x/doom2.wad");
        Check(doom2.size() == 33 && doom2[1].label == "MAP01" && doom2.back().label == "MAP32", "doom2.wad: MAP01 .. MAP32");
        Check(LevelsOfWad("freedoom2.wad").size() == 33 && LevelsOfWad("plutonia.wad").size() == 33 && LevelsOfWad("TNT.WAD").size() == 33, "freedoom2, plutonia and tnt are MAPnn too");
        const auto unknown = LevelsOfWad("mymod.wad");
        Check(unknown.size() == 10, "a WAD we do not know: the first episode");
        Check(LevelsOfWad("")[0].label.find("Title") == 0, "an empty name does not crash");

        // the arguments of DOOM
        Check(WarpArguments("doom1.wad", shareware[0], 3).empty(), "the title screen: no warp");
        const auto e1m3 = WarpArguments("doom1.wad", shareware[3], 4);
        Check(e1m3.size() == 5 && e1m3[0] == "-warp" && e1m3[1] == "1" && e1m3[2] == "3" && e1m3[3] == "-skill" && e1m3[4] == "4", "E1M3 at skill 4: -warp 1 3 -skill 4");
        const auto e3m2 = WarpArguments("doom.wad", doom[1 + 2 * 9 + 1], 5);
        Check(e3m2.size() == 5 && e3m2[1] == "3" && e3m2[2] == "2" && e3m2[4] == "5", "E3M2: -warp 3 2");
        const auto map07 = WarpArguments("doom2.wad", doom2[7], 2);
        Check(map07.size() == 4 && map07[0] == "-warp" && map07[1] == "7" && map07[2] == "-skill" && map07[3] == "2", "MAP07 takes one number: -warp 7 -skill 2");
        const auto clamped = WarpArguments("freedoom2.wad", LevelsOfWad("freedoom2.wad")[1], 99);
        Check(clamped.back() == "5", "the skill is kept in 1 .. 5");
        Check(WarpArguments("doom1.wad", shareware[1], -3).back() == "1", "and from below");
    }

    // a sound of DOOM as DMX has it: format 3, the rate, the count of samples (with 16 bytes of padding on both sides), the bytes unsigned
    std::vector<unsigned char> DmxLump(const std::uint16_t rate, const std::vector<unsigned char>& samples)
    {
        std::vector<unsigned char> lump = {3, 0, static_cast<unsigned char>(rate & 0xFF), static_cast<unsigned char>(rate >> 8)};
        const std::uint32_t count = static_cast<std::uint32_t>(samples.size()) + 32;
        for (int i = 0; i < 4; ++i)
        {
            lump.push_back(static_cast<unsigned char>((count >> (8 * i)) & 0xFF));
        }
        lump.insert(lump.end(), 16, 128);
        lump.insert(lump.end(), samples.begin(), samples.end());
        lump.insert(lump.end(), 16, 128);
        return lump;
    }

    void PutU32(std::string& out, const std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
        }
    }

    void TestMenuSounds(const fs::path& directory)
    {
        std::uint32_t rate = 0;
        const std::vector<unsigned char> lump = DmxLump(11025, {128, 255, 0, 192});
        const std::vector<std::int16_t> samples = DecodeDmxSound(lump, rate);
        Check(rate == 11025 && samples.size() == 4, "a DMX sound: the rate and the samples without the padding");
        Check(samples[0] == 0 && samples[1] == 127 * 256 && samples[2] == -128 * 256 && samples[3] == 64 * 256, "8 bit unsigned becomes 16 bit signed round zero");
        std::vector<unsigned char> wrongFormat = lump;
        wrongFormat[0] = 0;
        Check(DecodeDmxSound(wrongFormat, rate).empty(), "not format 3: nothing");
        Check(DecodeDmxSound({1, 2, 3}, rate).empty() && DecodeDmxSound({}, rate).empty(), "bytes that are too short: nothing, no crash");
        std::vector<unsigned char> cut = lump;
        cut.resize(cut.size() - 20);
        Check(DecodeDmxSound(cut, rate).empty(), "a lump that is shorter than its count says: nothing");

        // a WAD of three lumps: the directory is at the end
        const std::string name1 = "DSPSTOP";
        const std::string name2 = "PLAYPAL";
        const std::vector<unsigned char> palette(40, 7);
        std::string body;
        const std::uint32_t first = 12;
        body.append(reinterpret_cast<const char*>(lump.data()), lump.size());
        const std::uint32_t second = first + static_cast<std::uint32_t>(lump.size());
        body.append(reinterpret_cast<const char*>(palette.data()), palette.size());
        const std::uint32_t directoryAt = second + static_cast<std::uint32_t>(palette.size());
        std::string wad = "IWAD";
        PutU32(wad, 3);
        PutU32(wad, directoryAt);
        wad += body;
        const auto entry = [&](const std::uint32_t at, const std::uint32_t size, const std::string& name)
        {
            PutU32(wad, at);
            PutU32(wad, size);
            std::string padded = name;
            padded.resize(8, '\0');
            wad += padded;
        };
        entry(first, static_cast<std::uint32_t>(lump.size()), name1);
        entry(second, static_cast<std::uint32_t>(palette.size()), name2);
        entry(second, 0, "F_START");
        const fs::path file = directory / "tiny.wad";
        Write(file, "");
        {
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            stream.write(wad.data(), static_cast<std::streamsize>(wad.size()));
        }
        const auto lumps = ReadWadLumps(file, {"DSPSTOP", "DSOOF", "PLAYPAL", "F_START"});
        Check(lumps.size() == 2 && lumps[0].first == "DSPSTOP" && lumps[0].second == lump && lumps[1].first == "PLAYPAL" && lumps[1].second == palette,
              "the lumps that are there come out as they are; a name that is missing and a lump without bytes are left out");
        Check(ReadWadLumps(directory / "none.wad", {"DSPSTOP"}).empty(), "no file: nothing");
        Write(file, "NOTAWAD-----------------------");
        Check(ReadWadLumps(file, {"DSPSTOP"}).empty(), "a file that is no WAD: nothing, no crash");
    }

    void TestLabels()
    {
        Check(WadLabel("C:/x/freedoom1.wad", true) == "Freedoom: Phase 1" && WadLabel("freedoom2.wad", true) == "Freedoom: Phase 2", "Freedoom");
        Check(WadLabel("doom1.wad", false) == "DOOM (shareware, episode 1)" && WadLabel("DOOM.WAD", false) == "DOOM" && WadLabel("doom2.wad", false) == "DOOM 2", "DOOM");
        Check(WadLabel("plutonia.wad", false).find("Plutonia") != std::string::npos && WadLabel("tnt.wad", false).find("TNT") != std::string::npos, "Final DOOM");
        Check(WadLabel("C:/x/my_map.wad", false) == "my_map.wad" && WadLabel("C:/x/my_map.wad", true) == "Freedoom: my_map.wad", "a file we do not know is named by its file");
    }
}

int main()
{
    try
    {
        const fs::path directory = fs::temp_directory_path() / "myengine_doom_frontend_test";
        std::error_code ignored;
        fs::remove_all(directory, ignored);
        TestSettings(directory);
        TestLevels();
        TestLabels();
        TestMenuSounds(directory);
        fs::remove_all(directory, ignored);
        std::cout << "doom_frontend: settings, levels, labels and the sounds of the menu ok\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "doom_frontend FAILED: " << exception.what() << "\n";
        return 1;
    }
}
