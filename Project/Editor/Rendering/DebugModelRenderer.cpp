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

void Editor::Rendering::DebugModelRenderer::DrawModelWithSingleMaterial(
    NLS::Render::Data::PipelineState pso,
    NLS::Render::Resources::Model& model,
    NLS::Render::Resources::Material& material,
    const Maths::Matrix4& modelMatrix)
{
    auto stateMask = material.GenerateStateMask();

    auto engineDrawableDescriptor = Engine::Rendering::EngineDrawableDescriptor{
        modelMatrix,
        Maths::Matrix4::Identity
    };

    for (auto mesh : model.GetMeshes())
    {
        NLS::Render::Entities::Drawable element;
        element.mesh = mesh;
        element.material = &material;
        element.stateMask = stateMask;
        element.AddDescriptor(engineDrawableDescriptor);

        m_renderer.DrawEntity(pso, element);
    }
}
