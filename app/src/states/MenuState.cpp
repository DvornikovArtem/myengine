// MenuState.cpp

#include <states/MenuState.h>
#include <states/GameplayState.h>

#include <myengine/core/Application.h>

// If the WIN32_LEAN_AND_MEAN macro is defined before including windows.h, rarely used parts are excluded from the header to speed up compilation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::appstate
{
    const char* MenuState::Name() const
    {
        return "MenuState";
    }

    void MenuState::OnEnter(core::Application& app)
    {
        app.SetStateLabel("Menu");
        app.GetLogger().Info("Menu state: press Enter to start gameplay, Esc to exit.");
    }

    void MenuState::OnExit(core::Application& app)
    {
        app.GetLogger().Info("Leaving menu state");
    }

    void MenuState::Update(core::Application& app, const float deltaTime)
    {
        // The lines below suppress the compiler warning: yes, the parameter exists, but we are not using it yet
        static_cast<void>(app);
        static_cast<void>(deltaTime);
    }

    void MenuState::Render(core::Application& app)
    {
        // The lines below suppress the compiler warning: yes, the parameter exists, but we are not using it yet
        static_cast<void>(app);
    }

    void MenuState::HandleEvent(core::Application& app, const core::InputEvent& event)
    {
        if (event.type != core::InputEventType::KeyDown)
        {
            return;
        }

        if (event.key == VK_RETURN)
        {
            app.GetLogger().Info("MenuState: Enter pressed -> GameplayState");
            app.GetStateMachine().ChangeState(std::make_unique<GameplayState>(), app);
            return;
        }

        if (event.key == VK_ESCAPE)
        {
            app.GetLogger().Info("MenuState: Esc pressed -> RequestQuit");
            app.RequestQuit();
            PostQuitMessage(0);
        }
    }
}