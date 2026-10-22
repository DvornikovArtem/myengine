// RlGameSettings.cpp

#include <myengine/game/RlGameSettings.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include <myengine/core/AppPaths.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::game
{
    namespace
    {
        using nlohmann::json;

        template <class T>
        void Read(const json& object, const char* key, T& value)
        {
            const auto it = object.find(key);
            if (it == object.end() || it->is_null())
            {
                return;
            }
            try
            {
                value = it->get<T>();
            }
            catch (const json::exception&)
            {
                // a value of the wrong kind keeps the default
            }
        }

        // At most `bytes` bytes, never cutting a character in the middle (the byte after the cut must not be a continuation byte)
        std::string TruncateUtf8(std::string text, const std::size_t bytes)
        {
            if (text.size() <= bytes)
            {
                return text;
            }
            std::size_t length = bytes;
            while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
            {
                --length;
            }
            text.resize(length);
            return text;
        }

        std::string Utf8FromWide(const std::wstring& text)
        {
            if (text.empty())
            {
                return {};
            }
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
            if (bytes > 0)
            {
                WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr);
            }
            return result;
        }
    }

    RlCameraSettings RlCameraSettings::FromTuning(const rl::RlCameraTuning& tuning)
    {
        RlCameraSettings settings;
        settings.fov = tuning.fov;
        settings.distanceCm = tuning.distance * 100.0f;
        settings.heightCm = tuning.height * 100.0f;
        settings.angle = tuning.pitchDegrees;
        settings.stiffness = tuning.stiffness;
        settings.swivelSpeed = tuning.swivelSpeed;
        settings.transitionSpeed = tuning.transitionSpeed;
        return settings;
    }

    rl::RlCameraTuning RlCameraSettings::ToTuning(const rl::RlCameraTuning& base) const
    {
        rl::RlCameraTuning tuning = base;
        tuning.fov = fov;
        tuning.distance = distanceCm * 0.01f;
        tuning.height = heightCm * 0.01f;
        tuning.pitchDegrees = angle;
        tuning.stiffness = stiffness;
        tuning.swivelSpeed = swivelSpeed;
        tuning.transitionSpeed = transitionSpeed;
        return tuning;
    }

    void RlGameSettings::Clamp()
    {
        video.windowMode = std::clamp(video.windowMode, 0, 2);
        video.width = std::clamp(video.width, 640, 7680);
        video.height = std::clamp(video.height, 360, 4320);
        video.fpsLimit = std::clamp(video.fpsLimit, 0, 1000);
        video.shadows = std::clamp(video.shadows, 0, 2);
        video.msaa = video.msaa >= 4 ? 4 : 1;
        video.uiScalePercent = std::clamp(video.uiScalePercent, 75, 125);

        camera.fov = std::clamp(camera.fov, 60.0f, 110.0f);
        camera.distanceCm = std::clamp(camera.distanceCm, 100.0f, 400.0f);
        camera.heightCm = std::clamp(camera.heightCm, 40.0f, 200.0f);
        camera.angle = std::clamp(camera.angle, -15.0f, 0.0f);
        camera.stiffness = std::clamp(camera.stiffness, 0.0f, 1.0f);
        camera.swivelSpeed = std::clamp(camera.swivelSpeed, 1.0f, 10.0f);
        camera.transitionSpeed = std::clamp(camera.transitionSpeed, 1.0f, 2.0f);

        audio.master = std::clamp(audio.master, 0.0f, 1.0f);
        audio.effects = std::clamp(audio.effects, 0.0f, 1.0f);
        audio.music = std::clamp(audio.music, 0.0f, 1.0f);

        play.mode = std::clamp(play.mode, 1, 4);
        play.botLevel = std::clamp(play.botLevel, 0, 2);
        play.hostTeamSize = std::clamp(play.hostTeamSize, 1, 4);
        play.hostPort = std::clamp(play.hostPort, 1, 65535);
        if (play.recent.size() > 5)
        {
            play.recent.resize(5);
        }
        play.playerName = TruncateUtf8(play.playerName, 15);
        play.joinAddress = TruncateUtf8(play.joinAddress, 40);
        // a key of a car is lower case letters, digits and "_" / "-": anything else is not a car of ours
        std::string key;
        for (const char c : car)
        {
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            {
                key += c;
            }
        }
        car = key.empty() ? std::string("pulse") : key.substr(0, 32);
    }

    std::string RlGameSettings::ToJson() const
    {
        json root;
        root["version"] = 1;
        json v;
        v["windowMode"] = video.windowMode;
        v["width"] = video.width;
        v["height"] = video.height;
        v["vsync"] = video.vsync;
        v["fpsLimit"] = video.fpsLimit;
        v["shadows"] = video.shadows;
        v["msaa"] = video.msaa;
        v["bloom"] = video.bloom;
        v["uiScalePercent"] = video.uiScalePercent;
        v["showFps"] = video.showFps;
        root["video"] = v;
        json c;
        c["fov"] = camera.fov;
        c["distanceCm"] = camera.distanceCm;
        c["heightCm"] = camera.heightCm;
        c["angle"] = camera.angle;
        c["stiffness"] = camera.stiffness;
        c["swivelSpeed"] = camera.swivelSpeed;
        c["transitionSpeed"] = camera.transitionSpeed;
        c["ballCamHold"] = camera.ballCamHold;
        root["camera"] = c;
        json a;
        a["master"] = audio.master;
        a["effects"] = audio.effects;
        a["music"] = audio.music;
        root["audio"] = a;
        json p;
        p["mode"] = play.mode;
        p["botLevel"] = play.botLevel;
        p["hostTeamSize"] = play.hostTeamSize;
        p["hostFillBots"] = play.hostFillBots;
        p["hostPort"] = play.hostPort;
        p["joinAddress"] = play.joinAddress;
        p["recent"] = play.recent;
        p["playerName"] = play.playerName;
        root["play"] = p;
        root["invertPitch"] = invertPitch;
        root["car"] = car;
        return root.dump(2) + "\n";
    }

    bool RlGameSettings::FromJson(const std::string& text, std::string* error)
    {
        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            if (error != nullptr)
            {
                *error = "game_settings.json is not a json object";
            }
            return false;
        }
        RlGameSettings loaded;
        if (const auto it = root.find("video"); it != root.end() && it->is_object())
        {
            Read(*it, "windowMode", loaded.video.windowMode);
            Read(*it, "width", loaded.video.width);
            Read(*it, "height", loaded.video.height);
            Read(*it, "vsync", loaded.video.vsync);
            Read(*it, "fpsLimit", loaded.video.fpsLimit);
            Read(*it, "shadows", loaded.video.shadows);
            Read(*it, "msaa", loaded.video.msaa);
            Read(*it, "bloom", loaded.video.bloom);
            Read(*it, "uiScalePercent", loaded.video.uiScalePercent);
            Read(*it, "showFps", loaded.video.showFps);
        }
        if (const auto it = root.find("camera"); it != root.end() && it->is_object())
        {
            Read(*it, "fov", loaded.camera.fov);
            Read(*it, "distanceCm", loaded.camera.distanceCm);
            Read(*it, "heightCm", loaded.camera.heightCm);
            Read(*it, "angle", loaded.camera.angle);
            Read(*it, "stiffness", loaded.camera.stiffness);
            Read(*it, "swivelSpeed", loaded.camera.swivelSpeed);
            Read(*it, "transitionSpeed", loaded.camera.transitionSpeed);
            Read(*it, "ballCamHold", loaded.camera.ballCamHold);
        }
        if (const auto it = root.find("audio"); it != root.end() && it->is_object())
        {
            Read(*it, "master", loaded.audio.master);
            Read(*it, "effects", loaded.audio.effects);
            Read(*it, "music", loaded.audio.music);
        }
        if (const auto it = root.find("play"); it != root.end() && it->is_object())
        {
            Read(*it, "mode", loaded.play.mode);
            Read(*it, "botLevel", loaded.play.botLevel);
            Read(*it, "hostTeamSize", loaded.play.hostTeamSize);
            Read(*it, "hostFillBots", loaded.play.hostFillBots);
            Read(*it, "hostPort", loaded.play.hostPort);
            Read(*it, "joinAddress", loaded.play.joinAddress);
            Read(*it, "recent", loaded.play.recent);
            Read(*it, "playerName", loaded.play.playerName);
        }
        Read(root, "invertPitch", loaded.invertPitch);
        Read(root, "car", loaded.car);
        loaded.Clamp();
        *this = loaded;
        return true;
    }

    bool RlGameSettings::Load(const std::filesystem::path& file, std::string* error)
    {
        std::error_code ec;
        if (!std::filesystem::exists(file, ec))
        {
            return true;  // the first start: the defaults
        }
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            if (error != nullptr)
            {
                *error = "cannot read " + file.u8string();
            }
            return false;
        }
        std::stringstream buffer;
        buffer << stream.rdbuf();
        std::string text = buffer.str();
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        {
            text.erase(0, 3);
        }
        return FromJson(text, error);
    }

    bool RlGameSettings::Save(const std::filesystem::path& file, std::string* error) const
    {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        // written beside and renamed, so that a crash in the middle never leaves a half file
        const std::filesystem::path temporary = file.string() + ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            const std::string text = ToJson();
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!stream)
            {
                if (error != nullptr)
                {
                    *error = "cannot write " + file.u8string();
                }
                return false;
            }
        }
        std::filesystem::rename(temporary, file, ec);
        if (ec)
        {
            std::filesystem::remove(file, ec);
            std::filesystem::rename(temporary, file, ec);
        }
        if (ec && error != nullptr)
        {
            *error = "cannot replace " + file.u8string() + ": " + ec.message();
        }
        return !ec;
    }

    std::filesystem::path RlGameSettings::DefaultPath()
    {
        return core::apppaths::UserDataDirectory() / "game_settings.json";
    }

    std::string RlGameSettings::PlayerName() const
    {
        if (!play.playerName.empty())
        {
            return play.playerName;
        }
        wchar_t buffer[64]{};
        DWORD length = static_cast<DWORD>(std::size(buffer));
        if (GetUserNameW(buffer, &length) != FALSE && length > 1)
        {
            const std::string name = TruncateUtf8(Utf8FromWide(std::wstring(buffer, length - 1)), 15);
            if (!name.empty())
            {
                return name;
            }
        }
        return "Player";
    }
}
