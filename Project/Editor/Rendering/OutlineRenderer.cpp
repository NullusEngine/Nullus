#include "Rendering/OutlineRenderer.h"

#include "Core/EditorActions.h"
#include "Rendering/DebugModelRenderer.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/EditorDefaultResources.h"
#include "Rendering/EditorPipelineStatePresets.h"

#include <Components/CameraComponent.h>
#include <Components/MeshFilter.h>
#include <Components/MeshRenderer.h>
#include <Components/TransformComponent.h>

#include <algorithm>
#include <Rendering/Resources/Mesh.h>

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
    auto* editorCameraMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Camera");
    auto* owner = modelRenderer.gameobject();
    auto* meshFilter = owner != nullptr ? owner->GetComponent<Engine::Components::MeshFilter>() : nullptr;
    return editorCameraMesh != nullptr &&
        meshFilter != nullptr &&
        meshFilter->ResolveMesh() == editorCameraMesh;
}

Maths::Matrix4 BuildOutlineShellMatrix(
    const Maths::Matrix4& worldMatrix,
    const Maths::Vector3& worldScale,
    NLS::Render::Resources::Mesh& mesh,
    const float thickness)
{
    const auto& bounds = mesh.GetBoundingSphere();
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

void Editor::Rendering::OutlineRenderer::CaptureOutlineDrawCommands(
    Engine::GameObject& actor,
    const Maths::Vector4& color,
    const float thickness,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    CaptureStencilPass(actor, outDrawCommands);
    CaptureOutlinePass(actor, color, thickness, outDrawCommands);
}

void Editor::Rendering::OutlineRenderer::CaptureStencilPass(
    Engine::GameObject& actor,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    auto pso = Editor::Rendering::CreateEditorOutlineStencilPipelineState(
        m_renderer,
        kStencilMask,
        kStencilReference);

    CaptureGameObjectToStencil(pso, actor, outDrawCommands);
}

void Editor::Rendering::OutlineRenderer::CaptureOutlinePass(
    Engine::GameObject& actor,
    const Maths::Vector4& color,
    const float thickness,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    auto pso = Editor::Rendering::CreateEditorOutlineShellPipelineState(
        m_renderer,
        kStencilReference,
        kStencilMask);

    m_outlineMaterial.Set("u_Diffuse", color);
    CaptureGameObjectOutline(pso, actor, thickness, outDrawCommands);
}

void Editor::Rendering::OutlineRenderer::DrawStencilPass(Engine::GameObject& actor)
{
    auto pso = Editor::Rendering::CreateEditorOutlineStencilPipelineState(
        m_renderer,
        kStencilMask,
        kStencilReference);

    DrawGameObjectToStencil(pso, actor);
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
    DrawGameObjectOutline(pso, actor, thickness);
}

void Editor::Rendering::OutlineRenderer::CaptureGameObjectToStencil(
    NLS::Render::Data::PipelineState pso,
    Engine::GameObject& actor,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    if (!actor.IsActive())
        return;

    const bool hasCameraComponent = actor.GetComponent<Engine::Components::CameraComponent>() != nullptr;

    if (auto modelRenderer = actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer)
    {
        auto* meshFilter = actor.GetComponent<Engine::Components::MeshFilter>();
        auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
        if (mesh != nullptr && !(hasCameraComponent && IsEditorCameraIconModel(*modelRenderer)))
            m_debugModelRenderer.CaptureMeshDrawCommandsWithSingleMaterial(
                pso,
                *mesh,
                m_stencilFillMaterial,
                actor.GetTransform()->GetWorldMatrix(),
                outDrawCommands);
    }

    if (auto cameraComponent = actor.GetComponent<Engine::Components::CameraComponent>(); cameraComponent)
    {
        auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
        auto model = translation * rotation;
        if (auto* cameraMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Camera"))
            CaptureMeshToStencil(pso, model, *cameraMesh, outDrawCommands);
    }

    for (auto& child : actor.GetChildren())
        CaptureGameObjectToStencil(pso, *child, outDrawCommands);
}

void Editor::Rendering::OutlineRenderer::DrawGameObjectToStencil(
    NLS::Render::Data::PipelineState pso,
    Engine::GameObject& actor)
{
    if (!actor.IsActive())
        return;

    const bool hasCameraComponent = actor.GetComponent<Engine::Components::CameraComponent>() != nullptr;

    if (auto modelRenderer = actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer)
    {
        auto* meshFilter = actor.GetComponent<Engine::Components::MeshFilter>();
        auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
        if (mesh != nullptr && !(hasCameraComponent && IsEditorCameraIconModel(*modelRenderer)))
            m_debugModelRenderer.DrawMeshWithSingleMaterial(
                pso,
                *mesh,
                m_stencilFillMaterial,
                actor.GetTransform()->GetWorldMatrix());
    }

    if (auto cameraComponent = actor.GetComponent<Engine::Components::CameraComponent>(); cameraComponent)
    {
        auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
        auto model = translation * rotation;
        if (auto* cameraMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Camera"))
            DrawMeshToStencil(pso, model, *cameraMesh);
    }

    for (auto& child : actor.GetChildren())
        DrawGameObjectToStencil(pso, *child);
}

void Editor::Rendering::OutlineRenderer::CaptureGameObjectOutline(
    NLS::Render::Data::PipelineState pso,
    Engine::GameObject& actor,
    const float thickness,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    if (!actor.IsActive())
        return;

    const bool hasCameraComponent = actor.GetComponent<Engine::Components::CameraComponent>() != nullptr;

    if (auto modelRenderer = actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer)
    {
        auto* meshFilter = actor.GetComponent<Engine::Components::MeshFilter>();
        auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
        if (mesh != nullptr && !(hasCameraComponent && IsEditorCameraIconModel(*modelRenderer)))
        {
            const auto outlineModel = BuildOutlineShellMatrix(
                actor.GetTransform()->GetWorldMatrix(),
                actor.GetTransform()->GetWorldScale(),
                *mesh,
                thickness);
            m_debugModelRenderer.CaptureMeshDrawCommandsWithSingleMaterial(
                pso,
                *mesh,
                m_outlineMaterial,
                outlineModel,
                outDrawCommands);
        }
    }

    if (auto cameraComponent = actor.GetComponent<Engine::Components::CameraComponent>(); cameraComponent)
    {
        auto* cameraMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Camera");
        if (cameraMesh == nullptr)
            return;
        auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
        auto model = translation * rotation;
        const auto outlineModel = BuildOutlineShellMatrix(
            model,
            Maths::Vector3::One,
            *cameraMesh,
            thickness);
        CaptureMeshOutline(pso, outlineModel, *cameraMesh, outDrawCommands);
    }

    for (auto& child : actor.GetChildren())
        CaptureGameObjectOutline(pso, *child, thickness, outDrawCommands);
}

void Editor::Rendering::OutlineRenderer::DrawGameObjectOutline(
    NLS::Render::Data::PipelineState pso,
    Engine::GameObject& actor,
    const float thickness)
{
    if (!actor.IsActive())
        return;

    const bool hasCameraComponent = actor.GetComponent<Engine::Components::CameraComponent>() != nullptr;

    if (auto modelRenderer = actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer)
    {
        auto* meshFilter = actor.GetComponent<Engine::Components::MeshFilter>();
        auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
        if (mesh != nullptr && !(hasCameraComponent && IsEditorCameraIconModel(*modelRenderer)))
        {
            const auto outlineModel = BuildOutlineShellMatrix(
                actor.GetTransform()->GetWorldMatrix(),
                actor.GetTransform()->GetWorldScale(),
                *mesh,
                thickness);
            m_debugModelRenderer.DrawMeshWithSingleMaterial(
                pso,
                *mesh,
                m_outlineMaterial,
                outlineModel);
        }
    }

    if (auto cameraComponent = actor.GetComponent<Engine::Components::CameraComponent>(); cameraComponent)
    {
        auto* cameraMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Camera");
        if (cameraMesh == nullptr)
            return;
        auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
        auto model = translation * rotation;
        const auto outlineModel = BuildOutlineShellMatrix(
            model,
            Maths::Vector3::One,
            *cameraMesh,
            thickness);
        DrawMeshOutline(pso, outlineModel, *cameraMesh);
    }

    for (auto& child : actor.GetChildren())
        DrawGameObjectOutline(pso, *child, thickness);
}

void Editor::Rendering::OutlineRenderer::CaptureMeshToStencil(
    NLS::Render::Data::PipelineState pso,
    const Maths::Matrix4& worldMatrix,
    NLS::Render::Resources::Mesh& mesh,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    m_debugModelRenderer.CaptureMeshDrawCommandsWithSingleMaterial(
        pso,
        mesh,
        m_stencilFillMaterial,
        worldMatrix,
        outDrawCommands);
}

void Editor::Rendering::OutlineRenderer::DrawMeshToStencil(
    NLS::Render::Data::PipelineState pso,
    const Maths::Matrix4& worldMatrix,
    NLS::Render::Resources::Mesh& mesh)
{
    m_debugModelRenderer.DrawMeshWithSingleMaterial(pso, mesh, m_stencilFillMaterial, worldMatrix);
}

void Editor::Rendering::OutlineRenderer::CaptureMeshOutline(
    NLS::Render::Data::PipelineState pso,
    const Maths::Matrix4& worldMatrix,
    NLS::Render::Resources::Mesh& mesh,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    m_debugModelRenderer.CaptureMeshDrawCommandsWithSingleMaterial(
        pso,
        mesh,
        m_outlineMaterial,
        worldMatrix,
        outDrawCommands);
}

void Editor::Rendering::OutlineRenderer::DrawMeshOutline(
    NLS::Render::Data::PipelineState pso,
    const Maths::Matrix4& worldMatrix,
    NLS::Render::Resources::Mesh& mesh)
{
    m_debugModelRenderer.DrawMeshWithSingleMaterial(pso, mesh, m_outlineMaterial, worldMatrix);
}
