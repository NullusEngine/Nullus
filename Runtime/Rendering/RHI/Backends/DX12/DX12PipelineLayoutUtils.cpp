#include "Rendering/RHI/Backends/DX12/DX12PipelineLayoutUtils.h"

#include <algorithm>
#include <map>

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
        case BindingType::StorageBuffer:
            return DX12DescriptorRangeCategory::ConstantBuffer;
        case BindingType::Texture:
        case BindingType::RWTexture:
            return DX12DescriptorRangeCategory::ShaderResource;
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
                    entry.count
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

#if defined(_WIN32)
    DX12OwnedRootParameters BuildDX12OwnedRootParameters(const RHIPipelineLayoutDesc& desc)
    {
        DX12OwnedRootParameters owned;
        owned.descriptorTables = BuildDX12DescriptorTableDescs(desc);
        const auto& tables = owned.descriptorTables;

        owned.descriptorRanges.reserve(tables.size());
        owned.rootParameters.reserve(tables.size());

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

        return owned;
    }
#endif
}
