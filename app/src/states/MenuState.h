// MenuState.h

#pragma once

#include <myengine/state/IGameState.h>

namespace myengine::appstate
{
	class MenuState final : public state::IGameState
	{
	public:
		const char* Name() const override;

		void OnEnter(core::Application& app) override;
		void OnExit(core::Application& app) override;

		void Update(core::Application& app, float deltaTime) override;
		void Render(core::Application& app) override;
		void HandleEvent(core::Application& app, const core::InputEvent& event) override;
	};
}