// LoadingState.cpp

#include <states/LoadingState.h>
#include <states/MenuState.h>

#include <myengine/core/Application.h>

namespace myengine::appstate
{
    const char* LoadingState::Name() const
    {
        return "LoadingState";
    }

    void LoadingState::OnEnter(core::Application& app)
    {
        elapsed_ = 0.0f;
        app.SetStateLabel("Loading");
        app.GetLogger().Info("Loading state: simulating short bootstrap step");
    }

    void LoadingState::OnExit(core::Application& app)
    {
        app.GetLogger().Info("Loading state completed");
    }

    void LoadingState::Update(core::Application& app, const float deltaTime)
    {
        elapsed_ += deltaTime;
        if (elapsed_ > 0.5f)
        {
            app.GetStateMachine().ChangeState(std::make_unique<MenuState>(), app);
        }
    }

    void LoadingState::Render(core::Application& app)
    {
        // The lines below suppress the compiler warning: yes, the parameter exists, but we are not using it yet
        static_cast<void>(app);
    }

    void LoadingState::HandleEvent(core::Application& app, const core::InputEvent& event)
    {
        // The lines below suppress the compiler warning: yes, the parameter exists, but we are not using it yet
        static_cast<void>(app);
        static_cast<void>(event);
    }
}