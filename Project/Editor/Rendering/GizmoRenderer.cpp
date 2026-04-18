#include "Rendering/GizmoRenderer.h"

#include "Core/EditorActions.h"
#include "Core/EditorResources.h"
#include "Rendering/DebugModelRenderer.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/EditorPipelineStatePresets.h"

using namespace NLS;

namespace
{
std::string GetArrowModelName(Editor::Core::EGizmoOperation operation)
{
    using namespace Editor::Core;

    switch (operation)
    {
    default:
    case EGizmoOperation::TRANSLATE: return "Arrow_Translate";
    case EGizmoOperation::ROTATE: return "Arrow_Rotate";
    case EGizmoOperation::SCALE: return "Arrow_Scale";
    }
}

int GetAxisIndexFromDirection(std::optional<Editor::Core::GizmoBehaviour::EDirection> direction)
{
    return direction ? static_cast<int>(direction.value()) : -1;
}
}

Editor::Rendering::GizmoRenderer::GizmoRenderer(
    NLS::Render::Core::CompositeRenderer& renderer,
    DebugModelRenderer& debugModelRenderer)
    : m_renderer(renderer)
    , m_debugModelRenderer(debugModelRenderer)
{
    m_gizmoArrowMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Gizmo"));
    m_gizmoArrowMaterial.SetGPUInstances(3);
    m_gizmoArrowMaterial.Set("u_IsBall", false);
    m_gizmoArrowMaterial.Set("u_IsPickable", false);

    m_gizmoBallMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Gizmo"));
    m_gizmoBallMaterial.Set("u_IsBall", true);
    m_gizmoBallMaterial.Set("u_IsPickable", false);
}

void Editor::Rendering::GizmoRenderer::DrawGizmo(
    const Maths::Vector3& position,
    const Maths::Quaternion& rotation,
    Editor::Core::EGizmoOperation operation,
    bool pickable,
    std::optional<Editor::Core::GizmoBehaviour::EDirection> highlightedDirection)
{
    auto pso = Editor::Rendering::CreateEditorOverlayPipelineState(m_renderer);

    auto modelMatrix =
        Maths::Matrix4::Translation(position) *
        Maths::Quaternion::ToMatrix4(Maths::Quaternion::Normalize(rotation));

    if (auto sphereModel = EDITOR_CONTEXT(editorResources)->GetModel("Sphere"))
    {
        auto sphereModelMatrix = modelMatrix * Maths::Matrix4::Scaling({ 0.1f, 0.1f, 0.1f });

        m_debugModelRenderer.DrawModelWithSingleMaterial(
            pso,
            *sphereModel,
            m_gizmoBallMaterial,
            sphereModelMatrix);
    }

    auto arrowModelName = GetArrowModelName(operation);

    if (auto arrowModel = EDITOR_CONTEXT(editorResources)->GetModel(arrowModelName))
    {
        const auto axisIndex = GetAxisIndexFromDirection(highlightedDirection);
        m_gizmoArrowMaterial.Set("u_HighlightedAxis", axisIndex);
        m_gizmoArrowMaterial.Set("u_IsPickable", pickable);

        m_debugModelRenderer.DrawModelWithSingleMaterial(
            pso,
            *arrowModel,
            m_gizmoArrowMaterial,
            modelMatrix);
    }
}
