#include <Rendering/Features/DebugShapeRenderFeature.h>
#include <cstdlib>
#include <cstring>

#include <Debug/Assertion.h>

#include "Rendering/DebugModelRenderFeature.h"
#include "Core/EditorResources.h"
#include "Core/EditorActions.h"
#include "Rendering/EditorPipelineStatePresets.h"
#include "Rendering/GridRenderPass.h"
using namespace NLS;

namespace
{
	bool IsEnvFlagEnabled(const char* name)
	{
		if (name == nullptr || name[0] == '\0')
			return false;

		const char* value = std::getenv(name);
		if (value == nullptr)
			return false;

		return std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0;
	}
}

Editor::Rendering::GridRenderPass::GridRenderPass(NLS::Render::Core::CompositeRenderer& p_renderer) :
	NLS::Render::Core::ARenderPass(p_renderer)
{
	/* Grid Material */
	m_gridMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Grid"));
	m_gridMaterial.SetBlendable(true);
}

void Editor::Rendering::GridRenderPass::Draw(NLS::Render::Data::PipelineState p_pso)
{
	NLS_ASSERT(m_renderer.HasDescriptor<GridDescriptor>(), "Cannot find GridDescriptor attached to this renderer");
	NLS_ASSERT(m_renderer.HasFeature<NLS::Render::Features::DebugShapeRenderFeature>(), "Cannot find DebugShapeRenderFeature attached to this renderer");
	NLS_ASSERT(m_renderer.HasFeature<Editor::Rendering::DebugModelRenderFeature>(), "Cannot find DebugModelRenderFeature attached to this renderer");

	auto& gridDescriptor = m_renderer.GetDescriptor<GridDescriptor>();
	auto& debugShapeRenderer = m_renderer.GetFeature<NLS::Render::Features::DebugShapeRenderFeature>();

	auto pso = Editor::Rendering::CreateEditorGridPipelineState(p_pso);

	constexpr float gridSize = 5000.0f;

	Maths::Matrix4 model =
		Maths::Matrix4::Translation({ gridDescriptor.viewPosition.x, 0.0f, gridDescriptor.viewPosition.z }) *
		Maths::Matrix4::Scaling({ gridSize * 2.0f, 1.f, gridSize * 2.0f });

	m_gridMaterial.Set("u_Color", gridDescriptor.gridColor);

	if (!IsEnvFlagEnabled("NLS_EDITOR_GRID_SKIP_PLANE"))
	{
		m_renderer.GetFeature<DebugModelRenderFeature>()
		.DrawModelWithSingleMaterial(pso, *EDITOR_CONTEXT(editorResources)->GetModel("Plane"), m_gridMaterial, model);
	}

	if (!IsEnvFlagEnabled("NLS_EDITOR_GRID_SKIP_AXES"))
	{
		debugShapeRenderer.DrawLine(pso, Maths::Vector3(-gridSize + gridDescriptor.viewPosition.x, 0.0f, 0.0f), Maths::Vector3(gridSize + gridDescriptor.viewPosition.x, 0.0f, 0.0f), Maths::Vector3(1.0f, 0.0f, 0.0f), 1.0f);
		debugShapeRenderer.DrawLine(pso, Maths::Vector3(0.0f, -gridSize + gridDescriptor.viewPosition.y, 0.0f), Maths::Vector3(0.0f, gridSize + gridDescriptor.viewPosition.y, 0.0f), Maths::Vector3(0.0f, 1.0f, 0.0f), 1.0f);
		debugShapeRenderer.DrawLine(pso, Maths::Vector3(0.0f, 0.0f, -gridSize + gridDescriptor.viewPosition.z), Maths::Vector3(0.0f, 0.0f, gridSize + gridDescriptor.viewPosition.z), Maths::Vector3(0.0f, 0.0f, 1.0f), 1.0f);
	}
}
