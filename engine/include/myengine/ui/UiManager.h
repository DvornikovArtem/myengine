#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <myengine/core/Types.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/ui/SceneEditor.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct ImFontAtlas;

namespace myengine::core
{
    class Logger;
}

namespace myengine::ui
{
    // The game UI (the UI kit of the Rocket League game, ui/rl/RlUiHost.h) takes the place of the editor: `bakeFonts` adds its fonts to the atlas
    // of a window before the atlas is built, `build` draws the UI of a window every frame (after ImGui::NewFrame, on the foreground draw list).
    struct GameUiHooks
    {
        std::function<void(ImFontAtlas& atlas, std::uint32_t width, std::uint32_t height)> bakeFonts;
        std::function<void(core::WindowId windowId, std::uint32_t width, std::uint32_t height, float deltaTime)> build;
    };

    class UiManager
    {
    public:
        UiManager();
        ~UiManager();

        bool Initialize(render::IRenderAdapter& renderAdapter, core::Logger& logger, SceneEditorServices services);
        void Shutdown();

        bool RegisterWindow(core::WindowId windowId, HWND hwnd, render::RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height);
        void UnregisterWindow(core::WindowId windowId);

        void HandleWindowMessage(core::WindowId windowId, HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
        // The held mouse buttons are released at the current position (the capture was lost, the window lost the focus)
        void ReleaseMouseButtons(core::WindowId windowId);
        void Update(float deltaTime);
        void RenderWindow(core::WindowId windowId);

        // A game UI instead of the editor (empty `build`: the editor again). The atlas of every open window is rebuilt with the new fonts.
        void SetGameUi(GameUiHooks hooks);
        bool GameUiActive() const { return static_cast<bool>(gameUi_.build); }
        // The game UI wants the mouse and the keyboard (a menu is open): WantsMouseCapture / WantsKeyboardCapture say yes while it is on
        void SetGameUiCapturesInput(bool captures) { gameUiCaptures_ = captures; }
        // The fonts have to be baked again (the window got another size class, the UI scale changed): done at the start of the next Update
        void RequestFontRebuild() { fontRebuild_ = true; }

        void SetStateLabel(std::string stateLabel);
        bool WantsMouseCapture(core::WindowId windowId) const;
        bool WantsKeyboardCapture(core::WindowId windowId) const;
        bool WantsTextInput(core::WindowId windowId) const; // a text field is being edited (not just an ImGui item held by the mouse)
        bool CanStartSceneNavigation(core::WindowId windowId, float mouseX, float mouseY) const;

    private:
        struct WindowUiContext;

        std::unique_ptr<WindowUiContext> CreateWindowContext(core::WindowId windowId, HWND hwnd, render::RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height);
        void BuildWindowUi(WindowUiContext& windowContext);

        void RebuildWindowContexts();

        GameUiHooks gameUi_;
        bool gameUiCaptures_ = false;
        bool fontRebuild_ = false;
        render::IRenderAdapter* renderAdapter_ = nullptr;
        core::Logger* logger_ = nullptr;
        std::string stateLabel_ = "Gameplay";
        bool initialized_ = false;
        std::unordered_map<core::WindowId, std::unique_ptr<WindowUiContext>> windows_;
        SceneEditor sceneEditor_;
    };
}