// StateMachine.h

#pragma once

#include <memory>

#include <myengine/state/IGameState.h>

namespace myengine::core
{
	class Application;
	struct InputEvent;
}

namespace myengine::state
{
	class StateMachine
	{
	public:
		/// <summary>
		/// State change
		/// </summary>
		void ChangeState(std::unique_ptr<IGameState> nextState, core::Application& app);

		void Update(core::Application& app, float deltaTime);
		void Render(core::Application& app);
		void HandleEvent(core::Application& app, const core::InputEvent& event);

		const IGameState* CurrentState() const;

	private:
		std::unique_ptr<IGameState> currentState_;
	};
}