#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "RenderDef.h"

namespace NLS::Render::RHI::DX12
{
    inline constexpr uint32_t GetDX12UIFenceWaitTimeoutMilliseconds()
    {
        return 5000u;
    }

    struct DX12UIFrameReuseWait
    {
        bool shouldWait = false;
        uint64_t fenceValue = 0u;
    };

    struct DX12UIAllocatorReuse
    {
        std::optional<uint32_t> allocatorIndex;
        bool shouldWait = false;
        uint64_t fenceValue = 0u;
    };

    class DX12UIFrameFenceTracker
    {
    public:
        void ResetBackbufferCount(uint32_t backbufferCount)
        {
            m_submittedFenceValues.assign(backbufferCount, 0u);
            m_lastSubmittedFenceValue = 0u;
        }

        void ResetAllocatorCount(uint32_t allocatorCount)
        {
            m_allocatorFenceValues.assign(allocatorCount, 0u);
        }

        DX12UIFrameReuseWait ResolveReuseWait(
            uint32_t backbufferIndex,
            uint64_t completedFenceValue) const
        {
            if (backbufferIndex >= m_submittedFenceValues.size())
                return {};

            const uint64_t submittedFenceValue = m_submittedFenceValues[backbufferIndex];
            return {
                submittedFenceValue != 0u && completedFenceValue < submittedFenceValue,
                submittedFenceValue
            };
        }

        void RecordSubmitted(uint32_t backbufferIndex, uint64_t fenceValue)
        {
            if (backbufferIndex >= m_submittedFenceValues.size())
                return;

            m_submittedFenceValues[backbufferIndex] = fenceValue;
            m_lastSubmittedFenceValue = fenceValue;
        }

        void RecordSubmitted(uint32_t backbufferIndex, uint64_t fenceValue, uint32_t allocatorIndex)
        {
            RecordSubmitted(backbufferIndex, fenceValue);
            RecordAllocatorSubmitted(allocatorIndex, fenceValue);
        }

        void RecordAllocatorSubmitted(uint32_t allocatorIndex, uint64_t fenceValue)
        {
            if (allocatorIndex >= m_allocatorFenceValues.size())
                return;

            m_allocatorFenceValues[allocatorIndex] = fenceValue;
        }

        DX12UIAllocatorReuse ResolveAllocatorReuse(uint64_t completedFenceValue) const
        {
            uint32_t oldestIncompleteIndex = 0u;
            uint64_t oldestIncompleteFence = 0u;
            bool hasIncompleteAllocator = false;

            for (uint32_t index = 0u; index < m_allocatorFenceValues.size(); ++index)
            {
                const uint64_t fenceValue = m_allocatorFenceValues[index];
                if (fenceValue == 0u || completedFenceValue >= fenceValue)
                    return { index, false, fenceValue };

                if (!hasIncompleteAllocator || fenceValue < oldestIncompleteFence)
                {
                    oldestIncompleteIndex = index;
                    oldestIncompleteFence = fenceValue;
                    hasIncompleteAllocator = true;
                }
            }

            if (hasIncompleteAllocator)
                return { oldestIncompleteIndex, true, oldestIncompleteFence };

            return {};
        }

        uint64_t GetLastSubmittedFenceValue() const
        {
            return m_lastSubmittedFenceValue;
        }

    private:
        std::vector<uint64_t> m_submittedFenceValues;
        std::vector<uint64_t> m_allocatorFenceValues;
        uint64_t m_lastSubmittedFenceValue = 0u;
    };
}
