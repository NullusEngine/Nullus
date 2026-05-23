#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"

#include <algorithm>
#include <unordered_map>

#include "Rendering/RHI/Core/RHISubresourceRangeUtils.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI
{
    namespace
    {
        struct BufferKey
        {
            const RHIBuffer* buffer = nullptr;

            bool operator==(const BufferKey& rhs) const
            {
                return buffer == rhs.buffer;
            }
        };

        struct BufferKeyHash
        {
            size_t operator()(const BufferKey& key) const
            {
                return std::hash<const RHIBuffer*>{}(key.buffer);
            }
        };

        using TextureStateBucket = std::vector<TrackedTextureState>;

        struct TransientBufferRetirement
        {
            std::shared_ptr<RHIBuffer> buffer;
            uint64_t retireAfterFrameIndex = 0u;
        };

        struct TransientTextureRetirement
        {
            std::shared_ptr<RHITexture> texture;
            RHISubresourceRange subresourceRange{};
            uint64_t retireAfterFrameIndex = 0u;
        };

        struct TransientTextureViewRetirement
        {
            std::shared_ptr<RHITextureView> textureView;
            uint64_t retireAfterFrameIndex = 0u;
        };

        bool IsCpuVisibleBuffer(const std::shared_ptr<RHIBuffer>& buffer)
        {
            if (buffer == nullptr)
                return false;

            const auto memoryUsage = buffer->GetDesc().memoryUsage;
            return memoryUsage == MemoryUsage::CPUToGPU ||
                memoryUsage == MemoryUsage::GPUToCPU;
        }

        bool IsLegalCpuVisibleBufferState(const std::shared_ptr<RHIBuffer>& buffer, ResourceState state)
        {
            if (!IsCpuVisibleBuffer(buffer))
                return false;

            const auto memoryUsage = buffer->GetDesc().memoryUsage;
            if (memoryUsage == MemoryUsage::CPUToGPU)
                return state == ResourceState::Unknown || state == ResourceState::GenericRead;
            if (memoryUsage == MemoryUsage::GPUToCPU)
                return state == ResourceState::Unknown || state == ResourceState::CopyDst;
            return false;
        }

        bool IsLegalCpuVisibleBufferTransition(
            const std::shared_ptr<RHIBuffer>& buffer,
            ResourceState before,
            ResourceState after)
        {
            return IsLegalCpuVisibleBufferState(buffer, before) &&
                IsLegalCpuVisibleBufferState(buffer, after);
        }

        uint64_t CountTrackedTextureStates(
            const std::unordered_map<const RHITexture*, TextureStateBucket>& textureStates)
        {
            uint64_t count = 0u;
            for (const auto& [_, bucket] : textureStates)
                count += static_cast<uint64_t>(bucket.size());
            return count;
        }

        std::vector<RHISubresourceRange> SubtractCoveredRanges(
            const RHISubresourceRange& sourceRange,
            const TextureStateBucket& coveringStates,
            const ResourceState requiredState)
        {
            std::vector<RHISubresourceRange> remainingRanges{ sourceRange };
            for (const auto& trackedState : coveringStates)
            {
                if (trackedState.state != requiredState)
                    continue;

                std::vector<RHISubresourceRange> nextRemaining;
                for (const auto& remainingRange : remainingRanges)
                {
                    const auto remainders = SubtractSubresourceRange(
                        remainingRange,
                        trackedState.subresourceRange);
                    for (const auto& remainder : remainders)
                    {
                        if (remainder.mipLevelCount != 0u && remainder.arrayLayerCount != 0u)
                            nextRemaining.push_back(remainder);
                    }
                }
                remainingRanges = std::move(nextRemaining);
                if (remainingRanges.empty())
                    break;
            }
            return remainingRanges;
        }

        std::vector<RHISubresourceRange> SubtractRangesFromRemaining(
            std::vector<RHISubresourceRange> remainingRanges,
            const RHISubresourceRange& subtractRange)
        {
            std::vector<RHISubresourceRange> nextRemaining;
            for (const auto& remainingRange : remainingRanges)
            {
                const auto remainders = SubtractSubresourceRange(remainingRange, subtractRange);
                for (const auto& remainder : remainders)
                {
                    if (remainder.mipLevelCount != 0u && remainder.arrayLayerCount != 0u)
                        nextRemaining.push_back(remainder);
                }
            }
            return nextRemaining;
        }

        class DefaultResourceStateTracker final : public ResourceStateTracker
        {
        public:
            void BeginFrame(uint64_t frameIndex) override
            {
                m_stats.currentFrameIndex = frameIndex;
            }

            void Reset() override
            {
                m_bufferStates.clear();
                m_textureStates.clear();
                m_transientBuffers.clear();
                m_transientTextures.clear();
                m_transientTextureViews.clear();
                m_stats.currentFrameIndex = 0u;
                m_stats.trackedBufferCount = 0u;
                m_stats.trackedTextureCount = 0u;
                m_stats.transientBufferRegistrations = 0u;
                m_stats.transientTextureRegistrations = 0u;
                m_stats.transientTextureViewRegistrations = 0u;
                m_stats.retiredTransientBuffers = 0u;
                m_stats.retiredTransientTextures = 0u;
                m_stats.retiredTransientTextureViews = 0u;
            }

            std::optional<TrackedBufferState> GetBufferState(const std::shared_ptr<RHIBuffer>& buffer) const override
            {
                if (buffer == nullptr)
                    return std::nullopt;

                const auto it = m_bufferStates.find(BufferKey{ buffer.get() });
                if (it == m_bufferStates.end())
                    return std::nullopt;

                return it->second;
            }

            std::optional<TrackedTextureState> GetTextureState(
                const std::shared_ptr<RHITexture>& texture,
                const RHISubresourceRange& subresourceRange) const override
            {
                if (texture == nullptr)
                    return std::nullopt;

                const auto normalizedRange = NormalizeTextureSubresourceRange(texture->GetDesc(), subresourceRange);
                if (!normalizedRange.has_value())
                    return std::nullopt;

                const auto bucketIt = m_textureStates.find(texture.get());
                if (bucketIt == m_textureStates.end())
                    return std::nullopt;

                const auto& bucket = bucketIt->second;
                const auto it = std::find_if(
                    bucket.begin(),
                    bucket.end(),
                    [&](const TrackedTextureState& trackedState)
                    {
                        return trackedState.subresourceRange.baseMipLevel == normalizedRange->baseMipLevel &&
                            trackedState.subresourceRange.mipLevelCount == normalizedRange->mipLevelCount &&
                            trackedState.subresourceRange.baseArrayLayer == normalizedRange->baseArrayLayer &&
                            trackedState.subresourceRange.arrayLayerCount == normalizedRange->arrayLayerCount;
                    });
                if (it != bucket.end())
                    return *it;

                const auto coveringIt = std::find_if(
                    bucket.begin(),
                    bucket.end(),
                    [&](const TrackedTextureState& trackedState)
                    {
                        return DoesSubresourceRangeCover(trackedState.subresourceRange, *normalizedRange);
                    });
                if (coveringIt != bucket.end())
                    return *coveringIt;

                return std::nullopt;
            }

            RHIBarrierDesc BuildTransitionBarriers(
                const std::vector<RHIBufferBarrier>& bufferBarriers,
                const std::vector<RHITextureBarrier>& textureBarriers) const override
            {
                RHIBarrierDesc resolved{};
                resolved.bufferBarriers.reserve(bufferBarriers.size());
                for (auto barrier : bufferBarriers)
                {
                    if (barrier.buffer == nullptr)
                        continue;
                    if (barrier.before == ResourceState::Unknown)
                    {
                        if (const auto tracked = GetBufferState(barrier.buffer); tracked.has_value())
                        {
                            barrier.before = tracked->state;
                            barrier.sourceStageMask = tracked->stageMask;
                            barrier.sourceAccessMask = tracked->accessMask;
                        }
                    }

                    if (IsLegalCpuVisibleBufferTransition(barrier.buffer, barrier.before, barrier.after))
                        continue;

                    resolved.bufferBarriers.push_back(barrier);
                }

                resolved.textureBarriers.reserve(textureBarriers.size());
                for (auto barrier : textureBarriers)
                {
                    if (barrier.texture == nullptr)
                        continue;

                    const auto normalizedRange = NormalizeTextureSubresourceRange(
                        barrier.texture->GetDesc(),
                        barrier.subresourceRange);
                    if (normalizedRange.has_value())
                        barrier.subresourceRange = *normalizedRange;

                    if (barrier.before == ResourceState::Unknown)
                    {
                        if (const auto tracked = GetTextureState(barrier.texture, barrier.subresourceRange); tracked.has_value())
                        {
                            barrier.before = tracked->state;
                            barrier.sourceStageMask = tracked->stageMask;
                            barrier.sourceAccessMask = tracked->accessMask;
                        }
                        else if (normalizedRange.has_value())
                        {
                            const auto bucketIt = m_textureStates.find(barrier.texture.get());
                            if (bucketIt == m_textureStates.end())
                            {
                                resolved.textureBarriers.push_back(barrier);
                                continue;
                            }

                            std::vector<RHISubresourceRange> remainingRanges{ *normalizedRange };
                            for (const auto& trackedState : bucketIt->second)
                            {
                                if (!DoesSubresourceRangeOverlap(trackedState.subresourceRange, *normalizedRange))
                                    continue;

                                if (trackedState.state == barrier.after)
                                {
                                    remainingRanges = SubtractRangesFromRemaining(
                                        std::move(remainingRanges),
                                        trackedState.subresourceRange);
                                    if (remainingRanges.empty())
                                        break;
                                    continue;
                                }

                                const auto intersection = IntersectSubresourceRanges(
                                    trackedState.subresourceRange,
                                    *normalizedRange);
                                if (!intersection.has_value())
                                    continue;

                                resolved.textureBarriers.push_back({
                                    barrier.texture,
                                    trackedState.state,
                                    barrier.after,
                                    *intersection,
                                    trackedState.stageMask,
                                    barrier.destinationStageMask,
                                    trackedState.accessMask,
                                    barrier.destinationAccessMask
                                });
                                remainingRanges = SubtractRangesFromRemaining(
                                    std::move(remainingRanges),
                                    *intersection);
                                if (remainingRanges.empty())
                                    break;
                            }

                            for (const auto& remainingRange : remainingRanges)
                            {
                                resolved.textureBarriers.push_back({
                                    barrier.texture,
                                    barrier.before,
                                    barrier.after,
                                    remainingRange,
                                    barrier.sourceStageMask,
                                    barrier.destinationStageMask,
                                    barrier.sourceAccessMask,
                                    barrier.destinationAccessMask
                                });
                            }
                            continue;
                        }
                    }

                    resolved.textureBarriers.push_back(barrier);
                }

                return resolved;
            }

            void RegisterTransientBuffer(
                const std::shared_ptr<RHIBuffer>& buffer,
                uint64_t retireAfterFrameIndex) override
            {
                if (buffer == nullptr)
                    return;

                m_transientBuffers.push_back({ buffer, retireAfterFrameIndex });
                ++m_stats.transientBufferRegistrations;
            }

            void RegisterTransientTexture(
                const std::shared_ptr<RHITexture>& texture,
                const RHISubresourceRange& subresourceRange,
                uint64_t retireAfterFrameIndex) override
            {
                if (texture == nullptr)
                    return;

                const auto normalizedRange = NormalizeTextureSubresourceRange(texture->GetDesc(), subresourceRange);
                if (!normalizedRange.has_value())
                    return;

                m_transientTextures.push_back({ texture, *normalizedRange, retireAfterFrameIndex });
                ++m_stats.transientTextureRegistrations;
            }

            void RegisterTransientTextureView(
                const std::shared_ptr<RHITextureView>& textureView,
                uint64_t retireAfterFrameIndex) override
            {
                if (textureView == nullptr)
                    return;

                m_transientTextureViews.push_back({ textureView, retireAfterFrameIndex });
                ++m_stats.transientTextureViewRegistrations;
            }

            void RetireTransientResources(uint64_t completedFrameIndex) override
            {
                for (auto it = m_transientBuffers.begin(); it != m_transientBuffers.end();)
                {
                    if (it->buffer == nullptr || it->retireAfterFrameIndex > completedFrameIndex)
                    {
                        ++it;
                        continue;
                    }

                    m_bufferStates.erase(BufferKey{ it->buffer.get() });
                    ++m_stats.retiredTransientBuffers;
                    it = m_transientBuffers.erase(it);
                }

                for (auto it = m_transientTextures.begin(); it != m_transientTextures.end();)
                {
                    if (it->texture == nullptr || it->retireAfterFrameIndex > completedFrameIndex)
                    {
                        ++it;
                        continue;
                    }

                    const auto bucketIt = m_textureStates.find(it->texture.get());
                    if (bucketIt != m_textureStates.end())
                    {
                        if (IsFullTextureSubresourceRange(it->texture->GetDesc(), it->subresourceRange))
                        {
                            m_textureStates.erase(bucketIt);
                        }
                        else
                        {
                            TextureStateBucket remainingBucket;
                            remainingBucket.reserve(bucketIt->second.size());
                            for (const auto& trackedState : bucketIt->second)
                            {
                                if (!DoesSubresourceRangeOverlap(trackedState.subresourceRange, it->subresourceRange))
                                {
                                    remainingBucket.push_back(trackedState);
                                    continue;
                                }

                                const auto remainders = SubtractSubresourceRange(
                                    trackedState.subresourceRange,
                                    it->subresourceRange);
                                for (const auto& remainder : remainders)
                                {
                                    if (remainder.mipLevelCount == 0u || remainder.arrayLayerCount == 0u)
                                        continue;

                                    remainingBucket.push_back({
                                        trackedState.texture,
                                        remainder,
                                        trackedState.state,
                                        trackedState.stageMask,
                                        trackedState.accessMask
                                    });
                                }
                            }

                            if (remainingBucket.empty())
                                m_textureStates.erase(bucketIt);
                            else
                                bucketIt->second = std::move(remainingBucket);
                        }
                    }
                    ++m_stats.retiredTransientTextures;
                    it = m_transientTextures.erase(it);
                }

                for (auto it = m_transientTextureViews.begin(); it != m_transientTextureViews.end();)
                {
                    if (it->textureView == nullptr || it->retireAfterFrameIndex > completedFrameIndex)
                    {
                        ++it;
                        continue;
                    }

                    ++m_stats.retiredTransientTextureViews;
                    it = m_transientTextureViews.erase(it);
                }

                m_stats.trackedBufferCount = static_cast<uint64_t>(m_bufferStates.size());
                m_stats.trackedTextureCount = CountTrackedTextureStates(m_textureStates);
            }

            void Commit(const RHIBarrierDesc& barriers) override
            {
                for (const auto& barrier : barriers.bufferBarriers)
                {
                    if (barrier.buffer == nullptr)
                        continue;
                    if (IsCpuVisibleBuffer(barrier.buffer))
                        continue;

                    m_bufferStates[BufferKey{ barrier.buffer.get() }] = TrackedBufferState{
                        barrier.buffer,
                        barrier.after,
                        barrier.destinationStageMask,
                        barrier.destinationAccessMask
                    };
                }

                for (const auto& barrier : barriers.textureBarriers)
                {
                    if (barrier.texture == nullptr)
                        continue;

                    const auto normalizedRange = NormalizeTextureSubresourceRange(
                        barrier.texture->GetDesc(),
                        barrier.subresourceRange);
                    if (!normalizedRange.has_value())
                        continue;

                    const auto& textureDesc = barrier.texture->GetDesc();
                    if (IsFullTextureSubresourceRange(textureDesc, *normalizedRange))
                    {
                        auto& bucket = m_textureStates[barrier.texture.get()];
                        bucket.clear();
                        bucket.push_back({
                            barrier.texture,
                            *normalizedRange,
                            barrier.after,
                            barrier.destinationStageMask,
                            barrier.destinationAccessMask
                        });
                        continue;
                    }

                    auto& bucket = m_textureStates[barrier.texture.get()];
                    bool isCoveredBySameState = false;
                    for (const auto& trackedState : bucket)
                    {
                        if (trackedState.state != barrier.after)
                            continue;

                        if (DoesSubresourceRangeCover(trackedState.subresourceRange, *normalizedRange))
                        {
                            isCoveredBySameState = true;
                            break;
                        }
                    }
                    if (isCoveredBySameState)
                        continue;

                    TextureStateBucket updatedBucket;
                    updatedBucket.reserve(bucket.size() + 4u);
                    for (const auto& trackedState : bucket)
                    {
                        if (!DoesSubresourceRangeOverlap(trackedState.subresourceRange, *normalizedRange))
                        {
                            updatedBucket.push_back(trackedState);
                            continue;
                        }

                        const auto remainders = SubtractSubresourceRange(trackedState.subresourceRange, *normalizedRange);
                        for (const auto& remainder : remainders)
                        {
                            if (remainder.mipLevelCount == 0u || remainder.arrayLayerCount == 0u)
                                continue;

                            updatedBucket.push_back({
                                barrier.texture,
                                remainder,
                                trackedState.state,
                                trackedState.stageMask,
                                trackedState.accessMask
                            });
                        }
                    }

                    updatedBucket.push_back({
                        barrier.texture,
                        *normalizedRange,
                        barrier.after,
                        barrier.destinationStageMask,
                        barrier.destinationAccessMask
                    });
                    bucket = std::move(updatedBucket);
                }

                m_stats.trackedBufferCount = static_cast<uint64_t>(m_bufferStates.size());
                m_stats.trackedTextureCount = CountTrackedTextureStates(m_textureStates);
            }

            ResourceStateTrackerStats GetStats() const override
            {
                return m_stats;
            }

        private:
            ResourceStateTrackerStats m_stats{};
            std::unordered_map<BufferKey, TrackedBufferState, BufferKeyHash> m_bufferStates;
            std::unordered_map<const RHITexture*, TextureStateBucket> m_textureStates;
            std::vector<TransientBufferRetirement> m_transientBuffers;
            std::vector<TransientTextureRetirement> m_transientTextures;
            std::vector<TransientTextureViewRetirement> m_transientTextureViews;
        };
    }

    std::shared_ptr<ResourceStateTracker> CreateDefaultResourceStateTracker()
    {
        return std::make_shared<DefaultResourceStateTracker>();
    }
}
