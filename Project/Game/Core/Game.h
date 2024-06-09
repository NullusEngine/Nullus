#pragma once

#include <Components/CameraComponent.h>
#include <Engine/Rendering/SceneRenderer.h>

#include "Core/Context.h"

namespace Game::Core
{
	/**
	* Handle the game logic
	*/
	class Game
	{
	public:
		/**
		* Create the game
		* @param p_context
		*/
		Game(Context& p_context);

		/**
		* Destroy the game
		*/
		~Game();

		/**
		* Pre-update of the game logic
		*/
		void PreUpdate();

		/**
		* Update the game logic
		* @param p_deltaTime
		*/
		void Update(float p_deltaTime);

		/**
		* Post-update of the game logic
		*/
		void PostUpdate();

	private:
		Context& m_context;

		Engine::Rendering::SceneRenderer m_sceneRenderer;
	};
}