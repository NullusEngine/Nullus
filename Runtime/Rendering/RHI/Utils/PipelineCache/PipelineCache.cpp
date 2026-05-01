#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"

#include <algorithm>
#include <unordered_map>

namespace NLS::Render::RHI
{
    namespace
    {
        template<typename TValue>
        void HashCombine(uint64_t& seed, const TValue& value)
        {
            seed ^= static_cast<uint64_t>(std::hash<TValue>{}(value)) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        }

        uint64_t ResolveEffectivePipelineCacheHash(const PipelineCacheKey& key)
        {
            uint64_t resolvedHash = key.hash;
            HashCombine(resolvedHash, static_cast<uint32_t>(key.backend));
            if (!key.stableDebugName.empty())
                HashCombine(resolvedHash, key.stableDebugName);
            return resolvedHash;
        }

        class DefaultPipelineCache final : public PipelineCache
        {
        public:
            void Reset() override
            {
                m_graphicsPipelines.clear();
                m_computePipelines.clear();
                m_stats = {};
            }

            std::shared_ptr<RHIGraphicsPipeline> FindGraphicsPipeline(const PipelineCacheKey& key) const override
            {
                if (!key.IsValid())
                    return nullptr;

                const auto it = m_graphicsPipelines.find(ResolveEffectivePipelineCacheHash(key));
                if (it == m_graphicsPipelines.end())
                {
                    ++m_stats.graphicsMisses;
                    return nullptr;
                }

                ++m_stats.graphicsHits;
                return it->second;
            }

            std::shared_ptr<RHIComputePipeline> FindComputePipeline(const PipelineCacheKey& key) const override
            {
                if (!key.IsValid())
                    return nullptr;

                const auto it = m_computePipelines.find(ResolveEffectivePipelineCacheHash(key));
                if (it == m_computePipelines.end())
                {
                    ++m_stats.computeMisses;
                    return nullptr;
                }

                ++m_stats.computeHits;
                return it->second;
            }

            void StoreGraphicsPipeline(const PipelineCacheKey& key, const std::shared_ptr<RHIGraphicsPipeline>& pipeline) override
            {
                if (!key.IsValid() || pipeline == nullptr)
                    return;

                m_graphicsPipelines[ResolveEffectivePipelineCacheHash(key)] = pipeline;
                ++m_stats.graphicsStores;
                m_stats.graphicsEntryCount = static_cast<uint64_t>(m_graphicsPipelines.size());
            }

            void StoreComputePipeline(const PipelineCacheKey& key, const std::shared_ptr<RHIComputePipeline>& pipeline) override
            {
                if (!key.IsValid() || pipeline == nullptr)
                    return;

                m_computePipelines[ResolveEffectivePipelineCacheHash(key)] = pipeline;
                ++m_stats.computeStores;
                m_stats.computeEntryCount = static_cast<uint64_t>(m_computePipelines.size());
            }

            std::shared_ptr<RHIGraphicsPipeline> GetOrCreateGraphicsPipeline(
                const PipelineCacheKey& key,
                const std::function<std::shared_ptr<RHIGraphicsPipeline>()>& createPipeline,
                const PipelineCacheRequestMode requestMode) override
            {
                if (requestMode == PipelineCacheRequestMode::Prewarm)
                    ++m_stats.graphicsPrewarmRequests;

                if (auto pipeline = FindGraphicsPipeline(key); pipeline != nullptr)
                {
                    if (requestMode == PipelineCacheRequestMode::Prewarm)
                        ++m_stats.graphicsPrewarmHits;
                    return pipeline;
                }

                if (!createPipeline)
                    return nullptr;

                auto pipeline = createPipeline();
                if (pipeline != nullptr)
                    StoreGraphicsPipeline(key, pipeline);
                return pipeline;
            }

            std::shared_ptr<RHIComputePipeline> GetOrCreateComputePipeline(
                const PipelineCacheKey& key,
                const std::function<std::shared_ptr<RHIComputePipeline>()>& createPipeline,
                const PipelineCacheRequestMode requestMode) override
            {
                if (requestMode == PipelineCacheRequestMode::Prewarm)
                    ++m_stats.computePrewarmRequests;

                if (auto pipeline = FindComputePipeline(key); pipeline != nullptr)
                {
                    if (requestMode == PipelineCacheRequestMode::Prewarm)
                        ++m_stats.computePrewarmHits;
                    return pipeline;
                }

                if (!createPipeline)
                    return nullptr;

                auto pipeline = createPipeline();
                if (pipeline != nullptr)
                    StoreComputePipeline(key, pipeline);
                return pipeline;
            }

            PipelineCacheStats GetStats() const override
            {
                return m_stats;
            }

        private:
            mutable PipelineCacheStats m_stats{};
            std::unordered_map<uint64_t, std::shared_ptr<RHIGraphicsPipeline>> m_graphicsPipelines;
            std::unordered_map<uint64_t, std::shared_ptr<RHIComputePipeline>> m_computePipelines;
        };
    }

    std::shared_ptr<PipelineCache> CreateDefaultPipelineCache()
    {
        return std::make_shared<DefaultPipelineCache>();
    }
}
