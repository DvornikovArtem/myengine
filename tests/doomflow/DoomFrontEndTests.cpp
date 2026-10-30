// DoomFrontEndTests.cpp
// V1 (FlyDoom): what the menu of FlyDoom does without a window: the settings in their file (a round trip, a file that was edited by hand or is broken, the limits), the levels
// of a WAD by its name, the arguments of DOOM that start a level, the label of a WAD.

#include <myengine/game/DoomFrontEnd.h>
#include <myengine/game/DoomLevels.h>

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
        Check(settings.Save(file) && fs::exists(file), "the settings are written");
        DoomSettings back;
        Check(back.Load(file), "and read");
        Check(back.sfxVolume == 0.35f && back.musicVolume == 0.0f && !back.musicOn && back.mouseSensitivity == 1.75f && back.windowMode == 2 && back.uiScalePercent == 110 && !back.brainBackdrop,
              "the numbers and the switches come back");
        Check(back.lastMode == "fly" && back.controlSet == "bio" && back.backend == "gpu" && back.iwad == settings.iwad && back.episode == 2 && back.map == 5 && back.skill == 4,
              "the choices of the menu come back (also a path with a backslash)");

        // edited by hand: values out of the range are brought into it, a value of the wrong kind keeps the default, an unknown key is ignored
        Write(file, R"({"sfxVolume": 7.5, "musicVolume": -2, "mouseSensitivity": 0.0, "windowMode": 9, "uiScalePercent": 500, "skill": 0, "episode": 99, "map": 0,
                        "controlSet": "elephant", "backend": "quantum", "lastMode": "", "musicOn": "yes", "windowTitle": "x"})");
        DoomSettings edited;
        Check(edited.Load(file), "a file edited by hand is read");
        Check(edited.sfxVolume == 1.0f && edited.musicVolume == 0.0f && edited.mouseSensitivity == 0.25f && edited.windowMode == 2 && edited.uiScalePercent == 125, "numbers go into their range");
        Check(edited.skill == 1 && edited.episode == 9 && edited.map == 1, "the level and the skill too");
        Check(edited.controlSet == "doomfly" && edited.backend == "auto" && edited.lastMode == "human", "a name that the game does not know is the default");
        Check(edited.musicOn, "a text where a switch is expected keeps the default");

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
        fs::remove_all(directory, ignored);
        std::cout << "doom_frontend: settings, levels and labels ok\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "doom_frontend FAILED: " << exception.what() << "\n";
        return 1;
    }
}
