// RlGameFlow.h
//
// The screens of the Rocket League game and the way between them (docs/game/rl-ui.md, "Игровой режим"; the design is .team/design/rl-ui-spec.md):
//
//   Boot -> Main menu -> Play (offline with bots / private match) -> Loading -> Match (the HUD) <-> Pause -> Results -> Main menu
//                     -> Training ----------^                          Settings (Controls / Camera / Video / Audio) from the menu and the pause
//
// The flow draws with the UI kit (ui/rl) and gives the commands to RlGameSystem, which owns the match and the 3D scene behind the screens (the
// podium of the menu, the arena). It owns the settings of the player (RlGameSettings, game_settings.json): it reads them at the start, applies them
// (the window, the quality, the camera, the volumes) and writes them when the player leaves Settings.
//
//     game::RlGameFlowServices services;  ... fill ...
//     game::RlGameFlow flow(services);
//     flow.Install();                      // after UiManager::Initialize and before RegisterWindow: the fonts are baked into every window

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <myengine/game/RlGameSettings.h>
#include <myengine/ui/rl/RlUiInput.h>

namespace myengine::ui
{
    class UiManager;
}

namespace myengine::ecs::systems
{
    class RlGameSystem;
}

namespace myengine::game
{
    struct RlGameFlowServices
    {
        ecs::systems::RlGameSystem* system = nullptr;
        ui::UiManager* ui = nullptr;
        std::function<void()> quit;                                                              // Exit
        std::function<void(const RlVideoSettings&)> applyVideo;                                  // the window, vsync, the limit of frames, the quality
        std::function<std::vector<std::pair<std::uint32_t, std::uint32_t>>()> resolutions;       // what the display can be set to
        std::function<std::pair<std::uint32_t, std::uint32_t>()> windowSize;                     // the window now
        std::function<bool()> sceneReady;                                                        // the map is loaded: the boot screen ends
        std::function<float()> loadingProgress;                                                  // 0..1 of what the engine is loading, negative: unknown
        std::function<float()> fps;                                                              // frames per second (the overlay of the FPS)
        std::function<void(const std::string&)> log;                                              // one line of the log (the screens, the settings that were saved)
        std::function<void(bool menu)> setLighting;                                              // the dim light behind the menus / the light of the match
        std::filesystem::path settingsFile;                                                      // empty: RlGameSettings::DefaultPath()
    };

    class RlGameFlow
    {
    public:
        explicit RlGameFlow(const RlGameFlowServices& services);
        ~RlGameFlow();
        RlGameFlow(const RlGameFlow&) = delete;
        RlGameFlow& operator=(const RlGameFlow&) = delete;

        // Reads the settings, applies them and hooks the screens into the UiManager
        void Install();
        // Writes the settings (the player's choices of the Play screen included)
        void SaveSettings();
        const RlGameSettings& Settings() const;

        // The scripted input of the tests: a hook on the devices that the screens see (see Application, MYENGINE_UI_SCRIPT)
        void SetRawInputHook(std::function<void(rlui::RawInput&)> hook);

        // The screen that is shown, for the logs and the tests ("boot", "main", "play", "loading", "match", "pause", "results", "settings", "lobby")
        const char* ScreenName() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
