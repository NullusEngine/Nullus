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
        std::optional<NLS::Render::Context::RenderPassCommandInput> GetPreparedThreadedPassInput() const;

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
        std::optional<NLS::Render::Context::RenderPassCommandInput> BuildThreadedPassInput(
            NLS::Render::Data::PipelineState p_pso);
        DebugModelRenderer m_debugModelRenderer;
        NLS::Render::Resources::Material m_gridMaterial;
        std::optional<NLS::Render::Context::RenderPassCommandInput> m_preparedThreadedPassInput;
	};
}
