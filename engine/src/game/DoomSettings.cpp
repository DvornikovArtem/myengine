// DoomSettings.cpp
//
// The settings of FlyDoom in a JSON file (%LOCALAPPDATA%\FlyDoom\settings.json). A value of the wrong kind or a missing one keeps the default; the numbers are clamped to
// what the menu offers, so a file that was edited by hand cannot make the game unplayable.

#include <myengine/game/DoomFrontEnd.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace myengine::game
{
    void DoomSettings::Clamp()
    {
        sfxVolume = std::clamp(sfxVolume, 0.0f, 1.0f);
        musicVolume = std::clamp(musicVolume, 0.0f, 1.0f);
        mouseSensitivity = std::clamp(mouseSensitivity, 0.25f, 4.0f);
        menuMotion = std::clamp(menuMotion, 0, 1);
        flies = std::clamp(flies, 8, 32);
        learnSpeed = learnSpeed == 1 ? 1 : 0;   // 1 real time, anything else MAX (the old file had x2 .. x8: that is MAX now)
        if (learnStart != "scratch" && learnStart != "trained")
        {
            learnStart = "scratch";
        }
        if (flyBrain != "trained" && flyBrain != "scratch")
        {
            flyBrain = "trained";
        }
        windowMode = std::clamp(windowMode, 0, 2);
        uiScalePercent = std::clamp(uiScalePercent, 75, 125);
        episode = std::clamp(episode, 0, 9);
        map = std::clamp(map, 1, 32);
        skill = std::clamp(skill, 1, 5);
        if (controlSet != "doomfly" && controlSet != "bio")
        {
            controlSet = "doomfly";
        }
        if (backend != "auto" && backend != "gpu" && backend != "cpu")
        {
            backend = "auto";
        }
        if (lastMode.empty() || lastMode.size() > 32)
        {
            lastMode = "human";
        }
    }

    bool DoomSettings::Load(const std::filesystem::path& file)
    {
        std::ifstream stream(file);
        if (!stream)
        {
            return false;
        }
        const nlohmann::json root = nlohmann::json::parse(stream, nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            return false;
        }
        const auto read = [&](const char* key, auto& value)
        {
            const auto it = root.find(key);
            if (it != root.end())
            {
                try
                {
                    value = it->get<std::decay_t<decltype(value)>>();
                }
                catch (const nlohmann::json::exception&)
                {
                    // a value of the wrong kind keeps the default
                }
            }
        };
        read("sfxVolume", sfxVolume);
        read("musicVolume", musicVolume);
        read("musicOn", musicOn);
        read("mouseSensitivity", mouseSensitivity);
        read("windowMode", windowMode);
        read("uiScalePercent", uiScalePercent);
        read("brainBackdrop", brainBackdrop);
        read("menuMotion", menuMotion);
        read("lastMode", lastMode);
        read("controlSet", controlSet);
        read("backend", backend);
        read("iwad", iwad);
        read("episode", episode);
        read("map", map);
        read("skill", skill);
        read("flies", flies);
        read("learnSpeed", learnSpeed);
        read("learnStart", learnStart);
        read("flyBrain", flyBrain);
        Clamp();
        return true;
    }

    bool DoomSettings::Save(const std::filesystem::path& file) const
    {
        std::error_code ec;
        if (!file.parent_path().empty())
        {
            std::filesystem::create_directories(file.parent_path(), ec);
        }
        nlohmann::json root;
        root["sfxVolume"] = sfxVolume;
        root["musicVolume"] = musicVolume;
        root["musicOn"] = musicOn;
        root["mouseSensitivity"] = mouseSensitivity;
        root["windowMode"] = windowMode;
        root["uiScalePercent"] = uiScalePercent;
        root["brainBackdrop"] = brainBackdrop;
        root["menuMotion"] = menuMotion;
        root["lastMode"] = lastMode;
        root["controlSet"] = controlSet;
        root["backend"] = backend;
        root["iwad"] = iwad;
        root["episode"] = episode;
        root["map"] = map;
        root["skill"] = skill;
        root["flies"] = flies;
        root["learnSpeed"] = learnSpeed;
        root["learnStart"] = learnStart;
        root["flyBrain"] = flyBrain;
        const std::filesystem::path temporary = file.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::trunc);
            // a text that is not UTF-8 (a path in the code page of Windows) must not stop the game: its bad bytes become U+FFFD
            stream << root.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
            if (!stream)
            {
                return false;
            }
        }
        std::filesystem::rename(temporary, file, ec);
        if (ec)
        {
            std::filesystem::remove(file, ec);
            ec.clear();
            std::filesystem::rename(temporary, file, ec);
        }
        return !ec;
    }
}
