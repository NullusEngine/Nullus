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
