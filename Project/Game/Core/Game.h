#pragma once

#include <memory>

#include <Engine/Rendering/BaseSceneRenderer.h>
#include <Components/CameraComponent.h>
#include <Engine/Rendering/SceneRendererFactory.h>

#include "Core/Context.h"

namespace NLS::Game::Core
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

		std::unique_ptr<Engine::Rendering::BaseSceneRenderer> m_sceneRenderer;
	};
}
