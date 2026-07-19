#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Core/EditorActions.h"
#include "Core/SceneCameraFocus.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Panels/AViewControllable.h"
#include "Rendering/DebugSceneRenderer.h"
#include "Panels/SceneViewPickingPolicy.h"
#include "Core/SceneViewImGuizmo.h"

namespace NLS::Editor::Core
{
}

namespace NLS::Editor::Panels
{
            std::optional<Editor::Rendering::DebugSceneRenderer::PrefabDragProxyDescriptor>
            BuildSceneViewPrefabDragProxyDescriptor(
                const std::optional<Maths::Vector3>& placement,
                bool hasActivePayload,
                const Engine::GameObject* activeRoot,
                bool activeRootVisible = true);
            // Transform-only prefab preview movement must not use the scene-revision fast path.
            bool ShouldTrustSceneViewRenderContentRevision(
                bool hasActivePrefabDragPayload,
                bool activePrefabDragCommitPending);

            bool ShouldDeferSceneViewRenderForPendingSceneLoadResources(size_t pendingTaskCount);
            bool ShouldSkipSceneViewSceneDrawablesForPendingSceneLoadResources(
                size_t pendingTaskCount,
                bool placeholderAlreadyRendered = false,
                size_t visibleObjectCount = 0u);
            bool ShouldSuppressSceneViewLightGridComputeForPendingSceneLoadResources(
                size_t pendingTaskCount,
                bool skipSceneDrawables);
            bool ShouldForceSceneViewRenderForPendingSceneLoadResources(
                size_t pendingTaskCount,
                bool validationReadbackRequested);
            bool ShouldWaitForSceneViewValidationReadbackSceneLoadResources(
                bool activeResolution,
                size_t pendingTaskCount,
                size_t visibleObjectCount);
            bool ShouldWaitForSceneViewValidationReadbackAfterSceneLoadResources(
                bool observedSceneLoadResources,
                uint32_t stableFramesAfterResourcesReady,
                uint32_t requiredStableFrames = 4u);
            std::string BuildSceneViewValidationReadbackStatus(
                uint64_t nonBlackPixels,
                uint32_t maxChannel,
                size_t pendingSceneLoadTextureLoads = 0u);
            uint64_t BuildSceneViewSceneLoadResourceCacheVersion(
                size_t pendingTaskCount,
                bool placeholderAlreadyRendered = false,
                size_t visibleObjectCount = 0u);

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
        void ApplyValidationCameraForwardStep(float step);
        void SetValidationCameraMotionActive(bool active);

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

#if defined(NLS_ENABLE_TEST_HOOKS)
        struct PrefabDragProxySceneViewLoopValidation
        {
            bool dragLoopExercised = false;
            bool payloadAcceptedBeforeDelivery = false;
            bool followedPlacement = true;
            bool sceneRootCreatedByProxy = false;
            std::vector<Maths::Vector3> descriptorPlacements;
        };

        PrefabDragProxySceneViewLoopValidation ValidatePrefabDragProxySceneViewLoopForTesting(
            const NLS::Editor::Assets::EditorAssetDragPayload& payload,
            const std::vector<Maths::Vector3>& placements);
#endif

		protected:
        virtual Engine::Rendering::BaseSceneRenderer::SceneDescriptor CreateSceneDescriptor() override;
        bool ShouldUseStaticFrameCache() const override;
        uint64_t BuildStaticFrameCacheKey(
            const Render::Entities::Camera& camera,
            const Engine::SceneSystem::Scene& scene,
            uint16_t width,
            uint16_t height) const override;
        void TraceStaticFrameCacheKeyChanged(
            uint64_t previousKey,
            uint64_t currentKey) const override;
        void CommitStaticFrameCacheKey(uint64_t staticFrameCacheKey) override;
        bool ShouldForceStaticFrameRender() const override;
        bool ShouldDeferRenderFrame() const override;
        bool RequiresSynchronizedRetiredFramePresentation() const override;
        void AfterRenderFrame() override;
        void DrawPreRenderViewportOverlay() override;
        void OnAfterDrawWidgets() override;
        void DrawViewportOverlay() override;

		private:
            class ViewportDragDropTarget;

			void HandleGameObjectPicking();
        void EnsureCameraFocus();
        bool IsCameraControlActive() const;
        bool ShouldRequestPickingFrame() const;
        void TryWriteValidationReadback();
	        void HandleViewportAssetDragDrop();
	        void UpdateActivePrefabDragInstance(const NLS::Editor::Assets::EditorAssetDragPayload& payload);
	        void TryRefreshActivePrefabDragHotCacheKey();
	        void ClearActivePrefabDragState();
	        void CancelActivePrefabDragInstance();
	        bool CommitActivePrefabDragInstance();
	        Editor::Core::EditorActions::SceneMutationToken CaptureActivePrefabDragSceneToken() const;
	        bool IsActivePrefabDragSceneTokenCurrent(
	            const Editor::Core::EditorActions::SceneMutationToken& token);
        std::optional<Maths::Vector3> ResolveActivePrefabDragPlacement(const Maths::Vector2& mousePosition) const;

		private:
		Engine::SceneSystem::SceneManager& m_sceneManager;
		Editor::Core::EGizmoOperation m_currentOperation = Editor::Core::EGizmoOperation::TRANSLATE;
		Editor::Core::SceneViewGizmoPivot m_currentPivot = Editor::Core::SceneViewGizmoPivot::Pivot;
		Editor::Core::SceneViewGizmoSpace m_currentSpace = Editor::Core::SceneViewGizmoSpace::Global;

		Engine::GameObject* m_highlightedGameObject = nullptr;
		uint64_t m_destroyedListener = 0;
        bool m_validationCameraMotionActive = false;
        Editor::Core::SceneViewGizmoInteraction m_gizmoInteraction;
        Editor::Core::SceneCameraFocusState m_cameraFocus;
        Maths::Vector2 m_lastPickingMousePos { -10000.0f, -10000.0f };
        std::optional<Maths::Vector2> m_pendingClickPickRenderPos;
        std::optional<NLS::Editor::Panels::HitProxyPickingSignature> m_pendingClickPickingSignature;
			std::chrono::steady_clock::time_point m_lastPickingSampleTime {};
        uint64_t m_pendingClickMinReadablePickingFrameSerial = 0u;
        bool m_hasPickingSample = false;
	        bool m_requestPickingFrame = true;
	        bool m_requestPickingFrameForClick = false;
	        std::optional<NLS::Editor::Assets::EditorAssetDragPayload> m_activeDraggedPrefabPayload;
	        std::string m_activeDraggedPrefabPayloadKey;
	        std::string m_activeDraggedPrefabAssetPath;
	        std::string m_activeDraggedPrefabSubAssetKey;
	        NLS::Core::Assets::AssetId m_activeDraggedPrefabAssetId {};
	        NLS::Core::Assets::AssetType m_activeDraggedPrefabAssetType = NLS::Core::Assets::AssetType::Unknown;
	        std::optional<NLS::Editor::Assets::UnifiedPrefabLoadKey> m_activeDraggedPrefabHotCacheKey;
	        bool m_activeDraggedPrefabHotCacheKeyBuildAttempted = false;
	        Engine::GameObject* m_activeDraggedPrefabRoot = nullptr;
	        std::optional<Editor::Core::EditorActions::SceneMutationToken> m_activeDraggedPrefabDropSceneToken;
        std::optional<Editor::Core::EditorActions::SceneMutationToken> m_activeDraggedPrefabRootSceneToken;
		std::optional<Maths::Vector3> m_activeDraggedPrefabDropPlacement;
        std::optional<Maths::Vector3> m_activeDraggedPrefabProxyPlacement;
        bool m_activeDraggedPrefabRootAwaitingRendererResources = false;
        bool m_activeDraggedPrefabCommitPending = false;
        bool m_hasRenderedSceneLoadResourcePlaceholder = false;
#if defined(NLS_ENABLE_TEST_HOOKS)
        std::optional<Maths::Vector3> m_activePrefabDragPlacementOverrideForTesting;
        bool m_disableActivePrefabDragPreloadForTesting = false;
#endif
        bool m_cameraMovedForPresentation = true;
        bool m_validationReadbackWritten = false;
        uint32_t m_validationReadbackWarmupFrames = 0u;
        uint32_t m_validationReadbackReadyFrames = 0u;
        uint32_t m_validationReadbackSceneLoadReadyFrames = 0u;
        bool m_validationReadbackObservedSceneLoadResources = false;
        mutable uint64_t m_lastComputedStaticCacheBaseKey = 0u;
        mutable uint64_t m_lastComputedStaticCacheHighlightKey = 0u;
        mutable uint64_t m_lastComputedStaticCacheGizmoKey = 0u;
        mutable uint64_t m_lastComputedStaticCacheFocusKey = 0u;
        mutable uint64_t m_lastComputedStaticCacheSelectionKey = 0u;
        mutable uint64_t m_lastComputedStaticCacheSceneLoadResourcesKey = 0u;
        mutable size_t m_lastLoggedDeferredSceneLoadResourceTasks = std::numeric_limits<size_t>::max();
        uint64_t m_committedStaticCacheBaseKey = 0u;
        uint64_t m_committedStaticCacheHighlightKey = 0u;
        uint64_t m_committedStaticCacheGizmoKey = 0u;
        uint64_t m_committedStaticCacheFocusKey = 0u;
        uint64_t m_committedStaticCacheSelectionKey = 0u;
        uint64_t m_committedStaticCacheSceneLoadResourcesKey = 0u;
	};
}
