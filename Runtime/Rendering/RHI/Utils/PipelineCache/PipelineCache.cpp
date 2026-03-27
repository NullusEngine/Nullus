#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"

#include <unordered_map>

namespace NLS::Render::RHI
{
    namespace
    {
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

                const auto it = m_graphicsPipelines.find(key.hash);
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

                const auto it = m_computePipelines.find(key.hash);
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

                m_graphicsPipelines[key.hash] = pipeline;
            }

            void StoreComputePipeline(const PipelineCacheKey& key, const std::shared_ptr<RHIComputePipeline>& pipeline) override
            {
                if (!key.IsValid() || pipeline == nullptr)
                    return;

                m_computePipelines[key.hash] = pipeline;
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
