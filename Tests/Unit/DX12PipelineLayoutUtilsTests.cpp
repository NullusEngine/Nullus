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

    void ExpectJobHandleCompletionStatusThenClear(const std::string& source, const size_t expectedCount)
    {
        size_t cursor = 0u;
        for (size_t index = 0u; index < expectedCount; ++index)
        {
            SCOPED_TRACE(index);
            const auto completeNoClear = source.find("Jobs::CompleteNoClear(handle)", cursor);
            ASSERT_NE(completeNoClear, std::string::npos);
            const auto unexpectedClearBeforeComplete =
                source.find("Jobs::ClearWithoutSync(handle)", cursor);
            EXPECT_TRUE(unexpectedClearBeforeComplete == std::string::npos ||
                unexpectedClearBeforeComplete > completeNoClear);

            const auto completionStatus = source.find("GetJobCompletionStatus(handle)", completeNoClear);
            ASSERT_NE(completionStatus, std::string::npos);
            EXPECT_LT(completeNoClear, completionStatus);

            const auto clearWithoutSync = source.find("Jobs::ClearWithoutSync(handle)", completeNoClear);
            ASSERT_NE(clearWithoutSync, std::string::npos);
            EXPECT_LT(completionStatus, clearWithoutSync);

            cursor = clearWithoutSync + 1u;
        }

        EXPECT_EQ(source.find("Jobs::CompleteNoClear(handle)", cursor), std::string::npos);
        EXPECT_EQ(source.find("Jobs::ClearWithoutSync(handle)", cursor), std::string::npos);
        EXPECT_EQ(source.find("Jobs::Complete(handle)"), std::string::npos);
    }
}

TEST(DX12PipelineLayoutUtilsTests, DX12RuntimeDoesNotAdvertiseInRenderPassChildBundlesByDefault)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp");

    const auto parallelRecording = source.find("SetFeature(RHIDeviceFeature::ParallelCommandRecording, true)");
    ASSERT_NE(parallelRecording, std::string::npos);
    const auto parallelTranslation = source.find("SetFeature(RHIDeviceFeature::ParallelCommandTranslation, true)", parallelRecording);
    ASSERT_NE(parallelTranslation, std::string::npos);
    const auto childBundles = source.find("RHIDeviceFeature::InRenderPassChildCommandBuffers", parallelTranslation);
    ASSERT_NE(childBundles, std::string::npos);
    const auto centralizedDescriptors = source.find("SetFeature(RHIDeviceFeature::CentralizedDescriptorManagement", childBundles);
    ASSERT_NE(centralizedDescriptors, std::string::npos);

    const auto childBundleCapabilityBody = source.substr(childBundles, centralizedDescriptors - childBundles);
    EXPECT_NE(childBundleCapabilityBody.find("false"), std::string::npos)
        << "DX12 in-render-pass child bundles must stay opt-in until real model-load DRED/RenderDoc coverage proves they are stable.";
    EXPECT_NE(childBundleCapabilityBody.find("device-hung"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12DredDiagnosticsAreEnabledOutsideDebugLayerGate)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp");

    const auto createResources = source.find("DX12DeviceResources CreateDX12DeviceResources");
    ASSERT_NE(createResources, std::string::npos);
    const auto factoryFlags = source.find("const UINT factoryFlags = BuildDx12FactoryFlags(debugMode)", createResources);
    ASSERT_NE(factoryFlags, std::string::npos);
    const auto dredEnable = source.find("EnableDx12Dred()", createResources);
    ASSERT_NE(dredEnable, std::string::npos);

    const auto debugGate = source.rfind("if (debugMode)", dredEnable);
    EXPECT_TRUE(debugGate == std::string::npos || debugGate < createResources)
        << "DRED must stay enabled for release model-load device-hung diagnostics; only the debug layer should be debugMode-gated.";
    EXPECT_LT(dredEnable, factoryFlags);
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

    const auto flushBindingSetTables = source.find("bool NativeDX12CommandBuffer::FlushBoundDescriptorTables");
    ASSERT_NE(flushBindingSetTables, std::string::npos);
    const auto beginFunction = source.find("void NativeDX12CommandBuffer::Begin", flushBindingSetTables);
    ASSERT_NE(beginFunction, std::string::npos);
    const auto body = source.substr(flushBindingSetTables, beginFunction - flushBindingSetTables);
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

TEST(DX12PipelineLayoutUtilsTests, DX12DrawCommandsForwardInstanceCountsToNativeInstancedCalls)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto draw = source.find("void NativeDX12CommandBuffer::Draw(");
    ASSERT_NE(draw, std::string::npos);
    const auto drawIndexed = source.find("void NativeDX12CommandBuffer::DrawIndexed", draw);
    ASSERT_NE(drawIndexed, std::string::npos);
    const auto drawBody = source.substr(draw, drawIndexed - draw);
    EXPECT_NE(
        drawBody.find("(void)DrawChecked(vertexCount, instanceCount, firstVertex, firstInstance)"),
        std::string::npos);

    const auto drawChecked = source.find("RHICommandRecordingResult NativeDX12CommandBuffer::DrawChecked", drawIndexed);
    ASSERT_NE(drawChecked, std::string::npos);
    const auto drawCheckedEnd = source.find("RHICommandRecordingResult NativeDX12CommandBuffer::DrawIndexedChecked", drawChecked);
    ASSERT_NE(drawCheckedEnd, std::string::npos);
    const auto drawCheckedBody = source.substr(drawChecked, drawCheckedEnd - drawChecked);
    EXPECT_NE(
        drawCheckedBody.find("m_commandList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance)"),
        std::string::npos);

    const auto drawIndexedChecked = drawCheckedEnd;
    const auto dispatch = source.find("void NativeDX12CommandBuffer::Dispatch", drawIndexedChecked);
    ASSERT_NE(dispatch, std::string::npos);
    const auto drawIndexedBody = source.substr(drawIndexed, drawIndexedChecked - drawIndexed);
    EXPECT_NE(
        drawIndexedBody.find("(void)DrawIndexedChecked(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance)"),
        std::string::npos);

    const auto drawIndexedCheckedBody = source.substr(drawIndexedChecked, dispatch - drawIndexedChecked);
    EXPECT_NE(
        drawIndexedCheckedBody.find("m_commandList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance)"),
        std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12ChildCommandBuffersUseBundlesExecutedByParentCommandList)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto createChild = source.find("NativeDX12CommandPool::CreateChildCommandBuffer");
    ASSERT_NE(createChild, std::string::npos);
    const auto bundleType = source.find("D3D12_COMMAND_LIST_TYPE_BUNDLE", createChild);
    ASSERT_NE(bundleType, std::string::npos);

    const auto executeChild = source.find("NativeDX12CommandBuffer::ExecuteChildCommandBuffer");
    ASSERT_NE(executeChild, std::string::npos);
    const auto executeBundle = source.find("ExecuteBundle", executeChild);
    ASSERT_NE(executeBundle, std::string::npos);
    const auto childGuard = source.find("!nativeChild->IsChildCommandBuffer()", executeChild);
    ASSERT_NE(childGuard, std::string::npos);
    EXPECT_LT(childGuard, executeBundle);
    const auto parentRecordingGuard = source.find("parent must be a recording direct command list", executeChild);
    ASSERT_NE(parentRecordingGuard, std::string::npos);
    EXPECT_LT(parentRecordingGuard, executeBundle);
    const auto childClosedGuard = source.find("child bundle must be closed and valid before execution", executeChild);
    ASSERT_NE(childClosedGuard, std::string::npos);
    EXPECT_LT(childClosedGuard, executeBundle);
    const auto childValidityGuard = source.find("!nativeChild->m_childRecordingValid", executeChild);
    ASSERT_NE(childValidityGuard, std::string::npos);
    EXPECT_LT(childValidityGuard, executeBundle);
    const auto closeSuccessGuard = source.find("m_hasClosedRecording = SUCCEEDED(closeHr)");
    ASSERT_NE(closeSuccessGuard, std::string::npos);
    const auto descriptorHeapSync = source.find("SetDescriptorHeaps(activeHeapCount, activeHeaps)", executeChild);
    ASSERT_NE(descriptorHeapSync, std::string::npos);
    EXPECT_LT(descriptorHeapSync, executeBundle);
    const auto parentStateInvalidation = source.find("InvalidateParentStateAfterChildExecution()", executeBundle);
    ASSERT_NE(parentStateInvalidation, std::string::npos);
    EXPECT_GT(parentStateInvalidation, executeBundle);
}

TEST(DX12PipelineLayoutUtilsTests, DX12ChildExecutionInvalidatesParentPipelineAndDescriptorBindingCache)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto invalidation = source.find("void NativeDX12CommandBuffer::InvalidateParentStateAfterChildExecution()");
    ASSERT_NE(invalidation, std::string::npos);
    const auto executeChild = source.find("NativeDX12CommandBuffer::ExecuteChildCommandBuffer");
    ASSERT_NE(executeChild, std::string::npos);
    const auto executeBundle = source.find("ExecuteBundle", executeChild);
    ASSERT_NE(executeBundle, std::string::npos);
    const auto call = source.find("InvalidateParentStateAfterChildExecution()", executeBundle);
    ASSERT_NE(call, std::string::npos);

    const auto nextFunction = source.find("\n#endif", invalidation);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto body = source.substr(invalidation, nextFunction - invalidation);
    EXPECT_NE(body.find("m_boundPipeline.reset()"), std::string::npos);
    EXPECT_NE(body.find("m_boundComputePipeline.reset()"), std::string::npos);
    EXPECT_NE(body.find("m_boundDescriptorTables.clear()"), std::string::npos);
    EXPECT_NE(body.find("m_boundPushConstantRootParameters.clear()"), std::string::npos);
    EXPECT_NE(body.find("m_initializedRootDescriptorTables.clear()"), std::string::npos);
    EXPECT_NE(body.find("m_boundBindingSets.clear()"), std::string::npos);
    EXPECT_NE(body.find("m_bindingComputePipeline = false"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12BundleRecordingBindsPipelineInsideEachChildRange)
{
    const auto driverSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/Context/Driver.cpp");
    const auto coordinatorSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/Context/RhiThreadCoordinator.cpp");

    const auto recordDrawCommand = driverSource.find("RecordPreparedDrawCommand");
    ASSERT_NE(recordDrawCommand, std::string::npos);
    const auto bindGraphics = driverSource.find("commandBuffer.BindGraphicsPipeline(drawCommand.pipeline)", recordDrawCommand);
    ASSERT_NE(bindGraphics, std::string::npos);
    const auto drawChecked = driverSource.find("DrawChecked", bindGraphics);
    const auto drawIndexedChecked = driverSource.find("DrawIndexedChecked", bindGraphics);
    ASSERT_TRUE(drawChecked != std::string::npos || drawIndexedChecked != std::string::npos);
    if (drawChecked != std::string::npos)
        EXPECT_LT(bindGraphics, drawChecked);
    if (drawIndexedChecked != std::string::npos)
        EXPECT_LT(bindGraphics, drawIndexedChecked);

    const auto recordChild = coordinatorSource.find("RecordSingleInRenderPassChildWorkUnit");
    ASSERT_NE(recordChild, std::string::npos);
    const auto childRangeRecord = coordinatorSource.find("RecordPreparedDrawCommandsForPassRange", recordChild);
    ASSERT_NE(childRangeRecord, std::string::npos);
    const auto childRangeEnd = coordinatorSource.find("RecordSingleInRenderPassChildWorkUnit", recordChild + 1u);
    const auto childBody = coordinatorSource.substr(
        recordChild,
        childRangeEnd == std::string::npos ? std::string::npos : childRangeEnd - recordChild);
    EXPECT_NE(childBody.find("parentPassInput.recordedDrawCommands"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12ChildCommandBuffersRejectRenderPassAndBarrierOwnership)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto beginRenderPass = source.find("void NativeDX12CommandBuffer::BeginRenderPass");
    ASSERT_NE(beginRenderPass, std::string::npos);
    const auto beginChildGuard = source.find("m_isChildCommandBuffer", beginRenderPass);
    ASSERT_NE(beginChildGuard, std::string::npos);

    const auto endRenderPass = source.find("void NativeDX12CommandBuffer::EndRenderPass");
    ASSERT_NE(endRenderPass, std::string::npos);
    const auto endChildGuard = source.find("m_isChildCommandBuffer", endRenderPass);
    ASSERT_NE(endChildGuard, std::string::npos);

    const auto barrierChecked = source.find("NativeDX12CommandBuffer::BarrierChecked");
    ASSERT_NE(barrierChecked, std::string::npos);
    const auto barrierChildGuard = source.find("m_isChildCommandBuffer", barrierChecked);
    ASSERT_NE(barrierChildGuard, std::string::npos);

    const auto setViewport = source.find("void NativeDX12CommandBuffer::SetViewport");
    ASSERT_NE(setViewport, std::string::npos);
    const auto viewportChildGuard = source.find("m_isChildCommandBuffer", setViewport);
    ASSERT_NE(viewportChildGuard, std::string::npos);

    const auto copyBuffer = source.find("void NativeDX12CommandBuffer::CopyBuffer");
    ASSERT_NE(copyBuffer, std::string::npos);
    const auto copyChildGuard = source.find("m_isChildCommandBuffer", copyBuffer);
    ASSERT_NE(copyChildGuard, std::string::npos);

    const auto bindCompute = source.find("void NativeDX12CommandBuffer::BindComputePipeline");
    ASSERT_NE(bindCompute, std::string::npos);
    const auto computeChildGuard = source.find("m_isChildCommandBuffer", bindCompute);
    ASSERT_NE(computeChildGuard, std::string::npos);

    const auto dispatch = source.find("void NativeDX12CommandBuffer::Dispatch");
    ASSERT_NE(dispatch, std::string::npos);
    const auto dispatchChildGuard = source.find("m_isChildCommandBuffer", dispatch);
    ASSERT_NE(dispatchChildGuard, std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12ChildForbiddenCommandsPoisonBundleValidity)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto executeChild = source.find("NativeDX12CommandBuffer::ExecuteChildCommandBuffer");
    ASSERT_NE(executeChild, std::string::npos);
    ASSERT_NE(source.find("!nativeChild->m_childRecordingValid", executeChild), std::string::npos);

    const std::array<const char*, 7> forbiddenFunctions = {
        "void NativeDX12CommandBuffer::BeginRenderPass",
        "void NativeDX12CommandBuffer::EndRenderPass",
        "NLS::Render::RHI::RHICommandRecordingResult NativeDX12CommandBuffer::BarrierChecked",
        "void NativeDX12CommandBuffer::SetViewport",
        "void NativeDX12CommandBuffer::SetScissor",
        "void NativeDX12CommandBuffer::CopyBuffer(",
        "void NativeDX12CommandBuffer::CopyTexture("
    };

    for (const char* functionName : forbiddenFunctions)
    {
        const auto functionBegin = source.find(functionName);
        ASSERT_NE(functionBegin, std::string::npos) << functionName;
        const auto nextFunction = source.find("\n\tvoid NativeDX12CommandBuffer::", functionBegin + 1u);
        const auto body = source.substr(
            functionBegin,
            nextFunction == std::string::npos ? std::string::npos : nextFunction - functionBegin);
        EXPECT_NE(body.find("m_childRecordingValid = false"), std::string::npos) << functionName;
    }
}

TEST(DX12PipelineLayoutUtilsTests, DX12BundlesFailVisibleWhenDescriptorHeapChangesBeforeDraw)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto bindBindingSet = source.find("void NativeDX12CommandBuffer::BindBindingSet");
    ASSERT_NE(bindBindingSet, std::string::npos);
    const auto pushConstants = source.find("void NativeDX12CommandBuffer::PushConstants", bindBindingSet);
    ASSERT_NE(pushConstants, std::string::npos);
    const auto bindBody = source.substr(bindBindingSet, pushConstants - bindBindingSet);
    EXPECT_NE(bindBody.find("m_descriptorTablesDirty = true"), std::string::npos);
    EXPECT_EQ(bindBody.find("FlushBoundDescriptorTablesIfDirty"), std::string::npos)
        << "Binding inside child bundles must only mark descriptor tables dirty; heap changes are rejected at draw-time flush.";

    const auto flushDescriptorTables = source.find("bool NativeDX12CommandBuffer::FlushBoundDescriptorTables");
    ASSERT_NE(flushDescriptorTables, std::string::npos);
    const auto hasInitialized = source.find("bool NativeDX12CommandBuffer::HasInitializedRequiredRootDescriptorTables");
    ASSERT_NE(hasInitialized, std::string::npos);
    const auto flushBody = source.substr(flushDescriptorTables, hasInitialized - flushDescriptorTables);
    EXPECT_NE(flushBody.find("FlushBoundDescriptorTables rejected descriptor heap change inside child bundle"), std::string::npos);
    EXPECT_NE(flushBody.find("m_childRecordingValid = false"), std::string::npos);
    EXPECT_NE(flushBody.find("(!m_isChildCommandBuffer || !m_descriptorHeapsSet)"), std::string::npos);

    const auto draw = source.find("void NativeDX12CommandBuffer::Draw(");
    ASSERT_NE(draw, std::string::npos);
    const auto drawFlush = source.find("FlushBoundDescriptorTablesIfDirty()", draw);
    ASSERT_NE(drawFlush, std::string::npos);
    const auto drawInitialized = source.find("HasInitializedRequiredRootDescriptorTables", draw);
    ASSERT_NE(drawInitialized, std::string::npos);
    EXPECT_LT(drawFlush, drawInitialized);
}

TEST(DX12PipelineLayoutUtilsTests, DX12BindingSetCreationFailsClosedWhenNativeDescriptorTablesAreInvalid)
{
    const auto descriptorHeader = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.h");
    const auto descriptorSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp");
    const auto deviceSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");

    ASSERT_FALSE(descriptorHeader.empty());
    ASSERT_FALSE(descriptorSource.empty());
    ASSERT_FALSE(deviceSource.empty());

    EXPECT_NE(descriptorHeader.find("bool IsValid() const"), std::string::npos);
    EXPECT_NE(descriptorHeader.find("bool m_valid = false"), std::string::npos);

    const auto nativeHandle = descriptorSource.find("NativeHandle NativeDX12BindingSet::GetNativeBindingSetHandle");
    ASSERT_NE(nativeHandle, std::string::npos);
    EXPECT_NE(descriptorSource.find("if (!m_valid)", nativeHandle), std::string::npos);

    const auto compatibility = descriptorSource.find("bool NativeDX12BindingSet::IsCompatibleWithDescriptorTable");
    ASSERT_NE(compatibility, std::string::npos);
    EXPECT_NE(descriptorSource.find("if (!m_valid)", compatibility), std::string::npos);

    const auto createBindingSet = deviceSource.find("NativeDX12ExplicitDevice::CreateBindingSet");
    ASSERT_NE(createBindingSet, std::string::npos);
    EXPECT_NE(deviceSource.find("bindingSet->IsValid() ? bindingSet : nullptr", createBindingSet), std::string::npos);

    const auto resourceGpuHandle = descriptorSource.find("NativeDX12BindingSet::ComputeResourceGpuHandle");
    ASSERT_NE(resourceGpuHandle, std::string::npos);
    const auto samplerGpuHandle = descriptorSource.find("NativeDX12BindingSet::ComputeSamplerGpuHandle");
    ASSERT_NE(samplerGpuHandle, std::string::npos);
    const auto resourceGpuHandleBody = descriptorSource.substr(resourceGpuHandle, samplerGpuHandle - resourceGpuHandle);
    EXPECT_NE(resourceGpuHandleBody.find("if (handle.ptr == 0)"), std::string::npos);
    EXPECT_NE(resourceGpuHandleBody.find("return {};"), std::string::npos);

    const auto samplerGpuHandleEnd = descriptorSource.find("void NativeDX12BindingSet::WriteSamplerDescriptor", samplerGpuHandle);
    ASSERT_NE(samplerGpuHandleEnd, std::string::npos);
    const auto samplerGpuHandleBody = descriptorSource.substr(samplerGpuHandle, samplerGpuHandleEnd - samplerGpuHandle);
    EXPECT_NE(samplerGpuHandleBody.find("if (handle.ptr == 0)"), std::string::npos);
    EXPECT_NE(samplerGpuHandleBody.find("return {};"), std::string::npos);

    const auto descriptorExecute = descriptorSource.find("m_commandQueue->ExecuteCommandLists(1, cmdLists);");
    ASSERT_NE(descriptorExecute, std::string::npos);
    const auto waitForHeapInitialization = descriptorSource.find("WaitForDX12FenceValue(", descriptorExecute);
    ASSERT_NE(waitForHeapInitialization, std::string::npos);
    const auto waitForHeapInitializationEnd = descriptorSource.find("CloseHandle(fenceEvent)", waitForHeapInitialization);
    ASSERT_NE(waitForHeapInitializationEnd, std::string::npos);
    const auto waitBody = descriptorSource.substr(
        waitForHeapInitialization,
        waitForHeapInitializationEnd - waitForHeapInitialization);
    EXPECT_NE(waitBody.find("m_quarantined = true"), std::string::npos);
    EXPECT_NE(waitBody.find("QuarantineDX12DescriptorInitializationSubmissionAfterExecute"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12DrawFailsClosedWhenRequiredDescriptorTableBindingIsInvalid)
{
    const auto commandHeader = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.h");
    const auto commandSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    ASSERT_FALSE(commandHeader.empty());
    ASSERT_FALSE(commandSource.empty());

    EXPECT_NE(commandHeader.find("std::string m_descriptorTableBindingError"), std::string::npos);
    EXPECT_NE(commandHeader.find("std::vector<PlannedRootDescriptorTable> m_plannedRootDescriptorTables"), std::string::npos);
    EXPECT_NE(commandHeader.find("ValidateDescriptorTableBindings"), std::string::npos);
    EXPECT_NE(commandHeader.find("SetDescriptorTableBindingError"), std::string::npos);
    EXPECT_NE(commandHeader.find("RemoveBoundBindingSet"), std::string::npos);

    const auto setDescriptorTableBindingError =
        commandSource.find("void NativeDX12CommandBuffer::SetDescriptorTableBindingError");
    ASSERT_NE(setDescriptorTableBindingError, std::string::npos);
    const auto clearDescriptorTableBindingError =
        commandSource.find("void NativeDX12CommandBuffer::ClearDescriptorTableBindingError", setDescriptorTableBindingError);
    ASSERT_NE(clearDescriptorTableBindingError, std::string::npos);
    const auto errorBody =
        commandSource.substr(setDescriptorTableBindingError, clearDescriptorTableBindingError - setDescriptorTableBindingError);
    EXPECT_EQ(errorBody.find("m_descriptorTablesDirty = true"), std::string::npos)
        << "Binding errors should not force an early flush from intermediate BindBindingSet state.";

    const auto bindBindingSet = commandSource.find("void NativeDX12CommandBuffer::BindBindingSet");
    ASSERT_NE(bindBindingSet, std::string::npos);
    const auto pushConstants = commandSource.find("void NativeDX12CommandBuffer::PushConstants", bindBindingSet);
    ASSERT_NE(pushConstants, std::string::npos);
    const auto bindBody = commandSource.substr(bindBindingSet, pushConstants - bindBindingSet);
    EXPECT_NE(bindBody.find("has no valid DX12 native binding set"), std::string::npos);
    EXPECT_NE(bindBody.find("incompatible with the bound DX12 pipeline layout"), std::string::npos);
    EXPECT_NE(bindBody.find("DX12 descriptor table GPU handle is zero"), std::string::npos);
    EXPECT_NE(bindBody.find("SetDescriptorTableBindingError"), std::string::npos);
    EXPECT_NE(bindBody.find("RemoveBoundBindingSet(setIndex)"), std::string::npos);
    EXPECT_EQ(bindBody.find("FlushBoundDescriptorTablesIfDirty"), std::string::npos)
        << "BindBindingSet must only record final binding state; required-set completeness is checked at draw/dispatch time.";

    const auto flushDescriptorTables =
        commandSource.find("bool NativeDX12CommandBuffer::FlushBoundDescriptorTables");
    ASSERT_NE(flushDescriptorTables, std::string::npos);
    const auto flushDescriptorTablesIfDirty =
        commandSource.find("bool NativeDX12CommandBuffer::FlushBoundDescriptorTablesIfDirty", flushDescriptorTables);
    ASSERT_NE(flushDescriptorTablesIfDirty, std::string::npos);
    const auto flushBody =
        commandSource.substr(flushDescriptorTables, flushDescriptorTablesIfDirty - flushDescriptorTables);
    EXPECT_NE(flushBody.find("root descriptor table"), std::string::npos);
    EXPECT_NE(flushBody.find("but no binding set was bound"), std::string::npos);
    EXPECT_NE(flushBody.find("has no valid DX12 native binding set"), std::string::npos);
    EXPECT_NE(flushBody.find("is incompatible with root descriptor table"), std::string::npos);
    EXPECT_NE(flushBody.find("returned a zero GPU descriptor handle"), std::string::npos);
    EXPECT_NE(flushBody.find("m_plannedRootDescriptorTables.clear()"), std::string::npos);
    EXPECT_NE(flushBody.find("m_plannedRootDescriptorTables.reserve"), std::string::npos);
    EXPECT_NE(flushBody.find("m_plannedRootDescriptorTables.push_back"), std::string::npos);
    EXPECT_EQ(flushBody.find("std::vector<PlannedRootDescriptorTable> plannedTables"), std::string::npos)
        << "Flush must reuse command-buffer scratch storage instead of allocating a local vector per dirty draw.";
    EXPECT_NE(flushBody.find("multiple DX12 shader-visible"), std::string::npos);
    EXPECT_NE(flushBody.find("SetDescriptorHeaps(activeHeapCount, activeHeaps)"), std::string::npos);
    EXPECT_LT(flushBody.find("m_plannedRootDescriptorTables.push_back"), flushBody.find("SetDescriptorHeaps(activeHeapCount, activeHeaps)"));
    EXPECT_NE(flushBody.find("ValidateDescriptorTableBindings(\"FlushBoundDescriptorTables\")"), std::string::npos);

    const auto drawChecked = commandSource.find("NativeDX12CommandBuffer::DrawChecked");
    ASSERT_NE(drawChecked, std::string::npos);
    const auto drawIndexedChecked = commandSource.find("NativeDX12CommandBuffer::DrawIndexedChecked", drawChecked);
    ASSERT_NE(drawIndexedChecked, std::string::npos);
    EXPECT_LT(commandSource.find("FlushBoundDescriptorTablesIfDirty()", drawChecked), drawIndexedChecked);
    EXPECT_NE(commandSource.find("ValidateDescriptorTableBindings(\"Draw\")", drawChecked), std::string::npos);
    EXPECT_NE(commandSource.find("ValidateDescriptorTableBindings(\"DrawIndexed\")", drawIndexedChecked), std::string::npos);
    EXPECT_NE(commandSource.find("FlushBoundDescriptorTablesIfDirty()", drawIndexedChecked), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12QueueRejectsTopLevelChildBundleSubmission)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp");

    const auto collectCommandLists = source.find("NativeDX12Queue::CollectCommandLists");
    ASSERT_NE(collectCommandLists, std::string::npos);
    const auto executeCommandLists = source.find("ExecuteCommandLists", collectCommandLists);
    ASSERT_NE(executeCommandLists, std::string::npos);
    const auto collectBody = source.substr(collectCommandLists, executeCommandLists - collectCommandLists);

    EXPECT_NE(collectBody.find("cmdBuffer->IsChildCommandBuffer()"), std::string::npos);
    EXPECT_NE(collectBody.find("DX12 child bundle command buffers must be executed by a parent command list"), std::string::npos);
    EXPECT_NE(collectBody.find("RHIQueueOperationStatusCode::InvalidArgument"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12QueueRejectsUnclosedCommandBufferSubmission)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp");

    const auto collectCommandLists = source.find("NativeDX12Queue::CollectCommandLists");
    ASSERT_NE(collectCommandLists, std::string::npos);
    const auto executeCommandLists = source.find("ExecuteCommandLists", collectCommandLists);
    ASSERT_NE(executeCommandLists, std::string::npos);
    const auto collectBody = source.substr(collectCommandLists, executeCommandLists - collectCommandLists);

    EXPECT_NE(collectBody.find("cmdBuffer->IsClosedForSubmission()"), std::string::npos);
    EXPECT_NE(collectBody.find("DX12 command buffer must be closed successfully before submission"), std::string::npos);
    EXPECT_NE(collectBody.find("RHIQueueOperationStatusCode::InvalidArgument"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12QueueClassifiesPostExecuteSignalFailureByDeviceStatus)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp");

    const auto collectCommandLists = source.find("NativeDX12Queue::CollectCommandLists");
    ASSERT_NE(collectCommandLists, std::string::npos);
    const auto waitSemaphores = source.find("NativeDX12Queue::WaitSubmitSemaphores", collectCommandLists);
    ASSERT_NE(waitSemaphores, std::string::npos);
    EXPECT_LT(collectCommandLists, waitSemaphores);

    const auto semaphoreSignal = source.find("NativeDX12Queue::SignalSubmitSemaphores", waitSemaphores);
    ASSERT_NE(semaphoreSignal, std::string::npos);
    const auto semaphoreWaitBody = source.substr(waitSemaphores, semaphoreSignal - waitSemaphores);
    EXPECT_NE(semaphoreWaitBody.find("mayHaveQueuedGpuWork"), std::string::npos);
    const auto semaphoreWaitWorkMark = semaphoreWaitBody.find("mayHaveQueuedGpuWork = true");
    ASSERT_NE(semaphoreWaitWorkMark, std::string::npos);
    const auto queueWaitCall = semaphoreWaitBody.find("m_queue->Wait");
    ASSERT_NE(queueWaitCall, std::string::npos);
    EXPECT_LT(semaphoreWaitWorkMark, queueWaitCall);
    EXPECT_NE(semaphoreWaitBody.find("ClassifyDx12QueuedWorkFailure"), std::string::npos);

    const auto signalSemaphores = source.find("NativeDX12Queue::SignalSubmitSemaphores");
    ASSERT_NE(signalSemaphores, std::string::npos);
    const auto signalFence = source.find("NativeDX12Queue::SignalSubmitFence", signalSemaphores);
    ASSERT_NE(signalFence, std::string::npos);
    const auto semaphoreSignalBody = source.substr(signalSemaphores, signalFence - signalSemaphores);
    EXPECT_NE(semaphoreSignalBody.find("ClassifyDx12QueuedWorkFailure"), std::string::npos);
    EXPECT_NE(semaphoreSignalBody.find("mayHaveQueuedGpuWork"), std::string::npos);
    const auto semaphoreWorkMark = semaphoreSignalBody.find("mayHaveQueuedGpuWork = true");
    ASSERT_NE(semaphoreWorkMark, std::string::npos);
    const auto semaphoreSignalCall = semaphoreSignalBody.find("SignalOnQueueChecked");
    ASSERT_NE(semaphoreSignalCall, std::string::npos);
    EXPECT_LT(semaphoreWorkMark, semaphoreSignalCall);

    const auto present = source.find("NativeDX12Queue::Present", signalFence);
    ASSERT_NE(present, std::string::npos);
    const auto fenceSignalBody = source.substr(signalFence, present - signalFence);
    EXPECT_NE(fenceSignalBody.find("ClassifyDx12QueuedWorkFailure"), std::string::npos);
    EXPECT_NE(fenceSignalBody.find("mayHaveQueuedGpuWork"), std::string::npos);

    const auto presentWaitSemaphores = source.find("NativeDX12Queue::WaitPresentSemaphores", present);
    ASSERT_NE(presentWaitSemaphores, std::string::npos);
    const auto uiFenceBeforePresent = source.find("NativeDX12Queue::WaitUiFenceBeforePresent", presentWaitSemaphores);
    ASSERT_NE(uiFenceBeforePresent, std::string::npos);
    const auto presentWaitBody = source.substr(presentWaitSemaphores, uiFenceBeforePresent - presentWaitSemaphores);
    const auto presentWaitWorkMark = presentWaitBody.find("mayHaveQueuedGpuWork = true");
    ASSERT_NE(presentWaitWorkMark, std::string::npos);
    const auto presentQueueWaitCall = presentWaitBody.find("m_queue->Wait");
    ASSERT_NE(presentQueueWaitCall, std::string::npos);
    EXPECT_LT(presentWaitWorkMark, presentQueueWaitCall);
    EXPECT_NE(presentWaitBody.find("ClassifyDx12QueuedWorkFailure"), std::string::npos);

    const auto presentCall = source.find("IDXGISwapChain::Present", uiFenceBeforePresent);
    ASSERT_NE(presentCall, std::string::npos);
    const auto uiFenceBody = source.substr(uiFenceBeforePresent, presentCall - uiFenceBeforePresent);
    const auto uiFenceWorkMark = uiFenceBody.find("mayHaveQueuedGpuWork = true");
    ASSERT_NE(uiFenceWorkMark, std::string::npos);
    const auto uiFenceWaitCall = uiFenceBody.find("m_queue->Wait");
    ASSERT_NE(uiFenceWaitCall, std::string::npos);
    EXPECT_LT(uiFenceWorkMark, uiFenceWaitCall);
    EXPECT_NE(uiFenceBody.find("ClassifyDx12QueuedWorkFailure"), std::string::npos);

    const auto presentFailure = source.find("if (FAILED(hr))", presentCall);
    ASSERT_NE(presentFailure, std::string::npos);
    const auto presentBody = source.substr(presentCall, presentFailure - presentCall);
    const auto presentWorkMark = presentBody.find("mayHaveQueuedGpuWork = true");
    ASSERT_NE(presentWorkMark, std::string::npos);
    const auto swapchainPresentCall = presentBody.find("swapchain->Present");
    ASSERT_NE(swapchainPresentCall, std::string::npos);
    EXPECT_LT(presentWorkMark, swapchainPresentCall);

    const auto classifier = source.find("ClassifyDx12QueuedWorkFailure");
    ASSERT_NE(classifier, std::string::npos);
    const auto submit = source.find("NativeDX12Queue::SubmitChecked", classifier);
    ASSERT_NE(submit, std::string::npos);
    const auto classifierBody = source.substr(classifier, submit - classifier);
    EXPECT_NE(classifierBody.find("GetDeviceRemovedReason"), std::string::npos);
    EXPECT_NE(classifierBody.find("RHIQueueOperationStatusCode::BackendFailure"), std::string::npos);
    EXPECT_NE(classifierBody.find("RHIQueueOperationStatusCode::DeviceLost"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12QueueValidatesAllSemaphoreWaitsBeforeQueueingAnyWait)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp");

    const auto collectCommandLists = source.find("NativeDX12Queue::CollectCommandLists");
    ASSERT_NE(collectCommandLists, std::string::npos);
    const auto validateSubmitWaits = source.find("NativeDX12Queue::ValidateSubmitSemaphoreWaits", collectCommandLists);
    ASSERT_NE(validateSubmitWaits, std::string::npos);
    const auto waitSubmitSemaphores = source.find("NativeDX12Queue::WaitSubmitSemaphores", validateSubmitWaits);
    ASSERT_NE(waitSubmitSemaphores, std::string::npos);
    const auto executeCommandLists = source.find("ID3D12CommandQueue::ExecuteCommandLists", waitSubmitSemaphores);
    ASSERT_NE(executeCommandLists, std::string::npos);

    const auto submitValidationBody = source.substr(validateSubmitWaits, waitSubmitSemaphores - validateSubmitWaits);
    EXPECT_NE(submitValidationBody.find("ValidateDX12SemaphoreWaitValue"), std::string::npos);
    EXPECT_EQ(submitValidationBody.find("m_queue->Wait"), std::string::npos);

    const auto submitWaitBody = source.substr(waitSubmitSemaphores, executeCommandLists - waitSubmitSemaphores);
    EXPECT_EQ(submitWaitBody.find("ValidateDX12SemaphoreWaitValue"), std::string::npos);
    EXPECT_NE(submitWaitBody.find("m_queue->Wait"), std::string::npos);

    const auto present = source.find("NativeDX12Queue::Present", executeCommandLists);
    ASSERT_NE(present, std::string::npos);
    const auto validatePresentWaits = source.find("NativeDX12Queue::ValidatePresentSemaphoreWaits", present);
    ASSERT_NE(validatePresentWaits, std::string::npos);
    const auto waitPresentSemaphores = source.find("NativeDX12Queue::WaitPresentSemaphores", validatePresentWaits);
    ASSERT_NE(waitPresentSemaphores, std::string::npos);
    const auto uiFenceBeforePresent = source.find("NativeDX12Queue::WaitUiFenceBeforePresent", waitPresentSemaphores);
    ASSERT_NE(uiFenceBeforePresent, std::string::npos);

    const auto presentValidationBody = source.substr(validatePresentWaits, waitPresentSemaphores - validatePresentWaits);
    EXPECT_NE(presentValidationBody.find("ValidateDX12SemaphoreWaitValue"), std::string::npos);
    EXPECT_EQ(presentValidationBody.find("m_queue->Wait"), std::string::npos);

    const auto presentWaitBody = source.substr(waitPresentSemaphores, uiFenceBeforePresent - waitPresentSemaphores);
    EXPECT_EQ(presentWaitBody.find("ValidateDX12SemaphoreWaitValue"), std::string::npos);
    EXPECT_NE(presentWaitBody.find("m_queue->Wait"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12NativeQueueOperationsUseSharedQueueLock)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp");

    EXPECT_NE(source.find("DX12QueueSynchronization.h"), std::string::npos);

    const auto submit = source.find("NativeDX12Queue::SubmitChecked");
    ASSERT_NE(submit, std::string::npos);
    const auto waitSubmitSemaphores = source.find("NativeDX12Queue::WaitSubmitSemaphores", submit);
    ASSERT_NE(waitSubmitSemaphores, std::string::npos);
    const auto executeCommandLists = source.find("m_queue->ExecuteCommandLists", waitSubmitSemaphores);
    ASSERT_NE(executeCommandLists, std::string::npos);
    const auto submitSignalFence = source.find("NativeDX12Queue::SignalSubmitFence", executeCommandLists);
    ASSERT_NE(submitSignalFence, std::string::npos);
    const auto submitBody = source.substr(submit, submitSignalFence - submit);
    const auto submitLock = submitBody.find("ScopedDX12QueueLock queueLock(m_queue)");
    ASSERT_NE(submitLock, std::string::npos);
    EXPECT_LT(submitLock, submitBody.find("m_queue->Wait"));
    EXPECT_LT(submitLock, submitBody.find("m_queue->ExecuteCommandLists"));
    EXPECT_LT(submitLock, submitBody.find("SignalOnQueueChecked"));

    const auto present = source.find("NativeDX12Queue::PresentChecked", submitSignalFence);
    ASSERT_NE(present, std::string::npos);
    const auto waitPresentSemaphores = source.find("NativeDX12Queue::WaitPresentSemaphores", present);
    ASSERT_NE(waitPresentSemaphores, std::string::npos);
    const auto swapchainPresent = source.find("swapchain->Present", waitPresentSemaphores);
    ASSERT_NE(swapchainPresent, std::string::npos);
    const auto signalPresentFence = source.find("NativeDX12Queue::SignalPresentFence", swapchainPresent);
    ASSERT_NE(signalPresentFence, std::string::npos);
    const auto presentBody = source.substr(present, source.find("#else", signalPresentFence) - present);
    const auto presentLock = presentBody.find("ScopedDX12QueueLock queueLock(m_queue)");
    ASSERT_NE(presentLock, std::string::npos);
    EXPECT_LT(presentLock, presentBody.find("m_queue->Wait"));
    EXPECT_LT(presentLock, presentBody.find("swapchain->Present"));
    EXPECT_LT(presentLock, presentBody.find("m_queue->Signal"));
    const auto presentCallBody = source.substr(swapchainPresent, signalPresentFence - swapchainPresent);
    EXPECT_EQ(presentCallBody.find("ScopedDX12QueueLock"), std::string::npos)
        << "Present must share the outer queue transaction lock instead of acquiring a nested lock.";
    const auto presentSignalBody = source.substr(signalPresentFence, source.find("#else", signalPresentFence) - signalPresentFence);
    EXPECT_EQ(presentSignalBody.find("ScopedDX12QueueLock queueLock(m_queue)"), std::string::npos);
    EXPECT_NE(presentSignalBody.find("m_queue->Signal"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12PresentFailureLogsDredBreadcrumbs)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp");

    const auto present = source.find("NativeDX12Queue::PresentChecked");
    ASSERT_NE(present, std::string::npos);
    const auto presentFailure = source.find("NativeDX12Queue::Present: Present failed with hr=", present);
    ASSERT_NE(presentFailure, std::string::npos);
    const auto statusCode = source.find("const auto statusCode", presentFailure);
    ASSERT_NE(statusCode, std::string::npos);
    const auto failureBody = source.substr(presentFailure, statusCode - presentFailure);
    EXPECT_NE(failureBody.find("LogDx12DredBreadcrumbs(m_device, \"NativeDX12Queue::Present\")"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12UploadReadbackAndDescriptorInitializationSerializeNativeQueueSubmissions)
{
    const auto resourceSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp");
    EXPECT_NE(resourceSource.find("DX12QueueSynchronization.h"), std::string::npos);
    EXPECT_NE(resourceSource.find("ScopedDX12QueueLock queueLock(m_graphicsQueue)"), std::string::npos);
    EXPECT_NE(resourceSource.find("m_graphicsQueue->ExecuteCommandLists(1, commandLists);"), std::string::npos);
    EXPECT_NE(resourceSource.find("m_graphicsQueue->Signal(fence.Get(), fenceValue)"), std::string::npos);

    const auto readbackSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12ReadbackUtils.cpp");
    EXPECT_NE(readbackSource.find("DX12QueueSynchronization.h"), std::string::npos);
    EXPECT_NE(readbackSource.find("ScopedDX12QueueLock queueLock(graphicsQueue)"), std::string::npos);
    EXPECT_NE(readbackSource.find("graphicsQueue->ExecuteCommandLists(1, commandLists);"), std::string::npos);
    EXPECT_NE(readbackSource.find("graphicsQueue->Signal(m_impl->fence.Get(), fenceValue)"), std::string::npos);

    const auto descriptorSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp");
    EXPECT_NE(descriptorSource.find("DX12QueueSynchronization.h"), std::string::npos);
    EXPECT_NE(descriptorSource.find("ScopedDX12QueueLock queueLock(m_commandQueue)"), std::string::npos);
    EXPECT_NE(descriptorSource.find("m_commandQueue->ExecuteCommandLists(1, cmdLists);"), std::string::npos);
    EXPECT_NE(descriptorSource.find("m_commandQueue->Signal(fence.Get(), fenceValue)"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12ReadbackSignalsFenceBeforePostExecuteDeviceRemovedCheck)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12ReadbackUtils.cpp");

    const auto execute = source.find("graphicsQueue->ExecuteCommandLists(1, commandLists);");
    ASSERT_NE(execute, std::string::npos);
    const auto signal = source.find("graphicsQueue->Signal(m_impl->fence.Get(), fenceValue)", execute);
    ASSERT_NE(signal, std::string::npos);
    const auto postExecuteStatus = source.find("ReadPixels after ExecuteCommandLists", execute);
    ASSERT_NE(postExecuteStatus, std::string::npos);
    EXPECT_LT(signal, postExecuteStatus)
        << "If ExecuteCommandLists is the first device-lost detector, readback should queue its retirement fence before returning failure when possible.";
    const auto inFlightStore = source.find("readbackInFlight->store(true)", signal);
    ASSERT_NE(inFlightStore, std::string::npos);
    EXPECT_LT(signal, inFlightStore);
    EXPECT_LT(inFlightStore, postExecuteStatus)
        << "Device-lost readback failures after queued work still need an in-flight completion token to protect scratch resource reuse.";
}

TEST(DX12PipelineLayoutUtilsTests, DX12ReadbackCompletionClassifiesDeviceLostAfterSubmittedWork)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12ReadbackUtils.cpp");

    const auto pendingCopy = source.find("struct DX12ReadbackPendingCopy");
    ASSERT_NE(pendingCopy, std::string::npos);
    const auto completionToken = source.find("class DX12ReadbackCompletionToken", pendingCopy);
    ASSERT_NE(completionToken, std::string::npos);
    const auto pendingCopyBody = source.substr(pendingCopy, completionToken - pendingCopy);
    EXPECT_NE(pendingCopyBody.find("ComPtr<ID3D12Device> device"), std::string::npos);

    const auto setEventFailure = source.find("ReadPixels failed to set fence completion event", completionToken);
    ASSERT_NE(setEventFailure, std::string::npos);
    const auto mapFailure = source.find("ReadPixels failed to map readback resource", completionToken);
    ASSERT_NE(mapFailure, std::string::npos);
    const auto deviceStatusFailure = source.find("RHICompletionStatus CompleteWithDeviceStatusFailure", completionToken);
    ASSERT_NE(deviceStatusFailure, std::string::npos);
    EXPECT_NE(
        source.substr(setEventFailure, deviceStatusFailure - setEventFailure).find("CompleteWithDeviceStatusFailure"),
        std::string::npos);
    const auto mapFailureBranch = source.rfind("return CompleteWithDeviceStatusFailure", mapFailure);
    ASSERT_NE(mapFailureBranch, std::string::npos);
    EXPECT_LT(mapFailureBranch, mapFailure);

    const auto deviceStatusFailureEnd = source.find("DX12ReadbackPendingCopy m_pendingCopy", deviceStatusFailure);
    ASSERT_NE(deviceStatusFailureEnd, std::string::npos);
    const auto deviceStatusFailureBody = source.substr(deviceStatusFailure, deviceStatusFailureEnd - deviceStatusFailure);
    EXPECT_NE(deviceStatusFailureBody.find("GetDeviceRemovedReason()"), std::string::npos);
    EXPECT_NE(deviceStatusFailureBody.find("RHICompletionStatusCode::DeviceLost"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12TemporaryGpuWorkQuarantinesSubmittedResourcesWhenFenceSignalFails)
{
    const auto resourceSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp");
    EXPECT_NE(resourceSource.find("QuarantineDX12InitialUploadSubmissionAfterExecute"), std::string::npos);

    const auto textureExecute = resourceSource.find("m_graphicsQueue->ExecuteCommandLists(1, commandLists);");
    ASSERT_NE(textureExecute, std::string::npos);
    const auto textureSignalFailure = resourceSource.find("UploadInitialTextureData: failed to signal fence", textureExecute);
    ASSERT_NE(textureSignalFailure, std::string::npos);
    const auto textureQuarantine =
        resourceSource.find("QuarantineDX12InitialUploadSubmissionAfterExecute", textureExecute);
    ASSERT_NE(textureQuarantine, std::string::npos);
    EXPECT_LT(textureExecute, textureQuarantine);
    EXPECT_LT(textureQuarantine, textureSignalFailure);

    const auto bufferExecute = resourceSource.find("m_graphicsQueue->ExecuteCommandLists(1, commandLists);", textureSignalFailure);
    ASSERT_NE(bufferExecute, std::string::npos);
    const auto bufferSignalFailure = resourceSource.find("UploadInitialBufferData: failed to signal upload fence", bufferExecute);
    ASSERT_NE(bufferSignalFailure, std::string::npos);
    const auto bufferQuarantine =
        resourceSource.find("QuarantineDX12InitialUploadSubmissionAfterExecute", bufferExecute);
    ASSERT_NE(bufferQuarantine, std::string::npos);
    EXPECT_LT(bufferExecute, bufferQuarantine);
    EXPECT_LT(bufferQuarantine, bufferSignalFailure);

    const auto readbackSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12ReadbackUtils.cpp");
    EXPECT_NE(readbackSource.find("QuarantineDX12ReadbackSubmissionAfterExecute"), std::string::npos);
    EXPECT_NE(readbackSource.find("readbackQuarantined"), std::string::npos);
    const auto readbackExecute = readbackSource.find("graphicsQueue->ExecuteCommandLists(1, commandLists);");
    ASSERT_NE(readbackExecute, std::string::npos);
    const auto readbackSignalFailure = readbackSource.find("ReadPixels failed to signal fence", readbackExecute);
    ASSERT_NE(readbackSignalFailure, std::string::npos);
    const auto readbackQuarantine =
        readbackSource.find("QuarantineDX12ReadbackSubmissionAfterExecute", readbackExecute);
    ASSERT_NE(readbackQuarantine, std::string::npos);
    EXPECT_LT(readbackExecute, readbackQuarantine);
    EXPECT_LT(readbackQuarantine, readbackSignalFailure);

    const auto descriptorSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp");
    EXPECT_NE(descriptorSource.find("QuarantineDX12DescriptorInitializationSubmissionAfterExecute"), std::string::npos);
    const auto descriptorExecute = descriptorSource.find("m_commandQueue->ExecuteCommandLists(1, cmdLists);");
    ASSERT_NE(descriptorExecute, std::string::npos);
    const auto descriptorSignalFailure =
        descriptorSource.find("failed to signal descriptor heap initialization fence", descriptorExecute);
    ASSERT_NE(descriptorSignalFailure, std::string::npos);
    const auto descriptorQuarantine =
        descriptorSource.find("QuarantineDX12DescriptorInitializationSubmissionAfterExecute", descriptorExecute);
    ASSERT_NE(descriptorQuarantine, std::string::npos);
    EXPECT_LT(descriptorExecute, descriptorQuarantine);
    EXPECT_LT(descriptorQuarantine, descriptorSignalFailure);
    const auto descriptorFailClosed =
        descriptorSource.find("m_quarantined = true", descriptorExecute);
    ASSERT_NE(descriptorFailClosed, std::string::npos);
    EXPECT_LT(descriptorFailClosed, descriptorQuarantine);
    EXPECT_NE(descriptorSource.find("if (m_quarantined)", descriptorFailClosed), std::string::npos);
    EXPECT_NE(descriptorSource.find("return UINT_MAX;", descriptorFailClosed), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12InitialUploadQuarantinesSubmittedResourcesWhenFenceWaitFails)
{
    const auto resourceSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp");

    const auto textureWait = resourceSource.find("UploadInitialTextureData \\\"");
    ASSERT_NE(textureWait, std::string::npos);
    const auto textureWaitFailure =
        resourceSource.find("failed to wait for upload fence for texture", textureWait);
    ASSERT_NE(textureWaitFailure, std::string::npos);
    const auto textureWaitBranch =
        resourceSource.rfind("if (!WaitForDX12FenceValue(", textureWaitFailure);
    ASSERT_NE(textureWaitBranch, std::string::npos);
    const auto textureWaitReturn = resourceSource.find("return false;", textureWaitFailure);
    ASSERT_NE(textureWaitReturn, std::string::npos);
    const auto textureWaitBody = resourceSource.substr(textureWaitBranch, textureWaitReturn - textureWaitBranch);
    EXPECT_NE(textureWaitBody.find("QuarantineDX12InitialUploadSubmissionAfterExecute"), std::string::npos);
    EXPECT_NE(textureWaitBody.find("textureResource"), std::string::npos);
    EXPECT_NE(textureWaitBody.find("uploadBuffer"), std::string::npos);
    EXPECT_NE(textureWaitBody.find("commandAllocator"), std::string::npos);
    EXPECT_NE(textureWaitBody.find("commandList"), std::string::npos);
    EXPECT_NE(textureWaitBody.find("fence"), std::string::npos);

    const auto bufferWait = resourceSource.find("UploadInitialBufferData \\\"", textureWaitFailure);
    ASSERT_NE(bufferWait, std::string::npos);
    const auto bufferWaitFailure =
        resourceSource.find("failed to wait for upload fence for \\\"", bufferWait);
    ASSERT_NE(bufferWaitFailure, std::string::npos);
    const auto bufferWaitBranch =
        resourceSource.rfind("if (!WaitForDX12FenceValue(", bufferWaitFailure);
    ASSERT_NE(bufferWaitBranch, std::string::npos);
    const auto bufferWaitReturn = resourceSource.find("return false;", bufferWaitFailure);
    ASSERT_NE(bufferWaitReturn, std::string::npos);
    const auto bufferWaitBody = resourceSource.substr(bufferWaitBranch, bufferWaitReturn - bufferWaitBranch);
    EXPECT_NE(bufferWaitBody.find("QuarantineDX12InitialUploadSubmissionAfterExecute"), std::string::npos);
    EXPECT_NE(bufferWaitBody.find("bufferResource"), std::string::npos);
    EXPECT_NE(bufferWaitBody.find("uploadBuffer"), std::string::npos);
    EXPECT_NE(bufferWaitBody.find("commandAllocator"), std::string::npos);
    EXPECT_NE(bufferWaitBody.find("commandList"), std::string::npos);
    EXPECT_NE(bufferWaitBody.find("fence"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12TemporaryGpuWorkQuarantineReportsDriverSafetyGate)
{
    const auto resourceSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp");
    const auto readbackSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12ReadbackUtils.cpp");
    const auto descriptorSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp");

    const auto expectQuarantineHelperReportsDriver =
        [](const std::string& source, const std::string& helperName, const std::string& nextSymbol)
        {
            const auto helper = source.find(helperName);
            ASSERT_NE(helper, std::string::npos);
            const auto helperEnd = source.find(nextSymbol, helper);
            ASSERT_NE(helperEnd, std::string::npos);
            const auto helperBody = source.substr(helper, helperEnd - helper);
            EXPECT_NE(helperBody.find("MarkLocatedDriverUnsafeGpuWorkQuarantined"), std::string::npos)
                << helperName << " must update Driver unsafe GPU quarantine, not only local keep-alive.";
            EXPECT_NE(helperBody.find("MarkLocatedDriverDeviceLost"), std::string::npos)
                << helperName << " must propagate DX12 device-removed status when available.";
        };

    expectQuarantineHelperReportsDriver(
        resourceSource,
        "void QuarantineDX12InitialUploadSubmissionAfterExecute",
        "NLS::Render::RHI::ResourceState ResolveUploadedTextureState");
    expectQuarantineHelperReportsDriver(
        readbackSource,
        "void QuarantineDX12ReadbackSubmissionAfterExecute",
        "void CopyDX12ReadbackRowsToDestination");
    expectQuarantineHelperReportsDriver(
        descriptorSource,
        "void QuarantineDX12DescriptorInitializationSubmissionAfterExecute",
        "DX12ShaderVisibleDescriptorHeapAllocator::DX12ShaderVisibleDescriptorHeapAllocator");
}

TEST(DX12PipelineLayoutUtilsTests, DX12QueueLockRegistryKeepsStableMutexOwnership)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12QueueSynchronization.h");
    const auto queueSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp");

    EXPECT_NE(source.find("std::unordered_map<ID3D12CommandQueue*, std::unique_ptr<std::mutex>>"), std::string::npos);
    EXPECT_NE(source.find("std::make_unique<std::mutex>()"), std::string::npos);
    EXPECT_NE(source.find("std::mutex* ResolveQueueMutex"), std::string::npos);
    EXPECT_NE(source.find("void ReleaseQueueMutex(ID3D12CommandQueue* queue)"), std::string::npos);
    EXPECT_NE(source.find("std::mutex* m_mutex = nullptr"), std::string::npos);
    EXPECT_EQ(source.find("std::weak_ptr<std::mutex>"), std::string::npos)
        << "Queue mutex registry must not drop ownership after each ScopedDX12QueueLock.";
    EXPECT_EQ(source.find("weakMutex.lock()"), std::string::npos);

    const auto destructor = queueSource.find("NativeDX12Queue::~NativeDX12Queue()");
    ASSERT_NE(destructor, std::string::npos);
    const auto getDebugName = queueSource.find("NativeDX12Queue::GetDebugName", destructor);
    ASSERT_NE(getDebugName, std::string::npos);
    const auto destructorBody = queueSource.substr(destructor, getDebugName - destructor);
    EXPECT_NE(destructorBody.find("ReleaseQueueMutex(m_queue)"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, DX12UIBridgeSerializesQueueWaitSubmitAndSignalOperations)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp");

    EXPECT_NE(source.find("DX12QueueSynchronization.h"), std::string::npos);
    EXPECT_NE(source.find("DX12::ScopedDX12QueueLock queueLock(queue)"), std::string::npos);
    EXPECT_NE(source.find("return queue->Wait(fence, waitSemaphore.value);"), std::string::npos);

    const auto renderDrawData = source.find("void RenderDrawData(");
    ASSERT_NE(renderDrawData, std::string::npos);
    const auto executeCommandLists = source.find("m_queue->ExecuteCommandLists(1, commandLists);", renderDrawData);
    ASSERT_NE(executeCommandLists, std::string::npos);
    const auto frameSignal = source.find("frameSignalHr = m_queue->Signal(m_fence.Get(), fenceValue);", executeCommandLists);
    ASSERT_NE(frameSignal, std::string::npos);
    const auto uiSignal = source.find("uiSignalHr = m_queue->Signal(m_uiFence.Get(), fenceValue);", frameSignal);
    ASSERT_NE(uiSignal, std::string::npos);
    const auto renderDrawDataLock = source.rfind("DX12::ScopedDX12QueueLock queueLock(m_queue.Get())", executeCommandLists);
    ASSERT_NE(renderDrawDataLock, std::string::npos);
    EXPECT_LT(renderDrawDataLock, executeCommandLists);
    EXPECT_LT(executeCommandLists, frameSignal);
    EXPECT_LT(frameSignal, uiSignal);
}

TEST(DX12PipelineLayoutUtilsTests, DX12UIBridgeQuarantinesSubmittedUiWorkWhenFrameFenceSignalFails)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp");

    const auto renderDrawData = source.find("void RenderDrawData(");
    ASSERT_NE(renderDrawData, std::string::npos);
    const auto executeCommandLists = source.find("m_queue->ExecuteCommandLists(1, commandLists);", renderDrawData);
    ASSERT_NE(executeCommandLists, std::string::npos);
    const auto frameSignalFailure = source.find("UI frame fence signal failed", executeCommandLists);
    ASSERT_NE(frameSignalFailure, std::string::npos);
    const auto quarantine = source.find("QuarantineSubmittedUiFrameAfterExecute", executeCommandLists);
    ASSERT_NE(quarantine, std::string::npos);
    EXPECT_LT(executeCommandLists, quarantine);
    EXPECT_LT(quarantine, frameSignalFailure);
    const auto failureBody = source.substr(frameSignalFailure, source.find("return;", frameSignalFailure) - frameSignalFailure);
    EXPECT_EQ(failureBody.find("DiscardCurrentFrameTextureHandles()"), std::string::npos)
        << "Submitted UI work without a reliable frame fence must retain current texture handles instead of discarding them.";

    const auto releaseResources = source.find("void ReleaseSwapchainRenderResources()");
    ASSERT_NE(releaseResources, std::string::npos);
    const auto waitCall = source.find("WaitForSubmittedUiWork()", releaseResources);
    ASSERT_NE(waitCall, std::string::npos);
    EXPECT_NE(source.find("if (!WaitForSubmittedUiWork())", releaseResources), std::string::npos);
    const auto clearAllocators = source.find("m_commandAllocators.clear()", releaseResources);
    ASSERT_NE(clearAllocators, std::string::npos);
    EXPECT_LT(waitCall, clearAllocators);
}

TEST(DX12PipelineLayoutUtilsTests, DX12UIBridgeReportsQuarantineAndDeviceLostToDriver)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp");

    EXPECT_NE(source.find("MarkLocatedDriverUnsafeGpuWorkQuarantined"), std::string::npos);
    EXPECT_NE(source.find("MarkLocatedDriverDeviceLost"), std::string::npos);

    const auto deviceStatus = source.find("device status after ExecuteCommandLists");
    ASSERT_NE(deviceStatus, std::string::npos);
    const auto deviceLostMarker = source.find("MarkDriverDeviceLost(message)", deviceStatus);
    ASSERT_NE(deviceLostMarker, std::string::npos);
    const auto deviceRemovedQuarantine =
        source.find("QuarantineSubmittedUiFrameAfterExecute", deviceStatus);
    ASSERT_NE(deviceRemovedQuarantine, std::string::npos);
    EXPECT_LT(deviceRemovedQuarantine, deviceLostMarker);
    const auto deviceStatusReturn = source.find("return;", deviceLostMarker);
    ASSERT_NE(deviceStatusReturn, std::string::npos);
    const auto deviceStatusBody = source.substr(deviceStatus, deviceStatusReturn - deviceStatus);
    EXPECT_EQ(deviceStatusBody.find("DiscardCurrentFrameTextureHandles()"), std::string::npos)
        << "UI work is already submitted once device status is checked, so resources must be quarantined.";

    const auto uiSignalFailure = source.find("UI composition fence signal failed");
    ASSERT_NE(uiSignalFailure, std::string::npos);
    const auto uiSignalReturn = source.find("return;", uiSignalFailure);
    ASSERT_NE(uiSignalReturn, std::string::npos);
    const auto uiSignalBranch = source.rfind("if (FAILED(uiSignalHr))", uiSignalFailure);
    ASSERT_NE(uiSignalBranch, std::string::npos);
    const auto uiSignalBody = source.substr(uiSignalBranch, uiSignalReturn - uiSignalBranch);
    EXPECT_NE(uiSignalBody.find("GetDeviceRemovedReason()"), std::string::npos);
    EXPECT_NE(uiSignalBody.find("MarkDriverDeviceLost"), std::string::npos);
    EXPECT_EQ(uiSignalBody.find("m_lastSubmittedUiSignalValue = fenceValue"), std::string::npos);
    const auto clearUiSignal = source.rfind("m_lastSubmittedUiSignalValue = 0u;", uiSignalFailure);
    ASSERT_NE(clearUiSignal, std::string::npos);
    EXPECT_LT(clearUiSignal, uiSignalFailure);

    const auto quarantineSubmitted = source.find("void QuarantineSubmittedUiFrameAfterExecute");
    ASSERT_NE(quarantineSubmitted, std::string::npos);
    const auto quarantineSubmittedEnd = source.find("void QuarantineAllUiBridgeResources", quarantineSubmitted);
    ASSERT_NE(quarantineSubmittedEnd, std::string::npos);
    const auto quarantineSubmittedBody =
        source.substr(quarantineSubmitted, quarantineSubmittedEnd - quarantineSubmitted);
    EXPECT_NE(quarantineSubmittedBody.find("MarkDriverUnsafeGpuWorkQuarantined"), std::string::npos);

    const auto quarantineAll = source.find("void QuarantineAllUiBridgeResources");
    ASSERT_NE(quarantineAll, std::string::npos);
    const auto quarantineAllEnd = source.find("bool IsTextureDescriptorReferencedByCurrentFrame", quarantineAll);
    ASSERT_NE(quarantineAllEnd, std::string::npos);
    const auto quarantineAllBody = source.substr(quarantineAll, quarantineAllEnd - quarantineAll);
    EXPECT_NE(quarantineAllBody.find("MarkDriverUnsafeGpuWorkQuarantined"), std::string::npos);

    const auto releaseResources = source.find("void ReleaseSwapchainRenderResources()");
    ASSERT_NE(releaseResources, std::string::npos);
    const auto failedDrain = source.find("if (!WaitForSubmittedUiWork())", releaseResources);
    ASSERT_NE(failedDrain, std::string::npos);
    const auto failedDrainReturn = source.find("return;", failedDrain);
    ASSERT_NE(failedDrainReturn, std::string::npos);
    const auto failedDrainBody = source.substr(failedDrain, failedDrainReturn - failedDrain);
    EXPECT_NE(failedDrainBody.find("MarkDriverUnsafeGpuWorkQuarantined"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, ThreadedRenderingParallelRecordingUsesJobSystemWorkers)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/Context/RhiThreadCoordinator.cpp");

    EXPECT_NE(source.find("Jobs::ScheduleParallelFor"), std::string::npos);
    ExpectJobHandleCompletionStatusThenClear(source, 2u);
    EXPECT_EQ(source.find("std::vector<std::thread> workers"), std::string::npos);
}

TEST(DX12PipelineLayoutUtilsTests, RenderSceneVisibilityUsesJobSystemInsteadOfAsyncThreads)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Engine/Rendering/RenderScene.cpp");

    EXPECT_NE(source.find("Jobs::ScheduleParallelFor"), std::string::npos);
    ExpectJobHandleCompletionStatusThenClear(source, 1u);
    EXPECT_EQ(source.find("std::async"), std::string::npos);
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

TEST(DX12PipelineLayoutUtilsTests, DX12CommandAllowsCommonPromotableUnresolvedPartialTextureBarriers)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto barrier = source.find("NativeDX12CommandBuffer::BarrierChecked");
    ASSERT_NE(barrier, std::string::npos);
    const auto wholeTextureStateKnown = source.find("wholeTextureStateKnown", barrier);
    ASSERT_NE(wholeTextureStateKnown, std::string::npos);
    const auto promotionGuard = source.find(
        "IsD3D12CommonPromotionAllowedForTexture(textureBarrier.after)",
        wholeTextureStateKnown);
    ASSERT_NE(promotionGuard, std::string::npos);
    const auto promotionContinue = source.find("continue;", promotionGuard);
    ASSERT_NE(promotionContinue, std::string::npos);
    const auto stateConversion = source.find(
        "const auto stateBefore = ToD3D12ResourceState(effectiveBefore)",
        wholeTextureStateKnown);
    ASSERT_NE(stateConversion, std::string::npos);
    const auto partialDirtyGuard = source.rfind("!wholeTextureStateKnown", stateConversion);
    ASSERT_NE(partialDirtyGuard, std::string::npos);
    EXPECT_GT(partialDirtyGuard, wholeTextureStateKnown);
    EXPECT_LT(promotionGuard, stateConversion);
    EXPECT_LT(promotionContinue, stateConversion);
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
