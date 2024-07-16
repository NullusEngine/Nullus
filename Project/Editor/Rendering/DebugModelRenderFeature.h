#pragma once

#include <Rendering/Features/ARenderFeature.h>

namespace NLS
{
namespace Editor::Rendering
{
/**
 * Provide utility methods to draw a model quickly using a single material for all its submeshes
 */
class DebugModelRenderFeature : public NLS::Render::Features::ARenderFeature
{
public:
    /**
     * Constructor
     * @param p_renderer
     */
    DebugModelRenderFeature(NLS::Render::Core::CompositeRenderer& p_renderer);

    /**
     * Utility function to draw a whole model with a single material,
     * instead of drawing sub-meshes with their individual materials
     * @param p_pso
     * @param p_model
     * @param p_material
     * @param p_modelMatrix
     */
    virtual void DrawModelWithSingleMaterial(
        NLS::Render::Data::PipelineState p_pso,
        NLS::Render::Resources::Model& p_model,
        NLS::Render::Resources::Material& p_material,
        const Maths::Matrix4& p_modelMatrix);
};
} // namespace Editor::Rendering

}
   