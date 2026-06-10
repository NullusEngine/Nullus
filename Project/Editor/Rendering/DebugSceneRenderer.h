#pragma once

#include <cstdint>
#include <vector>

#include <Rendering/Entities/Camera.h>

#include "GameObject.h"
#include <Engine/SceneSystem/SceneManager.h>
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include <Rendering/Resources/Material.h>
#include "Components/LightComponent.h"
#include <Engine/Rendering/DeferredSceneRenderer.h>
#include "Core/Context.h"

namespace NLS::Editor::Panels { class AView; }

namespace NLS::Editor::Rendering
{
	/**
	* Provide a debug layer on top of the default scene renderer to see "invisible" entities such as
	* lights, cameras, 
	*/
class DebugSceneRenderer : public Engine::Rendering::DeferredSceneRenderer
	{
	public:
		struct DebugSceneDescriptor
		{
			Engine::GameObject* highlightedGameObject;
            Engine::GameObject* selectedGameObject;
            bool requestPickingFrame = false;
            Engine::SceneSystem::Scene* previewScene = nullptr;
            bool requestPickingFrameForClick = false;
            uint64_t hoverPickingVisibleDrawBudget = 1024u;
		};

		struct CullingOverlayOptions
		{
			bool enabled = false;
			bool includeVisiblePrimitives = false;
			uint64_t maxItems = 512u;
		};

		struct CullingOverlayItem
		{
			uint64_t sceneId = 0u;
			uint32_t primitiveIndex = ~0u;
			uint32_t primitiveGeneration = 0u;
			uint8_t reason = 0u;
			bool visible = false;
		};

		/**
		* Constructor of the Renderer
		* @param p_driver
		*/
		DebugSceneRenderer(NLS::Render::Context::Driver& p_driver);
		void SetCullingOverlayOptions(const CullingOverlayOptions& options);
		const CullingOverlayOptions& GetCullingOverlayOptions() const;
		std::vector<CullingOverlayItem> BuildCullingOverlayItems(
			const NLS::Render::Context::FrameSnapshot& snapshot) const;

	protected:
        std::optional<NLS::Render::Context::FrameSnapshot> BuildFrameSnapshot(
			const NLS::Render::Data::FrameDescriptor& frameDescriptor) const override;
        NLS::Render::Context::PreparedRenderSceneBuilder BuildPreparedRenderSceneBuilder(
            const NLS::Render::Context::FrameSnapshot& snapshot) const override;
        bool ShouldPublishCullReasonDebugSnapshots() const override;
        uint64_t GetCullReasonDebugSnapshotMaxEntries() const override;
        void OnThreadedFramePublished(uint64_t publishedFrameId) override;
        void OnThreadedFramePublishFailed() override;

	private:
		CullingOverlayOptions m_cullingOverlayOptions;
	};

	std::vector<DebugSceneRenderer::CullingOverlayItem> BuildDebugSceneCullingOverlayItems(
		const NLS::Render::Context::FrameSnapshot& snapshot,
		const DebugSceneRenderer::CullingOverlayOptions& options);
	bool ShouldPublishDebugSceneCullReasonSnapshots(
		const DebugSceneRenderer::CullingOverlayOptions& options);
}
