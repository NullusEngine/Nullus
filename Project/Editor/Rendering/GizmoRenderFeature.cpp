#include <Components/CameraComponent.h>
#include <Components/LightComponent.h>

#include <Rendering/Features/DebugShapeRenderFeature.h>

#include <Debug/Assertion.h>

#include "Rendering/DebugModelRenderFeature.h"
#include "Rendering/DebugSceneRenderer.h"
#include "Core/EditorResources.h"
//#include "Panels/AView.h"
#include "Settings/EditorSettings.h"
#include "Core/GizmoBehaviour.h"
#include "Core/EditorActions.h"
#include "Rendering/GizmoRenderFeature.h"
using namespace NLS;
Editor::Rendering::GizmoRenderFeature::GizmoRenderFeature(NLS::Rendering::Core::CompositeRenderer& p_renderer)
    : NLS::Rendering::Features::ARenderFeature(p_renderer)
{
	/* Gizmo Arrow Material */
	m_gizmoArrowMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Gizmo"));
	m_gizmoArrowMaterial.SetGPUInstances(3);
	m_gizmoArrowMaterial.Set("u_IsBall", false);
	m_gizmoArrowMaterial.Set("u_IsPickable", false);

	/* Gizmo Ball Material */
	m_gizmoBallMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Gizmo"));
	m_gizmoBallMaterial.Set("u_IsBall", true);
	m_gizmoBallMaterial.Set("u_IsPickable", false);
}

std::string GetArrowModelName(Editor::Core::EGizmoOperation p_operation)
{
	using namespace Editor::Core;

	switch (p_operation)
	{
		default:
		case EGizmoOperation::TRANSLATE: return "Arrow_Translate";
		case EGizmoOperation::ROTATE: return "Arrow_Rotate";
		case EGizmoOperation::SCALE: return "Arrow_Scale";
	}
}

int GetAxisIndexFromDirection(std::optional<Editor::Core::GizmoBehaviour::EDirection> p_direction)
{
	return p_direction ? static_cast<int>(p_direction.value()) : -1;
}

void Editor::Rendering::GizmoRenderFeature::DrawGizmo(
	const Maths::Vector3& p_position,
	const Maths::Quaternion& p_rotation,
	Editor::Core::EGizmoOperation p_operation,
	bool p_pickable,
	std::optional<Editor::Core::GizmoBehaviour::EDirection> p_highlightedDirection
)
{
	auto pso = m_renderer.CreatePipelineState();

	auto modelMatrix =
		Maths::Matrix4::Translation(p_position) *
		Maths::Quaternion::ToMatrix4(Maths::Quaternion::Normalize(p_rotation));

	if (auto sphereModel = EDITOR_CONTEXT(editorResources)->GetModel("Sphere"))
	{
		auto sphereModelMatrix = modelMatrix * Maths::Matrix4::Scaling({ 0.1f, 0.1f, 0.1f });

		m_renderer.GetFeature<DebugModelRenderFeature>()
		.DrawModelWithSingleMaterial(
			pso,
			*sphereModel,
			m_gizmoBallMaterial,
			sphereModelMatrix
		);
	}
	
	auto arrowModelName = GetArrowModelName(p_operation);

	if (auto arrowModel = EDITOR_CONTEXT(editorResources)->GetModel(arrowModelName))
	{
		const auto axisIndex = GetAxisIndexFromDirection(p_highlightedDirection);
		m_gizmoArrowMaterial.Set("u_HighlightedAxis", axisIndex);

		m_renderer.GetFeature<DebugModelRenderFeature>()
		.DrawModelWithSingleMaterial(
			pso,
			*arrowModel,
			m_gizmoArrowMaterial,
			modelMatrix
		);
	}
}
