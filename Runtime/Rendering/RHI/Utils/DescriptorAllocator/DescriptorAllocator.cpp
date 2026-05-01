#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"

#include <algorithm>
#include <mutex>
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
                m_stats.transientCapacity = m_transientCapacity;
            }

            void BeginFrame(uint64_t frameIndex) override
            {
                std::scoped_lock lock(m_mutex);
                if (frameIndex != m_currentFrameIndex)
                {
                    m_stats.transientRetired += m_transientOffset;
                    m_currentFrameIndex = frameIndex;
                    m_transientOffset = 0;
                }
                m_stats.currentFrameIndex = frameIndex;
                m_stats.transientUsed = m_transientOffset;
            }

            void EndFrame(uint64_t) override
            {
            }

            DescriptorAllocation Allocate(const DescriptorAllocationRequest& request) override
            {
                if (request.count == 0)
                    return {};

                std::scoped_lock lock(m_mutex);
                if (request.lifetime == DescriptorAllocationLifetime::TransientFrame)
                {
                    if (m_transientOffset + request.count > m_transientCapacity)
                    {
                        ++m_stats.allocationFailures;
                        return {};
                    }

                    DescriptorAllocation allocation{};
                    allocation.offset = m_transientOffset;
                    allocation.count = request.count;
                    allocation.lifetime = request.lifetime;
                    allocation.frameIndex = request.frameIndex;
                    allocation.debugName = request.debugName;
                    m_transientOffset += request.count;
                    m_stats.transientUsed = m_transientOffset;
                    m_stats.transientPeak = std::max<uint64_t>(m_stats.transientPeak, m_transientOffset);
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
                    m_stats.persistentUsed += request.count;
                    m_stats.persistentPeak = std::max<uint64_t>(m_stats.persistentPeak, m_stats.persistentUsed);
                    return allocation;
                }

                DescriptorAllocation allocation{};
                allocation.offset = m_persistentOffset;
                allocation.count = request.count;
                allocation.lifetime = request.lifetime;
                allocation.frameIndex = request.frameIndex;
                allocation.debugName = request.debugName;
                m_persistentOffset += request.count;
                m_stats.persistentUsed += request.count;
                m_stats.persistentPeak = std::max<uint64_t>(m_stats.persistentPeak, m_stats.persistentUsed);
                return allocation;
            }

            void Release(const DescriptorAllocation& allocation) override
            {
                if (!allocation.IsValid() || allocation.lifetime != DescriptorAllocationLifetime::Persistent)
                    return;

                std::scoped_lock lock(m_mutex);
                m_persistentFreeRanges.push_back({ allocation.offset, allocation.count });
                m_stats.persistentUsed = m_stats.persistentUsed >= allocation.count
                    ? m_stats.persistentUsed - allocation.count
                    : 0u;
                m_stats.persistentReleased += allocation.count;
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
                std::scoped_lock lock(m_mutex);
                m_currentFrameIndex = 0;
                m_transientOffset = 0;
                m_persistentOffset = 0;
                m_persistentFreeRanges.clear();
                m_stats = {};
                m_stats.transientCapacity = m_transientCapacity;
            }

            DescriptorAllocatorStats GetStats() const override
            {
                std::scoped_lock lock(m_mutex);
                return m_stats;
            }

        private:
            mutable std::mutex m_mutex;
            DescriptorAllocatorStats m_stats{};
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
