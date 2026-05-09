#include "Rendering/RHI/Backends/DX12/DX12GraphicsPipelineUtils.h"

#if defined(_WIN32)
#include <algorithm>
#include <climits>

namespace
{
    DXGI_FORMAT ToDXGIFormat(const uint32_t elementSize)
    {
        switch (elementSize)
        {
        case 8u:
            return DXGI_FORMAT_R32G32_FLOAT;
        case 12u:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case 16u:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case 4u:
        default:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
    }

    const char* ToSemanticName(const uint32_t location)
    {
        switch (location)
        {
        case 0u:
            return "POSITION";
        case 1u:
            return "TEXCOORD";
        case 2u:
            return "NORMAL";
        case 3u:
        case 4u:
            return "TEXCOORD";
        default:
            return "COLOR";
        }
    }

    UINT ToSemanticIndex(const uint32_t location)
    {
        switch (location)
        {
        case 1u:
            return 0u;
        case 3u:
            return 1u;
        case 4u:
            return 2u;
        default:
            return 0u;
        }
    }

    D3D12_BLEND ToD3D12Blend(const NLS::Render::RHI::RHIBlendFactor factor)
    {
        switch (factor)
        {
        case NLS::Render::RHI::RHIBlendFactor::Zero: return D3D12_BLEND_ZERO;
        case NLS::Render::RHI::RHIBlendFactor::One: return D3D12_BLEND_ONE;
        case NLS::Render::RHI::RHIBlendFactor::SrcColor: return D3D12_BLEND_SRC_COLOR;
        case NLS::Render::RHI::RHIBlendFactor::InvSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
        case NLS::Render::RHI::RHIBlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
        case NLS::Render::RHI::RHIBlendFactor::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case NLS::Render::RHI::RHIBlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
        case NLS::Render::RHI::RHIBlendFactor::InvDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        case NLS::Render::RHI::RHIBlendFactor::DstColor: return D3D12_BLEND_DEST_COLOR;
        case NLS::Render::RHI::RHIBlendFactor::InvDstColor: return D3D12_BLEND_INV_DEST_COLOR;
        default:
            return D3D12_BLEND_ONE;
        }
    }

    D3D12_BLEND_OP ToD3D12BlendOp(const NLS::Render::RHI::RHIBlendOp op)
    {
        switch (op)
        {
        case NLS::Render::RHI::RHIBlendOp::Add: return D3D12_BLEND_OP_ADD;
        case NLS::Render::RHI::RHIBlendOp::Subtract: return D3D12_BLEND_OP_SUBTRACT;
        case NLS::Render::RHI::RHIBlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
        case NLS::Render::RHI::RHIBlendOp::Min: return D3D12_BLEND_OP_MIN;
        case NLS::Render::RHI::RHIBlendOp::Max: return D3D12_BLEND_OP_MAX;
        default:
            return D3D12_BLEND_OP_ADD;
        }
    }

    UINT8 ToD3D12ColorWriteMask(const NLS::Render::RHI::RHIColorWriteMask mask)
    {
        UINT8 d3dMask = 0u;
        if (NLS::Render::RHI::HasColorWriteMask(mask, NLS::Render::RHI::RHIColorWriteMask::Red))
            d3dMask |= D3D12_COLOR_WRITE_ENABLE_RED;
        if (NLS::Render::RHI::HasColorWriteMask(mask, NLS::Render::RHI::RHIColorWriteMask::Green))
            d3dMask |= D3D12_COLOR_WRITE_ENABLE_GREEN;
        if (NLS::Render::RHI::HasColorWriteMask(mask, NLS::Render::RHI::RHIColorWriteMask::Blue))
            d3dMask |= D3D12_COLOR_WRITE_ENABLE_BLUE;
        if (NLS::Render::RHI::HasColorWriteMask(mask, NLS::Render::RHI::RHIColorWriteMask::Alpha))
            d3dMask |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
        return d3dMask;
    }

    D3D12_RENDER_TARGET_BLEND_DESC ToD3D12RenderTargetBlendState(
        const NLS::Render::RHI::RHIRenderTargetBlendStateDesc& desc)
    {
        D3D12_RENDER_TARGET_BLEND_DESC target = {};
        target.BlendEnable = desc.blendEnable ? TRUE : FALSE;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = ToD3D12Blend(desc.srcColor);
        target.DestBlend = ToD3D12Blend(desc.dstColor);
        target.BlendOp = ToD3D12BlendOp(desc.colorOp);
        target.SrcBlendAlpha = ToD3D12Blend(desc.srcAlpha);
        target.DestBlendAlpha = ToD3D12Blend(desc.dstAlpha);
        target.BlendOpAlpha = ToD3D12BlendOp(desc.alphaOp);
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = ToD3D12ColorWriteMask(desc.colorWriteMask);
        return target;
    }
}

namespace NLS::Render::RHI::DX12
{
    DX12OwnedInputLayout BuildDX12OwnedInputLayout(const RHIGraphicsPipelineDesc& desc)
    {
        DX12OwnedInputLayout inputLayout;
        inputLayout.semanticNames.reserve(desc.vertexAttributes.size());
        inputLayout.elements.reserve(desc.vertexAttributes.size());

        for (const RHIVertexAttributeDesc& attribute : desc.vertexAttributes)
        {
            bool perInstance = false;
            for (const RHIVertexBufferLayoutDesc& bufferLayout : desc.vertexBuffers)
            {
                if (bufferLayout.binding == attribute.binding)
                {
                    perInstance = bufferLayout.perInstance;
                    break;
                }
            }

            const char* semanticName = ToSemanticName(attribute.location);
            inputLayout.semanticNames.emplace_back(semanticName);

            D3D12_INPUT_ELEMENT_DESC element = {};
            element.SemanticName = semanticName;
            element.SemanticIndex = ToSemanticIndex(attribute.location);
            element.Format = ToDXGIFormat(attribute.elementSize);
            element.InputSlot = attribute.binding;
            element.AlignedByteOffset = attribute.offset;
            element.InputSlotClass = perInstance
                ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            element.InstanceDataStepRate = perInstance ? 1u : 0u;
            inputLayout.elements.push_back(element);
        }

        return inputLayout;
    }

    D3D12_BLEND_DESC BuildDX12BlendState(const RHIGraphicsPipelineDesc& desc)
    {
        D3D12_BLEND_DESC blendState = {};
        blendState.AlphaToCoverageEnable = desc.blendState.alphaToCoverageEnable ? TRUE : FALSE;
        blendState.IndependentBlendEnable = desc.blendState.independentBlendEnable ? TRUE : FALSE;

        RHIRenderTargetBlendStateDesc legacyTarget;
        legacyTarget.blendEnable = desc.blendState.enabled;
        legacyTarget.colorWriteMask = desc.blendState.colorWrite ? RHIColorWriteMask::All : RHIColorWriteMask::None;

        const size_t requestedTargetCount = std::max<size_t>(1u, desc.blendState.renderTargets.size());
        const size_t targetCount = std::min<size_t>(8u, requestedTargetCount);
        for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex)
        {
            const auto& targetDesc = targetIndex < desc.blendState.renderTargets.size()
                ? desc.blendState.renderTargets[targetIndex]
                : legacyTarget;
            blendState.RenderTarget[targetIndex] = ToD3D12RenderTargetBlendState(targetDesc);
        }

        for (size_t targetIndex = targetCount; targetIndex < 8u; ++targetIndex)
            blendState.RenderTarget[targetIndex] = ToD3D12RenderTargetBlendState(legacyTarget);

        return blendState;
    }

    D3D12_RASTERIZER_DESC BuildDX12RasterizerState(const RHIGraphicsPipelineDesc& desc)
    {
        D3D12_RASTERIZER_DESC rasterizerState = {};
        rasterizerState.FillMode = desc.rasterState.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
        rasterizerState.CullMode = desc.rasterState.cullEnabled
            ? (desc.rasterState.cullFace == NLS::Render::Settings::ECullFace::FRONT ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK)
            : D3D12_CULL_MODE_NONE;
        rasterizerState.FrontCounterClockwise = TRUE;
        rasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        rasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        rasterizerState.DepthClipEnable = TRUE;
        rasterizerState.MultisampleEnable = desc.rasterState.multisampleEnable ? TRUE : FALSE;
        rasterizerState.AntialiasedLineEnable = FALSE;
        rasterizerState.ForcedSampleCount = 0;
        rasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        return rasterizerState;
    }
}
#endif
