#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"

#include <algorithm>
#include <deque>
#include <vector>

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
            bool IsComplete() const override { return true; }
            RHICompletionStatus GetStatus() const override { return m_status; }
            RHICompletionStatus Wait(uint64_t = 0) override { return m_status; }

        private:
            RHICompletionStatus m_status;
        };

        struct RetiredAllocation
        {
            uint64_t frameIndex = 0;
            size_t begin = 0;
            size_t end = 0;
        };

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
                submission.diagnostic = submission.accepted
                    ? std::string{}
                    : "UploadBatch request has no uploads";
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
                return request.destination != nullptr && request.data != nullptr && request.size != 0
                    ? std::string{}
                    : "UploadBuffer request is missing destination, data, or size";
            }

            static std::string ValidateUploadTextureRequest(const UploadTextureRequest& request)
            {
                return request.destination != nullptr && request.data != nullptr && request.dataSize != 0
                    ? std::string{}
                    : "UploadTexture request is missing destination, data, or dataSize";
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
    }

    std::shared_ptr<UploadContext> CreateDefaultUploadContext(size_t ringCapacity)
    {
        return std::make_shared<DefaultUploadContext>(ringCapacity);
    }
}
