#include "Rendering/Resources/MaterialCompatibilityDrawState.h"

#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/IRenderDevice.h"
#include "Rendering/Resources/Material.h"

namespace NLS::Render::Resources
{
    const Data::PipelineState& MaterialCompatibilityDrawState::GetPipelineState() const
    {
        return m_pipelineState;
    }

    void MaterialCompatibilityDrawState::Bind(Context::Driver& driver) const
    {
        driver.BindGraphicsPipeline(m_pipelineDesc, m_hasBindingSet ? &m_bindingSetStorage : nullptr);
    }

    void MaterialCompatibilityDrawState::Bind(RHI::IRenderDevice& renderDevice) const
    {
        renderDevice.BindGraphicsPipeline(m_pipelineDesc, m_hasBindingSet ? &m_bindingSetStorage : nullptr);
    }

    MaterialCompatibilityDrawState BuildMaterialCompatibilityDrawState(
        const Material& material,
        Data::PipelineState pipelineState,
        Settings::EPrimitiveMode primitiveMode,
        Settings::EComparaisonAlgorithm depthCompare)
    {
        MaterialCompatibilityDrawState drawState;
        drawState.m_pipelineDesc = material.BuildGraphicsPipelineDesc();
        drawState.m_pipelineDesc.primitiveMode = primitiveMode;
        // Preserve pass-level depth overrides such as skybox LESS_EQUAL while
        // still baking the final compare op into backend pipeline creation.
        drawState.m_pipelineDesc.depthStencilState.depthCompare = depthCompare;

        drawState.m_pipelineState = pipelineState;
        drawState.m_pipelineState.depthWriting = drawState.m_pipelineDesc.depthStencilState.depthWrite;
        drawState.m_pipelineState.colorWriting.mask = drawState.m_pipelineDesc.blendState.colorWrite ? 0xFF : 0x00;
        drawState.m_pipelineState.blending = drawState.m_pipelineDesc.blendState.enabled;
        drawState.m_pipelineState.culling = drawState.m_pipelineDesc.rasterState.culling;
        drawState.m_pipelineState.depthTest = drawState.m_pipelineDesc.depthStencilState.depthTest;

        if (drawState.m_pipelineState.culling)
            drawState.m_pipelineState.cullFace = drawState.m_pipelineDesc.rasterState.cullFace;

        drawState.m_bindingSetStorage = material.GetBindingSetInstance();
        drawState.m_hasBindingSet = true;
        return drawState;
    }
}
