#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"

#include <algorithm>
#include <deque>
#include <cstring>
#include <vector>

#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI
{
    namespace
    {
        class CompletedUploadToken final : public RHICompletionToken
        {
        public:
            explicit CompletedUploadToken(RHICompletionStatus status)
                : m_status(std::move(status))
            {
            }

            std::string_view GetDebugName() const override { return "CompletedUploadToken"; }
            RHICompletionStatus Poll() override { return m_status; }
            bool IsComplete() override { return true; }
            RHICompletionStatus GetStatus() override { return m_status; }
            RHICompletionStatus Wait(uint64_t = 0) override { return m_status; }

        private:
            RHICompletionStatus m_status;
        };

        class FenceBackedUploadToken final : public RHICompletionToken
        {
        public:
            FenceBackedUploadToken(std::shared_ptr<RHIFence> fence, std::string debugName)
                : m_fence(std::move(fence))
                , m_debugName(std::move(debugName))
            {
            }

            std::string_view GetDebugName() const override { return m_debugName; }
            RHICompletionStatus Poll() override
            {
                if (m_fence == nullptr)
                    return { RHICompletionStatusCode::Failed, m_debugName + ": missing upload completion fence" };
                if (!m_fence->IsSignaled())
                    return { RHICompletionStatusCode::Pending, m_debugName + ": waiting for upload fence" };
                return { RHICompletionStatusCode::Success, m_debugName + ": upload fence signaled" };
            }

            RHICompletionStatus Wait(uint64_t timeoutNanoseconds = 0) override
            {
                if (m_fence == nullptr)
                    return { RHICompletionStatusCode::Failed, m_debugName + ": missing upload completion fence" };
                if (!m_fence->Wait(timeoutNanoseconds))
                    return { RHICompletionStatusCode::Pending, m_debugName + ": upload fence wait timed out or failed" };
                return { RHICompletionStatusCode::Success, m_debugName + ": upload fence signaled" };
            }

        private:
            std::shared_ptr<RHIFence> m_fence;
            std::string m_debugName;
        };

        struct RetiredAllocation
        {
            uint64_t frameIndex = 0;
            size_t begin = 0;
            size_t end = 0;
        };

        struct RetiredStagingResource
        {
            uint64_t frameIndex = 0u;
            std::shared_ptr<RHIBuffer> buffer;
        };

        struct PreparedBufferUpload
        {
            const UploadBufferRequest* request = nullptr;
            std::shared_ptr<RHIBuffer> staging;
        };

        struct PreparedTextureUpload
        {
            const UploadTextureRequest* request = nullptr;
            std::shared_ptr<RHIBuffer> staging;
        };

        std::string MemoryUsageName(const MemoryUsage memoryUsage)
        {
            switch (memoryUsage)
            {
            case MemoryUsage::GPUOnly:
                return "GPUOnly";
            case MemoryUsage::CPUToGPU:
                return "CPUToGPU";
            case MemoryUsage::GPUToCPU:
                return "GPUToCPU";
            default:
                return "Unknown";
            }
        }

        ResourceState ResolveUploadedTextureState(const RHITextureDesc& desc)
        {
            if (HasTextureUsage(desc.usage, TextureUsageFlags::Sampled))
                return ResourceState::ShaderRead;
            if (HasTextureUsage(desc.usage, TextureUsageFlags::Storage))
                return ResourceState::ShaderWrite;
            if (HasTextureUsage(desc.usage, TextureUsageFlags::ColorAttachment))
                return ResourceState::RenderTarget;
            if (HasTextureUsage(desc.usage, TextureUsageFlags::DepthStencilAttachment))
                return ResourceState::DepthWrite;
            if (HasTextureUsage(desc.usage, TextureUsageFlags::CopySrc))
                return ResourceState::CopySrc;
            if (HasTextureUsage(desc.usage, TextureUsageFlags::CopyDst))
                return ResourceState::CopyDst;
            if (HasTextureUsage(desc.usage, TextureUsageFlags::Present))
                return ResourceState::Present;
            return ResourceState::Unknown;
        }

        RHIBarrierDesc BuildBufferBarrier(
            const std::shared_ptr<RHIBuffer>& buffer,
            ResourceState before,
            ResourceState after)
        {
            RHIBarrierDesc barrierDesc;
            RHIBufferBarrier barrier;
            barrier.buffer = buffer;
            barrier.before = before;
            barrier.after = after;
            barrier.sourceStageMask = PipelineStageMask::AllCommands;
            barrier.destinationStageMask = PipelineStageMask::Copy;
            barrier.sourceAccessMask = AccessMask::MemoryRead | AccessMask::MemoryWrite;
            barrier.destinationAccessMask = AccessMask::CopyWrite;
            barrierDesc.bufferBarriers.push_back(std::move(barrier));
            return barrierDesc;
        }

        RHIBarrierDesc BuildTextureBarrier(
            const std::shared_ptr<RHITexture>& texture,
            ResourceState before,
            ResourceState after)
        {
            RHIBarrierDesc barrierDesc;
            RHITextureBarrier barrier;
            barrier.texture = texture;
            barrier.before = before;
            barrier.after = after;
            barrier.sourceStageMask = PipelineStageMask::AllCommands;
            barrier.destinationStageMask = after == ResourceState::CopyDst
                ? PipelineStageMask::Copy
                : PipelineStageMask::AllCommands;
            barrier.sourceAccessMask = AccessMask::MemoryRead | AccessMask::MemoryWrite;
            barrier.destinationAccessMask = after == ResourceState::CopyDst
                ? AccessMask::CopyWrite
                : (AccessMask::MemoryRead | AccessMask::MemoryWrite);
            barrier.subresourceRange = {};
            barrierDesc.textureBarriers.push_back(std::move(barrier));
            return barrierDesc;
        }

        class DefaultUploadContext final : public UploadContext
        {
        public:
            explicit DefaultUploadContext(size_t ringCapacity)
                : m_ring(std::max<size_t>(ringCapacity, 64 * 1024), std::byte{ 0 })
            {
            }

            void BeginFrame(uint64_t frameIndex) override
            {
                m_currentFrameIndex = frameIndex;
            }

            void EndFrame(uint64_t completedFrameIndex) override
            {
                CollectGarbage(completedFrameIndex);
            }

            UploadAllocation Allocate(size_t size, size_t alignment = 1, std::string debugName = {}) override
            {
                UploadAllocation allocation{};
                if (size == 0 || size > m_ring.size() || m_ring.empty())
                    return allocation;

                alignment = std::max<size_t>(alignment, 1);
                size_t alignedOffset = AlignUp(m_head, alignment);
                if (CanAllocateRange(alignedOffset, size))
                    return CommitAllocation(alignedOffset, size, alignment, std::move(debugName));

                if (alignedOffset + size > m_ring.size())
                {
                    alignedOffset = AlignUp(size_t{ 0 }, alignment);
                    if (CanAllocateRange(alignedOffset, size))
                        return CommitAllocation(alignedOffset, size, alignment, std::move(debugName));
                }

                return allocation;
            }

            UploadBatchSubmission SubmitUploadBatch(RHICommandBuffer&, const UploadBatchRequest& request) override
            {
                UploadBatchSubmission submission{};
                for (size_t index = 0u; index < request.bufferUploads.size(); ++index)
                {
                    const auto validation = ValidateUploadBufferRequest(request.bufferUploads[index]);
                    if (!validation.empty())
                    {
                        submission.diagnostic =
                            "UploadBatch " + request.debugName + " bufferUploads[" +
                            std::to_string(index) + "]: " + validation;
                        submission.completion = BuildImmediateToken(false, submission.diagnostic);
                        return submission;
                    }
                    ++submission.acceptedBufferUploads;
                }

                for (size_t index = 0u; index < request.textureUploads.size(); ++index)
                {
                    const auto validation = ValidateUploadTextureRequest(request.textureUploads[index]);
                    if (!validation.empty())
                    {
                        submission.diagnostic =
                            "UploadBatch " + request.debugName + " textureUploads[" +
                            std::to_string(index) + "]: " + validation;
                        submission.completion = BuildImmediateToken(false, submission.diagnostic);
                        return submission;
                    }
                    ++submission.acceptedTextureUploads;
                }

                submission.accepted = submission.acceptedBufferUploads != 0u ||
                    submission.acceptedTextureUploads != 0u;
                if (submission.accepted && request.requireRecordedBackendWork)
                {
                    submission.diagnostic =
                        "UploadBatch " + request.debugName +
                        " requires backend-recorded upload work, but DefaultUploadContext only validates requests";
                    submission.accepted = false;
                    submission.recordedBackendWork = false;
                    submission.completion = BuildImmediateToken(false, submission.diagnostic);
                    return submission;
                }

                submission.diagnostic = submission.accepted
                    ? "DefaultUploadContext accepted upload batch as validation-only; no backend copy/barrier commands were recorded"
                    : "UploadBatch request has no uploads";
                submission.recordedBackendWork = false;
                submission.completion = BuildImmediateToken(submission.accepted, submission.diagnostic);
                return submission;
            }

            UploadSubmission SubmitUploadBuffer(RHICommandBuffer& commandBuffer, const UploadBufferRequest& request) override
            {
                UploadBatchRequest batchRequest;
                batchRequest.bufferUploads.push_back(request);
                batchRequest.debugName = request.debugName;
                const auto batchSubmission = SubmitUploadBatch(commandBuffer, batchRequest);
                return {
                    batchSubmission.accepted,
                    batchSubmission.completion,
                    batchSubmission.accepted ? std::string{} : batchSubmission.diagnostic
                };
            }

            UploadSubmission SubmitUploadTexture(RHICommandBuffer& commandBuffer, const UploadTextureRequest& request) override
            {
                UploadBatchRequest batchRequest;
                batchRequest.textureUploads.push_back(request);
                batchRequest.debugName = request.debugName;
                const auto batchSubmission = SubmitUploadBatch(commandBuffer, batchRequest);
                return {
                    batchSubmission.accepted,
                    batchSubmission.completion,
                    batchSubmission.accepted ? std::string{} : batchSubmission.diagnostic
                };
            }

            bool UploadBuffer(RHICommandBuffer& commandBuffer, const UploadBufferRequest& request) override
            {
                return SubmitUploadBuffer(commandBuffer, request).accepted;
            }

            bool UploadTexture(RHICommandBuffer& commandBuffer, const UploadTextureRequest& request) override
            {
                return SubmitUploadTexture(commandBuffer, request).accepted;
            }

            void CollectGarbage(uint64_t completedFrameIndex) override
            {
                while (!m_retiredAllocations.empty() && m_retiredAllocations.front().frameIndex <= completedFrameIndex)
                {
                    m_tail = m_retiredAllocations.front().end;
                    m_retiredAllocations.pop_front();
                }

                if (m_retiredAllocations.empty() && m_tail == m_head)
                {
                    m_tail = 0;
                    m_head = 0;
                }
            }

        private:
            static size_t AlignUp(size_t value, size_t alignment)
            {
                return (value + alignment - 1) & ~(alignment - 1);
            }

            bool CanAllocateRange(size_t begin, size_t size) const
            {
                const size_t end = begin + size;
                if (size == 0 || begin >= m_ring.size() || end > m_ring.size() || end < begin)
                    return false;

                return std::none_of(
                    m_retiredAllocations.begin(),
                    m_retiredAllocations.end(),
                    [begin, end](const RetiredAllocation& retired)
                    {
                        return begin < retired.end && end > retired.begin;
                    });
            }

            UploadAllocation CommitAllocation(
                size_t alignedOffset,
                size_t size,
                size_t alignment,
                std::string debugName)
            {
                UploadAllocation allocation{};
                allocation.cpuAddress = m_ring.data() + alignedOffset;
                allocation.gpuOffset = alignedOffset;
                allocation.size = size;
                allocation.alignment = alignment;
                allocation.debugName = std::move(debugName);
                m_head = alignedOffset + size;
                m_retiredAllocations.push_back({ m_currentFrameIndex, alignedOffset, alignedOffset + size });
                return allocation;
            }

            static std::string ValidateUploadBufferRequest(const UploadBufferRequest& request)
            {
                if (request.destination == nullptr || request.data == nullptr || request.size == 0)
                    return "UploadBuffer request is missing destination, data, or size";
                if (request.destination->GetDesc().memoryUsage != MemoryUsage::GPUOnly)
                    return "UploadBuffer destination uses " +
                        MemoryUsageName(request.destination->GetDesc().memoryUsage) +
                        " memory; buffer upload destinations must be GPUOnly";
                return {};
            }

            static std::string ValidateUploadTextureRequest(const UploadTextureRequest& request)
            {
                if (request.destination == nullptr || request.data == nullptr || request.dataSize == 0)
                    return "UploadTexture request is missing destination, data, or dataSize";
                if (request.destination->GetDesc().memoryUsage != MemoryUsage::GPUOnly)
                    return "UploadTexture destination uses " +
                        MemoryUsageName(request.destination->GetDesc().memoryUsage) +
                        " memory; texture upload destinations must be GPUOnly";
                return {};
            }

            static std::shared_ptr<RHICompletionToken> BuildImmediateToken(bool accepted, const std::string& diagnostic)
            {
                return std::make_shared<CompletedUploadToken>(RHICompletionStatus{
                    accepted ? RHICompletionStatusCode::Success : RHICompletionStatusCode::Failed,
                    diagnostic
                });
            }

        private:
            uint64_t m_currentFrameIndex = 0;
            size_t m_head = 0;
            size_t m_tail = 0;
            std::vector<std::byte> m_ring;
            std::deque<RetiredAllocation> m_retiredAllocations;
        };

        class BackendUploadContext final : public UploadContext
        {
        public:
            BackendUploadContext(std::shared_ptr<UploadBackend> backend, size_t ringCapacity)
                : m_backend(std::move(backend))
                , m_validationContext(CreateDefaultUploadContext(ringCapacity))
            {
            }

            void BeginFrame(uint64_t frameIndex) override
            {
                m_currentFrameIndex = frameIndex;
                if (m_validationContext != nullptr)
                    m_validationContext->BeginFrame(frameIndex);
            }

            void EndFrame(uint64_t completedFrameIndex) override
            {
                if (m_validationContext != nullptr)
                    m_validationContext->EndFrame(completedFrameIndex);
                CollectGarbage(completedFrameIndex);
            }

            UploadAllocation Allocate(size_t size, size_t alignment = 1, std::string debugName = {}) override
            {
                return m_validationContext != nullptr
                    ? m_validationContext->Allocate(size, alignment, std::move(debugName))
                    : UploadAllocation{};
            }

            UploadBatchSubmission SubmitUploadBatch(RHICommandBuffer& commandBuffer, const UploadBatchRequest& request) override
            {
                UploadBatchSubmission validation = m_validationContext != nullptr
                    ? m_validationContext->SubmitUploadBatch(commandBuffer, BuildValidationOnlyRequest(request))
                    : UploadBatchSubmission{};
                if (!validation.accepted)
                    return validation;

                if (m_backend == nullptr)
                {
                    validation.accepted = false;
                    validation.recordedBackendWork = false;
                    validation.diagnostic =
                        "UploadBatch " + request.debugName + " requires backend upload work, but no backend is attached";
                    validation.completion = BuildImmediateToken(false, validation.diagnostic);
                    return validation;
                }

                UploadBatchSubmission submission{};
                submission.acceptedBufferUploads = validation.acceptedBufferUploads;
                submission.acceptedTextureUploads = validation.acceptedTextureUploads;
                submission.accepted = true;
                submission.recordedBackendWork = true;

                std::vector<PreparedBufferUpload> preparedBufferUploads;
                preparedBufferUploads.reserve(request.bufferUploads.size());
                for (size_t index = 0u; index < request.bufferUploads.size(); ++index)
                {
                    if (!PrepareBufferUpload(request.bufferUploads[index], request.debugName, index, submission, preparedBufferUploads))
                        return submission;
                }

                std::vector<PreparedTextureUpload> preparedTextureUploads;
                preparedTextureUploads.reserve(request.textureUploads.size());
                for (size_t index = 0u; index < request.textureUploads.size(); ++index)
                {
                    if (!PrepareTextureUpload(request.textureUploads[index], request.debugName, index, submission, preparedTextureUploads))
                        return submission;
                }

                for (auto& preparedUpload : preparedBufferUploads)
                    RecordBufferUpload(commandBuffer, preparedUpload);

                for (auto& preparedUpload : preparedTextureUploads)
                    RecordTextureUpload(commandBuffer, preparedUpload);

                submission.diagnostic = "UploadBatch " + request.debugName + " recorded backend upload work";
                submission.completion = BuildCompletionToken(request, submission.diagnostic);
                return submission;
            }

            UploadSubmission SubmitUploadBuffer(RHICommandBuffer& commandBuffer, const UploadBufferRequest& request) override
            {
                UploadBatchRequest batchRequest;
                batchRequest.bufferUploads.push_back(request);
                batchRequest.requireRecordedBackendWork = true;
                batchRequest.debugName = request.debugName;
                const auto batchSubmission = SubmitUploadBatch(commandBuffer, batchRequest);
                return {
                    batchSubmission.accepted,
                    batchSubmission.completion,
                    batchSubmission.accepted ? std::string{} : batchSubmission.diagnostic
                };
            }

            UploadSubmission SubmitUploadTexture(RHICommandBuffer& commandBuffer, const UploadTextureRequest& request) override
            {
                UploadBatchRequest batchRequest;
                batchRequest.textureUploads.push_back(request);
                batchRequest.requireRecordedBackendWork = true;
                batchRequest.debugName = request.debugName;
                const auto batchSubmission = SubmitUploadBatch(commandBuffer, batchRequest);
                return {
                    batchSubmission.accepted,
                    batchSubmission.completion,
                    batchSubmission.accepted ? std::string{} : batchSubmission.diagnostic
                };
            }

            bool UploadBuffer(RHICommandBuffer& commandBuffer, const UploadBufferRequest& request) override
            {
                return SubmitUploadBuffer(commandBuffer, request).accepted;
            }

            bool UploadTexture(RHICommandBuffer& commandBuffer, const UploadTextureRequest& request) override
            {
                return SubmitUploadTexture(commandBuffer, request).accepted;
            }

            void CollectGarbage(uint64_t completedFrameIndex) override
            {
                const uint64_t safeFrameIndex = completedFrameIndex == 0u ? 0u : completedFrameIndex - 1u;
                while (!m_retiredStagingBuffers.empty() &&
                    m_retiredStagingBuffers.front().frameIndex <= safeFrameIndex)
                {
                    m_retiredStagingBuffers.pop_front();
                }
            }

        private:
            static std::shared_ptr<RHICompletionToken> BuildImmediateToken(bool accepted, const std::string& diagnostic)
            {
                return std::make_shared<CompletedUploadToken>(RHICompletionStatus{
                    accepted ? RHICompletionStatusCode::Success : RHICompletionStatusCode::Failed,
                    diagnostic
                });
            }

            static UploadBatchRequest BuildValidationOnlyRequest(const UploadBatchRequest& request)
            {
                UploadBatchRequest validationRequest = request;
                validationRequest.requireRecordedBackendWork = false;
                return validationRequest;
            }

            static std::shared_ptr<RHICompletionToken> BuildCompletionToken(
                const UploadBatchRequest& request,
                const std::string& diagnostic)
            {
                if (request.completionFence != nullptr)
                {
                    const std::string debugName = request.debugName.empty()
                        ? "UploadBatchCompletion"
                        : "UploadBatch " + request.debugName;
                    return std::make_shared<FenceBackedUploadToken>(request.completionFence, debugName);
                }
                return BuildImmediateToken(true, diagnostic);
            }

            bool PrepareBufferUpload(
                const UploadBufferRequest& request,
                const std::string& batchName,
                size_t index,
                UploadBatchSubmission& submission,
                std::vector<PreparedBufferUpload>& preparedUploads)
            {
                auto staging = m_backend->CreateStagingBuffer(
                    request.data,
                    request.size,
                    request.debugName.empty() ? batchName + "BufferUpload" + std::to_string(index) : request.debugName);
                if (staging == nullptr)
                {
                    return FailSubmission(
                        submission,
                        "UploadBatch " + batchName + " bufferUploads[" + std::to_string(index) +
                        "]: backend failed to create staging buffer");
                }

                preparedUploads.push_back({ &request, std::move(staging) });
                return true;
            }

            bool PrepareTextureUpload(
                const UploadTextureRequest& request,
                const std::string& batchName,
                size_t index,
                UploadBatchSubmission& submission,
                std::vector<PreparedTextureUpload>& preparedUploads)
            {
                auto staging = m_backend->CreateStagingBuffer(
                    request.data,
                    request.dataSize,
                    request.debugName.empty() ? batchName + "TextureUpload" + std::to_string(index) : request.debugName);
                if (staging == nullptr)
                {
                    return FailSubmission(
                        submission,
                        "UploadBatch " + batchName + " textureUploads[" + std::to_string(index) +
                        "]: backend failed to create staging buffer");
                }

                preparedUploads.push_back({ &request, std::move(staging) });
                return true;
            }

            void RecordBufferUpload(
                RHICommandBuffer& commandBuffer,
                PreparedBufferUpload& preparedUpload)
            {
                const auto& request = *preparedUpload.request;
                const auto beforeState = request.destination->GetState();
                commandBuffer.Barrier(BuildBufferBarrier(request.destination, beforeState, ResourceState::CopyDst));
                commandBuffer.CopyBuffer(
                    preparedUpload.staging,
                    request.destination,
                    RHIBufferCopyRegion{ 0u, request.destinationOffset, request.size });
                commandBuffer.Barrier(BuildBufferBarrier(request.destination, ResourceState::CopyDst, beforeState));
                RetireStagingBuffer(std::move(preparedUpload.staging));
            }

            void RecordTextureUpload(
                RHICommandBuffer& commandBuffer,
                PreparedTextureUpload& preparedUpload)
            {
                const auto& request = *preparedUpload.request;
                const auto beforeState = request.destination->GetState();
                const auto afterState = ResolveUploadedTextureState(request.destination->GetDesc());
                commandBuffer.Barrier(BuildTextureBarrier(request.destination, beforeState, ResourceState::CopyDst));

                RHIBufferToTextureCopyDesc copyDesc;
                copyDesc.source = preparedUpload.staging;
                copyDesc.destination = request.destination;
                copyDesc.bufferOffset = 0u;
                copyDesc.mipLevel = request.mipLevel;
                copyDesc.arrayLayer = request.arrayLayer;
                copyDesc.textureOffset = {};
                copyDesc.extent = request.extent;
                copyDesc.rowPitch = request.rowPitch;
                copyDesc.slicePitch = request.slicePitch;
                commandBuffer.CopyBufferToTexture(copyDesc);

                commandBuffer.Barrier(BuildTextureBarrier(request.destination, ResourceState::CopyDst, afterState));
                RetireStagingBuffer(std::move(preparedUpload.staging));
            }

            bool FailSubmission(UploadBatchSubmission& submission, std::string diagnostic)
            {
                submission.accepted = false;
                submission.recordedBackendWork = false;
                submission.diagnostic = std::move(diagnostic);
                submission.completion = BuildImmediateToken(false, submission.diagnostic);
                return false;
            }

            void RetireStagingBuffer(std::shared_ptr<RHIBuffer> buffer)
            {
                if (buffer == nullptr)
                    return;
                m_retiredStagingBuffers.push_back({ m_currentFrameIndex, std::move(buffer) });
            }

        private:
            std::shared_ptr<UploadBackend> m_backend;
            std::shared_ptr<UploadContext> m_validationContext;
            std::deque<RetiredStagingResource> m_retiredStagingBuffers;
            uint64_t m_currentFrameIndex = 0u;
        };
    }

    std::shared_ptr<UploadContext> CreateDefaultUploadContext(size_t ringCapacity)
    {
        return std::make_shared<DefaultUploadContext>(ringCapacity);
    }

    std::shared_ptr<UploadContext> CreateBackendUploadContext(
        std::shared_ptr<UploadBackend> backend,
        size_t ringCapacity)
    {
        return std::make_shared<BackendUploadContext>(std::move(backend), ringCapacity);
    }
}
