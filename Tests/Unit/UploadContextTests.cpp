#include <gtest/gtest.h>

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX12/DX12Device.h"
#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/DX12/DX12Resource.h"
#endif

#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"

namespace
{
    class TestBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        TestBuffer() = default;
        explicit TestBuffer(NLS::Render::RHI::RHIBufferDesc desc)
            : m_desc(std::move(desc))
        {
        }
        TestBuffer(NLS::Render::RHI::RHIBufferDesc desc, NLS::Render::RHI::ResourceState state)
            : m_desc(std::move(desc))
            , m_state(state)
        {
        }

        std::string_view GetDebugName() const override { return "UploadContextTestsBuffer"; }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
        uint64_t GetGPUAddress() const override { return 0u; }

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc{};
        NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
    };

    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        TestTexture() = default;
        explicit TestTexture(NLS::Render::RHI::RHITextureDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return "UploadContextTestsTexture"; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc{};
    };

    class TestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "UploadContextTestsCommandBuffer"; }
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        bool IsRecording() const override { return false; }
        NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
        void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetViewport(const NLS::Render::RHI::RHIViewport&) override {}
        void SetScissor(const NLS::Render::RHI::RHIRect2D&) override {}
        void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>&) override {}
        void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}
        void BindBindingSet(uint32_t, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>&) override {}
        void PushConstants(NLS::Render::RHI::ShaderStageMask, uint32_t, uint32_t, const void*) override {}
        void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override {}
        void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override {}
        void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
        void Dispatch(uint32_t, uint32_t, uint32_t) override {}
        void CopyBuffer(
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const NLS::Render::RHI::RHIBufferCopyRegion& region) override
        {
            ++copyBufferCalls;
            lastBufferCopyRegion = region;
        }
        void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc) override
        {
            ++copyBufferToTextureCalls;
            lastTextureCopyDesc = desc;
        }
        void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc& desc) override
        {
            ++barrierCalls;
            lastBarrierDesc = desc;
        }

        size_t copyBufferCalls = 0u;
        size_t copyBufferToTextureCalls = 0u;
        size_t barrierCalls = 0u;
        NLS::Render::RHI::RHIBufferCopyRegion lastBufferCopyRegion {};
        NLS::Render::RHI::RHIBufferToTextureCopyDesc lastTextureCopyDesc {};
        NLS::Render::RHI::RHIBarrierDesc lastBarrierDesc {};
    };

    class TestUploadBackend final : public NLS::Render::RHI::UploadBackend
    {
    public:
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateStagingBuffer(
            const void* data,
            size_t size,
            std::string debugName) override
        {
            ++createStagingBufferCalls;
            if (failOnCreateCall != 0u && createStagingBufferCalls == failOnCreateCall)
                return nullptr;

            lastStagingDebugName = std::move(debugName);
            lastStagingBytes.assign(
                static_cast<const std::byte*>(data),
                static_cast<const std::byte*>(data) + size);

            NLS::Render::RHI::RHIBufferDesc desc;
            desc.size = size;
            desc.usage = NLS::Render::RHI::BufferUsageFlags::CopySrc;
            desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
            desc.debugName = lastStagingDebugName;
            return std::make_shared<TestBuffer>(std::move(desc));
        }

        std::vector<std::byte> lastStagingBytes;
        std::string lastStagingDebugName;
        size_t createStagingBufferCalls = 0u;
        size_t failOnCreateCall = 0u;
    };

    class TestFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        std::string_view GetDebugName() const override { return "UploadContextTestsFence"; }
        bool IsSignaled() const override { return signaled; }
        void Reset() override { signaled = false; }
        bool Wait(uint64_t timeoutNanoseconds = 0) override
        {
            ++waitCalls;
            lastWaitTimeoutNanoseconds = timeoutNanoseconds;
            return signaled;
        }

        bool signaled = false;
        size_t waitCalls = 0u;
        uint64_t lastWaitTimeoutNanoseconds = 0u;
    };
}

TEST(UploadContextTests, SubmitUploadBufferReturnsCompletedTokenForAcceptedRequest)
{
    auto context = NLS::Render::RHI::CreateDefaultUploadContext();
    TestCommandBuffer commandBuffer;
    uint32_t value = 42u;

    NLS::Render::RHI::UploadBufferRequest request;
    request.destination = std::make_shared<TestBuffer>();
    request.data = &value;
    request.size = sizeof(value);

    const auto submission = context->SubmitUploadBuffer(commandBuffer, request);

    ASSERT_TRUE(submission.accepted);
    ASSERT_NE(submission.completion, nullptr);
    EXPECT_TRUE(submission.completion->IsComplete());
    EXPECT_TRUE(submission.completion->Wait().Succeeded());
}

TEST(UploadContextTests, SubmitUploadBufferReturnsFailedTokenForRejectedRequest)
{
    auto context = NLS::Render::RHI::CreateDefaultUploadContext();
    TestCommandBuffer commandBuffer;

    const auto submission = context->SubmitUploadBuffer(commandBuffer, {});

    ASSERT_FALSE(submission.accepted);
    ASSERT_NE(submission.completion, nullptr);
    const auto status = submission.completion->Wait();
    EXPECT_EQ(status.code, NLS::Render::RHI::RHICompletionStatusCode::Failed);
    EXPECT_NE(status.message.find("UploadBuffer"), std::string::npos);
}

TEST(UploadContextTests, SubmitUploadBatchReturnsSingleCompletionForAcceptedShortLivedRequests)
{
    auto context = NLS::Render::RHI::CreateDefaultUploadContext();
    TestCommandBuffer commandBuffer;
    uint32_t bufferValue = 42u;
    uint32_t textureValue = 7u;

    NLS::Render::RHI::UploadBufferRequest bufferRequest;
    bufferRequest.destination = std::make_shared<TestBuffer>();
    bufferRequest.data = &bufferValue;
    bufferRequest.size = sizeof(bufferValue);

    NLS::Render::RHI::UploadTextureRequest textureRequest;
    textureRequest.destination = std::make_shared<TestTexture>();
    textureRequest.data = &textureValue;
    textureRequest.dataSize = sizeof(textureValue);
    textureRequest.extent = { 1u, 1u, 1u };
    textureRequest.rowPitch = sizeof(textureValue);
    textureRequest.slicePitch = sizeof(textureValue);

    NLS::Render::RHI::UploadBatchRequest batchRequest;
    batchRequest.bufferUploads.push_back(bufferRequest);
    batchRequest.textureUploads.push_back(textureRequest);
    batchRequest.debugName = "ShortLivedUploadBatch";

    const auto submission = context->SubmitUploadBatch(commandBuffer, batchRequest);

    ASSERT_TRUE(submission.accepted);
    EXPECT_EQ(submission.acceptedBufferUploads, 1u);
    EXPECT_EQ(submission.acceptedTextureUploads, 1u);
    EXPECT_FALSE(submission.recordedBackendWork);
    EXPECT_NE(submission.diagnostic.find("validation-only"), std::string::npos);
    ASSERT_NE(submission.completion, nullptr);
    EXPECT_TRUE(submission.completion->Wait().Succeeded());
}

TEST(UploadContextTests, SubmitUploadBatchCanRequireRealBackendRecording)
{
    auto context = NLS::Render::RHI::CreateDefaultUploadContext();
    TestCommandBuffer commandBuffer;
    auto destination = std::make_shared<TestBuffer>();

    const std::array<std::byte, 4> data {
        std::byte{ 1 },
        std::byte{ 2 },
        std::byte{ 3 },
        std::byte{ 4 }
    };

    NLS::Render::RHI::UploadBatchRequest request;
    request.debugName = "RequireBackendWork";
    request.requireRecordedBackendWork = true;
    request.bufferUploads.push_back({
        destination,
        0u,
        data.data(),
        data.size(),
        1u,
        "BufferUpload"
    });

    const auto submission = context->SubmitUploadBatch(commandBuffer, request);

    EXPECT_FALSE(submission.accepted);
    EXPECT_FALSE(submission.recordedBackendWork);
    ASSERT_NE(submission.completion, nullptr);
    EXPECT_EQ(submission.completion->Wait().code, NLS::Render::RHI::RHICompletionStatusCode::Failed);
    EXPECT_NE(submission.diagnostic.find("requires backend-recorded upload work"), std::string::npos);
}

TEST(UploadContextTests, BackendUploadContextRecordsBufferCopyAndKeepsStagingAliveUntilRetired)
{
    auto backend = std::make_shared<TestUploadBackend>();
    auto context = NLS::Render::RHI::CreateBackendUploadContext(backend);
    TestCommandBuffer commandBuffer;
    auto destination = std::make_shared<TestBuffer>();

    const std::array<std::byte, 4> data {
        std::byte{ 1 },
        std::byte{ 2 },
        std::byte{ 3 },
        std::byte{ 4 }
    };

    NLS::Render::RHI::UploadBatchRequest request;
    request.debugName = "RecordedBufferUploadBatch";
    request.requireRecordedBackendWork = true;
    request.bufferUploads.push_back({
        destination,
        8u,
        data.data(),
        data.size(),
        1u,
        "RecordedBufferUpload"
    });

    context->BeginFrame(3u);
    const auto submission = context->SubmitUploadBatch(commandBuffer, request);

    ASSERT_TRUE(submission.accepted) << submission.diagnostic;
    EXPECT_TRUE(submission.recordedBackendWork);
    EXPECT_EQ(submission.acceptedBufferUploads, 1u);
    EXPECT_EQ(backend->createStagingBufferCalls, 1u);
    EXPECT_EQ(backend->lastStagingBytes, std::vector<std::byte>(data.begin(), data.end()));
    EXPECT_EQ(commandBuffer.barrierCalls, 2u);
    ASSERT_EQ(commandBuffer.lastBarrierDesc.bufferBarriers.size(), 1u);
    EXPECT_EQ(commandBuffer.lastBarrierDesc.bufferBarriers.front().buffer, destination);
    EXPECT_EQ(commandBuffer.lastBarrierDesc.bufferBarriers.front().after, NLS::Render::RHI::ResourceState::Unknown);
    EXPECT_EQ(commandBuffer.copyBufferCalls, 1u);
    EXPECT_EQ(commandBuffer.lastBufferCopyRegion.dstOffset, 8u);
    EXPECT_EQ(commandBuffer.lastBufferCopyRegion.size, data.size());
    ASSERT_NE(submission.completion, nullptr);
    EXPECT_TRUE(submission.completion->Wait().Succeeded());
}

TEST(UploadContextTests, BackendUploadContextRejectsCpuVisibleBufferDestinations)
{
    auto backend = std::make_shared<TestUploadBackend>();
    auto context = NLS::Render::RHI::CreateBackendUploadContext(backend);
    TestCommandBuffer commandBuffer;

    NLS::Render::RHI::RHIBufferDesc destinationDesc;
    destinationDesc.size = 16u;
    destinationDesc.usage = NLS::Render::RHI::BufferUsageFlags::Vertex;
    destinationDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    destinationDesc.debugName = "UploadHeapDestination";
    auto destination = std::make_shared<TestBuffer>(
        destinationDesc,
        NLS::Render::RHI::ResourceState::GenericRead);

    const std::array<std::byte, 4> data {
        std::byte{ 1 },
        std::byte{ 2 },
        std::byte{ 3 },
        std::byte{ 4 }
    };

    NLS::Render::RHI::UploadBatchRequest request;
    request.debugName = "RejectUploadHeapDestinationBatch";
    request.requireRecordedBackendWork = true;
    request.bufferUploads.push_back({
        destination,
        0u,
        data.data(),
        data.size(),
        1u,
        "RejectUploadHeapDestination"
    });

    const auto submission = context->SubmitUploadBatch(commandBuffer, request);

    EXPECT_FALSE(submission.accepted);
    EXPECT_FALSE(submission.recordedBackendWork);
    EXPECT_EQ(backend->createStagingBufferCalls, 0u);
    EXPECT_EQ(commandBuffer.barrierCalls, 0u);
    EXPECT_EQ(commandBuffer.copyBufferCalls, 0u);
    ASSERT_NE(submission.completion, nullptr);
    EXPECT_EQ(submission.completion->Wait().code, NLS::Render::RHI::RHICompletionStatusCode::Failed);
    EXPECT_NE(submission.diagnostic.find("CPUToGPU"), std::string::npos);

    destinationDesc.usage = NLS::Render::RHI::BufferUsageFlags::CopyDst;
    destinationDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUToCPU;
    destinationDesc.debugName = "ReadbackHeapUploadDestination";
    request.bufferUploads.front().destination = std::make_shared<TestBuffer>(
        destinationDesc,
        NLS::Render::RHI::ResourceState::CopyDst);

    const auto readbackSubmission = context->SubmitUploadBatch(commandBuffer, request);

    EXPECT_FALSE(readbackSubmission.accepted);
    EXPECT_FALSE(readbackSubmission.recordedBackendWork);
    EXPECT_EQ(backend->createStagingBufferCalls, 0u);
    EXPECT_EQ(commandBuffer.barrierCalls, 0u);
    EXPECT_EQ(commandBuffer.copyBufferCalls, 0u);
    ASSERT_NE(readbackSubmission.completion, nullptr);
    EXPECT_EQ(readbackSubmission.completion->Wait().code, NLS::Render::RHI::RHICompletionStatusCode::Failed);
    EXPECT_NE(readbackSubmission.diagnostic.find("GPUToCPU"), std::string::npos);
}

TEST(UploadContextTests, BackendUploadContextRejectsCpuVisibleTextureDestinations)
{
    auto backend = std::make_shared<TestUploadBackend>();
    auto context = NLS::Render::RHI::CreateBackendUploadContext(backend, 256u);
    TestCommandBuffer commandBuffer;
    const uint32_t pixels[] = { 0xffffffffu, 0xff000000u, 0xffff0000u, 0xff00ff00u };

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.extent = { 2u, 2u, 1u };
    textureDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    auto destination = std::make_shared<TestTexture>(textureDesc);

    NLS::Render::RHI::UploadTextureRequest request;
    request.destination = destination;
    request.data = pixels;
    request.dataSize = sizeof(pixels);
    request.extent = textureDesc.extent;
    request.rowPitch = 8u;
    request.slicePitch = sizeof(pixels);

    const auto submission = context->SubmitUploadTexture(commandBuffer, request);

    EXPECT_FALSE(submission.accepted);
    EXPECT_EQ(commandBuffer.copyBufferToTextureCalls, 0u);
    EXPECT_EQ(commandBuffer.barrierCalls, 0u);
    EXPECT_EQ(backend->createStagingBufferCalls, 0u);
    EXPECT_NE(submission.diagnostic.find("CPUToGPU"), std::string::npos);

    textureDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUToCPU;
    request.destination = std::make_shared<TestTexture>(textureDesc);

    const auto readbackSubmission = context->SubmitUploadTexture(commandBuffer, request);

    EXPECT_FALSE(readbackSubmission.accepted);
    EXPECT_EQ(commandBuffer.copyBufferToTextureCalls, 0u);
    EXPECT_EQ(commandBuffer.barrierCalls, 0u);
    EXPECT_EQ(backend->createStagingBufferCalls, 0u);
    EXPECT_NE(readbackSubmission.diagnostic.find("GPUToCPU"), std::string::npos);
}

TEST(UploadContextTests, ResourceStateTrackerIgnoresOnlyLegalCpuVisibleBufferStates)
{
    auto tracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHIBufferDesc uploadDesc;
    uploadDesc.size = 16u;
    uploadDesc.usage = NLS::Render::RHI::BufferUsageFlags::Vertex;
    uploadDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    auto uploadBuffer = std::make_shared<TestBuffer>(
        uploadDesc,
        NLS::Render::RHI::ResourceState::GenericRead);

    NLS::Render::RHI::RHIBufferDesc gpuDesc;
    gpuDesc.size = 16u;
    gpuDesc.usage = NLS::Render::RHI::BufferUsageFlags::CopyDst;
    gpuDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
    auto gpuBuffer = std::make_shared<TestBuffer>(gpuDesc);

    NLS::Render::RHI::RHIBarrierDesc barriers;
    barriers.bufferBarriers.push_back({
        uploadBuffer,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::CopyDst
    });
    barriers.bufferBarriers.push_back({
        gpuBuffer,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::CopyDst
    });

    const auto resolved = tracker->BuildTransitionBarriers(
        barriers.bufferBarriers,
        barriers.textureBarriers);
    ASSERT_EQ(resolved.bufferBarriers.size(), 2u);
    EXPECT_EQ(resolved.bufferBarriers[0].buffer, uploadBuffer);
    EXPECT_EQ(resolved.bufferBarriers[0].after, NLS::Render::RHI::ResourceState::CopyDst);
    EXPECT_EQ(resolved.bufferBarriers[1].buffer, gpuBuffer);

    tracker->Commit(barriers);
    EXPECT_FALSE(tracker->GetBufferState(uploadBuffer).has_value());
    ASSERT_TRUE(tracker->GetBufferState(gpuBuffer).has_value());
    EXPECT_EQ(
        tracker->GetBufferState(gpuBuffer)->state,
        NLS::Render::RHI::ResourceState::CopyDst);

    NLS::Render::RHI::RHIBarrierDesc legalCpuVisibleBarriers;
    legalCpuVisibleBarriers.bufferBarriers.push_back({
        uploadBuffer,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::GenericRead
    });
    legalCpuVisibleBarriers.bufferBarriers.push_back({
        uploadBuffer,
        NLS::Render::RHI::ResourceState::GenericRead,
        NLS::Render::RHI::ResourceState::GenericRead
    });

    const auto resolvedLegal = tracker->BuildTransitionBarriers(
        legalCpuVisibleBarriers.bufferBarriers,
        legalCpuVisibleBarriers.textureBarriers);
    EXPECT_TRUE(resolvedLegal.bufferBarriers.empty());

    NLS::Render::RHI::RHIBarrierDesc illegalBeforeBarriers;
    illegalBeforeBarriers.bufferBarriers.push_back({
        uploadBuffer,
        NLS::Render::RHI::ResourceState::CopyDst,
        NLS::Render::RHI::ResourceState::GenericRead
    });
    illegalBeforeBarriers.bufferBarriers.push_back({
        uploadBuffer,
        NLS::Render::RHI::ResourceState::GenericRead,
        NLS::Render::RHI::ResourceState::CopyDst
    });

    const auto resolvedIllegalBefore = tracker->BuildTransitionBarriers(
        illegalBeforeBarriers.bufferBarriers,
        illegalBeforeBarriers.textureBarriers);
    ASSERT_EQ(resolvedIllegalBefore.bufferBarriers.size(), 2u);
    EXPECT_EQ(resolvedIllegalBefore.bufferBarriers[0].before, NLS::Render::RHI::ResourceState::CopyDst);
    EXPECT_EQ(resolvedIllegalBefore.bufferBarriers[0].after, NLS::Render::RHI::ResourceState::GenericRead);
    EXPECT_EQ(resolvedIllegalBefore.bufferBarriers[1].before, NLS::Render::RHI::ResourceState::GenericRead);
    EXPECT_EQ(resolvedIllegalBefore.bufferBarriers[1].after, NLS::Render::RHI::ResourceState::CopyDst);
}

TEST(UploadContextTests, ResourceStateTrackerInvalidatesOverlappingTextureRangesOnCommit)
{
    auto tracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHITextureDesc desc;
    desc.debugName = "OverlappingTrackedTexture";
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    desc.extent = { 64u, 64u, 1u };
    desc.mipLevels = 2u;
    desc.arrayLayers = 2u;
    auto texture = std::make_shared<TestTexture>(desc);

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 2u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 2u;

    NLS::Render::RHI::RHISubresourceRange partialRange;
    partialRange.baseMipLevel = 1u;
    partialRange.mipLevelCount = 1u;
    partialRange.baseArrayLayer = 1u;
    partialRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHIBarrierDesc initialFullState;
    initialFullState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        fullRange,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    tracker->Commit(initialFullState);

    NLS::Render::RHI::RHIBarrierDesc partialWriteState;
    partialWriteState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::ResourceState::RenderTarget,
        partialRange,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::AccessMask::ShaderRead,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });
    tracker->Commit(partialWriteState);

    auto trackedPartial = tracker->GetTextureState(texture, partialRange);
    ASSERT_TRUE(trackedPartial.has_value());
    EXPECT_EQ(trackedPartial->state, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_FALSE(tracker->GetTextureState(texture, fullRange).has_value());

    NLS::Render::RHI::RHIBarrierDesc fullCopyState;
    fullCopyState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::CopySrc,
        fullRange,
        NLS::Render::RHI::PipelineStageMask::AllCommands,
        NLS::Render::RHI::PipelineStageMask::Copy,
        NLS::Render::RHI::AccessMask::MemoryRead,
        NLS::Render::RHI::AccessMask::CopyRead
    });
    tracker->Commit(fullCopyState);

    trackedPartial = tracker->GetTextureState(texture, partialRange);
    ASSERT_TRUE(trackedPartial.has_value());
    EXPECT_EQ(trackedPartial->state, NLS::Render::RHI::ResourceState::CopySrc);

    const auto resolved = tracker->BuildTransitionBarriers(
        {},
        {
            {
                texture,
                NLS::Render::RHI::ResourceState::Unknown,
                NLS::Render::RHI::ResourceState::ShaderRead,
                partialRange,
                NLS::Render::RHI::PipelineStageMask::Copy,
                NLS::Render::RHI::PipelineStageMask::FragmentShader,
                NLS::Render::RHI::AccessMask::CopyRead,
                NLS::Render::RHI::AccessMask::ShaderRead
            }
        });
    ASSERT_EQ(resolved.textureBarriers.size(), 1u);
    EXPECT_EQ(resolved.textureBarriers[0].before, NLS::Render::RHI::ResourceState::CopySrc);
}

TEST(UploadContextTests, ResourceStateTrackerKeepsSameStateTextureRangesNonOverlapping)
{
    auto tracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHITextureDesc desc;
    desc.debugName = "SameStateOverlapTrackedTexture";
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    desc.extent = { 64u, 64u, 1u };
    desc.mipLevels = 1u;
    desc.arrayLayers = 3u;
    auto texture = std::make_shared<TestTexture>(desc);

    NLS::Render::RHI::RHISubresourceRange firstRange;
    firstRange.baseMipLevel = 0u;
    firstRange.mipLevelCount = 1u;
    firstRange.baseArrayLayer = 0u;
    firstRange.arrayLayerCount = 2u;

    NLS::Render::RHI::RHISubresourceRange secondRange;
    secondRange.baseMipLevel = 0u;
    secondRange.mipLevelCount = 1u;
    secondRange.baseArrayLayer = 1u;
    secondRange.arrayLayerCount = 2u;

    NLS::Render::RHI::RHIBarrierDesc firstState;
    firstState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        firstRange,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    tracker->Commit(firstState);

    NLS::Render::RHI::RHIBarrierDesc overlappingSameState;
    overlappingSameState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        secondRange,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    tracker->Commit(overlappingSameState);

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 3u;

    const auto resolved = tracker->BuildTransitionBarriers(
        {},
        {
            {
                texture,
                NLS::Render::RHI::ResourceState::Unknown,
                NLS::Render::RHI::ResourceState::CopySrc,
                fullRange,
                NLS::Render::RHI::PipelineStageMask::FragmentShader,
                NLS::Render::RHI::PipelineStageMask::Copy,
                NLS::Render::RHI::AccessMask::ShaderRead,
                NLS::Render::RHI::AccessMask::CopyRead
            }
        });

    ASSERT_EQ(resolved.textureBarriers.size(), 2u);
    EXPECT_EQ(resolved.textureBarriers[0].before, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(resolved.textureBarriers[0].after, NLS::Render::RHI::ResourceState::CopySrc);
    EXPECT_EQ(resolved.textureBarriers[1].before, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(resolved.textureBarriers[1].after, NLS::Render::RHI::ResourceState::CopySrc);

    const auto totalLayerCount =
        resolved.textureBarriers[0].subresourceRange.arrayLayerCount +
        resolved.textureBarriers[1].subresourceRange.arrayLayerCount;
    EXPECT_EQ(totalLayerCount, 3u);
}

TEST(UploadContextTests, ResourceStateTrackerPreservesUnknownHolesWhenResolvingPartialTextureState)
{
    auto tracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHITextureDesc desc;
    desc.debugName = "PartialTrackedTextureWithHole";
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    desc.extent = { 64u, 64u, 1u };
    desc.mipLevels = 1u;
    desc.arrayLayers = 3u;
    auto texture = std::make_shared<TestTexture>(desc);

    NLS::Render::RHI::RHISubresourceRange trackedRange;
    trackedRange.baseMipLevel = 0u;
    trackedRange.mipLevelCount = 1u;
    trackedRange.baseArrayLayer = 0u;
    trackedRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHIBarrierDesc trackedState;
    trackedState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::RenderTarget,
        trackedRange,
        NLS::Render::RHI::PipelineStageMask::AllCommands,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::AccessMask::MemoryRead,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });
    tracker->Commit(trackedState);

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 3u;

    const auto resolved = tracker->BuildTransitionBarriers(
        {},
        {
            {
                texture,
                NLS::Render::RHI::ResourceState::Unknown,
                NLS::Render::RHI::ResourceState::ShaderRead,
                fullRange,
                NLS::Render::RHI::PipelineStageMask::AllCommands,
                NLS::Render::RHI::PipelineStageMask::FragmentShader,
                NLS::Render::RHI::AccessMask::MemoryRead,
                NLS::Render::RHI::AccessMask::ShaderRead
            }
        });

    ASSERT_EQ(resolved.textureBarriers.size(), 2u);
    EXPECT_EQ(resolved.textureBarriers[0].before, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(resolved.textureBarriers[0].after, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(resolved.textureBarriers[0].subresourceRange.baseArrayLayer, 0u);
    EXPECT_EQ(resolved.textureBarriers[0].subresourceRange.arrayLayerCount, 1u);
    EXPECT_EQ(resolved.textureBarriers[1].before, NLS::Render::RHI::ResourceState::Unknown);
    EXPECT_EQ(resolved.textureBarriers[1].after, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(resolved.textureBarriers[1].subresourceRange.baseArrayLayer, 1u);
    EXPECT_EQ(resolved.textureBarriers[1].subresourceRange.arrayLayerCount, 2u);
}

TEST(UploadContextTests, ResourceStateTrackerSuppressesFullRangeNoOpAcrossFragmentedSameStateRanges)
{
    auto tracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHITextureDesc desc;
    desc.debugName = "FragmentedSameStateTexture";
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    desc.extent = { 64u, 64u, 1u };
    desc.mipLevels = 1u;
    desc.arrayLayers = 3u;
    auto texture = std::make_shared<TestTexture>(desc);

    NLS::Render::RHI::RHISubresourceRange firstRange;
    firstRange.baseMipLevel = 0u;
    firstRange.mipLevelCount = 1u;
    firstRange.baseArrayLayer = 0u;
    firstRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHISubresourceRange secondRange;
    secondRange.baseMipLevel = 0u;
    secondRange.mipLevelCount = 1u;
    secondRange.baseArrayLayer = 1u;
    secondRange.arrayLayerCount = 2u;

    NLS::Render::RHI::RHIBarrierDesc fragmentedState;
    fragmentedState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        firstRange,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    fragmentedState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        secondRange,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    tracker->Commit(fragmentedState);

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 3u;

    const auto resolved = tracker->BuildTransitionBarriers(
        {},
        {
            {
                texture,
                NLS::Render::RHI::ResourceState::Unknown,
                NLS::Render::RHI::ResourceState::ShaderRead,
                fullRange,
                NLS::Render::RHI::PipelineStageMask::FragmentShader,
                NLS::Render::RHI::PipelineStageMask::FragmentShader,
                NLS::Render::RHI::AccessMask::ShaderRead,
                NLS::Render::RHI::AccessMask::ShaderRead
            }
        });
    EXPECT_TRUE(resolved.textureBarriers.empty());
}

TEST(UploadContextTests, ResourceStateTrackerExpandsHalfSpecifiedTextureRanges)
{
    auto tracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHITextureDesc desc;
    desc.debugName = "HalfSpecifiedTrackedTexture";
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    desc.extent = { 64u, 64u, 1u };
    desc.mipLevels = 4u;
    desc.arrayLayers = 3u;
    auto texture = std::make_shared<TestTexture>(desc);

    NLS::Render::RHI::RHISubresourceRange mipOnlyRange;
    mipOnlyRange.baseMipLevel = 1u;
    mipOnlyRange.mipLevelCount = 1u;
    mipOnlyRange.baseArrayLayer = 0u;
    mipOnlyRange.arrayLayerCount = 0u;

    NLS::Render::RHI::RHIBarrierDesc mipOnlyState;
    mipOnlyState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        mipOnlyRange,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    tracker->Commit(mipOnlyState);

    NLS::Render::RHI::RHISubresourceRange expandedMipRange;
    expandedMipRange.baseMipLevel = 1u;
    expandedMipRange.mipLevelCount = 1u;
    expandedMipRange.baseArrayLayer = 2u;
    expandedMipRange.arrayLayerCount = 1u;
    auto trackedMipOnly = tracker->GetTextureState(texture, expandedMipRange);
    ASSERT_TRUE(trackedMipOnly.has_value());
    EXPECT_EQ(trackedMipOnly->state, NLS::Render::RHI::ResourceState::ShaderRead);

    NLS::Render::RHI::RHISubresourceRange layerOnlyRange;
    layerOnlyRange.baseMipLevel = 2u;
    layerOnlyRange.mipLevelCount = 0u;
    layerOnlyRange.baseArrayLayer = 1u;
    layerOnlyRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHIBarrierDesc layerOnlyState;
    layerOnlyState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::CopySrc,
        layerOnlyRange,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::PipelineStageMask::Copy,
        NLS::Render::RHI::AccessMask::ShaderRead,
        NLS::Render::RHI::AccessMask::CopyRead
    });
    tracker->Commit(layerOnlyState);

    NLS::Render::RHI::RHISubresourceRange expandedLayerRange;
    expandedLayerRange.baseMipLevel = 3u;
    expandedLayerRange.mipLevelCount = 1u;
    expandedLayerRange.baseArrayLayer = 1u;
    expandedLayerRange.arrayLayerCount = 1u;
    auto trackedLayerOnly = tracker->GetTextureState(texture, expandedLayerRange);
    ASSERT_TRUE(trackedLayerOnly.has_value());
    EXPECT_EQ(trackedLayerOnly->state, NLS::Render::RHI::ResourceState::CopySrc);
}

TEST(UploadContextTests, ResourceStateTrackerRetiresAllTrackedRangesForTransientTexture)
{
    auto tracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHITextureDesc desc;
    desc.debugName = "TransientTrackedTexture";
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    desc.extent = { 64u, 64u, 1u };
    desc.mipLevels = 1u;
    desc.arrayLayers = 3u;
    auto texture = std::make_shared<TestTexture>(desc);

    NLS::Render::RHI::RHISubresourceRange firstRange;
    firstRange.baseMipLevel = 0u;
    firstRange.mipLevelCount = 1u;
    firstRange.baseArrayLayer = 0u;
    firstRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHISubresourceRange secondRange;
    secondRange.baseMipLevel = 0u;
    secondRange.mipLevelCount = 1u;
    secondRange.baseArrayLayer = 1u;
    secondRange.arrayLayerCount = 2u;

    NLS::Render::RHI::RHIBarrierDesc barriers;
    barriers.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        firstRange,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    barriers.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::CopySrc,
        secondRange,
        NLS::Render::RHI::PipelineStageMask::Copy,
        NLS::Render::RHI::PipelineStageMask::Copy,
        NLS::Render::RHI::AccessMask::CopyRead,
        NLS::Render::RHI::AccessMask::CopyRead
    });
    tracker->Commit(barriers);

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 3u;

    EXPECT_EQ(tracker->GetStats().trackedTextureCount, 2u);
    tracker->RegisterTransientTexture(texture, fullRange, 7u);
    tracker->RetireTransientResources(7u);

    EXPECT_EQ(tracker->GetStats().trackedTextureCount, 0u);
    EXPECT_FALSE(tracker->GetTextureState(texture, firstRange).has_value());
    EXPECT_FALSE(tracker->GetTextureState(texture, secondRange).has_value());
}

TEST(UploadContextTests, ResourceStateTrackerRetiresOnlyRegisteredTransientTextureRange)
{
    auto tracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHITextureDesc desc;
    desc.debugName = "TransientPartialTrackedTexture";
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    desc.extent = { 64u, 64u, 1u };
    desc.mipLevels = 1u;
    desc.arrayLayers = 3u;
    auto texture = std::make_shared<TestTexture>(desc);

    NLS::Render::RHI::RHISubresourceRange firstRange;
    firstRange.baseMipLevel = 0u;
    firstRange.mipLevelCount = 1u;
    firstRange.baseArrayLayer = 0u;
    firstRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHISubresourceRange secondRange;
    secondRange.baseMipLevel = 0u;
    secondRange.mipLevelCount = 1u;
    secondRange.baseArrayLayer = 1u;
    secondRange.arrayLayerCount = 2u;

    NLS::Render::RHI::RHIBarrierDesc barriers;
    barriers.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        firstRange,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    barriers.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::CopySrc,
        secondRange,
        NLS::Render::RHI::PipelineStageMask::Copy,
        NLS::Render::RHI::PipelineStageMask::Copy,
        NLS::Render::RHI::AccessMask::CopyRead,
        NLS::Render::RHI::AccessMask::CopyRead
    });
    tracker->Commit(barriers);

    tracker->RegisterTransientTexture(texture, firstRange, 7u);
    tracker->RetireTransientResources(7u);

    EXPECT_FALSE(tracker->GetTextureState(texture, firstRange).has_value());
    const auto trackedSecondRange = tracker->GetTextureState(texture, secondRange);
    ASSERT_TRUE(trackedSecondRange.has_value());
    EXPECT_EQ(trackedSecondRange->state, NLS::Render::RHI::ResourceState::CopySrc);
}

TEST(UploadContextTests, ResourceStateTrackerCanonicalizesTexture3DWSlicesToMipSubresources)
{
    auto tracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHITextureDesc desc;
    desc.debugName = "Tracked3DTexture";
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture3D;
    desc.extent = { 16u, 16u, 8u };
    desc.mipLevels = 3u;
    desc.arrayLayers = 1u;
    auto texture = std::make_shared<TestTexture>(desc);

    NLS::Render::RHI::RHISubresourceRange firstWSlice;
    firstWSlice.baseMipLevel = 1u;
    firstWSlice.mipLevelCount = 1u;
    firstWSlice.baseArrayLayer = 0u;
    firstWSlice.arrayLayerCount = 1u;

    NLS::Render::RHI::RHISubresourceRange anotherWSlice = firstWSlice;
    anotherWSlice.baseArrayLayer = 4u;
    anotherWSlice.arrayLayerCount = 2u;

    NLS::Render::RHI::RHIBarrierDesc barriers;
    barriers.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::RenderTarget,
        firstWSlice,
        NLS::Render::RHI::PipelineStageMask::AllCommands,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::AccessMask::MemoryRead,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });
    tracker->Commit(barriers);

    const auto tracked = tracker->GetTextureState(texture, anotherWSlice);
    ASSERT_TRUE(tracked.has_value());
    EXPECT_EQ(tracked->state, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(tracked->subresourceRange.baseMipLevel, 1u);
    EXPECT_EQ(tracked->subresourceRange.mipLevelCount, 1u);
    EXPECT_EQ(tracked->subresourceRange.baseArrayLayer, 0u);
    EXPECT_EQ(tracked->subresourceRange.arrayLayerCount, 1u);
}

TEST(UploadContextTests, BackendUploadContextCompletionCanTrackSubmittedGpuFence)
{
    auto backend = std::make_shared<TestUploadBackend>();
    auto context = NLS::Render::RHI::CreateBackendUploadContext(backend);
    TestCommandBuffer commandBuffer;
    auto destination = std::make_shared<TestBuffer>();
    auto fence = std::make_shared<TestFence>();

    const std::array<std::byte, 4> data {
        std::byte{ 1 },
        std::byte{ 2 },
        std::byte{ 3 },
        std::byte{ 4 }
    };

    NLS::Render::RHI::UploadBatchRequest request;
    request.debugName = "FenceBackedUploadCompletion";
    request.requireRecordedBackendWork = true;
    request.completionFence = fence;
    request.bufferUploads.push_back({
        destination,
        0u,
        data.data(),
        data.size(),
        1u,
        "FenceBackedUpload"
    });

    const auto submission = context->SubmitUploadBatch(commandBuffer, request);

    ASSERT_TRUE(submission.accepted) << submission.diagnostic;
    ASSERT_NE(submission.completion, nullptr);
    EXPECT_EQ(submission.completion->Poll().code, NLS::Render::RHI::RHICompletionStatusCode::Pending);
    EXPECT_FALSE(submission.completion->Wait(123u).Succeeded());
    EXPECT_EQ(fence->waitCalls, 1u);
    EXPECT_EQ(fence->lastWaitTimeoutNanoseconds, 123u);

    fence->signaled = true;
    EXPECT_TRUE(submission.completion->Poll().Succeeded());
    EXPECT_TRUE(submission.completion->Wait().Succeeded());
}

TEST(UploadContextTests, BackendUploadContextRecordsTextureCopyWithUploadPitch)
{
    auto backend = std::make_shared<TestUploadBackend>();
    auto context = NLS::Render::RHI::CreateBackendUploadContext(backend);
    TestCommandBuffer commandBuffer;

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.extent = { 2u, 2u, 1u };
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto destination = std::make_shared<TestTexture>(textureDesc);

    const std::array<std::byte, 16> data {};

    NLS::Render::RHI::UploadBatchRequest request;
    request.debugName = "RecordedTextureUploadBatch";
    request.requireRecordedBackendWork = true;
    request.textureUploads.push_back({
        destination,
        data.data(),
        data.size(),
        0u,
        0u,
        8u,
        16u,
        { 2u, 2u, 1u },
        "RecordedTextureUpload"
    });

    context->BeginFrame(5u);
    const auto submission = context->SubmitUploadBatch(commandBuffer, request);

    ASSERT_TRUE(submission.accepted) << submission.diagnostic;
    EXPECT_TRUE(submission.recordedBackendWork);
    EXPECT_EQ(submission.acceptedTextureUploads, 1u);
    EXPECT_EQ(commandBuffer.copyBufferToTextureCalls, 1u);
    EXPECT_EQ(commandBuffer.lastTextureCopyDesc.destination, destination);
    EXPECT_EQ(commandBuffer.lastTextureCopyDesc.extent.width, 2u);
    EXPECT_EQ(commandBuffer.lastTextureCopyDesc.extent.height, 2u);
    EXPECT_EQ(commandBuffer.lastTextureCopyDesc.rowPitch, 8u);
    EXPECT_EQ(commandBuffer.lastTextureCopyDesc.slicePitch, 16u);
    ASSERT_EQ(commandBuffer.lastBarrierDesc.textureBarriers.size(), 1u);
    EXPECT_EQ(commandBuffer.lastBarrierDesc.textureBarriers.front().texture, destination);
    EXPECT_EQ(commandBuffer.lastBarrierDesc.textureBarriers.front().after, NLS::Render::RHI::ResourceState::ShaderRead);
}

TEST(UploadContextTests, BackendUploadContextDoesNotRecordPartialBatchWhenStagingCreationFails)
{
    auto backend = std::make_shared<TestUploadBackend>();
    backend->failOnCreateCall = 2u;
    auto context = NLS::Render::RHI::CreateBackendUploadContext(backend);
    TestCommandBuffer commandBuffer;

    const std::array<std::byte, 4> firstData {
        std::byte{ 1 },
        std::byte{ 2 },
        std::byte{ 3 },
        std::byte{ 4 }
    };
    const std::array<std::byte, 4> secondData {
        std::byte{ 5 },
        std::byte{ 6 },
        std::byte{ 7 },
        std::byte{ 8 }
    };

    NLS::Render::RHI::UploadBatchRequest request;
    request.debugName = "AtomicRecordedUploadBatch";
    request.requireRecordedBackendWork = true;
    request.bufferUploads.push_back({
        std::make_shared<TestBuffer>(),
        0u,
        firstData.data(),
        firstData.size(),
        1u,
        "FirstUpload"
    });
    request.bufferUploads.push_back({
        std::make_shared<TestBuffer>(),
        4u,
        secondData.data(),
        secondData.size(),
        1u,
        "SecondUpload"
    });

    context->BeginFrame(9u);
    const auto submission = context->SubmitUploadBatch(commandBuffer, request);

    EXPECT_FALSE(submission.accepted);
    EXPECT_FALSE(submission.recordedBackendWork);
    EXPECT_EQ(backend->createStagingBufferCalls, 2u);
    EXPECT_EQ(commandBuffer.barrierCalls, 0u);
    EXPECT_EQ(commandBuffer.copyBufferCalls, 0u);
    EXPECT_EQ(commandBuffer.copyBufferToTextureCalls, 0u);
    ASSERT_NE(submission.completion, nullptr);
    EXPECT_EQ(submission.completion->Wait().code, NLS::Render::RHI::RHICompletionStatusCode::Failed);
    EXPECT_NE(submission.diagnostic.find("bufferUploads[1]"), std::string::npos);
}

#if defined(_WIN32)
TEST(UploadContextTests, DX12BackendUploadContextRecordsRealBackendWork)
{
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
    {
        GTEST_SKIP() << "DX12 device unavailable on this test machine";
    }

    auto device = NLS::Render::Backend::CreateNativeDX12ExplicitDevice(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        resources.computeQueue.Get(),
        resources.factory.Get(),
        resources.adapter.Get(),
        resources.capabilities,
        resources.vendor,
        resources.hardware);
    ASSERT_NE(device, nullptr);

    auto commandPool = device->CreateCommandPool(NLS::Render::RHI::QueueType::Graphics, "DX12UploadContextPool");
    ASSERT_NE(commandPool, nullptr);
    auto commandBuffer = commandPool->CreateCommandBuffer("DX12UploadContextCommands");
    ASSERT_NE(commandBuffer, nullptr);

    NLS::Render::RHI::RHIBufferDesc destinationDesc;
    destinationDesc.size = 16u;
    destinationDesc.usage = NLS::Render::RHI::BufferUsageFlags::CopyDst;
    destinationDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
    destinationDesc.debugName = "DX12UploadContextDestination";
    auto destination = device->CreateBuffer(destinationDesc);
    ASSERT_NE(destination, nullptr);

    auto backend = NLS::Render::Backend::CreateDX12UploadBackend(resources.device.Get());
    ASSERT_NE(backend, nullptr);
    auto context = NLS::Render::RHI::CreateBackendUploadContext(std::move(backend));

    const std::array<std::byte, 4> data {
        std::byte{ 9 },
        std::byte{ 8 },
        std::byte{ 7 },
        std::byte{ 6 }
    };
    NLS::Render::RHI::UploadBatchRequest request;
    request.debugName = "DX12RecordedUpload";
    request.requireRecordedBackendWork = true;
    request.bufferUploads.push_back({
        destination,
        0u,
        data.data(),
        data.size(),
        1u,
        "DX12RecordedBufferUpload"
    });

    commandBuffer->Begin();
    context->BeginFrame(1u);
    const auto submission = context->SubmitUploadBatch(*commandBuffer, request);
    commandBuffer->End();

    EXPECT_TRUE(submission.accepted) << submission.diagnostic;
    EXPECT_TRUE(submission.recordedBackendWork);
    EXPECT_EQ(submission.acceptedBufferUploads, 1u);
    ASSERT_NE(submission.completion, nullptr);
    EXPECT_TRUE(submission.completion->Wait().Succeeded());
}
#endif

TEST(UploadContextTests, SubmitUploadBatchRejectsInvalidRequestWithIndexedDiagnostic)
{
    auto context = NLS::Render::RHI::CreateDefaultUploadContext();
    TestCommandBuffer commandBuffer;
    uint32_t value = 42u;

    NLS::Render::RHI::UploadBufferRequest validRequest;
    validRequest.destination = std::make_shared<TestBuffer>();
    validRequest.data = &value;
    validRequest.size = sizeof(value);

    NLS::Render::RHI::UploadBatchRequest batchRequest;
    batchRequest.bufferUploads.push_back(validRequest);
    batchRequest.bufferUploads.push_back({});

    const auto submission = context->SubmitUploadBatch(commandBuffer, batchRequest);

    ASSERT_FALSE(submission.accepted);
    EXPECT_EQ(submission.acceptedBufferUploads, 1u);
    EXPECT_EQ(submission.acceptedTextureUploads, 0u);
    EXPECT_NE(submission.diagnostic.find("bufferUploads[1]"), std::string::npos);
    ASSERT_NE(submission.completion, nullptr);
    EXPECT_EQ(submission.completion->Wait().code, NLS::Render::RHI::RHICompletionStatusCode::Failed);
}

TEST(UploadContextTests, RingAllocatorRejectsWrapThatWouldOverlapUnretiredAllocations)
{
    auto context = NLS::Render::RHI::CreateDefaultUploadContext(64u * 1024u);
    context->BeginFrame(7u);

    const auto first = context->Allocate(48u * 1024u, 1u, "Frame7UploadA");
    ASSERT_NE(first.cpuAddress, nullptr);
    EXPECT_EQ(first.gpuOffset, 0u);

    const auto overlapping = context->Allocate(32u * 1024u, 1u, "Frame7UploadB");
    EXPECT_EQ(overlapping.cpuAddress, nullptr);
    EXPECT_EQ(overlapping.size, 0u);

    context->EndFrame(7u);
    context->BeginFrame(8u);
    const auto afterRetirement = context->Allocate(32u * 1024u, 1u, "Frame8Upload");
    ASSERT_NE(afterRetirement.cpuAddress, nullptr);
    EXPECT_EQ(afterRetirement.gpuOffset, 0u);
}

TEST(UploadContextTests, RingAllocatorRejectsRequestsLargerThanCapacity)
{
    auto context = NLS::Render::RHI::CreateDefaultUploadContext(64u * 1024u);
    context->BeginFrame(1u);

    const auto allocation = context->Allocate(65u * 1024u, 1u, "TooLargeUpload");

    EXPECT_EQ(allocation.cpuAddress, nullptr);
    EXPECT_EQ(allocation.size, 0u);
}
