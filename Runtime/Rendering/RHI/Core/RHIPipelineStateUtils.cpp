#include "Rendering/RHI/Core/RHIPipelineStateUtils.h"

namespace NLS::Render::RHI
{
    void ApplyPipelineStateToGraphicsPipelineDesc(
        const NLS::Render::Data::PipelineState& pipelineState,
        RHIGraphicsPipelineDesc& desc)
    {
        desc.rasterState.cullEnabled = pipelineState.culling;
        desc.rasterState.cullFace = pipelineState.cullFace;
        desc.rasterState.wireframe =
            pipelineState.rasterizationMode == NLS::Render::Settings::ERasterizationMode::LINE;
        desc.blendState.colorWrite = pipelineState.colorWriting.mask != 0x00;
        desc.depthStencilState.depthTest = pipelineState.depthTest;
        desc.depthStencilState.depthWrite = pipelineState.depthWriting;
        desc.depthStencilState.depthCompare = pipelineState.depthFunc;
        desc.depthStencilState.stencilTest = pipelineState.stencilTest;
        desc.depthStencilState.stencilReadMask = pipelineState.stencilFuncMask;
        desc.depthStencilState.stencilWriteMask = pipelineState.stencilWriteMask;
        desc.depthStencilState.stencilReference = pipelineState.stencilFuncRef;
        desc.depthStencilState.stencilCompare = pipelineState.stencilFuncOp;
        desc.depthStencilState.stencilFailOp = pipelineState.stencilOpFail;
        desc.depthStencilState.stencilDepthFailOp = pipelineState.depthOpFail;
        desc.depthStencilState.stencilPassOp = pipelineState.bothOpFail;
    }
}
