#pragma once

#include <chrono>

#include "Panels/AViewControllable.h"
#include "Core/GizmoBehaviour.h"

namespace NLS::Editor::Panels
{
	class SceneView : public Editor::Panels::AViewControllable
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		SceneView(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);

		/**
		* Update the scene view
		*/
		virtual void Update(float p_deltaTime) override;

		/**
		* Prepare the renderer for rendering
		*/
		virtual void InitFrame() override;

		/**
		* Returns the scene used by this view
		*/
		virtual Engine::SceneSystem::Scene* GetScene();

	protected:
        virtual Engine::Rendering::BaseSceneRenderer::SceneDescriptor CreateSceneDescriptor() override;

	private:
		virtual void DrawFrame() override;
		virtual void AfterRenderFrame() override;
		void HandleActorPicking();

	private:
        Engine::SceneSystem::SceneManager& m_sceneManager;
		Render::Buffers::Framebuffer m_actorPickingFramebuffer;
		Editor::Core::GizmoBehaviour m_gizmoOperations;
		Editor::Core::EGizmoOperation m_currentOperation = Editor::Core::EGizmoOperation::TRANSLATE;
        Render::Resources::Material m_fallbackMaterial;

		Engine::GameObject* m_highlightedActor;
		std::optional<Editor::Core::GizmoBehaviour::EDirection> m_highlightedGizmoDirection;
		Maths::Vector2 m_lastPickingMousePos { -10000.0f, -10000.0f };
		std::chrono::steady_clock::time_point m_lastPickingSampleTime {};
		bool m_hasPickingSample = false;
	};
}
