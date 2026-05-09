#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIPipeline.h"

namespace NLS::Render::RHI
{
    enum class NLS_RENDER_API PipelineCacheRequestMode : uint8_t
    {
        Runtime,
        Prewarm
    };

    struct NLS_RENDER_API PipelineCacheKey
    {
        uint64_t hash = 0;
        NativeBackendType backend = NativeBackendType::None;
        std::string stableDebugName;

        [[nodiscard]] bool IsValid() const
        {
            return hash != 0 || !stableDebugName.empty();
        }
    };

    struct NLS_RENDER_API PipelineCacheStats
    {
        uint64_t graphicsHits = 0;
        uint64_t graphicsMisses = 0;
        uint64_t computeHits = 0;
        uint64_t computeMisses = 0;
        uint64_t graphicsStores = 0;
        uint64_t computeStores = 0;
        uint64_t graphicsEntryCount = 0;
        uint64_t computeEntryCount = 0;
        uint64_t graphicsPrewarmRequests = 0;
        uint64_t computePrewarmRequests = 0;
        uint64_t graphicsPrewarmHits = 0;
        uint64_t computePrewarmHits = 0;
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
        virtual std::shared_ptr<RHIGraphicsPipeline> GetOrCreateGraphicsPipeline(
            const PipelineCacheKey& key,
            const std::function<std::shared_ptr<RHIGraphicsPipeline>()>& createPipeline,
            PipelineCacheRequestMode requestMode = PipelineCacheRequestMode::Runtime) = 0;
        virtual std::shared_ptr<RHIComputePipeline> GetOrCreateComputePipeline(
            const PipelineCacheKey& key,
            const std::function<std::shared_ptr<RHIComputePipeline>()>& createPipeline,
            PipelineCacheRequestMode requestMode = PipelineCacheRequestMode::Runtime) = 0;
        virtual PipelineCacheStats GetStats() const = 0;
    };

    NLS_RENDER_API uint64_t BuildGraphicsPipelineCacheHash(const RHIGraphicsPipelineDesc& desc);
    NLS_RENDER_API PipelineCacheKey BuildGraphicsPipelineCacheKey(const RHIGraphicsPipelineDesc& desc);
    NLS_RENDER_API uint64_t BuildComputePipelineCacheHash(const RHIComputePipelineDesc& desc);
    NLS_RENDER_API PipelineCacheKey BuildComputePipelineCacheKey(const RHIComputePipelineDesc& desc);
    NLS_RENDER_API std::shared_ptr<PipelineCache> CreateDefaultPipelineCache();
}
