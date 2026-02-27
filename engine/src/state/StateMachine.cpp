// StateMachine.cpp

#include <myengine/state/StateMachine.h>
#include <myengine/core/Application.h>

namespace myengine::state
{
    void StateMachine::ChangeState(std::unique_ptr<IGameState> nextState, core::Application& app)
    {
        if (currentState_)
        {
            app.GetLogger().Info(std::string("State exit: ") + currentState_->Name());
            currentState_->OnExit(app);
        }

        currentState_ = std::move(nextState);

        if (currentState_)
        {
            app.GetLogger().Info(std::string("State enter: ") + currentState_->Name());
            currentState_->OnEnter(app);
        }
    }

    void StateMachine::Update(core::Application& app, const float deltaTime)
    {
        if (currentState_)
        {
            currentState_->Update(app, deltaTime);
        }
    }

    void StateMachine::Render(core::Application& app)
    {
        if (currentState_)
        {
            currentState_->Render(app);
        }
    }

    void StateMachine::HandleEvent(core::Application& app, const core::InputEvent& event)
    {
        if (currentState_)
        {
            currentState_->HandleEvent(app, event);
        }
    }

    const IGameState* StateMachine::CurrentState() const
    {
        return currentState_.get();
    }
}