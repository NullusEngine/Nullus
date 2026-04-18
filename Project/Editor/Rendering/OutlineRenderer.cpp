#include "Rendering/OutlineRenderer.h"

#include "Core/EditorActions.h"
#include "Rendering/DebugModelRenderer.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/EditorDefaultResources.h"
#include "Rendering/EditorPipelineStatePresets.h"

#include <Components/CameraComponent.h>
#include <Components/MeshRenderer.h>
#include <Components/TransformComponent.h>

#include <algorithm>

constexpr uint32_t kStencilMask = 0xFF;
constexpr int32_t kStencilReference = 1;
constexpr float kOutlineWorldThicknessPerUnit = 0.0075f;
constexpr float kMinimumOutlineWorldThickness = 0.01f;
constexpr float kMinimumOutlineRadius = 0.001f;

using namespace NLS;

namespace
{
bool IsEditorCameraIconModel(Engine::Components::MeshRenderer& modelRenderer)
{
    auto* editorCameraModel = EDITOR_CONTEXT(editorResources)->GetModel("Camera");
    return editorCameraModel != nullptr && modelRenderer.GetModel() == editorCameraModel;
}

Maths::Matrix4 BuildOutlineShellMatrix(
    const Maths::Matrix4& worldMatrix,
    const Maths::Vector3& worldScale,
    NLS::Render::Resources::Model& model,
    const float thickness)
{
    const auto& bounds = model.GetBoundingSphere();
    const float worldScaleMagnitude = std::max(worldScale.GetAbsMaxElement(), kMinimumOutlineRadius);
    const float worldRadius = std::max(bounds.radius * worldScaleMagnitude, kMinimumOutlineRadius);
    const float outlineWorldThickness = std::max(
        thickness * kOutlineWorldThicknessPerUnit,
        kMinimumOutlineWorldThickness);
    const float shellScale = 1.0f + outlineWorldThickness / worldRadius;
    const auto pivot = bounds.position;

    return worldMatrix *
        Maths::Matrix4::Translation(pivot) *
        Maths::Matrix4::Scaling(Maths::Vector3(shellScale, shellScale, shellScale)) *
        Maths::Matrix4::Translation(Maths::Vector3(-pivot.x, -pivot.y, -pivot.z));
}
}

Editor::Rendering::OutlineRenderer::OutlineRenderer(
    NLS::Render::Core::CompositeRenderer& renderer,
    DebugModelRenderer& debugModelRenderer)
    : m_renderer(renderer)
    , m_debugModelRenderer(debugModelRenderer)
{
    m_stencilFillMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders/Unlit.hlsl"]);
    m_stencilFillMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", Editor::Rendering::GetEditorDefaultWhiteTexture());

    m_outlineMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders/Unlit.hlsl"]);
    m_outlineMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", Editor::Rendering::GetEditorDefaultWhiteTexture());
}

void Editor::Rendering::OutlineRenderer::DrawOutline(
    Engine::GameObject& actor,
    const Maths::Vector4& color,
    const float thickness)
{
    DrawStencilPass(actor);
    DrawOutlinePass(actor, color, thickness);
}

void Editor::Rendering::OutlineRenderer::DrawStencilPass(Engine::GameObject& actor)
{
    auto pso = Editor::Rendering::CreateEditorOutlineStencilPipelineState(
        m_renderer,
        kStencilMask,
        kStencilReference);

    DrawActorToStencil(pso, actor);
}

void Editor::Rendering::OutlineRenderer::DrawOutlinePass(
    Engine::GameObject& actor,
    const Maths::Vector4& color,
    const float thickness)
{
    auto pso = Editor::Rendering::CreateEditorOutlineShellPipelineState(
        m_renderer,
        kStencilReference,
        kStencilMask);

    m_outlineMaterial.Set("u_Diffuse", color);
    DrawActorOutline(pso, actor, thickness);
}

void Editor::Rendering::OutlineRenderer::DrawActorToStencil(
    NLS::Render::Data::PipelineState pso,
    Engine::GameObject& actor)
{
    if (!actor.IsActive())
        return;

    const bool hasCameraComponent = actor.GetComponent<Engine::Components::CameraComponent>() != nullptr;

    if (auto modelRenderer = actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer && modelRenderer->GetModel())
    {
        if (!(hasCameraComponent && IsEditorCameraIconModel(*modelRenderer)))
            DrawModelToStencil(pso, actor.GetTransform()->GetWorldMatrix(), *modelRenderer->GetModel());
    }

    if (auto cameraComponent = actor.GetComponent<Engine::Components::CameraComponent>(); cameraComponent)
    {
        auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
        auto model = translation * rotation;
        DrawModelToStencil(pso, model, *EDITOR_CONTEXT(editorResources)->GetModel("Camera"));
    }

    for (auto& child : actor.GetChildren())
        DrawActorToStencil(pso, *child);
}

void Editor::Rendering::OutlineRenderer::DrawActorOutline(
    NLS::Render::Data::PipelineState pso,
    Engine::GameObject& actor,
    const float thickness)
{
    if (!actor.IsActive())
        return;

    const bool hasCameraComponent = actor.GetComponent<Engine::Components::CameraComponent>() != nullptr;

    if (auto modelRenderer = actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer && modelRenderer->GetModel())
    {
        if (!(hasCameraComponent && IsEditorCameraIconModel(*modelRenderer)))
        {
            const auto outlineModel = BuildOutlineShellMatrix(
                actor.GetTransform()->GetWorldMatrix(),
                actor.GetTransform()->GetWorldScale(),
                *modelRenderer->GetModel(),
                thickness);
            DrawModelOutline(pso, outlineModel, *modelRenderer->GetModel());
        }
    }

    if (auto cameraComponent = actor.GetComponent<Engine::Components::CameraComponent>(); cameraComponent)
    {
        auto& cameraModel = *EDITOR_CONTEXT(editorResources)->GetModel("Camera");
        auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
        auto model = translation * rotation;
        const auto outlineModel = BuildOutlineShellMatrix(
            model,
            Maths::Vector3::One,
            cameraModel,
            thickness);
        DrawModelOutline(pso, outlineModel, cameraModel);
    }

    for (auto& child : actor.GetChildren())
        DrawActorOutline(pso, *child, thickness);
}

void Editor::Rendering::OutlineRenderer::DrawModelToStencil(
    NLS::Render::Data::PipelineState pso,
    const Maths::Matrix4& worldMatrix,
    NLS::Render::Resources::Model& model)
{
    m_debugModelRenderer.DrawModelWithSingleMaterial(pso, model, m_stencilFillMaterial, worldMatrix);
}

void Editor::Rendering::OutlineRenderer::DrawModelOutline(
    NLS::Render::Data::PipelineState pso,
    const Maths::Matrix4& worldMatrix,
    NLS::Render::Resources::Model& model)
{
    m_debugModelRenderer.DrawModelWithSingleMaterial(pso, model, m_outlineMaterial, worldMatrix);
}
