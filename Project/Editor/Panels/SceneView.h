#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "Assets/EditorAssetDragPayload.h"
#include "Core/SceneCameraFocus.h"
#include "Core/RendererResourcePrewarmRequest.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Panels/AViewControllable.h"
#include "Core/SceneViewImGuizmo.h"

namespace NLS::Editor::Core
{
}

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
        void EnsureRenderer() override;

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
        void AfterRenderFrame() override;
        void DrawPreRenderViewportOverlay() override;
        void OnAfterDrawWidgets() override;
        void DrawViewportOverlay() override;

	private:
        class ViewportDragDropTarget;

		void HandleGameObjectPicking();
        void EnsureCameraFocus();
        bool ShouldRequestPickingFrame() const;
        void TryWriteValidationReadback();
        void UpdateImportedAssetDragPreview(const NLS::Editor::Assets::EditorAssetDragPayload& payload);
        bool EnsureImportedAssetDragPreviewMeshGhost(const NLS::Editor::Assets::EditorAssetDragPayload& payload);
        std::optional<Maths::Vector3> ResolveImportedAssetDragPreviewPlacement(const Maths::Vector2& mousePosition) const;
        void HandleViewportAssetDragDrop();
        void PumpImportedAssetDragPreviewBeforeRender();
        void PumpImportedAssetDragPreviewResources();
        NLS::Editor::Core::PrefabInstancePreviewResourceHandoff CollectImportedAssetDragPreviewResourceHandoff();
        void DrawImportedAssetDragPreview();
        void ClearImportedAssetDragPreview(bool cancelAsyncResourceRequests = true);

	private:
		Engine::SceneSystem::SceneManager& m_sceneManager;
		Editor::Core::EGizmoOperation m_currentOperation = Editor::Core::EGizmoOperation::TRANSLATE;
		Editor::Core::SceneViewGizmoPivot m_currentPivot = Editor::Core::SceneViewGizmoPivot::Pivot;
		Editor::Core::SceneViewGizmoSpace m_currentSpace = Editor::Core::SceneViewGizmoSpace::Global;

		Engine::GameObject* m_highlightedGameObject = nullptr;
		uint64_t m_destroyedListener = 0;
        Editor::Core::SceneViewGizmoInteraction m_gizmoInteraction;
        Editor::Core::SceneCameraFocusState m_cameraFocus;
        Maths::Vector2 m_lastPickingMousePos { -10000.0f, -10000.0f };
        std::optional<Maths::Vector2> m_pendingClickPickRenderPos;
		std::chrono::steady_clock::time_point m_lastPickingSampleTime {};
        uint64_t m_pendingClickMinReadablePickingFrameSerial = 0u;
        std::optional<NLS::Editor::Assets::EditorAssetDragPayload> m_importedAssetDragPreviewPayload;
        std::optional<NLS::Engine::Assets::PrefabArtifact> m_importedAssetDragPreviewArtifact;
        std::unique_ptr<Engine::SceneSystem::Scene> m_importedAssetDragPreviewScene;
        std::vector<std::unique_ptr<Engine::SceneSystem::Scene>> m_retiredImportedAssetDragPreviewScenes;
        Engine::GameObject* m_importedAssetDragPreviewRoot = nullptr;
        std::string m_importedAssetDragPreviewAssetGuid;
        std::string m_importedAssetDragPreviewSubAssetKey;
        bool m_importedAssetDragPreviewMeshGhostUnavailable = false;
        bool m_importedAssetDragPreviewRenderableReady = false;
        std::chrono::steady_clock::time_point m_importedAssetDragPreviewNextMeshGhostRetryTime {};
        NLS::Editor::Core::RendererResourcePrewarmRequest m_importedAssetDragPreviewPrewarmRequest;
        Maths::Vector2 m_importedAssetDragPreviewMousePos { 0.0f, 0.0f };
        std::optional<Maths::Vector3> m_importedAssetDragPreviewPlacement;
		bool m_hasPickingSample = false;
        bool m_requestPickingFrame = true;
        bool m_cameraMovedForPresentation = true;
        bool m_validationReadbackWritten = false;
        uint32_t m_validationReadbackReadyFrames = 0u;
	};
}
