#pragma once

#include <memory>

#include <Engine/Rendering/BaseSceneRenderer.h>
#include <Components/CameraComponent.h>
#include <Engine/Rendering/SceneRendererFactory.h>
#include <Rendering/Buffers/Framebuffer.h>
#include <Engine/UI/UIRuntimeService.h>
#include <Rendering/UI/UIDrawSnapshot.h>
#include <Rendering/UI/UIFontAtlasUploader.h>

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

		[[nodiscard]] std::shared_ptr<const NLS::Render::UI::UIDrawSnapshot> GetLatestUISnapshot() const
		{
			return m_uiSnapshot;
		}

	private:
		Context& m_context;

		std::unique_ptr<Engine::Rendering::BaseSceneRenderer> m_sceneRenderer;
		std::unique_ptr<Render::Buffers::Framebuffer> m_materialValidationFramebuffer;
		std::shared_ptr<Render::RHI::RHITexture> m_pendingMaterialValidationReadbackTexture;
		std::optional<Launch::MaterialValidationLaunchSettings> m_materialValidation;
		uint32_t m_presentedFrames = 0u;
		bool m_materialValidationCaptured = false;
		float m_fixedAccumulator = 0.0f;
		double m_time = 0.0;
		double m_unscaledTime = 0.0;
		uint64_t m_frameIndex = 0;
		uint64_t m_fixedFrameIndex = 0;
		Engine::UI::UIRuntimeService m_uiRuntimeService;
		std::shared_ptr<const NLS::Render::UI::UIDrawSnapshot> m_uiSnapshot;
		// Keeps the engine font atlas resident as a UI texture so retained text
		// has pixels to sample.
		NLS::Render::UI::UIFontAtlasUploader m_uiFontAtlasUploader;
	};
}
