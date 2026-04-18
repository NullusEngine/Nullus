#pragma once

#include <Math/Matrix4.h>
#include <Rendering/Data/PipelineState.h>

namespace NLS::Render::Core { class CompositeRenderer; }
namespace NLS::Render::Resources { class Material; class Model; }

namespace NLS::Editor::Rendering
{
/**
 * Utility for drawing every mesh in a model with the same material.
 */
class DebugModelRenderer
{
public:
    explicit DebugModelRenderer(NLS::Render::Core::CompositeRenderer& renderer);

    void DrawModelWithSingleMaterial(
        NLS::Render::Data::PipelineState pso,
        NLS::Render::Resources::Model& model,
        NLS::Render::Resources::Material& material,
        const Maths::Matrix4& modelMatrix);

private:
    NLS::Render::Core::CompositeRenderer& m_renderer;
};
}
