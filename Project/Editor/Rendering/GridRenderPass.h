#pragma once

#include <optional>

#include <Rendering/Entities/Camera.h>
#include <Rendering/Context/RenderScenePackage.h>

#include <GameObject.h>
#include <SceneSystem/SceneManager.h>
#include <Components/MeshRenderer.h>
#include <Resources/Material.h>
#include <Components/LightComponent.h>
#include <Rendering/BaseSceneRenderer.h>

#include "Core/Context.h"
#include "Rendering/EditorHelperLifecycle.h"
#include "Rendering/DebugModelRenderer.h"
#include "Rendering/EditorGridRenderer.h"

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
		void UpdateGridAxes(
			NLS::Render::Debug::DebugDrawService& debugDrawService,
			const GridDescriptor& gridDescriptor,
			float gridSize);
		// Writes the grid color only when it actually changed. Material parameter
		// writes bump the global material mutation epoch, which is part of the
		// Scene View static frame cache key.
		void ApplyGridColor(const Maths::Vector3& gridColor);
		std::optional<NLS::Render::Context::RenderPassCommandInput> BuildThreadedPassInput(
			NLS::Render::Data::PipelineState p_pso);
        DebugModelRenderer m_debugModelRenderer;
        EditorGridRenderer m_gridRenderer;
        std::shared_ptr<const EditorGridShaders> m_gridShaders;
        std::shared_ptr<const EditorGridShaders> m_axesShaders;
		NLS::Render::Resources::Material m_gridMaterial;
		std::optional<NLS::Render::Context::RenderPassCommandInput> m_preparedThreadedPassInput;
		Maths::Vector3 m_cachedGridAxesPosition{};
		bool m_hasCachedGridAxes = false;
	};
}
