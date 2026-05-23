#pragma once

#include <vector>

#include <GameObject.h>
#include <Resources/Material.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>

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
    void DrawOutline(Engine::GameObject& actor, const Maths::Vector4& color, float thickness);

private:
    void CaptureStencilPass(
        Engine::GameObject& actor,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void CaptureOutlinePass(
        Engine::GameObject& actor,
        const Maths::Vector4& color,
        float thickness,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void CaptureGameObjectToStencil(
        NLS::Render::Data::PipelineState pso,
        Engine::GameObject& actor,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void CaptureGameObjectOutline(
        NLS::Render::Data::PipelineState pso,
        Engine::GameObject& actor,
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
    void DrawStencilPass(Engine::GameObject& actor);
    void DrawOutlinePass(Engine::GameObject& actor, const Maths::Vector4& color, float thickness);
    void DrawGameObjectToStencil(NLS::Render::Data::PipelineState pso, Engine::GameObject& actor);
    void DrawGameObjectOutline(NLS::Render::Data::PipelineState pso, Engine::GameObject& actor, float thickness);
    void DrawMeshToStencil(NLS::Render::Data::PipelineState pso, const Maths::Matrix4& worldMatrix, NLS::Render::Resources::Mesh& mesh);
    void DrawMeshOutline(NLS::Render::Data::PipelineState pso, const Maths::Matrix4& worldMatrix, NLS::Render::Resources::Mesh& mesh);

private:
    NLS::Render::Core::CompositeRenderer& m_renderer;
    DebugModelRenderer& m_debugModelRenderer;
    NLS::Render::Resources::Material m_stencilFillMaterial;
    NLS::Render::Resources::Material m_outlineMaterial;
};
}
