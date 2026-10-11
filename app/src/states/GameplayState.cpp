// GameplayState.cpp

#include <states/GameplayState.h>

#include <myengine/core/Application.h>

namespace myengine::appstate
{
    const char* GameplayState::Name() const
    {
        return "GameplayState";
    }

    void GameplayState::OnEnter(core::Application& app)
    {
        app.SetStateLabel("Gameplay");
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
        // Esc is the editor's: Play stops (SceneEditor::HandleKeyboardShortcuts), Edit ignores it. The state used to
        // switch to MenuState on Esc, and Esc in MenuState quit the application
        static_cast<void>(app);
        static_cast<void>(event);
    }
}