#include "Rendering/RHI/Core/RHIPipelineStateUtils.h"

namespace NLS::Render::RHI
{
    namespace
    {
        RHIColorWriteMask ToColorWriteMask(const NLS::Render::Data::PipelineState& pipelineState)
        {
            RHIColorWriteMask mask = RHIColorWriteMask::None;
            if (pipelineState.colorWriting.r)
                mask = mask | RHIColorWriteMask::Red;
            if (pipelineState.colorWriting.g)
                mask = mask | RHIColorWriteMask::Green;
            if (pipelineState.colorWriting.b)
                mask = mask | RHIColorWriteMask::Blue;
            if (pipelineState.colorWriting.a)
                mask = mask | RHIColorWriteMask::Alpha;
            return mask;
        }

        void ApplyLegacyBlendState(
            const NLS::Render::Data::PipelineState& pipelineState,
            RHIGraphicsPipelineDesc& desc)
        {
            const auto writeMask = ToColorWriteMask(pipelineState);
            const size_t renderTargetCount = std::max<size_t>(1u, desc.renderTargetLayout.colorFormats.size());
            desc.blendState.enabled = pipelineState.blending;
            desc.blendState.colorWrite = writeMask != RHIColorWriteMask::None;
            desc.blendState.alphaToCoverageEnable = pipelineState.sampleAlphaToCoverage;
            desc.blendState.independentBlendEnable = false;
            desc.blendState.renderTargets.assign(renderTargetCount, RHIRenderTargetBlendStateDesc{});

            for (auto& target : desc.blendState.renderTargets)
            {
                target.blendEnable = pipelineState.blending;
                target.colorWriteMask = writeMask;
            }
        }
    }

    void ApplyPipelineStateToGraphicsPipelineDesc(
        const NLS::Render::Data::PipelineState& pipelineState,
        RHIGraphicsPipelineDesc& desc)
    {
        desc.rasterState.cullEnabled = pipelineState.culling;
        desc.rasterState.cullFace = pipelineState.cullFace;
        desc.rasterState.wireframe =
            pipelineState.rasterizationMode == NLS::Render::Settings::ERasterizationMode::LINE;
        desc.rasterState.multisampleEnable = pipelineState.multisample;
        ApplyLegacyBlendState(pipelineState, desc);
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
