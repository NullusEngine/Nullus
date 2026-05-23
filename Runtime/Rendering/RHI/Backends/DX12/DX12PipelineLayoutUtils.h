#pragma once

#include <cstdint>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIPipeline.h"

#if defined(_WIN32)
#include <d3d12.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::RHI::DX12
{
    enum class NLS_RENDER_API DX12DescriptorHeapKind : uint8_t
    {
        Resource = 0,
        Sampler = 1
    };

    enum class NLS_RENDER_API DX12DescriptorRangeCategory : uint8_t
    {
        ConstantBuffer = 0,
        ShaderResource = 1,
        UnorderedAccess = 2,
        Sampler = 3
    };

    struct NLS_RENDER_API DX12DescriptorTableRangeDesc
    {
        DX12DescriptorRangeCategory category = DX12DescriptorRangeCategory::ConstantBuffer;
        uint32_t registerSpace = 0;
        uint32_t binding = 0;
        uint32_t descriptorCount = 1;
        uint32_t elementStride = 0;
    };

    struct NLS_RENDER_API DX12DescriptorTableDesc
    {
        DX12DescriptorHeapKind heapKind = DX12DescriptorHeapKind::Resource;
        DX12DescriptorRangeCategory category = DX12DescriptorRangeCategory::ConstantBuffer;
        uint32_t set = 0;
        uint32_t registerSpace = 0;
        std::vector<DX12DescriptorTableRangeDesc> ranges;
    };

    NLS_RENDER_API std::vector<DX12DescriptorTableDesc> BuildDX12DescriptorTableDescs(const RHIPipelineLayoutDesc& desc);
    NLS_RENDER_API bool AreDX12DescriptorTablesCompatible(
        const DX12DescriptorTableDesc& expected,
        const DX12DescriptorTableDesc& actual);

    struct NLS_RENDER_API DX12PushConstantRootParameterDesc
    {
        NLS::Render::RHI::ShaderStageMask stageMask = NLS::Render::RHI::ShaderStageMask::AllGraphics;
        uint32_t offset = 0u;
        uint32_t size = 0u;
        uint32_t rootParameterIndex = 0u;
    };

#if defined(_WIN32)
    struct NLS_RENDER_API DX12OwnedRootParameters
    {
        std::vector<DX12DescriptorTableDesc> descriptorTables;
        std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> descriptorRanges;
        uint32_t pushConstantRootParameterOffset = 0u;
        std::vector<DX12PushConstantRootParameterDesc> pushConstantRootParameters;
        std::vector<D3D12_ROOT_PARAMETER> rootParameters;
    };

    NLS_RENDER_API DX12OwnedRootParameters BuildDX12OwnedRootParameters(const RHIPipelineLayoutDesc& desc);
#endif
}
