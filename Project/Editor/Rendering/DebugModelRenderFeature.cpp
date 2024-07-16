#include "Rendering/DebugModelRenderFeature.h"
#include "Engine/Rendering/EngineDrawableDescriptor.h"
#include "Rendering/Core/CompositeRenderer.h"
using namespace NLS;
Editor::Rendering::DebugModelRenderFeature::DebugModelRenderFeature(NLS::Rendering::Core::CompositeRenderer& p_renderer)
	: ARenderFeature(p_renderer)
{
}

void Editor::Rendering::DebugModelRenderFeature::DrawModelWithSingleMaterial(NLS::Rendering::Data::PipelineState p_pso, NLS::Rendering::Resources::Model& p_model, NLS::Rendering::Data::Material& p_material, const Maths::Matrix4& p_modelMatrix)
{
	auto stateMask = p_material.GenerateStateMask();

	auto engineDrawableDescriptor = Engine::Rendering::EngineDrawableDescriptor{
		p_modelMatrix,
		Maths::Matrix4::Identity
	};

	for (auto mesh : p_model.GetMeshes())
	{
		NLS::Rendering::Entities::Drawable element;
		element.mesh = mesh;
		element.material = &p_material;
		element.stateMask = stateMask;
		element.AddDescriptor(engineDrawableDescriptor);

		m_renderer.DrawEntity(p_pso, element);
	}
}
