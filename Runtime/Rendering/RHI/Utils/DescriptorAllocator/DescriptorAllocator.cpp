#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"

#include <algorithm>
#include <vector>

namespace NLS::Render::RHI
{
    namespace
    {
        struct FreeRange
        {
            uint64_t offset = 0;
            uint32_t count = 0;
        };

        class DefaultDescriptorAllocator final : public DescriptorAllocator
        {
        public:
            explicit DefaultDescriptorAllocator(uint64_t transientCapacity)
                : m_transientCapacity(std::max<uint64_t>(transientCapacity, 1024))
            {
            }

            void BeginFrame(uint64_t frameIndex) override
            {
                if (frameIndex != m_currentFrameIndex)
                {
                    m_currentFrameIndex = frameIndex;
                    m_transientOffset = 0;
                }
            }

            void EndFrame(uint64_t) override
            {
            }

            DescriptorAllocation Allocate(const DescriptorAllocationRequest& request) override
            {
                if (request.count == 0)
                    return {};

                if (request.lifetime == DescriptorAllocationLifetime::TransientFrame)
                {
                    if (m_transientOffset + request.count > m_transientCapacity)
                        return {};

                    DescriptorAllocation allocation{};
                    allocation.offset = m_transientOffset;
                    allocation.count = request.count;
                    allocation.lifetime = request.lifetime;
                    allocation.frameIndex = request.frameIndex;
                    allocation.debugName = request.debugName;
                    m_transientOffset += request.count;
                    return allocation;
                }

                for (auto it = m_persistentFreeRanges.begin(); it != m_persistentFreeRanges.end(); ++it)
                {
                    if (it->count < request.count)
                        continue;

                    DescriptorAllocation allocation{};
                    allocation.offset = it->offset;
                    allocation.count = request.count;
                    allocation.lifetime = request.lifetime;
                    allocation.frameIndex = request.frameIndex;
                    allocation.debugName = request.debugName;

                    it->offset += request.count;
                    it->count -= request.count;
                    if (it->count == 0)
                        m_persistentFreeRanges.erase(it);
                    return allocation;
                }

                DescriptorAllocation allocation{};
                allocation.offset = m_persistentOffset;
                allocation.count = request.count;
                allocation.lifetime = request.lifetime;
                allocation.frameIndex = request.frameIndex;
                allocation.debugName = request.debugName;
                m_persistentOffset += request.count;
                return allocation;
            }

            void Release(const DescriptorAllocation& allocation) override
            {
                if (!allocation.IsValid() || allocation.lifetime != DescriptorAllocationLifetime::Persistent)
                    return;

                m_persistentFreeRanges.push_back({ allocation.offset, allocation.count });
                std::sort(m_persistentFreeRanges.begin(), m_persistentFreeRanges.end(), [](const FreeRange& lhs, const FreeRange& rhs)
                {
                    return lhs.offset < rhs.offset;
                });

                for (size_t i = 1; i < m_persistentFreeRanges.size();)
                {
                    auto& previous = m_persistentFreeRanges[i - 1];
                    auto& current = m_persistentFreeRanges[i];
                    if (previous.offset + previous.count == current.offset)
                    {
                        previous.count += current.count;
                        m_persistentFreeRanges.erase(m_persistentFreeRanges.begin() + static_cast<std::ptrdiff_t>(i));
                    }
                    else
                    {
                        ++i;
                    }
                }
            }

            void Reset() override
            {
                m_currentFrameIndex = 0;
                m_transientOffset = 0;
                m_persistentOffset = 0;
                m_persistentFreeRanges.clear();
            }

        private:
            uint64_t m_transientCapacity = 0;
            uint64_t m_currentFrameIndex = 0;
            uint64_t m_transientOffset = 0;
            uint64_t m_persistentOffset = 0;
            std::vector<FreeRange> m_persistentFreeRanges;
        };
    }

    std::shared_ptr<DescriptorAllocator> CreateDefaultDescriptorAllocator(uint64_t transientCapacity)
    {
        return std::make_shared<DefaultDescriptorAllocator>(transientCapacity);
    }
}
