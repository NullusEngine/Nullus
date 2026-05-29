#pragma once

#include <GameObject.h>
#include <Math/Matrix4.h>
#include <Math/Vector3.h>
#include <Resources/Material.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>

#include <optional>
#include <vector>

#include "Rendering/DebugGameObjectSelectionCollector.h"
#include "Rendering/EditorHelperLifecycle.h"

namespace NLS::Render::Core { class CompositeRenderer; }
namespace NLS::Render::Resources { class Mesh; }

namespace NLS::Editor::Rendering
{
class DebugModelRenderer;

/**
 * Draws editor selection outlines with explicit renderer-owned helpers.
 */
class OutlineRenderer
{
public:
    OutlineRenderer(
        NLS::Render::Core::CompositeRenderer& renderer,
        DebugModelRenderer& debugModelRenderer);

    static bool ShouldIncludeInThreadedFrame(bool gameObjectPassEnabled, const Engine::GameObject* selectedGameObject)
    {
        ThreadedEditorHelperState helperState;
        helperState.gameObjectPassEnabled = gameObjectPassEnabled;
        helperState.hasSelectedGameObject = selectedGameObject != nullptr;
        return HasThreadedOutlineHelperPass(helperState);
    }

    void CaptureOutlineDrawCommands(
        Engine::GameObject& actor,
        const Maths::Vector4& color,
        float thickness,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void CaptureOutlineDrawCommands(
        const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems,
        const Maths::Vector4& color,
        float thickness,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void DrawOutline(Engine::GameObject& actor, const Maths::Vector4& color, float thickness);
    void DrawOutline(
        const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems,
        const Maths::Vector4& color,
        float thickness);
    bool PrepareOutlineDrawItems(const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems);
    void DrawPreparedOutline(const Maths::Vector4& color, float thickness);

private:
    struct OutlineDrawItem
    {
        NLS::Render::Resources::Mesh* mesh = nullptr;
        Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
        Maths::Vector3 worldScale = Maths::Vector3::One;
    };

    void CollectOutlineDrawItems(
        Engine::GameObject& actor,
        std::vector<OutlineDrawItem>& outItems,
        NLS::Render::Resources::Mesh* cameraMesh) const;
    bool PrepareOutlineScratchItems(const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems);
    void CaptureStencilPass(
        const std::vector<OutlineDrawItem>& outlineItems,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void CaptureOutlinePass(
        const std::vector<OutlineDrawItem>& outlineItems,
        const Maths::Vector4& color,
        float thickness,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void CaptureMeshToStencil(
        NLS::Render::Data::PipelineState pso,
        const Maths::Matrix4& worldMatrix,
        NLS::Render::Resources::Mesh& mesh,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void CaptureMeshOutline(
        NLS::Render::Data::PipelineState pso,
        const Maths::Matrix4& worldMatrix,
        NLS::Render::Resources::Mesh& mesh,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void DrawStencilPass(const std::vector<OutlineDrawItem>& outlineItems);
    void DrawOutlinePass(
        const std::vector<OutlineDrawItem>& outlineItems,
        const Maths::Vector4& color,
        float thickness);
    void ApplyOutlineMaterialColor(const Maths::Vector4& color);
    void DrawMeshToStencil(NLS::Render::Data::PipelineState pso, const Maths::Matrix4& worldMatrix, NLS::Render::Resources::Mesh& mesh);
    void DrawMeshOutline(NLS::Render::Data::PipelineState pso, const Maths::Matrix4& worldMatrix, NLS::Render::Resources::Mesh& mesh);
    bool PrepareOutlineScratchItems(Engine::GameObject& actor);

private:
    NLS::Render::Core::CompositeRenderer& m_renderer;
    DebugModelRenderer& m_debugModelRenderer;
    NLS::Render::Resources::Material m_stencilFillMaterial;
    NLS::Render::Resources::Material m_outlineMaterial;
    std::vector<OutlineDrawItem> m_outlineScratchItems;
    std::optional<Maths::Vector4> m_lastAppliedOutlineColor;
};
}
