#pragma once

#include <Rendering/Features/ARenderFeature.h>

namespace NLS
{
namespace Editor::Rendering
{
/**
 * Provide utility methods to draw a model quickly using a single material for all its submeshes
 */
class DebugModelRenderFeature : public NLS::Rendering::Features::ARenderFeature
{
public:
    /**
     * Constructor
     * @param p_renderer
     */
    DebugModelRenderFeature(NLS::Rendering::Core::CompositeRenderer& p_renderer);

    /**
     * Utility function to draw a whole model with a single material,
     * instead of drawing sub-meshes with their individual materials
     * @param p_pso
     * @param p_model
     * @param p_material
     * @param p_modelMatrix
     */
    virtual void DrawModelWithSingleMaterial(
        NLS::Rendering::Data::PipelineState p_pso,
        NLS::Rendering::Resources::Model& p_model,
        NLS::Rendering::Data::Material& p_material,
        const Maths::Matrix4& p_modelMatrix);
};
} // namespace Editor::Rendering

}
   