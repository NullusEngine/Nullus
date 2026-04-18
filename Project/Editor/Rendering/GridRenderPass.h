#pragma once

#include <Rendering/Entities/Camera.h>

#include <GameObject.h>
#include <SceneSystem/SceneManager.h>
#include <Components/MeshRenderer.h>
#include <Resources/Material.h>
#include <Components/LightComponent.h>
#include <Rendering/BaseSceneRenderer.h>

#include "Core/Context.h"
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

	protected:
        virtual void Draw(NLS::Render::Data::PipelineState p_pso) override;

	private:
        DebugModelRenderer m_debugModelRenderer;
        NLS::Render::Resources::Material m_gridMaterial;
	};
}
