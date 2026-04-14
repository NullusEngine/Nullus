#include <gtest/gtest.h>

#include "Rendering/RHI/Backends/DX12/DX12PipelineLayoutUtils.h"

namespace
{
    using NLS::Render::RHI::BindingType;
    using NLS::Render::RHI::RHIBindingLayout;
    using NLS::Render::RHI::RHIBindingLayoutDesc;
    using NLS::Render::RHI::RHIPipelineLayoutDesc;

    class TestBindingLayout final : public RHIBindingLayout
    {
    public:
        explicit TestBindingLayout(RHIBindingLayoutDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

    private:
        RHIBindingLayoutDesc m_desc;
    };

    std::shared_ptr<RHIBindingLayout> MakeLayout(std::initializer_list<NLS::Render::RHI::RHIBindingLayoutEntry> entries)
    {
        RHIBindingLayoutDesc desc;
        desc.entries.assign(entries.begin(), entries.end());
        desc.debugName = "TestLayout";
        return std::make_shared<TestBindingLayout>(std::move(desc));
    }
}

TEST(DX12PipelineLayoutUtilsTests, GroupsEntriesByLogicalSetAndDescriptorHeap)
{
    RHIPipelineLayoutDesc desc;
    desc.bindingLayouts = {
        MakeLayout({
            { "MaterialTextures", BindingType::Texture, 1u, 3u, 4u, NLS::Render::RHI::ShaderStageMask::AllGraphics, 2u },
            { "MaterialSampler", BindingType::Sampler, 1u, 0u, 1u, NLS::Render::RHI::ShaderStageMask::AllGraphics, 2u }
        }),
        MakeLayout({
            { "ObjectConstants", BindingType::UniformBuffer, 2u, 5u, 1u, NLS::Render::RHI::ShaderStageMask::AllGraphics, 3u },
            { "FrameConstants", BindingType::UniformBuffer, 2u, 1u, 2u, NLS::Render::RHI::ShaderStageMask::AllGraphics, 3u }
        })
    };

    const auto tables = NLS::Render::RHI::DX12::BuildDX12DescriptorTableDescs(desc);

    ASSERT_EQ(tables.size(), 3u);

    EXPECT_EQ(tables[0].heapKind, NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource);
    EXPECT_EQ(tables[0].set, 1u);
    EXPECT_EQ(tables[0].registerSpace, 2u);
    ASSERT_EQ(tables[0].ranges.size(), 1u);
    EXPECT_EQ(tables[0].ranges[0].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ShaderResource);
    EXPECT_EQ(tables[0].ranges[0].binding, 3u);
    EXPECT_EQ(tables[0].ranges[0].descriptorCount, 4u);

    EXPECT_EQ(tables[1].heapKind, NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler);
    EXPECT_EQ(tables[1].set, 1u);
    EXPECT_EQ(tables[1].registerSpace, 2u);
    ASSERT_EQ(tables[1].ranges.size(), 1u);
    EXPECT_EQ(tables[1].ranges[0].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::Sampler);
    EXPECT_EQ(tables[1].ranges[0].binding, 0u);
    EXPECT_EQ(tables[1].ranges[0].descriptorCount, 1u);

    EXPECT_EQ(tables[2].heapKind, NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource);
    EXPECT_EQ(tables[2].set, 2u);
    EXPECT_EQ(tables[2].registerSpace, 3u);
    ASSERT_EQ(tables[2].ranges.size(), 2u);
    EXPECT_EQ(tables[2].ranges[0].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ConstantBuffer);
    EXPECT_EQ(tables[2].ranges[0].binding, 1u);
    EXPECT_EQ(tables[2].ranges[0].descriptorCount, 2u);
    EXPECT_EQ(tables[2].ranges[1].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ConstantBuffer);
    EXPECT_EQ(tables[2].ranges[1].binding, 5u);
    EXPECT_EQ(tables[2].ranges[1].descriptorCount, 1u);
}

TEST(DX12PipelineLayoutUtilsTests, KeepsResourceRangesForOneLogicalSetInOneDescriptorTable)
{
    RHIPipelineLayoutDesc desc;
    desc.bindingLayouts = {
        MakeLayout({
            { "FrameConstants", BindingType::UniformBuffer, 0u, 0u, 1u, NLS::Render::RHI::ShaderStageMask::AllGraphics, 0u }
        }),
        MakeLayout({
            { "MaterialConstants", BindingType::UniformBuffer, 1u, 0u, 1u, NLS::Render::RHI::ShaderStageMask::AllGraphics, 2u },
            { "cubeTex", BindingType::Texture, 1u, 0u, 1u, NLS::Render::RHI::ShaderStageMask::Fragment, 2u },
            { "u_LinearWrapSampler", BindingType::Sampler, 1u, 0u, 1u, NLS::Render::RHI::ShaderStageMask::Fragment, 2u }
        })
    };

    const auto tables = NLS::Render::RHI::DX12::BuildDX12DescriptorTableDescs(desc);

    ASSERT_EQ(tables.size(), 3u);

    EXPECT_EQ(tables[0].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ConstantBuffer);
    EXPECT_EQ(tables[0].registerSpace, 0u);
    ASSERT_EQ(tables[0].ranges.size(), 1u);
    EXPECT_EQ(tables[0].ranges[0].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ConstantBuffer);

    EXPECT_EQ(tables[1].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ConstantBuffer);
    EXPECT_EQ(tables[1].registerSpace, 2u);
    ASSERT_EQ(tables[1].ranges.size(), 2u);
    EXPECT_EQ(tables[1].ranges[0].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ConstantBuffer);
    EXPECT_EQ(tables[1].ranges[0].binding, 0u);
    EXPECT_EQ(tables[1].ranges[1].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ShaderResource);
    EXPECT_EQ(tables[1].ranges[1].binding, 0u);

    EXPECT_EQ(tables[2].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::Sampler);
    EXPECT_EQ(tables[2].registerSpace, 2u);
    ASSERT_EQ(tables[2].ranges.size(), 1u);
    EXPECT_EQ(tables[2].ranges[0].category, NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::Sampler);
}

#if defined(_WIN32)
TEST(DX12PipelineLayoutUtilsTests, RootParametersReferenceOwnedDescriptorRanges)
{
    RHIPipelineLayoutDesc desc;
    desc.bindingLayouts = {
        MakeLayout({
            { "FrameConstants", BindingType::UniformBuffer, 0u, 0u, 1u, NLS::Render::RHI::ShaderStageMask::AllGraphics, 0u },
            { "LightingTextures", BindingType::Texture, 1u, 2u, 3u, NLS::Render::RHI::ShaderStageMask::AllGraphics, 2u }
        })
    };

    const auto owned = NLS::Render::RHI::DX12::BuildDX12OwnedRootParameters(desc);

    ASSERT_EQ(owned.rootParameters.size(), 2u);
    ASSERT_EQ(owned.descriptorRanges.size(), owned.rootParameters.size());

    for (size_t i = 0; i < owned.rootParameters.size(); ++i)
    {
        const auto& parameter = owned.rootParameters[i];
        const auto& ranges = owned.descriptorRanges[i];

        EXPECT_EQ(parameter.ParameterType, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
        EXPECT_EQ(parameter.DescriptorTable.NumDescriptorRanges, ranges.size());
        ASSERT_FALSE(ranges.empty());
        EXPECT_EQ(parameter.DescriptorTable.pDescriptorRanges, ranges.data());
    }

    EXPECT_EQ(owned.descriptorRanges[0][0].RangeType, D3D12_DESCRIPTOR_RANGE_TYPE_CBV);
    EXPECT_EQ(owned.descriptorRanges[0][0].RegisterSpace, 0u);
    EXPECT_EQ(owned.descriptorRanges[0][0].BaseShaderRegister, 0u);

    EXPECT_EQ(owned.descriptorRanges[1][0].RangeType, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
    EXPECT_EQ(owned.descriptorRanges[1][0].RegisterSpace, 2u);
    EXPECT_EQ(owned.descriptorRanges[1][0].BaseShaderRegister, 2u);
    EXPECT_EQ(owned.descriptorRanges[1][0].NumDescriptors, 3u);
}
#endif
