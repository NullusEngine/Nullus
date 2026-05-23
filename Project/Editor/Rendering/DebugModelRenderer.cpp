#include "Rendering/DebugModelRenderer.h"

#include "Engine/Rendering/EngineDrawableDescriptor.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"

using namespace NLS;

Editor::Rendering::DebugModelRenderer::DebugModelRenderer(NLS::Render::Core::CompositeRenderer& renderer)
    : m_renderer(renderer)
{
}

namespace
{
    NLS::Render::Entities::Drawable BuildDebugModelDrawable(
        NLS::Render::Resources::Material& material,
        const Maths::Matrix4& modelMatrix,
        NLS::Render::Resources::Mesh* mesh)
    {
        auto element = NLS::Render::Entities::Drawable {};
        element.mesh = mesh;
        element.material = &material;
        element.stateMask = material.GenerateStateMask();
        element.AddDescriptor(Engine::Rendering::EngineDrawableDescriptor {
            modelMatrix,
            Maths::Matrix4::Identity
        });
        return element;
    }
}

void Editor::Rendering::DebugModelRenderer::DrawMeshWithSingleMaterial(
    NLS::Render::Data::PipelineState pso,
    NLS::Render::Resources::Mesh& mesh,
    NLS::Render::Resources::Material& material,
    const Maths::Matrix4& modelMatrix)
{
    auto element = BuildDebugModelDrawable(material, modelMatrix, &mesh);
    m_renderer.DrawEntity(pso, element);
}

void Editor::Rendering::DebugModelRenderer::CaptureMeshDrawCommandsWithSingleMaterial(
    NLS::Render::Data::PipelineState pso,
    NLS::Render::Resources::Mesh& mesh,
    NLS::Render::Resources::Material& material,
    const Maths::Matrix4& modelMatrix,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    auto element = BuildDebugModelDrawable(material, modelMatrix, &mesh);
    NLS::Render::Context::RecordedDrawCommandInput drawCommand;
    if (m_renderer.CaptureRecordedDrawCommand(pso, element, drawCommand))
        outDrawCommands.push_back(std::move(drawCommand));
}
