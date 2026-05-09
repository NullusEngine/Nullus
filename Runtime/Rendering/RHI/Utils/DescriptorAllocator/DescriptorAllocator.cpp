#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace NLS::Render::RHI
{
    namespace
    {
        class DefaultDescriptorAllocator final : public DescriptorAllocator
        {
        public:
            explicit DefaultDescriptorAllocator(uint64_t transientCapacity)
            {
                DescriptorRangeAllocatorDesc desc;
                desc.transientCapacity = std::max<uint64_t>(transientCapacity, 1024);
                desc.debugName = "DefaultDescriptorAllocator";
                m_rangeAllocator.Configure(desc);
            }

            void BeginFrame(uint64_t frameIndex) override
            {
                m_rangeAllocator.BeginFrame(frameIndex);
            }

            void EndFrame(uint64_t) override
            {
            }

            DescriptorAllocation Allocate(const DescriptorAllocationRequest& request) override
            {
                return m_rangeAllocator.Allocate(request);
            }

            DescriptorAllocationBatch AllocateBatch(const std::vector<DescriptorAllocationRequest>& requests) override
            {
                return m_rangeAllocator.AllocateBatch(requests);
            }

            void Release(const DescriptorAllocation& allocation) override
            {
                m_rangeAllocator.Release(allocation);
            }

            void Reset() override
            {
                m_rangeAllocator.Reset();
            }

            DescriptorAllocatorStats GetStats() const override
            {
                return m_rangeAllocator.GetStats();
            }

        private:
            DescriptorRangeAllocator m_rangeAllocator;
        };
    }

    DescriptorRangeAllocator::DescriptorRangeAllocator(const DescriptorRangeAllocatorDesc& desc)
    {
        Configure(desc);
    }

    void DescriptorRangeAllocator::Configure(const DescriptorRangeAllocatorDesc& desc)
    {
        std::scoped_lock lock(m_mutex);
        m_desc = desc;
        m_desc.transientCapacity = std::max<uint64_t>(m_desc.transientCapacity, 1u);
        m_currentFrameIndex = 0u;
        m_transientOffset = 0u;
        m_persistentOffset = 0u;
        m_persistentFreeRanges.clear();
        m_stats = {};
        m_stats.transientCapacity = m_desc.transientCapacity;
        m_stats.persistentCapacity = m_desc.boundPersistentCapacity ? m_desc.persistentCapacity : 0u;
    }

    void DescriptorRangeAllocator::BeginFrame(uint64_t frameIndex)
    {
        std::scoped_lock lock(m_mutex);
        if (frameIndex != m_currentFrameIndex)
        {
            m_stats.transientRetired += m_transientOffset;
            m_currentFrameIndex = frameIndex;
            m_transientOffset = 0u;
        }
        m_stats.currentFrameIndex = frameIndex;
        m_stats.transientUsed = m_transientOffset;
    }

    DescriptorAllocation DescriptorRangeAllocator::Allocate(const DescriptorAllocationRequest& request)
    {
        if (request.count == 0u)
            return {};

        std::scoped_lock lock(m_mutex);
        return AllocateLocked(request);
    }

    DescriptorAllocationBatch DescriptorRangeAllocator::AllocateBatch(const std::vector<DescriptorAllocationRequest>& requests)
    {
        DescriptorAllocationBatch batch;
        batch.allocations.reserve(requests.size());

        std::scoped_lock lock(m_mutex);
        for (size_t index = 0u; index < requests.size(); ++index)
        {
            const auto& request = requests[index];
            batch.totalRequested += request.count;
            auto allocation = AllocateLocked(request);
            if (!allocation.IsValid())
            {
                batch.allSucceeded = false;
                batch.diagnostic =
                    "Descriptor allocation batch failed at requests[" +
                    std::to_string(index) + "]";
            }
            else
            {
                batch.totalAllocated += allocation.count;
            }
            batch.allocations.push_back(std::move(allocation));
        }

        return batch;
    }

    void DescriptorRangeAllocator::Release(const DescriptorAllocation& allocation)
    {
        if (!allocation.IsValid() || allocation.lifetime != DescriptorAllocationLifetime::Persistent)
            return;

        std::scoped_lock lock(m_mutex);
        ReleasePersistentLocked(allocation);
    }

    void DescriptorRangeAllocator::Reset()
    {
        Configure(m_desc);
    }

    DescriptorAllocatorStats DescriptorRangeAllocator::GetStats() const
    {
        std::scoped_lock lock(m_mutex);
        return m_stats;
    }

    DescriptorAllocation DescriptorRangeAllocator::AllocateLocked(const DescriptorAllocationRequest& request)
    {
        if (request.count == 0u)
            return {};

        if (request.lifetime == DescriptorAllocationLifetime::TransientFrame)
        {
            if (m_transientOffset + request.count > m_desc.transientCapacity)
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
            if (it->count == 0u)
                m_persistentFreeRanges.erase(it);
            m_stats.persistentUsed += request.count;
            m_stats.persistentPeak = std::max<uint64_t>(m_stats.persistentPeak, m_stats.persistentUsed);
            return allocation;
        }

        if (m_desc.boundPersistentCapacity &&
            m_persistentOffset + request.count > m_desc.persistentCapacity)
        {
            ++m_stats.allocationFailures;
            return {};
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

    void DescriptorRangeAllocator::ReleasePersistentLocked(const DescriptorAllocation& allocation)
    {
        m_persistentFreeRanges.push_back({ allocation.offset, allocation.count });
        m_stats.persistentUsed = m_stats.persistentUsed >= allocation.count
            ? m_stats.persistentUsed - allocation.count
            : 0u;
        m_stats.persistentReleased += allocation.count;
        std::sort(m_persistentFreeRanges.begin(), m_persistentFreeRanges.end(), [](const FreeRange& lhs, const FreeRange& rhs)
        {
            return lhs.offset < rhs.offset;
        });

        for (size_t i = 1u; i < m_persistentFreeRanges.size();)
        {
            auto& previous = m_persistentFreeRanges[i - 1u];
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

    std::shared_ptr<DescriptorAllocator> CreateDefaultDescriptorAllocator(uint64_t transientCapacity)
    {
        return std::make_shared<DefaultDescriptorAllocator>(transientCapacity);
    }
}
