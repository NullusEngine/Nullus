#include <Rendering/Features/DebugShapeRenderFeature.h>

#include <Debug/Assertion.h>

#include "Rendering/DebugModelRenderFeature.h"
#include "Core/EditorResources.h"
#include "Core/EditorActions.h"
#include "Rendering/GridRenderPass.h"
using namespace NLS;
Editor::Rendering::GridRenderPass::GridRenderPass(NLS::Rendering::Core::CompositeRenderer& p_renderer) :
	NLS::Rendering::Core::ARenderPass(p_renderer)
{
	/* Grid Material */
	m_gridMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Grid"));
	m_gridMaterial.SetBlendable(true);
	m_gridMaterial.SetBackfaceCulling(false);
	m_gridMaterial.SetDepthTest(false);
}

void Editor::Rendering::GridRenderPass::Draw(NLS::Rendering::Data::PipelineState p_pso)
{
	NLS_ASSERT(m_renderer.HasDescriptor<GridDescriptor>(), "Cannot find GridDescriptor attached to this renderer");
	NLS_ASSERT(m_renderer.HasFeature<NLS::Rendering::Features::DebugShapeRenderFeature>(), "Cannot find DebugShapeRenderFeature attached to this renderer");
	NLS_ASSERT(m_renderer.HasFeature<Editor::Rendering::DebugModelRenderFeature>(), "Cannot find DebugModelRenderFeature attached to this renderer");

	auto& gridDescriptor = m_renderer.GetDescriptor<GridDescriptor>();
	auto& debugShapeRenderer = m_renderer.GetFeature<NLS::Rendering::Features::DebugShapeRenderFeature>();

	auto pso = m_renderer.CreatePipelineState();

	constexpr float gridSize = 5000.0f;

	Maths::Matrix4 model =
		Maths::Matrix4::Translation({ gridDescriptor.viewPosition.x, 0.0f, gridDescriptor.viewPosition.z }) *
		Maths::Matrix4::Scaling({ gridSize * 2.0f, 1.f, gridSize * 2.0f });

	m_gridMaterial.Set("u_Color", gridDescriptor.gridColor);

	m_renderer.GetFeature<DebugModelRenderFeature>()
	.DrawModelWithSingleMaterial(pso, *EDITOR_CONTEXT(editorResources)->GetModel("Plane"), m_gridMaterial, model);

	debugShapeRenderer.DrawLine(pso, Maths::Vector3(-gridSize + gridDescriptor.viewPosition.x, 0.0f, 0.0f), Maths::Vector3(gridSize + gridDescriptor.viewPosition.x, 0.0f, 0.0f), Maths::Vector3(1.0f, 0.0f, 0.0f), 1.0f);
	debugShapeRenderer.DrawLine(pso, Maths::Vector3(0.0f, -gridSize + gridDescriptor.viewPosition.y, 0.0f), Maths::Vector3(0.0f, gridSize + gridDescriptor.viewPosition.y, 0.0f), Maths::Vector3(0.0f, 1.0f, 0.0f), 1.0f);
	debugShapeRenderer.DrawLine(pso, Maths::Vector3(0.0f, 0.0f, -gridSize + gridDescriptor.viewPosition.z), Maths::Vector3(0.0f, 0.0f, gridSize + gridDescriptor.viewPosition.z), Maths::Vector3(0.0f, 0.0f, 1.0f), 1.0f);
}
