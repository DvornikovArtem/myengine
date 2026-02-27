// GameplayState.cpp

#include <states/GameplayState.h>
#include <states/MenuState.h>

#include <myengine/core/Application.h>

// If the WIN32_LEAN_AND_MEAN macro is defined before including windows.h, rarely used parts are excluded from the header to speed up compilation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::appstate
{
    const char* GameplayState::Name() const
    {
        return "GameplayState";
    }

    void GameplayState::OnEnter(core::Application& app)
    {
        app.SetStateLabel("Gameplay");
        app.GetLogger().Info("Gameplay state: arrows move primitive, LMB scales, RMB rotates, Esc returns to menu.");
    }

    void GameplayState::OnExit(core::Application& app)
    {
        app.GetLogger().Info("Leaving gameplay state");
    }

    void GameplayState::Update(core::Application& app, const float deltaTime)
    {
        // The lines below suppress the compiler warning: yes, the parameter exists, but we are not using it yet
        static_cast<void>(app);
        static_cast<void>(deltaTime);
    }

    void GameplayState::Render(core::Application& app)
    {
        // The lines below suppress the compiler warning: yes, the parameter exists, but we are not using it yet
        static_cast<void>(app);
    }

    void GameplayState::HandleEvent(core::Application& app, const core::InputEvent& event)
    {
        if (event.type == core::InputEventType::KeyDown && event.key == VK_ESCAPE)
        {
            app.GetLogger().Info("GameplayState: Esc pressed -> MenuState");
            app.GetStateMachine().ChangeState(std::make_unique<MenuState>(), app);
        }
    }
}