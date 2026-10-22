// RlUiHost.h
//
// The glue between the kit and the engine: the Host takes care of the fonts, the devices and the draw list, and runs the screen of the game
// every frame instead of the editor. A game (coder-2: --game, RlGameSystem) does three things:
//
//     rlui::Host host;                                           // lives as long as the application
//     host.Install(uiManager, {});                               // once, after UiManager::Initialize: fonts are baked into every window
//     host.SetScreen([&](rlui::Frame& f) { menu.Draw(f); });     // what to draw each frame; the HUD of a match is drawn here too
//
// UiManager::SetGameUi (added with the kit) replaces the editor with this callback; without Install the editor is shown as before.
// The Host reads the keyboard, the mouse and the typed text of the window and XInput (the first pad) into RawInput, builds the Frame and calls the
// screen between Begin and End. It does not draw a background: the 3D scene of the game is behind the UI; a screen without a scene
// (the gallery) calls paint::MenuBackdrop itself.

#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <myengine/ui/rl/RlUiFrame.h>

namespace myengine::ui
{
    class UiManager;
}

namespace myengine::rlui
{
    struct HostConfig
    {
        std::filesystem::path fontDirectory;   // empty: the usual search (next to the exe, then the sources): assets/fonts
        float uiScale = 1.0f;                  // the setting
    };

    class Host
    {
    public:
        Host();
        ~Host();
        Host(const Host&) = delete;
        Host& operator=(const Host&) = delete;

        // Hooks the kit into the UiManager: the fonts are baked into the atlas of every window (the existing ones are rebuilt), the screen
        // runs instead of the editor. Calling it again changes the config.
        void Install(ui::UiManager& manager, const HostConfig& config);
        // The editor comes back
        void Uninstall();

        // What is drawn each frame (set it to another function to change the screen; ResetFocus when it changes)
        void SetScreen(std::function<void(Frame&)> screen);
        // The UI scale of the settings (0.75 .. 1.25): the fonts are baked again before the next frame
        void SetUiScale(float scale);
        float UiScale() const { return uiScale_; }

        // The last frame (the screens read the metrics, the last device and the focus through it between frames, e.g. to pick the hints of
        // a screen). Valid after the first frame.
        Frame& frame() { return frame_; }

        // Why the fonts could not be baked ("" when all is well): the kit then draws with the default font of ImGui
        const std::string& FontError() const { return fontError_; }

        // The state of the devices as the Host sees it (the game may want it for its own input: the pad, the keys): the last frame
        const RawInput& LastRawInput() const { return lastRaw_; }

    private:
        struct Impl;
        Impl* impl_ = nullptr;
        Frame frame_;
        RawInput lastRaw_;
        float uiScale_ = 1.0f;
        std::string fontError_;
        std::function<void(Frame&)> screen_;
    };
}
