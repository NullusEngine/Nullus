#pragma once

#include <array>
#include <memory>
#include <string_view>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
    struct ComputeShaderUtils final
    {
        static std::shared_ptr<RHI::RHIBindingLayout> CreatePassBindingLayout(
            const std::shared_ptr<RHI::RHIDevice>& device,
            const ShaderParameterStruct& parameters)
        {
            if (device == nullptr)
                return nullptr;

            return device->CreateBindingLayout(BuildBindingLayoutDescFromShaderParameters(parameters));
        }

        static std::shared_ptr<RHI::RHIPipelineLayout> CreatePipelineLayout(
            const std::shared_ptr<RHI::RHIDevice>& device,
            const std::shared_ptr<RHI::RHIBindingLayout>& passBindingLayout,
            std::string_view debugName)
        {
            if (device == nullptr || passBindingLayout == nullptr)
                return nullptr;

            RHI::RHIPipelineLayoutDesc desc;
            desc.debugName = std::string(debugName);
            desc.bindingLayouts.resize(RHI::BindingPointMap::kPassDescriptorSet + 1u);
            desc.bindingLayouts[RHI::BindingPointMap::kPassDescriptorSet] = passBindingLayout;
            return device->CreatePipelineLayout(desc);
        }

        static std::shared_ptr<RHI::RHIComputePipeline> CreateComputePipeline(
            const std::shared_ptr<RHI::RHIDevice>& device,
            const std::shared_ptr<RHI::PipelineCache>& pipelineCache,
            const GlobalShader& globalShader,
            const std::shared_ptr<RHI::RHIPipelineLayout>& pipelineLayout,
            std::shared_ptr<RHI::RHIComputePipeline> existingPipeline,
            RHI::PipelineCacheKey& existingPipelineKey,
            std::string_view debugName,
            const bool prewarm = true)
        {
            if (device == nullptr || pipelineCache == nullptr || globalShader.shader == nullptr || pipelineLayout == nullptr)
                return nullptr;

            auto shaderModule = globalShader.shader->GetOrCreateExplicitShaderModule(device, globalShader.stage);
            if (shaderModule == nullptr)
                return nullptr;

            RHI::RHIComputePipelineDesc desc;
            desc.pipelineLayout = pipelineLayout;
            desc.computeShader = shaderModule;
            desc.debugName = std::string(debugName);
            const auto cacheKey = RHI::BuildComputePipelineCacheKey(desc);
            if (existingPipeline != nullptr &&
                existingPipelineKey.hash == cacheKey.hash &&
                existingPipelineKey.backend == cacheKey.backend &&
                existingPipelineKey.stableDebugName == cacheKey.stableDebugName)
            {
                return existingPipeline;
            }

            existingPipelineKey = cacheKey;
            return pipelineCache->GetOrCreateComputePipeline(
                cacheKey,
                [device, desc]()
                {
                    return device->CreateComputePipeline(desc);
                },
                prewarm ? RHI::PipelineCacheRequestMode::Prewarm : RHI::PipelineCacheRequestMode::Runtime);
        }

        static Context::RecordedComputeDispatchInput BuildRecordedDispatch(
            const GlobalShader& globalShader,
            std::shared_ptr<RHI::RHIComputePipeline> pipeline,
            std::shared_ptr<RHI::RHIBindingSet> passBindingSet,
            const std::array<uint32_t, 3u>& groupCounts,
            std::string_view debugName)
        {
            Context::RecordedComputeDispatchInput input;
            input.debugName = std::string(debugName.empty() ? std::string_view(globalShader.debugName) : debugName);
            input.pipeline = std::move(pipeline);
            input.bindingSets.push_back({ globalShader.parameters.descriptorSet, std::move(passBindingSet) });
            input.groupCountX = groupCounts[0];
            input.groupCountY = groupCounts[1];
            input.groupCountZ = groupCounts[2];
            return input;
        }
    };
}
