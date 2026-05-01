#pragma once

#include <vector>

#include <GameObject.h>
#include <Resources/Material.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>

#include "Rendering/EditorHelperLifecycle.h"

namespace NLS::Render::Core { class CompositeRenderer; }
namespace NLS::Render::Resources { class Model; }

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

    static bool ShouldIncludeInThreadedFrame(bool actorPassEnabled, const Engine::GameObject* selectedActor)
    {
        ThreadedEditorHelperState helperState;
        helperState.actorPassEnabled = actorPassEnabled;
        helperState.hasSelectedActor = selectedActor != nullptr;
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
    void CaptureActorToStencil(
        NLS::Render::Data::PipelineState pso,
        Engine::GameObject& actor,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void CaptureActorOutline(
        NLS::Render::Data::PipelineState pso,
        Engine::GameObject& actor,
        float thickness,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void CaptureModelToStencil(
        NLS::Render::Data::PipelineState pso,
        const Maths::Matrix4& worldMatrix,
        NLS::Render::Resources::Model& model,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void CaptureModelOutline(
        NLS::Render::Data::PipelineState pso,
        const Maths::Matrix4& worldMatrix,
        NLS::Render::Resources::Model& model,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void DrawStencilPass(Engine::GameObject& actor);
    void DrawOutlinePass(Engine::GameObject& actor, const Maths::Vector4& color, float thickness);
    void DrawActorToStencil(NLS::Render::Data::PipelineState pso, Engine::GameObject& actor);
    void DrawActorOutline(NLS::Render::Data::PipelineState pso, Engine::GameObject& actor, float thickness);
    void DrawModelToStencil(NLS::Render::Data::PipelineState pso, const Maths::Matrix4& worldMatrix, NLS::Render::Resources::Model& model);
    void DrawModelOutline(NLS::Render::Data::PipelineState pso, const Maths::Matrix4& worldMatrix, NLS::Render::Resources::Model& model);

private:
    NLS::Render::Core::CompositeRenderer& m_renderer;
    DebugModelRenderer& m_debugModelRenderer;
    NLS::Render::Resources::Material m_stencilFillMaterial;
    NLS::Render::Resources::Material m_outlineMaterial;
};
}
