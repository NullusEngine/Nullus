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
#include <any>
#include <Profiling/Profiler.h>
#include <Rendering/Resources/Mesh.h>

constexpr uint32_t kStencilMask = 0xFF;
constexpr int32_t kStencilReference = 1;
constexpr float kOutlineWorldThicknessPerUnit = 0.0075f;
constexpr float kMinimumOutlineWorldThickness = 0.01f;
constexpr float kMinimumOutlineRadius = 0.001f;

using namespace NLS;

namespace
{
bool IsEditorCameraIconModel(
    Engine::Components::MeshRenderer& modelRenderer,
    NLS::Render::Resources::Mesh* editorCameraMesh,
    NLS::Render::Resources::Mesh* resolvedMesh = nullptr)
{
    auto* owner = modelRenderer.gameobject();
    auto* meshFilter = owner != nullptr ? owner->GetComponent<Engine::Components::MeshFilter>() : nullptr;
    auto* mesh = resolvedMesh != nullptr
        ? resolvedMesh
        : (meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr);
    return editorCameraMesh != nullptr &&
        mesh == editorCameraMesh;
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
    if (PrepareOutlineScratchItems(actor))
        DrawPreparedOutline(color, thickness);
}

void Editor::Rendering::OutlineRenderer::DrawOutline(
    const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems,
    const Maths::Vector4& color,
    const float thickness)
{
    if (PrepareOutlineDrawItems(debugDrawItems))
        DrawPreparedOutline(color, thickness);
}

bool Editor::Rendering::OutlineRenderer::PrepareOutlineDrawItems(
    const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems)
{
    return PrepareOutlineScratchItems(debugDrawItems);
}

void Editor::Rendering::OutlineRenderer::DrawPreparedOutline(
    const Maths::Vector4& color,
    const float thickness)
{
    DrawStencilPass(m_outlineScratchItems);
    DrawOutlinePass(m_outlineScratchItems, color, thickness);
}

void Editor::Rendering::OutlineRenderer::CaptureOutlineDrawCommands(
    Engine::GameObject& actor,
    const Maths::Vector4& color,
    const float thickness,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    NLS_PROFILE_NAMED_SCOPE("DebugGameObject::CaptureOutlineDrawCommands");

    if (PrepareOutlineScratchItems(actor))
    {
        outDrawCommands.reserve(outDrawCommands.size() + m_outlineScratchItems.size() * 2u);
        CaptureStencilPass(m_outlineScratchItems, outDrawCommands);
        CaptureOutlinePass(m_outlineScratchItems, color, thickness, outDrawCommands);
    }
}

void Editor::Rendering::OutlineRenderer::CaptureOutlineDrawCommands(
    const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems,
    const Maths::Vector4& color,
    const float thickness,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    NLS_PROFILE_NAMED_SCOPE("DebugGameObject::CaptureOutlineDrawCommands");

    if (PrepareOutlineScratchItems(debugDrawItems))
    {
        outDrawCommands.reserve(outDrawCommands.size() + m_outlineScratchItems.size() * 2u);
        CaptureStencilPass(m_outlineScratchItems, outDrawCommands);
        CaptureOutlinePass(m_outlineScratchItems, color, thickness, outDrawCommands);
    }
}

bool Editor::Rendering::OutlineRenderer::PrepareOutlineScratchItems(Engine::GameObject& actor)
{
    m_outlineScratchItems.clear();
    m_outlineScratchItems.reserve(16u);
    auto* cameraMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Camera");
    CollectOutlineDrawItems(actor, m_outlineScratchItems, cameraMesh);
    return !m_outlineScratchItems.empty();
}

bool Editor::Rendering::OutlineRenderer::PrepareOutlineScratchItems(
    const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems)
{
    m_outlineScratchItems.clear();
    auto* cameraMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Camera");
    m_outlineScratchItems.reserve(debugDrawItems.selectionMeshItems.size() + debugDrawItems.cameras.size());
    for (const auto& item : debugDrawItems.selectionMeshItems)
    {
        const bool isCameraIconModel =
            item.meshRenderer != nullptr &&
            item.meshRenderer->gameobject() != nullptr &&
            item.meshRenderer->gameobject()->GetComponent<Engine::Components::CameraComponent>() != nullptr &&
            IsEditorCameraIconModel(*item.meshRenderer, cameraMesh, item.mesh);
        if (item.mesh != nullptr && !isCameraIconModel)
            m_outlineScratchItems.push_back({ item.mesh, item.worldMatrix, item.worldScale });
    }

    if (cameraMesh == nullptr)
        return !m_outlineScratchItems.empty();

    for (const auto& cameraItem : debugDrawItems.cameras)
    {
        auto* cameraComponent = cameraItem.cameraComponent;
        auto* actor = cameraComponent != nullptr ? cameraComponent->gameobject() : nullptr;
        if (actor == nullptr)
            continue;

        auto translation = Maths::Matrix4::Translation(cameraItem.worldPosition);
        auto rotation = Maths::Quaternion::ToMatrix4(cameraItem.worldRotation);
        auto model = translation * rotation;
        m_outlineScratchItems.push_back({ cameraMesh, model, Maths::Vector3::One });
    }

    return !m_outlineScratchItems.empty();
}

void Editor::Rendering::OutlineRenderer::CollectOutlineDrawItems(
    Engine::GameObject& actor,
    std::vector<OutlineDrawItem>& outItems,
    NLS::Render::Resources::Mesh* cameraMesh) const
{
    if (!actor.IsActive())
        return;

    auto* cameraComponent = actor.GetComponent<Engine::Components::CameraComponent>();
    const bool hasCameraComponent = cameraComponent != nullptr;

    if (auto modelRenderer = actor.GetComponent<Engine::Components::MeshRenderer>(); modelRenderer)
    {
        auto* meshFilter = actor.GetComponent<Engine::Components::MeshFilter>();
        auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
        if (mesh != nullptr && !(hasCameraComponent && IsEditorCameraIconModel(*modelRenderer, cameraMesh, mesh)))
        {
            auto* transform = actor.GetTransform();
            outItems.push_back({ mesh, transform->GetWorldMatrix(), transform->GetWorldScale() });
        }
    }

    if (hasCameraComponent)
    {
        auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
        auto model = translation * rotation;
        if (cameraMesh != nullptr)
            outItems.push_back({ cameraMesh, model, Maths::Vector3::One });
    }

    for (auto& child : actor.GetChildren())
        CollectOutlineDrawItems(*child, outItems, cameraMesh);
}

void Editor::Rendering::OutlineRenderer::CaptureStencilPass(
    const std::vector<OutlineDrawItem>& outlineItems,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    NLS_PROFILE_NAMED_SCOPE("DebugGameObject::CaptureOutlineStencil");

    auto pso = Editor::Rendering::CreateEditorOutlineStencilPipelineState(
        m_renderer,
        kStencilMask,
        kStencilReference);

    for (const auto& item : outlineItems)
        CaptureMeshToStencil(pso, item.worldMatrix, *item.mesh, outDrawCommands);
}

void Editor::Rendering::OutlineRenderer::CaptureOutlinePass(
    const std::vector<OutlineDrawItem>& outlineItems,
    const Maths::Vector4& color,
    const float thickness,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    NLS_PROFILE_NAMED_SCOPE("DebugGameObject::CaptureOutlineShell");

    auto pso = Editor::Rendering::CreateEditorOutlineShellPipelineState(
        m_renderer,
        kStencilReference,
        kStencilMask);

    ApplyOutlineMaterialColor(color);
    for (const auto& item : outlineItems)
    {
        const auto outlineModel = BuildOutlineShellMatrix(
            item.worldMatrix,
            item.worldScale,
            *item.mesh,
            thickness);
        CaptureMeshOutline(pso, outlineModel, *item.mesh, outDrawCommands);
    }
}

void Editor::Rendering::OutlineRenderer::DrawStencilPass(const std::vector<OutlineDrawItem>& outlineItems)
{
    auto pso = Editor::Rendering::CreateEditorOutlineStencilPipelineState(
        m_renderer,
        kStencilMask,
        kStencilReference);

    for (const auto& item : outlineItems)
        DrawMeshToStencil(pso, item.worldMatrix, *item.mesh);
}

void Editor::Rendering::OutlineRenderer::DrawOutlinePass(
    const std::vector<OutlineDrawItem>& outlineItems,
    const Maths::Vector4& color,
    const float thickness)
{
    auto pso = Editor::Rendering::CreateEditorOutlineShellPipelineState(
        m_renderer,
        kStencilReference,
        kStencilMask);

    ApplyOutlineMaterialColor(color);
    for (const auto& item : outlineItems)
    {
        const auto outlineModel = BuildOutlineShellMatrix(
            item.worldMatrix,
            item.worldScale,
            *item.mesh,
            thickness);
        DrawMeshOutline(pso, outlineModel, *item.mesh);
    }
}

void Editor::Rendering::OutlineRenderer::ApplyOutlineMaterialColor(const Maths::Vector4& color)
{
    if (m_lastAppliedOutlineColor.has_value() && *m_lastAppliedOutlineColor == color)
    {
        const auto* cachedColor = m_outlineMaterial.GetParameterBlock().TryGet("u_Diffuse");
        if (cachedColor != nullptr &&
            cachedColor->type() == typeid(Maths::Vector4) &&
            std::any_cast<const Maths::Vector4&>(*cachedColor) == color)
        {
            return;
        }

        m_lastAppliedOutlineColor.reset();
    }

    const auto* appliedColor = m_outlineMaterial.GetParameterBlock().TryGet("u_Diffuse");
    if (appliedColor != nullptr &&
        appliedColor->type() == typeid(Maths::Vector4) &&
        std::any_cast<const Maths::Vector4&>(*appliedColor) == color)
    {
        m_lastAppliedOutlineColor = color;
        return;
    }

    if (!m_outlineMaterial.HasShader())
        return;

    m_outlineMaterial.Set("u_Diffuse", color);
    appliedColor = m_outlineMaterial.GetParameterBlock().TryGet("u_Diffuse");
    if (appliedColor != nullptr &&
        appliedColor->type() == typeid(Maths::Vector4) &&
        std::any_cast<const Maths::Vector4&>(*appliedColor) == color)
    {
        m_lastAppliedOutlineColor = color;
    }
    else
    {
        m_lastAppliedOutlineColor.reset();
    }
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
