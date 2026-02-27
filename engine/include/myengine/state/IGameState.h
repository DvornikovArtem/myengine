// IGameState.h

#pragma once

namespace myengine::core
{
	class Application;
	struct InputEvent;
}

namespace myengine::state
{
	class IGameState
	{
	public:
		virtual ~IGameState() = default;

		/// <summary>
		/// Returns the state name
		/// </summary>
		virtual const char* Name() const = 0;

		/// <summary>
		/// Called upon entering the state
		/// </summary>
		virtual void OnEnter(core::Application& app) = 0;
		/// <summary>
		/// Called upon exiting the state
		/// </summary>
		virtual void OnExit(core::Application& app) = 0;

		/// <summary>
		/// Per-frame update logic
		/// </summary>
		virtual void Update(core::Application& app, float deltaTime) = 0;
		/// <summary>
		/// State rendering
		/// </summary>
		virtual void Render(core::Application& app) = 0;
		/// <summary>
		/// Input handling
		/// </summary>
		virtual void HandleEvent(core::Application& app, const core::InputEvent& event) = 0;
	};
}