#include <gtest/gtest.h>

#include "Rendering/RHI/Backends/DX12/DX12PipelineLayoutUtils.h"

#include <filesystem>
#include <fstream>
#include <sstream>

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

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream stream;
        stream << input.rdbuf();
        return stream.str();
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

TEST(DX12PipelineLayoutUtilsTests, BindingLayoutsCarryStructuredBufferElementStride)
{
    RHIPipelineLayoutDesc desc;
    desc.bindingLayouts = {
        MakeLayout({
            {
                "ObjectData",
                BindingType::StructuredBuffer,
                2u,
                0u,
                1u,
                NLS::Render::RHI::ShaderStageMask::Vertex,
                3u,
                64u
            }
        })
    };

    const auto tables = NLS::Render::RHI::DX12::BuildDX12DescriptorTableDescs(desc);

    ASSERT_EQ(tables.size(), 1u);
    ASSERT_EQ(tables[0].ranges.size(), 1u);
    EXPECT_EQ(tables[0].ranges[0].elementStride, 64u);
}

TEST(DX12PipelineLayoutUtilsTests, DescriptorTableCompatibilityIncludesRegistersCountsTypesAndStructuredStride)
{
    NLS::Render::RHI::DX12::DX12DescriptorTableDesc expected;
    expected.heapKind = NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource;
    expected.category = NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ShaderResource;
    expected.set = 2u;
    expected.registerSpace = 3u;
    expected.ranges.push_back({
        NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ShaderResource,
        3u,
        0u,
        1u,
        64u
    });

    auto compatible = expected;
    EXPECT_TRUE(NLS::Render::RHI::DX12::AreDX12DescriptorTablesCompatible(expected, compatible));

    auto wrongBinding = expected;
    wrongBinding.ranges[0].binding = 1u;
    EXPECT_FALSE(NLS::Render::RHI::DX12::AreDX12DescriptorTablesCompatible(expected, wrongBinding));

    auto wrongSpace = expected;
    wrongSpace.ranges[0].registerSpace = 4u;
    EXPECT_FALSE(NLS::Render::RHI::DX12::AreDX12DescriptorTablesCompatible(expected, wrongSpace));

    auto wrongCount = expected;
    wrongCount.ranges[0].descriptorCount = 2u;
    EXPECT_FALSE(NLS::Render::RHI::DX12::AreDX12DescriptorTablesCompatible(expected, wrongCount));

    auto wrongStride = expected;
    wrongStride.ranges[0].elementStride = 4u;
    EXPECT_FALSE(NLS::Render::RHI::DX12::AreDX12DescriptorTablesCompatible(expected, wrongStride));
}

TEST(DX12PipelineLayoutUtilsTests, DX12CommandChecksBindingSetTableCompatibilityBeforeBinding)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto bindBindingSet = source.find("void NativeDX12CommandBuffer::BindBindingSet");
    ASSERT_NE(bindBindingSet, std::string::npos);
    const auto body = source.substr(bindBindingSet);
    const auto compatibilityCheck = body.find("IsCompatibleWithDescriptorTable(table)");
    const auto gpuHandleLookup = body.find("GetGPUHandle(table.set, table.heapKind)");
    ASSERT_NE(gpuHandleLookup, std::string::npos);
    ASSERT_NE(compatibilityCheck, std::string::npos);
    EXPECT_LT(compatibilityCheck, gpuHandleLookup);
}

TEST(DX12PipelineLayoutUtilsTests, DX12CommandFailsClosedWhenRequiredRootDescriptorTablesAreMissing)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto helper = source.find("bool NativeDX12CommandBuffer::HasInitializedRequiredRootDescriptorTables");
    ASSERT_NE(helper, std::string::npos);

    const auto drawIndexed = source.find("void NativeDX12CommandBuffer::DrawIndexed");
    ASSERT_NE(drawIndexed, std::string::npos);
    const auto drawIndexedCall = source.find("DrawIndexedInstanced", drawIndexed);
    ASSERT_NE(drawIndexedCall, std::string::npos);
    const auto drawIndexedGuard = source.rfind("HasInitializedRequiredRootDescriptorTables", drawIndexedCall);
    ASSERT_NE(drawIndexedGuard, std::string::npos);
    EXPECT_GT(drawIndexedGuard, drawIndexed);

    const auto dispatch = source.find("void NativeDX12CommandBuffer::Dispatch");
    ASSERT_NE(dispatch, std::string::npos);
    const auto dispatchCall = source.find("m_commandList->Dispatch", dispatch);
    ASSERT_NE(dispatchCall, std::string::npos);
    const auto dispatchGuard = source.rfind("HasInitializedRequiredRootDescriptorTables", dispatchCall);
    ASSERT_NE(dispatchGuard, std::string::npos);
    EXPECT_GT(dispatchGuard, dispatch);
}

TEST(DX12PipelineLayoutUtilsTests, DX12DrawRequiresBoundGraphicsPipeline)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto draw = source.find("void NativeDX12CommandBuffer::Draw(");
    ASSERT_NE(draw, std::string::npos);
    const auto drawInstanced = source.find("DrawInstanced", draw);
    ASSERT_NE(drawInstanced, std::string::npos);
    const auto pipelineGuard = source.rfind("m_boundPipeline == nullptr", drawInstanced);
    ASSERT_NE(pipelineGuard, std::string::npos);
    EXPECT_GT(pipelineGuard, draw);
}

TEST(DX12PipelineLayoutUtilsTests, DX12CommandRejectsInvalidNativePipelinesBeforeDrawOrDispatch)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto bindGraphics = source.find("void NativeDX12CommandBuffer::BindGraphicsPipeline");
    ASSERT_NE(bindGraphics, std::string::npos);
    const auto bindCompute = source.find("void NativeDX12CommandBuffer::BindComputePipeline");
    ASSERT_NE(bindCompute, std::string::npos);

    const auto graphicsPsoCheck = source.find("pso == nullptr", bindGraphics);
    ASSERT_NE(graphicsPsoCheck, std::string::npos);
    EXPECT_LT(graphicsPsoCheck, bindCompute);
    const auto graphicsRootCheck = source.find("rootSig == nullptr", bindGraphics);
    ASSERT_NE(graphicsRootCheck, std::string::npos);
    EXPECT_LT(graphicsRootCheck, bindCompute);

    const auto computePsoCheck = source.find("pso == nullptr", bindCompute);
    ASSERT_NE(computePsoCheck, std::string::npos);
    const auto computeRootCheck = source.find("rootSig == nullptr", bindCompute);
    ASSERT_NE(computeRootCheck, std::string::npos);

    const auto clearHelper = source.find("void NativeDX12CommandBuffer::ClearBoundPipelineState");
    ASSERT_NE(clearHelper, std::string::npos);
    const auto graphicsClear = source.find("ClearBoundPipelineState()", bindGraphics);
    ASSERT_NE(graphicsClear, std::string::npos);
    EXPECT_LT(graphicsClear, bindCompute);
    const auto computeClear = source.find("ClearBoundPipelineState()", bindCompute);
    ASSERT_NE(computeClear, std::string::npos);

    const auto draw = source.find("void NativeDX12CommandBuffer::Draw(");
    ASSERT_NE(draw, std::string::npos);
    const auto drawInstanced = source.find("DrawInstanced", draw);
    ASSERT_NE(drawInstanced, std::string::npos);
    const auto drawNativeGuard = source.rfind("IsBoundGraphicsPipelineNativeValid()", drawInstanced);
    ASSERT_NE(drawNativeGuard, std::string::npos);
    EXPECT_GT(drawNativeGuard, draw);

    const auto dispatch = source.find("void NativeDX12CommandBuffer::Dispatch");
    ASSERT_NE(dispatch, std::string::npos);
    const auto dispatchCall = source.find("m_commandList->Dispatch", dispatch);
    ASSERT_NE(dispatchCall, std::string::npos);
    const auto dispatchNativeGuard = source.rfind("IsBoundComputePipelineNativeValid()", dispatchCall);
    ASSERT_NE(dispatchNativeGuard, std::string::npos);
    EXPECT_GT(dispatchNativeGuard, dispatch);
}

TEST(DX12PipelineLayoutUtilsTests, DX12PipelineCreationRejectsMissingNativeRootSignatureOrPso)
{
    const auto pipelineSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Pipeline.cpp");
    const auto deviceSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");

    EXPECT_NE(pipelineSource.find("if (m_desc.pipelineLayout != nullptr && !usedLayoutRootSignature)"), std::string::npos);
    EXPECT_NE(deviceSource.find("pipeline->IsValid() ? pipeline : nullptr"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12StructuredBufferDescriptorsRejectUnalignedRanges)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp");

    const auto firstElement = source.find("const UINT firstElement = static_cast<UINT>(boundEntry->bufferOffset / elementStride);");
    ASSERT_NE(firstElement, std::string::npos);
    const auto unalignedOffset = source.rfind("boundEntry->bufferOffset % elementStride", firstElement);
    const auto unalignedRange = source.rfind("bufferSize % elementStride", firstElement);
    ASSERT_NE(unalignedOffset, std::string::npos);
    ASSERT_NE(unalignedRange, std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12NullStorageBufferDescriptorsUseLayoutStride)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp");

    EXPECT_NE(source.find("WriteNullStorageBufferDescriptor(destination, layoutEntry->elementStride)"), std::string::npos);
    EXPECT_NE(source.find("uavDesc.Buffer.StructureByteStride = elementStride != 0u ? elementStride : sizeof(uint32_t);"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, NativePipelineLayoutCreatesRootSignatureForPushConstantOnlyLayouts)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Pipeline.cpp");

    EXPECT_NE(source.find("const auto ownedRootParameters = NLS::Render::RHI::DX12::BuildDX12OwnedRootParameters(m_desc);"), std::string::npos);
    EXPECT_NE(source.find("rootSigDesc.NumParameters = static_cast<UINT>(ownedRootParameters.rootParameters.size());"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, NativePipelineLayoutCreatesRootSignatureForExplicitEmptyLayouts)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Pipeline.cpp");

    EXPECT_NE(source.find("rootSigDesc.pParameters = ownedRootParameters.rootParameters.empty() ? nullptr : ownedRootParameters.rootParameters.data()"), std::string::npos);
    EXPECT_NE(source.find("return m_rootSignature != nullptr;"), std::string::npos);
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

TEST(DX12PipelineLayoutUtilsTests, RootParametersAppendPushConstantsAfterDescriptorTables)
{
    RHIPipelineLayoutDesc desc;
    desc.bindingLayouts = {
        MakeLayout({
            { "ObjectData", BindingType::StructuredBuffer, 2u, 0u, 1u, NLS::Render::RHI::ShaderStageMask::Vertex, 3u }
        })
    };
    desc.pushConstants.push_back({
        NLS::Render::RHI::ShaderStageMask::Vertex,
        0u,
        sizeof(uint32_t),
        1u,
        3u
    });

    const auto owned = NLS::Render::RHI::DX12::BuildDX12OwnedRootParameters(desc);

    ASSERT_EQ(owned.rootParameters.size(), 2u);
    EXPECT_EQ(owned.rootParameters[0].ParameterType, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
    EXPECT_EQ(owned.rootParameters[1].ParameterType, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    EXPECT_EQ(owned.rootParameters[1].Constants.Num32BitValues, 1u);
    EXPECT_EQ(owned.rootParameters[1].Constants.ShaderRegister, 1u);
    EXPECT_EQ(owned.rootParameters[1].Constants.RegisterSpace, 3u);
}

TEST(DX12PipelineLayoutUtilsTests, RootParametersSupportPushConstantOnlyPipelineLayouts)
{
    RHIPipelineLayoutDesc desc;
    desc.pushConstants.push_back({
        NLS::Render::RHI::ShaderStageMask::Vertex,
        0u,
        sizeof(uint32_t),
        1u,
        3u
    });

    const auto owned = NLS::Render::RHI::DX12::BuildDX12OwnedRootParameters(desc);

    EXPECT_TRUE(owned.descriptorTables.empty());
    EXPECT_EQ(owned.pushConstantRootParameterOffset, 0u);
    ASSERT_EQ(owned.rootParameters.size(), 1u);
    ASSERT_EQ(owned.pushConstantRootParameters.size(), 1u);
    EXPECT_EQ(owned.rootParameters[0].ParameterType, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    EXPECT_EQ(owned.rootParameters[0].Constants.Num32BitValues, 1u);
    EXPECT_EQ(owned.rootParameters[0].Constants.ShaderRegister, 1u);
    EXPECT_EQ(owned.rootParameters[0].Constants.RegisterSpace, 3u);
    EXPECT_EQ(owned.pushConstantRootParameters[0].rootParameterIndex, 0u);
}

TEST(DX12PipelineLayoutUtilsTests, RootParametersTrackEachPushConstantRangeRootIndex)
{
    RHIPipelineLayoutDesc desc;
    desc.bindingLayouts = {
        MakeLayout({
            { "FrameConstants", BindingType::UniformBuffer, 0u, 0u, 1u, NLS::Render::RHI::ShaderStageMask::AllGraphics, 0u },
            { "ObjectData", BindingType::StructuredBuffer, 2u, 0u, 1u, NLS::Render::RHI::ShaderStageMask::Vertex, 3u }
        })
    };
    desc.pushConstants.push_back({
        NLS::Render::RHI::ShaderStageMask::Vertex,
        0u,
        sizeof(uint32_t),
        1u,
        3u
    });
    desc.pushConstants.push_back({
        NLS::Render::RHI::ShaderStageMask::Fragment,
        sizeof(uint32_t),
        sizeof(uint32_t) * 2u,
        2u,
        3u
    });

    const auto owned = NLS::Render::RHI::DX12::BuildDX12OwnedRootParameters(desc);

    ASSERT_EQ(owned.rootParameters.size(), 4u);
    ASSERT_EQ(owned.pushConstantRootParameters.size(), 2u);

    EXPECT_EQ(owned.pushConstantRootParameterOffset, 2u);
    EXPECT_EQ(owned.pushConstantRootParameters[0].stageMask, NLS::Render::RHI::ShaderStageMask::Vertex);
    EXPECT_EQ(owned.pushConstantRootParameters[0].offset, 0u);
    EXPECT_EQ(owned.pushConstantRootParameters[0].size, sizeof(uint32_t));
    EXPECT_EQ(owned.pushConstantRootParameters[0].rootParameterIndex, 2u);

    EXPECT_EQ(owned.pushConstantRootParameters[1].stageMask, NLS::Render::RHI::ShaderStageMask::Fragment);
    EXPECT_EQ(owned.pushConstantRootParameters[1].offset, sizeof(uint32_t));
    EXPECT_EQ(owned.pushConstantRootParameters[1].size, sizeof(uint32_t) * 2u);
    EXPECT_EQ(owned.pushConstantRootParameters[1].rootParameterIndex, 3u);

    EXPECT_EQ(owned.rootParameters[2].ParameterType, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    EXPECT_EQ(owned.rootParameters[2].Constants.ShaderRegister, 1u);
    EXPECT_EQ(owned.rootParameters[3].ParameterType, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    EXPECT_EQ(owned.rootParameters[3].Constants.ShaderRegister, 2u);
}
#endif
