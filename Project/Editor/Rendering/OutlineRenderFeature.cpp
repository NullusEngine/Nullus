#include "Rendering/DebugModelRenderFeature.h"
#include "Rendering/OutlineRenderFeature.h"
#include "Core/EditorActions.h"
#include "Rendering/EditorDefaultResources.h"
#include "Rendering/EditorPipelineStatePresets.h"
#include "Settings/EditorSettings.h"

#include <Components/MaterialRenderer.h>

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
    bool IsEditorCameraIconModel(
        Engine::Components::MeshRenderer& modelRenderer)
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

Editor::Rendering::OutlineRenderFeature::OutlineRenderFeature(NLS::Render::Core::CompositeRenderer& p_renderer)
    : NLS::Render::Features::ARenderFeature(p_renderer)
{
    /* Stencil Fill Material */
    m_stencilFillMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders/Unlit.hlsl"]);
    m_stencilFillMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", Editor::Rendering::GetEditorDefaultWhiteTexture());

    /* Outline Material */
    m_outlineMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders/Unlit.hlsl"]);
    m_outlineMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", Editor::Rendering::GetEditorDefaultWhiteTexture());
}

void Editor::Rendering::OutlineRenderFeature::DrawOutline(
    Engine::GameObject& p_actor,
    const Maths::Vector4& p_color,
    float p_thickness)
{
    DrawStencilPass(p_actor);
    DrawOutlinePass(p_actor, p_color, p_thickness);
}

void Editor::Rendering::OutlineRenderFeature::DrawStencilPass(Engine::GameObject& p_actor)
{
    auto pso = Editor::Rendering::CreateEditorOutlineStencilPipelineState(
        m_renderer,
        kStencilMask,
        kStencilReference);

    DrawActorToStencil(pso, p_actor);
}

void Editor::Rendering::OutlineRenderFeature::DrawOutlinePass(Engine::GameObject& p_actor, const Maths::Vector4& p_color, float p_thickness)
{
    auto pso = Editor::Rendering::CreateEditorOutlineShellPipelineState(
        m_renderer,
        kStencilReference,
        kStencilMask);

    // Prepare the outline material
    m_outlineMaterial.Set("u_Diffuse", p_color);

    DrawActorOutline(pso, p_actor, p_thickness);
}

void Editor::Rendering::OutlineRenderFeature::DrawActorToStencil(NLS::Render::Data::PipelineState p_pso, Engine::GameObject& p_actor)
{
    if (p_actor.IsActive())
    {
        const bool hasCameraComponent = p_actor.GetComponent<Engine::Components::CameraComponent>() != nullptr;

        /* Render static mesh outline and bounding spheres */
        if (auto modelRenderer = p_actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer && modelRenderer->GetModel())
        {
            if (!(hasCameraComponent && IsEditorCameraIconModel(*modelRenderer)))
                DrawModelToStencil(p_pso, p_actor.GetTransform()->GetWorldMatrix(), *modelRenderer->GetModel());
        }

        /* Render camera component outline */
        if (auto cameraComponent = p_actor.GetComponent<Engine::Components::CameraComponent>(); cameraComponent)
        {
            auto translation = Maths::Matrix4::Translation(p_actor.GetTransform()->GetWorldPosition());
            auto rotation = Maths::Quaternion::ToMatrix4(p_actor.GetTransform()->GetWorldRotation());
            auto model = translation * rotation;
            DrawModelToStencil(p_pso, model, *EDITOR_CONTEXT(editorResources)->GetModel("Camera"));
        }

        for (auto& child : p_actor.GetChildren())
        {
            DrawActorToStencil(p_pso, *child);
        }
    }
}

void Editor::Rendering::OutlineRenderFeature::DrawActorOutline(
    NLS::Render::Data::PipelineState p_pso,
    Engine::GameObject& p_actor,
    float p_thickness)
{
    if (p_actor.IsActive())
    {
        const bool hasCameraComponent = p_actor.GetComponent<Engine::Components::CameraComponent>() != nullptr;

        if (auto modelRenderer = p_actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer && modelRenderer->GetModel())
        {
            if (!(hasCameraComponent && IsEditorCameraIconModel(*modelRenderer)))
            {
                const auto outlineModel = BuildOutlineShellMatrix(
                    p_actor.GetTransform()->GetWorldMatrix(),
                    p_actor.GetTransform()->GetWorldScale(),
                    *modelRenderer->GetModel(),
                    p_thickness);
                DrawModelOutline(p_pso, outlineModel, *modelRenderer->GetModel());
            }
        }

        if (auto cameraComponent = p_actor.GetComponent<Engine::Components::CameraComponent>(); cameraComponent)
        {
            auto& cameraModel = *EDITOR_CONTEXT(editorResources)->GetModel("Camera");
            auto translation = Maths::Matrix4::Translation(p_actor.GetTransform()->GetWorldPosition());
            auto rotation = Maths::Quaternion::ToMatrix4(p_actor.GetTransform()->GetWorldRotation());
            auto model = translation * rotation;
            const auto outlineModel = BuildOutlineShellMatrix(
                model,
                Maths::Vector3::One,
                cameraModel,
                p_thickness);
            DrawModelOutline(p_pso, outlineModel, cameraModel);
        }

        for (auto& child : p_actor.GetChildren())
        {
            DrawActorOutline(p_pso, *child, p_thickness);
        }
    }
}

void Editor::Rendering::OutlineRenderFeature::DrawModelToStencil(
    NLS::Render::Data::PipelineState p_pso,
    const Maths::Matrix4& p_worldMatrix,
    NLS::Render::Resources::Model& p_model)
{
    m_renderer.GetFeature<DebugModelRenderFeature>()
        .DrawModelWithSingleMaterial(p_pso, p_model, m_stencilFillMaterial, p_worldMatrix);
}

void Editor::Rendering::OutlineRenderFeature::DrawModelOutline(
    NLS::Render::Data::PipelineState p_pso,
    const Maths::Matrix4& p_worldMatrix,
    NLS::Render::Resources::Model& p_model)
{
    m_renderer.GetFeature<DebugModelRenderFeature>()
        .DrawModelWithSingleMaterial(p_pso, p_model, m_outlineMaterial, p_worldMatrix);
}
