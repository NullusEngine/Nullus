#pragma once

#include <optional>

#include <Rendering/Entities/Camera.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>

#include <GameObject.h>
#include <SceneSystem/SceneManager.h>
#include <Components/MeshRenderer.h>
#include <Resources/Material.h>
#include <Components/LightComponent.h>
#include <Rendering/BaseSceneRenderer.h>

#include "Core/Context.h"
#include "Rendering/EditorHelperLifecycle.h"
#include "Rendering/DebugModelRenderer.h"

namespace NLS::Render::Debug { class DebugDrawService; }

namespace NLS::Editor::Rendering
{
	/**
	* Draw a grid
	*/
	class GridRenderPass : public NLS::Render::Core::ARenderPass
	{
	public:
		struct GridDescriptor
		{
            Maths::Vector3 gridColor;
            Maths::Vector3 viewPosition;
		};

		/**
		* Constructor
		* @param p_renderer
		*/
		GridRenderPass(NLS::Render::Core::CompositeRenderer& p_renderer);
        const std::optional<NLS::Render::Context::RenderPassCommandInput>& GetPreparedThreadedPassInput() const;
        std::optional<NLS::Render::Context::RenderPassCommandInput> ConsumePreparedThreadedPassInput();

        static bool ShouldIncludeInThreadedFrame(
            bool passEnabled,
            bool hasGridDescriptor,
            bool debugDrawEnabled,
            bool debugDrawGrid)
        {
            ThreadedEditorHelperState helperState;
            helperState.gridPassEnabled = passEnabled;
            helperState.gridEnabled = hasGridDescriptor && debugDrawEnabled && debugDrawGrid;
            return HasThreadedGridHelperPass(helperState);
        }

	protected:
        void OnBeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
        virtual void Draw(NLS::Render::Data::PipelineState p_pso) override;

	private:
		struct ThreadedCommandCacheKey
		{
			const NLS::Render::Resources::Mesh* mesh = nullptr;
			const NLS::Render::Resources::Shader* shader = nullptr;
			uint64_t deviceIdentity = 0u;
			uint64_t pipelineBits = 0u;
			Maths::Vector3 gridColor{};
			Maths::Vector3 viewPosition{};

			bool operator==(const ThreadedCommandCacheKey&) const = default;
		};

		void UpdateGridAxes(
			NLS::Render::Debug::DebugDrawService& debugDrawService,
			const GridDescriptor& gridDescriptor,
			float gridSize);
		std::optional<NLS::Render::Context::RenderPassCommandInput> BuildThreadedPassInput(
			NLS::Render::Data::PipelineState p_pso);
        DebugModelRenderer m_debugModelRenderer;
		NLS::Render::Resources::Material m_gridMaterial;
		std::optional<NLS::Render::Context::RenderPassCommandInput> m_preparedThreadedPassInput;
		std::optional<ThreadedCommandCacheKey> m_threadedCommandCacheKey;
		std::vector<NLS::Render::Context::RecordedDrawCommandInput> m_cachedThreadedDrawCommands;
		Maths::Vector3 m_cachedGridAxesPosition{};
		bool m_hasCachedGridAxes = false;
	};
}
