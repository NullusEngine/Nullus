#pragma once

#include <Math/Matrix4.h>
#include <Rendering/Data/PipelineState.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>

namespace NLS::Render::Core { class CompositeRenderer; }
namespace NLS::Render::Resources { class Material; class Mesh; }

namespace NLS::Editor::Rendering
{
/**
 * Utility for drawing editor helper meshes with a single material.
 */
class DebugModelRenderer
{
public:
    explicit DebugModelRenderer(NLS::Render::Core::CompositeRenderer& renderer);

    void DrawMeshWithSingleMaterial(
        NLS::Render::Data::PipelineState pso,
        NLS::Render::Resources::Mesh& mesh,
        NLS::Render::Resources::Material& material,
        const Maths::Matrix4& modelMatrix);
    void CaptureMeshDrawCommandsWithSingleMaterial(
        NLS::Render::Data::PipelineState pso,
        NLS::Render::Resources::Mesh& mesh,
        NLS::Render::Resources::Material& material,
        const Maths::Matrix4& modelMatrix,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);

private:
    NLS::Render::Core::CompositeRenderer& m_renderer;
};
}
