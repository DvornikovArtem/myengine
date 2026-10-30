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
        mouseSensitivity = std::clamp(mouseSensitivity, 0.25f, 3.0f);
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
        read("lastMode", lastMode);
        read("controlSet", controlSet);
        read("backend", backend);
        read("iwad", iwad);
        read("episode", episode);
        read("map", map);
        read("skill", skill);
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
        root["lastMode"] = lastMode;
        root["controlSet"] = controlSet;
        root["backend"] = backend;
        root["iwad"] = iwad;
        root["episode"] = episode;
        root["map"] = map;
        root["skill"] = skill;
        const std::filesystem::path temporary = file.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::trunc);
            stream << root.dump(2) << "\n";
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
