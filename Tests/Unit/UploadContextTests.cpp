#include <gtest/gtest.h>

#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"

namespace
{
    class TestBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "UploadContextTestsBuffer"; }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }
        uint64_t GetGPUAddress() const override { return 0u; }

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc{};
    };

    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
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
            const NLS::Render::RHI::RHIBufferCopyRegion&) override {}
        void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc&) override {}
        void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc&) override {}
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
    ASSERT_NE(submission.completion, nullptr);
    EXPECT_TRUE(submission.completion->Wait().Succeeded());
}

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
