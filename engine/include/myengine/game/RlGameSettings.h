// RlGameSettings.h
//
// The settings of the Rocket League game that the player changes in the menu (docs/game/rl-ui.md, "Игровой режим"): the video, the camera, the audio,
// the controls that are not bindings (invert pitch; the dead zone lives in rl_controls.json) and the last choices of the Play screen. One json file in the
// user folder (core::apppaths::UserDataDirectory()/game_settings.json): read at the start of the game, written when the player leaves Settings. A missing
// or broken file means the defaults; unknown keys are kept out of the way, missing ones keep their defaults, values are clamped to their ranges.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <rl/RlTuning.h>

namespace myengine::game
{
    constexpr int kFpsLimits[] = {0, 60, 120, 144, 240};  // 0 = unlimited; the order of the "Frame rate limit" row

    struct RlVideoSettings
    {
        int windowMode = 0;       // 0 Windowed, 1 Borderless, 2 Fullscreen (core::DisplayMode)
        int width = 1600;         // the client size of the window / the resolution of the fullscreen mode
        int height = 900;
        bool vsync = true;
        int fpsLimit = 0;         // frames per second, 0 unlimited
        int shadows = 2;          // 0 off, 1 low, 2 high
        int msaa = 4;             // 1 or 4
        bool bloom = true;
        int uiScalePercent = 100; // 75 .. 125
        bool showFps = false;
    };

    // The camera as the player sees it in the menu (centimetres like in Rocket League); ToTuning() turns it into the numbers of RlCameraUpdate
    struct RlCameraSettings
    {
        float fov = 110.0f;           // degrees (horizontal)
        float distanceCm = 270.0f;
        float heightCm = 100.0f;
        float angle = -3.0f;          // degrees
        float stiffness = 0.45f;
        float swivelSpeed = 4.7f;
        float transitionSpeed = 1.2f;
        bool ballCamHold = false;     // the ball cam is on while the button is held (false: it toggles)

        static RlCameraSettings FromTuning(const rl::RlCameraTuning& tuning);
        // `base` keeps the numbers the menu has no row for (up follow, wall margin ...)
        rl::RlCameraTuning ToTuning(const rl::RlCameraTuning& base) const;
    };

    struct RlAudioSettings
    {
        float master = 1.0f;
        float effects = 1.0f;
        float music = 0.6f;
    };

    // What the Play screen remembers
    struct RlPlaySettings
    {
        int mode = 2;                 // players per team
        int botLevel = 1;             // 0 Rookie, 1 Pro, 2 All-star
        int hostTeamSize = 2;
        bool hostFillBots = true;
        int hostPort = 27015;
        std::string joinAddress;      // the last typed address
        std::vector<std::string> recent;  // the addresses that worked, the newest first, at most 5
        std::string playerName;       // empty: the name of the Windows user
    };

    struct RlGameSettings
    {
        RlVideoSettings video;
        RlCameraSettings camera;
        RlAudioSettings audio;
        RlPlaySettings play;
        bool invertPitch = false;
        // The car of the garage (assets/data/cars/<id>.json, docs/game/rl-cars.md): the key of the car the player drives; a key that is not in the data of
        // the game (the car was removed) is put back to "pulse" by the garage, not here
        std::string car = "pulse";

        // Returns false (the settings stay at the defaults) when the file exists but cannot be read; a missing file is fine. `error` says why.
        bool Load(const std::filesystem::path& file, std::string* error = nullptr);
        bool Save(const std::filesystem::path& file, std::string* error = nullptr) const;
        std::string ToJson() const;
        bool FromJson(const std::string& text, std::string* error = nullptr);
        void Clamp();

        // %LOCALAPPDATA%\RocketArena\game_settings.json (shipping) or %APPDATA%\myengine\game_settings.json
        static std::filesystem::path DefaultPath();
        // The name of the player for the table: PlayerName, else the user of Windows, else "Player"
        std::string PlayerName() const;
    };
}
