#pragma once

#include <cstdint>
#include <memory>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIPipeline.h"

namespace NLS::Render::RHI
{
    struct NLS_RENDER_API PipelineCacheKey
    {
        uint64_t hash = 0;

        [[nodiscard]] bool IsValid() const
        {
            return hash != 0;
        }
    };

    struct NLS_RENDER_API PipelineCacheStats
    {
        uint64_t graphicsHits = 0;
        uint64_t graphicsMisses = 0;
        uint64_t computeHits = 0;
        uint64_t computeMisses = 0;
    };

    class NLS_RENDER_API PipelineCache
    {
    public:
        virtual ~PipelineCache() = default;
        virtual void Reset() = 0;
        virtual std::shared_ptr<RHIGraphicsPipeline> FindGraphicsPipeline(const PipelineCacheKey& key) const = 0;
        virtual std::shared_ptr<RHIComputePipeline> FindComputePipeline(const PipelineCacheKey& key) const = 0;
        virtual void StoreGraphicsPipeline(const PipelineCacheKey& key, const std::shared_ptr<RHIGraphicsPipeline>& pipeline) = 0;
        virtual void StoreComputePipeline(const PipelineCacheKey& key, const std::shared_ptr<RHIComputePipeline>& pipeline) = 0;
        virtual PipelineCacheStats GetStats() const = 0;
    };

    NLS_RENDER_API std::shared_ptr<PipelineCache> CreateDefaultPipelineCache();
}
