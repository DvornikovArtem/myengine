// LoadingState.cpp

#include <states/LoadingState.h>
#include <states/GameplayState.h>

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
        frames_ = 0;
        app.SetStateLabel("Loading");
        app.GetLogger().Info("Loading state: waiting for the start map and its assets");
    }

    void LoadingState::OnExit(core::Application& app)
    {
        app.GetLogger().Info("Loading state completed");
    }

    void LoadingState::Update(core::Application& app, const float deltaTime)
    {
        elapsed_ += deltaTime;
        ++frames_;

        // Gameplay starts when the start map is applied and the assets it asked for have streamed in (the editor
        // shows the progress meanwhile); a file that never loads must not hold it forever
        const bool ready = app.IsSceneLoaded() && app.GetResourceManager().PendingLoadCount() == 0 && frames_ >= 2;
        if (ready || elapsed_ > 20.0f)
        {
            app.GetStateMachine().ChangeState(std::make_unique<GameplayState>(), app);
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
