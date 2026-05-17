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

namespace myengine::core
{
    class Logger;
}

namespace myengine::ui
{
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
        void Update(float deltaTime);
        void RenderWindow(core::WindowId windowId);

        void SetStateLabel(std::string stateLabel);
        bool WantsMouseCapture(core::WindowId windowId) const;
        bool WantsKeyboardCapture(core::WindowId windowId) const;
        bool CanStartSceneNavigation(core::WindowId windowId, float mouseX, float mouseY) const;

    private:
        struct WindowUiContext;

        std::unique_ptr<WindowUiContext> CreateWindowContext(core::WindowId windowId, HWND hwnd, render::RenderSurfaceHandle surface, std::uint32_t width, std::uint32_t height);
        void BuildWindowUi(WindowUiContext& windowContext);

        render::IRenderAdapter* renderAdapter_ = nullptr;
        core::Logger* logger_ = nullptr;
        std::string stateLabel_ = "Gameplay";
        bool initialized_ = false;
        std::unordered_map<core::WindowId, std::unique_ptr<WindowUiContext>> windows_;
        SceneEditor sceneEditor_;
    };
}