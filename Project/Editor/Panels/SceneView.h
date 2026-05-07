#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "Core/SceneCameraFocus.h"
#include "Panels/AViewControllable.h"
#include "Core/SceneViewImGuizmo.h"

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
		~SceneView();

		/**
		* Update the scene view
		*/
		virtual void Update(float p_deltaTime) override;

        Editor::Core::EGizmoOperation GetCurrentGizmoOperation() const;
        void SetCurrentGizmoOperation(Editor::Core::EGizmoOperation p_operation);
        Editor::Core::SceneViewGizmoPivot GetCurrentGizmoPivot() const;
        void SetCurrentGizmoPivot(Editor::Core::SceneViewGizmoPivot p_pivot);
        void ToggleCurrentGizmoPivot();
        Editor::Core::SceneViewGizmoSpace GetCurrentGizmoSpace() const;
        void SetCurrentGizmoSpace(Editor::Core::SceneViewGizmoSpace p_space);
        void ToggleCurrentGizmoSpace();

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
        bool RequiresSynchronizedRetiredFramePresentation() const override;
        void DrawPreRenderViewportOverlay() override;
        void OnAfterDrawWidgets() override;
        void DrawViewportOverlay() override;

	private:
		void HandleActorPicking();
        void EnsureCameraFocus();
        bool ShouldRequestPickingFrame() const;

	private:
		Engine::SceneSystem::SceneManager& m_sceneManager;
		Editor::Core::EGizmoOperation m_currentOperation = Editor::Core::EGizmoOperation::TRANSLATE;
		Editor::Core::SceneViewGizmoPivot m_currentPivot = Editor::Core::SceneViewGizmoPivot::Pivot;
		Editor::Core::SceneViewGizmoSpace m_currentSpace = Editor::Core::SceneViewGizmoSpace::Global;

		Engine::GameObject* m_highlightedActor = nullptr;
		uint64_t m_destroyedListener = 0;
        Editor::Core::SceneViewGizmoInteraction m_gizmoInteraction;
        Editor::Core::SceneCameraFocusState m_cameraFocus;
        Maths::Vector2 m_lastPickingMousePos { -10000.0f, -10000.0f };
        std::optional<Maths::Vector2> m_pendingClickPickRenderPos;
		std::chrono::steady_clock::time_point m_lastPickingSampleTime {};
        uint64_t m_pendingClickMinReadablePickingFrameSerial = 0u;
		bool m_hasPickingSample = false;
        bool m_requestPickingFrame = true;
	};
}
