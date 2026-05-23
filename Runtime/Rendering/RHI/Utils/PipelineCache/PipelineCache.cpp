#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"

#include <algorithm>
#include <string_view>
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

        void HashBytes(uint64_t& seed, const std::vector<uint8_t>& bytes)
        {
            HashCombine(seed, bytes.size());
            uint64_t chunk = 0u;
            uint32_t chunkByteCount = 0u;
            for (const uint8_t byte : bytes)
            {
                chunk |= static_cast<uint64_t>(byte) << (chunkByteCount * 8u);
                ++chunkByteCount;
                if (chunkByteCount == 8u)
                {
                    HashCombine(seed, chunk);
                    chunk = 0u;
                    chunkByteCount = 0u;
                }
            }
            if (chunkByteCount > 0u)
                HashCombine(seed, chunk);
        }

        void HashShaderModuleDesc(uint64_t& seed, const RHIShaderModuleDesc& desc)
        {
            HashCombine(seed, static_cast<uint32_t>(desc.stage));
            HashCombine(seed, static_cast<uint32_t>(desc.targetBackend));
            HashCombine(seed, desc.entryPoint);
            HashCombine(seed, desc.debugName);
            HashCombine(seed, desc.shaderToolchainFingerprint);
            HashBytes(seed, desc.bytecode);
        }

        void HashBindingLayoutDesc(uint64_t& seed, const RHIBindingLayoutDesc& desc)
        {
            HashCombine(seed, desc.debugName);
            HashCombine(seed, desc.entries.size());
            for (const auto& entry : desc.entries)
            {
                HashCombine(seed, entry.name);
                HashCombine(seed, static_cast<uint32_t>(entry.type));
                HashCombine(seed, entry.set);
                HashCombine(seed, entry.binding);
                HashCombine(seed, entry.count);
                HashCombine(seed, static_cast<uint32_t>(entry.stageMask));
                HashCombine(seed, entry.registerSpace);
                HashCombine(seed, entry.elementStride);
            }
        }

        void HashPipelineLayoutDesc(uint64_t& seed, const RHIPipelineLayoutDesc& desc)
        {
            HashCombine(seed, desc.debugName);
            HashCombine(seed, desc.bindingLayouts.size());
            for (const auto& bindingLayout : desc.bindingLayouts)
            {
                if (bindingLayout != nullptr)
                    HashBindingLayoutDesc(seed, bindingLayout->GetDesc());
                else
                    HashCombine(seed, std::string_view("<null-binding-layout>"));
            }

            HashCombine(seed, desc.pushConstants.size());
            for (const auto& pushConstant : desc.pushConstants)
            {
                HashCombine(seed, static_cast<uint32_t>(pushConstant.stageMask));
                HashCombine(seed, pushConstant.offset);
                HashCombine(seed, pushConstant.size);
                HashCombine(seed, pushConstant.shaderRegister);
                HashCombine(seed, pushConstant.registerSpace);
            }
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

    uint64_t BuildGraphicsPipelineCacheHash(const RHIGraphicsPipelineDesc& desc)
    {
        uint64_t hash = 0u;
        if (desc.pipelineLayout != nullptr)
            HashPipelineLayoutDesc(hash, desc.pipelineLayout->GetDesc());
        else
            HashCombine(hash, std::string_view("<null-pipeline-layout>"));

        if (desc.vertexShader != nullptr)
            HashShaderModuleDesc(hash, desc.vertexShader->GetDesc());
        else
            HashCombine(hash, std::string_view("<null-vertex-shader>"));

        if (desc.fragmentShader != nullptr)
            HashShaderModuleDesc(hash, desc.fragmentShader->GetDesc());
        else
            HashCombine(hash, std::string_view("<null-fragment-shader>"));

        HashCombine(hash, static_cast<uint32_t>(desc.primitiveTopology));
        HashCombine(hash, desc.rasterState.cullEnabled);
        HashCombine(hash, static_cast<uint32_t>(desc.rasterState.cullFace));
        HashCombine(hash, desc.rasterState.wireframe);
        HashCombine(hash, desc.rasterState.multisampleEnable);
        HashCombine(hash, desc.blendState.enabled);
        HashCombine(hash, desc.blendState.colorWrite);
        HashCombine(hash, desc.blendState.alphaToCoverageEnable);
        HashCombine(hash, desc.blendState.independentBlendEnable);
        HashCombine(hash, desc.blendState.renderTargets.size());
        for (const auto& target : desc.blendState.renderTargets)
        {
            HashCombine(hash, target.blendEnable);
            HashCombine(hash, static_cast<uint32_t>(target.srcColor));
            HashCombine(hash, static_cast<uint32_t>(target.dstColor));
            HashCombine(hash, static_cast<uint32_t>(target.colorOp));
            HashCombine(hash, static_cast<uint32_t>(target.srcAlpha));
            HashCombine(hash, static_cast<uint32_t>(target.dstAlpha));
            HashCombine(hash, static_cast<uint32_t>(target.alphaOp));
            HashCombine(hash, static_cast<uint32_t>(target.colorWriteMask));
        }
        HashCombine(hash, desc.depthStencilState.depthTest);
        HashCombine(hash, desc.depthStencilState.depthWrite);
        HashCombine(hash, static_cast<uint32_t>(desc.depthStencilState.depthCompare));
        HashCombine(hash, desc.depthStencilState.stencilTest);
        HashCombine(hash, desc.depthStencilState.stencilReadMask);
        HashCombine(hash, desc.depthStencilState.stencilWriteMask);
        HashCombine(hash, desc.depthStencilState.stencilReference);
        HashCombine(hash, static_cast<uint32_t>(desc.depthStencilState.stencilCompare));
        HashCombine(hash, static_cast<uint32_t>(desc.depthStencilState.stencilFailOp));
        HashCombine(hash, static_cast<uint32_t>(desc.depthStencilState.stencilDepthFailOp));
        HashCombine(hash, static_cast<uint32_t>(desc.depthStencilState.stencilPassOp));
        HashCombine(hash, static_cast<uint32_t>(desc.renderTargetLayout.depthFormat));
        HashCombine(hash, desc.renderTargetLayout.hasDepth);
        HashCombine(hash, desc.renderTargetLayout.sampleCount);
        HashCombine(hash, desc.renderTargetLayout.colorFormats.size());
        for (const auto colorFormat : desc.renderTargetLayout.colorFormats)
            HashCombine(hash, static_cast<uint32_t>(colorFormat));
        HashCombine(hash, desc.vertexBuffers.size());
        for (const auto& vertexBuffer : desc.vertexBuffers)
        {
            HashCombine(hash, vertexBuffer.binding);
            HashCombine(hash, vertexBuffer.stride);
            HashCombine(hash, vertexBuffer.perInstance);
        }
        HashCombine(hash, desc.vertexAttributes.size());
        for (const auto& vertexAttribute : desc.vertexAttributes)
        {
            HashCombine(hash, vertexAttribute.location);
            HashCombine(hash, vertexAttribute.binding);
            HashCombine(hash, vertexAttribute.offset);
            HashCombine(hash, vertexAttribute.elementSize);
        }
        return hash;
    }

    PipelineCacheKey BuildGraphicsPipelineCacheKey(const RHIGraphicsPipelineDesc& desc)
    {
        PipelineCacheKey key;
        key.hash = BuildGraphicsPipelineCacheHash(desc);
        key.backend =
            desc.vertexShader != nullptr
                ? desc.vertexShader->GetDesc().targetBackend
                : desc.fragmentShader != nullptr
                    ? desc.fragmentShader->GetDesc().targetBackend
                    : NativeBackendType::None;
        key.stableDebugName = desc.debugName;
        return key;
    }

    uint64_t BuildComputePipelineCacheHash(const RHIComputePipelineDesc& desc)
    {
        uint64_t hash = 0u;
        if (desc.pipelineLayout != nullptr)
            HashPipelineLayoutDesc(hash, desc.pipelineLayout->GetDesc());
        else
            HashCombine(hash, std::string_view("<null-pipeline-layout>"));

        if (desc.computeShader != nullptr)
            HashShaderModuleDesc(hash, desc.computeShader->GetDesc());
        else
            HashCombine(hash, std::string_view("<null-compute-shader>"));
        return hash;
    }

    PipelineCacheKey BuildComputePipelineCacheKey(const RHIComputePipelineDesc& desc)
    {
        PipelineCacheKey key;
        key.hash = BuildComputePipelineCacheHash(desc);
        key.backend = desc.computeShader != nullptr
            ? desc.computeShader->GetDesc().targetBackend
            : NativeBackendType::None;
        key.stableDebugName = desc.debugName;
        return key;
    }

    std::shared_ptr<PipelineCache> CreateDefaultPipelineCache()
    {
        return std::make_shared<DefaultPipelineCache>();
    }
}
