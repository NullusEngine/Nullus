#include "Rendering/DebugModelRenderer.h"

#include "Engine/Rendering/EngineDrawableDescriptor.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Model.h"

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

void Editor::Rendering::DebugModelRenderer::DrawModelWithSingleMaterial(
    NLS::Render::Data::PipelineState pso,
    NLS::Render::Resources::Model& model,
    NLS::Render::Resources::Material& material,
    const Maths::Matrix4& modelMatrix)
{
    for (auto mesh : model.GetMeshes())
    {
        auto element = BuildDebugModelDrawable(material, modelMatrix, mesh);
        m_renderer.DrawEntity(pso, element);
    }
}

void Editor::Rendering::DebugModelRenderer::CaptureModelDrawCommandsWithSingleMaterial(
    NLS::Render::Data::PipelineState pso,
    NLS::Render::Resources::Model& model,
    NLS::Render::Resources::Material& material,
    const Maths::Matrix4& modelMatrix,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    for (auto mesh : model.GetMeshes())
    {
        auto element = BuildDebugModelDrawable(material, modelMatrix, mesh);
        NLS::Render::Context::RecordedDrawCommandInput drawCommand;
        if (m_renderer.CaptureRecordedDrawCommand(pso, element, drawCommand))
            outDrawCommands.push_back(std::move(drawCommand));
    }
}
