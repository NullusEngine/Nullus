#pragma once

#include <SceneSystem/SceneManager.h>

#include "Panels/AView.h"

namespace NLS::Editor::Panels
{
	class GameView : public Editor::Panels::AView
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		GameView(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);

		/**
		* Returns the main camera used by the attached scene
		*/
		virtual Render::Entities::Camera* GetCamera();

		/**
		* Returns the scene used by this view
		*/
		virtual Engine::SceneSystem::Scene* GetScene();

	private:
        Engine::SceneSystem::SceneManager& m_sceneManager;
	};
}