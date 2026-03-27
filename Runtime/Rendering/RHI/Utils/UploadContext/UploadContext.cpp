#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"

#include <algorithm>
#include <deque>
#include <vector>

namespace NLS::Render::RHI
{
    namespace
    {
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
                if (size == 0 || m_ring.empty())
                    return allocation;

                alignment = std::max<size_t>(alignment, 1);
                const size_t alignedOffset = AlignUp(m_head, alignment);
                if (alignedOffset + size > m_ring.size())
                {
                    m_head = 0;
                    return Allocate(size, alignment, std::move(debugName));
                }

                allocation.cpuAddress = m_ring.data() + alignedOffset;
                allocation.gpuOffset = alignedOffset;
                allocation.size = size;
                allocation.alignment = alignment;
                allocation.debugName = std::move(debugName);
                m_head = alignedOffset + size;
                m_retiredAllocations.push_back({ m_currentFrameIndex, alignedOffset, alignedOffset + size });
                return allocation;
            }

            bool UploadBuffer(RHICommandBuffer&, const UploadBufferRequest& request) override
            {
                return request.destination != nullptr && request.data != nullptr && request.size != 0;
            }

            bool UploadTexture(RHICommandBuffer&, const UploadTextureRequest& request) override
            {
                return request.destination != nullptr && request.data != nullptr && request.dataSize != 0;
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
