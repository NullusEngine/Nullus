#pragma once

#include <Rendering/Entities/Camera.h>
#include <Rendering/Features/DebugShapeRenderFeature.h>

#include <GameObject.h>
#include <SceneSystem/SceneManager.h>
#include <Components/MeshRenderer.h>
#include <Data/Material.h>
#include <Components/LightComponent.h>
#include <Rendering/SceneRenderer.h>

#include "Core/Context.h"

namespace NLS::Editor::Rendering
{
	/**
	* Draw a grid
	*/
	class GridRenderPass : public NLS::Rendering::Core::ARenderPass
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
		GridRenderPass(NLS::Rendering::Core::CompositeRenderer& p_renderer);

	protected:
        virtual void Draw(NLS::Rendering::Data::PipelineState p_pso) override;

	private:
        NLS::Rendering::Data::Material m_gridMaterial;
	};
}