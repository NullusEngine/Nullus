#pragma once

#include <GameObject.h>
#include <Resources/Material.h>

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

    void DrawOutline(Engine::GameObject& actor, const Maths::Vector4& color, float thickness);

private:
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
