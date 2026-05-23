#include "Rendering/RHI/Backends/DX12/DX12PipelineLayoutUtils.h"

#include <algorithm>
#include <map>
#include <cstddef>

namespace
{
    NLS::Render::RHI::DX12::DX12DescriptorHeapKind ToDescriptorHeapKind(
        const NLS::Render::RHI::DX12::DX12DescriptorRangeCategory category)
    {
        using NLS::Render::RHI::DX12::DX12DescriptorHeapKind;
        using NLS::Render::RHI::DX12::DX12DescriptorRangeCategory;

        return category == DX12DescriptorRangeCategory::Sampler
            ? DX12DescriptorHeapKind::Sampler
            : DX12DescriptorHeapKind::Resource;
    }

    NLS::Render::RHI::DX12::DX12DescriptorRangeCategory ToDescriptorRangeCategory(const NLS::Render::RHI::BindingType type)
    {
        using NLS::Render::RHI::BindingType;
        using NLS::Render::RHI::DX12::DX12DescriptorRangeCategory;

        switch (type)
        {
        case BindingType::UniformBuffer:
            return DX12DescriptorRangeCategory::ConstantBuffer;
        case BindingType::StructuredBuffer:
        case BindingType::Texture:
            return DX12DescriptorRangeCategory::ShaderResource;
        case BindingType::StorageBuffer:
        case BindingType::RWTexture:
            return DX12DescriptorRangeCategory::UnorderedAccess;
        case BindingType::Sampler:
            return DX12DescriptorRangeCategory::Sampler;
        }

        return DX12DescriptorRangeCategory::ConstantBuffer;
    }

    struct DescriptorRangeKey
    {
        NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind = NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource;
        uint32_t set = 0;

        bool operator<(const DescriptorRangeKey& other) const
        {
            if (set != other.set)
                return set < other.set;
            return heapKind < other.heapKind;
        }
    };

#if defined(_WIN32)
    D3D12_DESCRIPTOR_RANGE_TYPE ToD3D12DescriptorRangeType(const NLS::Render::RHI::DX12::DX12DescriptorRangeCategory category)
    {
        using NLS::Render::RHI::DX12::DX12DescriptorRangeCategory;

        switch (category)
        {
        case DX12DescriptorRangeCategory::ConstantBuffer:
            return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        case DX12DescriptorRangeCategory::ShaderResource:
            return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        case DX12DescriptorRangeCategory::UnorderedAccess:
            return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        case DX12DescriptorRangeCategory::Sampler:
            return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        }

        return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    }
#endif
}

namespace NLS::Render::RHI::DX12
{
    std::vector<DX12DescriptorTableDesc> BuildDX12DescriptorTableDescs(const RHIPipelineLayoutDesc& desc)
    {
        std::map<DescriptorRangeKey, std::vector<DX12DescriptorTableRangeDesc>> groupedEntries;

        for (const auto& layout : desc.bindingLayouts)
        {
            if (layout == nullptr)
                continue;

            for (const auto& entry : layout->GetDesc().entries)
            {
                const DX12DescriptorRangeCategory category = ToDescriptorRangeCategory(entry.type);
                const DescriptorRangeKey key{ ToDescriptorHeapKind(category), entry.set };
                groupedEntries[key].push_back({
                    category,
                    entry.registerSpace,
                    entry.binding,
                    entry.count,
                    entry.elementStride
                });
            }
        }

        std::vector<DX12DescriptorTableDesc> tables;
        tables.reserve(groupedEntries.size());

        for (auto& [key, ranges] : groupedEntries)
        {
            std::sort(
                ranges.begin(),
                ranges.end(),
                [](const DX12DescriptorTableRangeDesc& lhs, const DX12DescriptorTableRangeDesc& rhs)
                {
                    if (lhs.category != rhs.category)
                        return lhs.category < rhs.category;
                    if (lhs.registerSpace != rhs.registerSpace)
                        return lhs.registerSpace < rhs.registerSpace;
                    return lhs.binding < rhs.binding;
                });

            tables.push_back({
                key.heapKind,
                ranges.empty() ? DX12DescriptorRangeCategory::ConstantBuffer : ranges.front().category,
                key.set,
                ranges.empty() ? 0u : ranges.front().registerSpace,
                std::move(ranges)
            });
        }

        return tables;
    }

    bool AreDX12DescriptorTablesCompatible(
        const DX12DescriptorTableDesc& expected,
        const DX12DescriptorTableDesc& actual)
    {
        if (expected.heapKind != actual.heapKind ||
            expected.category != actual.category ||
            expected.set != actual.set ||
            expected.registerSpace != actual.registerSpace ||
            expected.ranges.size() != actual.ranges.size())
        {
            return false;
        }

        for (size_t index = 0u; index < expected.ranges.size(); ++index)
        {
            const auto& expectedRange = expected.ranges[index];
            const auto& actualRange = actual.ranges[index];
            if (expectedRange.category != actualRange.category ||
                expectedRange.registerSpace != actualRange.registerSpace ||
                expectedRange.binding != actualRange.binding ||
                expectedRange.descriptorCount != actualRange.descriptorCount ||
                expectedRange.elementStride != actualRange.elementStride)
            {
                return false;
            }
        }

        return true;
    }

#if defined(_WIN32)
    DX12OwnedRootParameters BuildDX12OwnedRootParameters(const RHIPipelineLayoutDesc& desc)
    {
        DX12OwnedRootParameters owned;
        owned.descriptorTables = BuildDX12DescriptorTableDescs(desc);
        const auto& tables = owned.descriptorTables;
        owned.pushConstantRootParameterOffset = static_cast<uint32_t>(tables.size());

        owned.descriptorRanges.reserve(tables.size());
        owned.rootParameters.reserve(tables.size() + desc.pushConstants.size());

        for (const DX12DescriptorTableDesc& table : tables)
        {
            auto& ranges = owned.descriptorRanges.emplace_back();
            ranges.reserve(table.ranges.size());

            for (const DX12DescriptorTableRangeDesc& rangeDesc : table.ranges)
            {
                D3D12_DESCRIPTOR_RANGE range = {};
                range.RangeType = ToD3D12DescriptorRangeType(rangeDesc.category);
                range.NumDescriptors = rangeDesc.descriptorCount;
                range.BaseShaderRegister = rangeDesc.binding;
                range.RegisterSpace = rangeDesc.registerSpace;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                ranges.push_back(range);
            }

            D3D12_ROOT_PARAMETER rootParam = {};
            rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rootParam.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges.size());
            rootParam.DescriptorTable.pDescriptorRanges = ranges.empty() ? nullptr : ranges.data();
            rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            owned.rootParameters.push_back(rootParam);
        }

        for (const RHIPushConstantRange& pushConstant : desc.pushConstants)
        {
            const auto rootParameterIndex = static_cast<uint32_t>(owned.rootParameters.size());
            D3D12_ROOT_PARAMETER rootParam = {};
            rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rootParam.Constants.ShaderRegister = pushConstant.shaderRegister;
            rootParam.Constants.RegisterSpace = pushConstant.registerSpace;
            rootParam.Constants.Num32BitValues = pushConstant.size / sizeof(uint32_t);
            rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            owned.rootParameters.push_back(rootParam);
            owned.pushConstantRootParameters.push_back({
                pushConstant.stageMask,
                pushConstant.offset,
                pushConstant.size,
                rootParameterIndex
            });
        }

        return owned;
    }
#endif
}
