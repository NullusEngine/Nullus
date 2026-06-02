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
    const auto parentRootTableInvalidation = source.find("m_initializedRootDescriptorTables.clear()", descriptorHeapSync);
    ASSERT_NE(parentRootTableInvalidation, std::string::npos);
    EXPECT_LT(parentRootTableInvalidation, executeBundle);
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
    EXPECT_NE(bindBody.find("if (m_isChildCommandBuffer)"), std::string::npos);
    EXPECT_NE(bindBody.find("return;"), std::string::npos);
    EXPECT_NE(bindBody.find("m_descriptorTablesDirty = true"), std::string::npos);

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

    const auto signalSemaphores = source.find("NativeDX12Queue::SignalSubmitSemaphores");
    ASSERT_NE(signalSemaphores, std::string::npos);
    const auto signalFence = source.find("NativeDX12Queue::SignalSubmitFence", signalSemaphores);
    ASSERT_NE(signalFence, std::string::npos);
    const auto semaphoreSignalBody = source.substr(signalSemaphores, signalFence - signalSemaphores);
    EXPECT_NE(semaphoreSignalBody.find("ClassifyDx12PostExecuteSignalFailure"), std::string::npos);
    EXPECT_NE(semaphoreSignalBody.find("mayHaveQueuedGpuWork"), std::string::npos);

    const auto present = source.find("NativeDX12Queue::Present", signalFence);
    ASSERT_NE(present, std::string::npos);
    const auto fenceSignalBody = source.substr(signalFence, present - signalFence);
    EXPECT_NE(fenceSignalBody.find("ClassifyDx12PostExecuteSignalFailure"), std::string::npos);
    EXPECT_NE(fenceSignalBody.find("mayHaveQueuedGpuWork"), std::string::npos);

    const auto classifier = source.find("ClassifyDx12PostExecuteSignalFailure");
    ASSERT_NE(classifier, std::string::npos);
    const auto submit = source.find("NativeDX12Queue::SubmitChecked", classifier);
    ASSERT_NE(submit, std::string::npos);
    const auto classifierBody = source.substr(classifier, submit - classifier);
    EXPECT_NE(classifierBody.find("GetDeviceRemovedReason"), std::string::npos);
    EXPECT_NE(classifierBody.find("RHIQueueOperationStatusCode::BackendFailure"), std::string::npos);
    EXPECT_NE(classifierBody.find("RHIQueueOperationStatusCode::DeviceLost"), std::string::npos);
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
