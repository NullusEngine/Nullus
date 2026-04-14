#include "Rendering/RHI/Backends/DX12/DX12GraphicsPipelineUtils.h"

#if defined(_WIN32)
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
}
#endif
