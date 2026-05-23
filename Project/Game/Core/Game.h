#pragma once

#include <memory>

#include <Engine/Rendering/BaseSceneRenderer.h>
#include <Components/CameraComponent.h>
#include <Engine/Rendering/SceneRendererFactory.h>
#include <Rendering/Buffers/Framebuffer.h>

#include "Core/Context.h"
#include "LaunchArgs.h"

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
		Game(
			Context& p_context,
			std::optional<Launch::MaterialValidationLaunchSettings> materialValidation = std::nullopt);

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
		std::unique_ptr<Render::Buffers::Framebuffer> m_materialValidationFramebuffer;
		std::shared_ptr<Render::RHI::RHITexture> m_pendingMaterialValidationReadbackTexture;
		std::optional<Launch::MaterialValidationLaunchSettings> m_materialValidation;
		uint32_t m_presentedFrames = 0u;
		bool m_materialValidationCaptured = false;
	};
}
