#include "Rendering/DebugModelRenderFeature.h"
#include "Rendering/OutlineRenderFeature.h"
#include "Core/EditorActions.h"
#include "Settings/EditorSettings.h"

#include <Components/MaterialRenderer.h>

#include <Components/TransformComponent.h>

#include <Rendering/Utils/Conversions.h>

constexpr uint32_t kStencilMask = 0xFF;
constexpr int32_t kStencilReference = 1;
using namespace NLS;
Editor::Rendering::OutlineRenderFeature::OutlineRenderFeature(NLS::Rendering::Core::CompositeRenderer& p_renderer)
    : NLS::Rendering::Features::ARenderFeature(p_renderer)
{
    /* Stencil Fill Material */
    m_stencilFillMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders/Unlit.glsl"]);
    m_stencilFillMaterial.SetBackfaceCulling(true);
    m_stencilFillMaterial.SetDepthTest(false);
    m_stencilFillMaterial.SetColorWriting(false);
    m_stencilFillMaterial.Set<NLS::Rendering::Resources::Texture*>("u_DiffuseMap", nullptr);

    /* Outline Material */
    m_outlineMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders/Unlit.glsl"]);
    m_outlineMaterial.Set<NLS::Rendering::Resources::Texture*>("u_DiffuseMap", nullptr);
    m_outlineMaterial.SetDepthTest(false);
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
    auto pso = m_renderer.CreatePipelineState();

    pso.stencilTest = true;
    pso.stencilWriteMask = kStencilMask;
    pso.stencilFuncRef = kStencilReference;
    pso.stencilFuncMask = kStencilMask;
    pso.stencilOpFail = NLS::Rendering::Settings::EOperation::REPLACE;
    pso.depthOpFail = NLS::Rendering::Settings::EOperation::REPLACE;
    pso.bothOpFail = NLS::Rendering::Settings::EOperation::REPLACE;
    pso.colorWriting.mask = 0x00;

    DrawActorToStencil(pso, p_actor);
}

void Editor::Rendering::OutlineRenderFeature::DrawOutlinePass(Engine::GameObject& p_actor, const Maths::Vector4& p_color, float p_thickness)
{
    auto pso = m_renderer.CreatePipelineState();

    pso.stencilTest = true;
    pso.stencilOpFail = NLS::Rendering::Settings::EOperation::KEEP;
    pso.depthOpFail = NLS::Rendering::Settings::EOperation::KEEP;
    pso.bothOpFail = NLS::Rendering::Settings::EOperation::REPLACE;
    pso.stencilFuncOp = NLS::Rendering::Settings::EComparaisonAlgorithm::NOTEQUAL;
    pso.stencilFuncRef = kStencilReference;
    pso.stencilFuncMask = kStencilMask;
    pso.rasterizationMode = NLS::Rendering::Settings::ERasterizationMode::LINE;
    pso.lineWidthPow2 = NLS::Rendering::Utils::Conversions::FloatToPow2(p_thickness);

    // Prepare the outline material
    m_outlineMaterial.Set("u_Diffuse", p_color);

    DrawActorOutline(pso, p_actor);
}

void Editor::Rendering::OutlineRenderFeature::DrawActorToStencil(NLS::Rendering::Data::PipelineState p_pso, Engine::GameObject& p_actor)
{
    if (p_actor.IsActive())
    {
        /* Render static mesh outline and bounding spheres */
        if (auto modelRenderer = p_actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer && modelRenderer->GetModel())
        {
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

void Editor::Rendering::OutlineRenderFeature::DrawActorOutline(NLS::Rendering::Data::PipelineState p_pso, Engine::GameObject& p_actor)
{
    if (p_actor.IsActive())
    {
        if (auto modelRenderer = p_actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer && modelRenderer->GetModel())
        {
            DrawModelOutline(p_pso, p_actor.GetTransform()->GetWorldMatrix(), *modelRenderer->GetModel());
        }

        if (auto cameraComponent = p_actor.GetComponent<Engine::Components::CameraComponent>(); cameraComponent)
        {
            auto translation = Maths::Matrix4::Translation(p_actor.GetTransform()->GetWorldPosition());
            auto rotation = Maths::Quaternion::ToMatrix4(p_actor.GetTransform()->GetWorldRotation());
            auto model = translation * rotation;
            DrawModelOutline(p_pso, model, *EDITOR_CONTEXT(editorResources)->GetModel("Camera"));
        }

        for (auto& child : p_actor.GetChildren())
        {
            DrawActorOutline(p_pso, *child);
        }
    }
}

void Editor::Rendering::OutlineRenderFeature::DrawModelToStencil(
    NLS::Rendering::Data::PipelineState p_pso,
    const Maths::Matrix4& p_worldMatrix,
    NLS::Rendering::Resources::Model& p_model)
{
    m_renderer.GetFeature<DebugModelRenderFeature>()
        .DrawModelWithSingleMaterial(p_pso, p_model, m_stencilFillMaterial, p_worldMatrix);
}

void Editor::Rendering::OutlineRenderFeature::DrawModelOutline(
    NLS::Rendering::Data::PipelineState p_pso,
    const Maths::Matrix4& p_worldMatrix,
    NLS::Rendering::Resources::Model& p_model)
{
    m_renderer.GetFeature<DebugModelRenderFeature>()
        .DrawModelWithSingleMaterial(p_pso, p_model, m_outlineMaterial, p_worldMatrix);
}
