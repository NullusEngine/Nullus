#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"

#include <unordered_map>

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

        struct TextureKey
        {
            const RHITexture* texture = nullptr;
            RHISubresourceRange subresourceRange{};

            bool operator==(const TextureKey& rhs) const
            {
                return texture == rhs.texture &&
                    subresourceRange.baseMipLevel == rhs.subresourceRange.baseMipLevel &&
                    subresourceRange.mipLevelCount == rhs.subresourceRange.mipLevelCount &&
                    subresourceRange.baseArrayLayer == rhs.subresourceRange.baseArrayLayer &&
                    subresourceRange.arrayLayerCount == rhs.subresourceRange.arrayLayerCount;
            }
        };

        struct BufferKeyHash
        {
            size_t operator()(const BufferKey& key) const
            {
                return std::hash<const RHIBuffer*>{}(key.buffer);
            }
        };

        struct TextureKeyHash
        {
            size_t operator()(const TextureKey& key) const
            {
                size_t hash = std::hash<const RHITexture*>{}(key.texture);
                hash ^= std::hash<uint32_t>{}(key.subresourceRange.baseMipLevel) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<uint32_t>{}(key.subresourceRange.mipLevelCount) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<uint32_t>{}(key.subresourceRange.baseArrayLayer) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<uint32_t>{}(key.subresourceRange.arrayLayerCount) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

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

                const auto it = m_textureStates.find(TextureKey{ texture.get(), subresourceRange });
                if (it == m_textureStates.end())
                    return std::nullopt;

                return it->second;
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

                    resolved.bufferBarriers.push_back(barrier);
                }

                resolved.textureBarriers.reserve(textureBarriers.size());
                for (auto barrier : textureBarriers)
                {
                    if (barrier.texture == nullptr)
                        continue;

                    if (barrier.before == ResourceState::Unknown)
                    {
                        if (const auto tracked = GetTextureState(barrier.texture, barrier.subresourceRange); tracked.has_value())
                        {
                            barrier.before = tracked->state;
                            barrier.sourceStageMask = tracked->stageMask;
                            barrier.sourceAccessMask = tracked->accessMask;
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

                m_transientTextures.push_back({ texture, subresourceRange, retireAfterFrameIndex });
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

                    m_textureStates.erase(TextureKey{ it->texture.get(), it->subresourceRange });
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
                m_stats.trackedTextureCount = static_cast<uint64_t>(m_textureStates.size());
            }

            void Commit(const RHIBarrierDesc& barriers) override
            {
                for (const auto& barrier : barriers.bufferBarriers)
                {
                    if (barrier.buffer == nullptr)
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

                    m_textureStates[TextureKey{ barrier.texture.get(), barrier.subresourceRange }] = TrackedTextureState{
                        barrier.texture,
                        barrier.subresourceRange,
                        barrier.after,
                        barrier.destinationStageMask,
                        barrier.destinationAccessMask
                    };
                }

                m_stats.trackedBufferCount = static_cast<uint64_t>(m_bufferStates.size());
                m_stats.trackedTextureCount = static_cast<uint64_t>(m_textureStates.size());
            }

            ResourceStateTrackerStats GetStats() const override
            {
                return m_stats;
            }

        private:
            ResourceStateTrackerStats m_stats{};
            std::unordered_map<BufferKey, TrackedBufferState, BufferKeyHash> m_bufferStates;
            std::unordered_map<TextureKey, TrackedTextureState, TextureKeyHash> m_textureStates;
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
