#include "Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h"

#include <algorithm>
#include <utility>

#include "Profiling/Profiler.h"
#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderInternal.h"

namespace NLS::Render::FrameGraph
{
    namespace
    {
        bool IsDeferredScenePassKind(const NLS::Render::Context::RenderPassCommandKind kind)
        {
            return GetDeferredScenePassExecutionKind(kind) != DeferredScenePassExecutionKind::Unknown;
        }

        struct DeferredGBufferGraphPassData
        {
            FrameGraphResource albedo = -1;
            FrameGraphResource normal = -1;
            FrameGraphResource material = -1;
            FrameGraphResource depth = -1;
        };

        struct DeferredLightingGraphPassData
        {
            FrameGraphResource albedo = -1;
            FrameGraphResource normal = -1;
            FrameGraphResource material = -1;
            FrameGraphResource depth = -1;
            FrameGraphResource outputColor = -1;
            FrameGraphResource outputDepth = -1;
        };

        void AddFullTextureResourceAccess(
            NLS::Render::Context::RenderPassCommandInput& passInput,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::Context::ResourceAccessMode mode,
            const NLS::Render::RHI::ResourceState state,
            const NLS::Render::RHI::PipelineStageMask stages,
            const NLS::Render::RHI::AccessMask access)
        {
            if (texture == nullptr)
                return;

            NLS::Render::RHI::RHISubresourceRange fullRange;
            fullRange.baseMipLevel = 0u;
            fullRange.mipLevelCount = texture->GetDesc().mipLevels;
            fullRange.baseArrayLayer = 0u;
            fullRange.arrayLayerCount = texture->GetDesc().arrayLayers;
            passInput.textureResourceAccesses.push_back({
                texture,
                fullRange,
                mode,
                state,
                stages,
                access
            });
        }

        bool HasTextureWriteResourceAccess(
            const NLS::Render::Context::RenderPassCommandInput& passInput,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHISubresourceRange& subresourceRange)
        {
            if (texture == nullptr)
                return true;

            return std::any_of(
                passInput.textureResourceAccesses.begin(),
                passInput.textureResourceAccesses.end(),
                [&texture, &subresourceRange](const NLS::Render::Context::TextureResourceAccess& access)
                {
                    if (access.texture != texture ||
                        access.mode != NLS::Render::Context::ResourceAccessMode::Write)
                    {
                        return false;
                    }

                    const auto accessRange = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                        texture->GetDesc(),
                        access.subresourceRange);
                    const auto requestedRange = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                        texture->GetDesc(),
                        subresourceRange);
                    return accessRange.has_value() &&
                        requestedRange.has_value() &&
                        NLS::Render::RHI::DoesSubresourceRangeCover(*accessRange, *requestedRange);
                });
        }

        void AddTextureResourceAccess(
            NLS::Render::Context::RenderPassCommandInput& passInput,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHISubresourceRange& subresourceRange,
            const NLS::Render::Context::ResourceAccessMode mode,
            const NLS::Render::RHI::ResourceState state,
            const NLS::Render::RHI::PipelineStageMask stages,
            const NLS::Render::RHI::AccessMask access)
        {
            if (texture == nullptr)
                return;

            passInput.textureResourceAccesses.push_back({
                texture,
                subresourceRange,
                mode,
                state,
                stages,
                access
            });
        }

        void AddColorAttachmentWriteResourceAccesses(
            NLS::Render::Context::RenderPassCommandInput& passInput)
        {
            for (const auto& colorView : passInput.colorAttachmentViews)
            {
                const auto& texture = colorView != nullptr ? colorView->GetTexture() : nullptr;
                const auto subresourceRange = colorView != nullptr
                    ? colorView->GetDesc().subresourceRange
                    : NLS::Render::RHI::RHISubresourceRange{};
                if (HasTextureWriteResourceAccess(passInput, texture, subresourceRange))
                    continue;

                AddTextureResourceAccess(
                    passInput,
                    texture,
                    subresourceRange,
                    NLS::Render::Context::ResourceAccessMode::Write,
                    NLS::Render::RHI::ResourceState::RenderTarget,
                    NLS::Render::RHI::PipelineStageMask::RenderTarget,
                    NLS::Render::RHI::AccessMask::ColorAttachmentRead |
                        NLS::Render::RHI::AccessMask::ColorAttachmentWrite);
            }
        }

        void AttachDeferredGBufferDepthForHelperPasses(
            NLS::Render::Context::RenderPassCommandInput& passInput,
            const DeferredPreparedSceneResources& resources)
        {
            if (passInput.kind != NLS::Render::Context::RenderPassCommandKind::Helper ||
                !passInput.usesDepthStencilAttachment ||
                resources.gbufferDepthView == nullptr)
            {
                return;
            }

            if (passInput.depthStencilAttachmentView != nullptr &&
                (passInput.clearDepth || passInput.clearStencil))
            {
                return;
            }

            passInput.depthStencilAttachmentView = resources.gbufferDepthView;
            AddFullTextureResourceAccess(
                passInput,
                resources.gbufferDepthView->GetTexture(),
                NLS::Render::Context::ResourceAccessMode::Read,
                NLS::Render::RHI::ResourceState::DepthRead,
                NLS::Render::RHI::PipelineStageMask::DepthStencil,
                NLS::Render::RHI::AccessMask::DepthStencilRead);
        }

        NLS::Render::FrameGraph::FrameGraphTexture::Desc MakeGBufferColorDesc(
            uint16_t width,
            uint16_t height,
            std::string debugName)
        {
            NLS::Render::FrameGraph::FrameGraphTexture::Desc desc;
            desc.extent.width = width;
            desc.extent.height = height;
            desc.extent.depth = 1u;
            desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
            desc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment | NLS::Render::RHI::TextureUsageFlags::Sampled;
            desc.debugName = std::move(debugName);
            return desc;
        }

        NLS::Render::FrameGraph::FrameGraphTexture::Desc MakeGBufferDepthDesc(uint16_t width, uint16_t height)
        {
            NLS::Render::FrameGraph::FrameGraphTexture::Desc desc;
            desc.extent.width = width;
            desc.extent.height = height;
            desc.extent.depth = 1u;
            desc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
            desc.usage = NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment | NLS::Render::RHI::TextureUsageFlags::Sampled;
            desc.debugName = "GBufferDepth";
            return desc;
        }

        template<typename TExecuteFn>
        const DeferredGBufferGraphPassData& AddDeferredCompiledGBufferPass(
            ::FrameGraph& frameGraph,
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            const FrameGraphResource gbufferAlbedo,
            const FrameGraphResource gbufferNormal,
            const FrameGraphResource gbufferMaterial,
            const FrameGraphResource gbufferDepth,
            TExecuteFn&& execute)
        {
            return AddExecutingMetadataPass<DeferredGBufferGraphPassData>(
                frameGraph,
                compiledPass,
                [=](::FrameGraph::Builder& builder, DeferredGBufferGraphPassData& data)
                {
                    data.albedo = builder.write(gbufferAlbedo);
                    data.normal = builder.write(gbufferNormal);
                    data.material = builder.write(gbufferMaterial);
                    data.depth = builder.write(gbufferDepth);
                },
                std::forward<TExecuteFn>(execute));
        }

        template<typename TExecuteFn>
        void AddDeferredCompiledLightingPass(
            ::FrameGraph& frameGraph,
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            ScenePassOutputResourceState& outputState,
            const DeferredGBufferGraphPassData* gBufferPassData,
            TExecuteFn&& execute)
        {
            AddExecutingSceneOutputPass<
                DeferredLightingGraphPassData,
                &DeferredLightingGraphPassData::outputColor,
                &DeferredLightingGraphPassData::outputDepth>(
                frameGraph,
                compiledPass,
                outputState,
                [gBufferPassData](::FrameGraph::Builder& builder, DeferredLightingGraphPassData& data)
                {
                    if (gBufferPassData == nullptr)
                        return;
                    data.albedo = builder.read(gBufferPassData->albedo);
                    data.normal = builder.read(gBufferPassData->normal);
                    data.material = builder.read(gBufferPassData->material);
                    data.depth = builder.read(gBufferPassData->depth);
                },
                std::forward<TExecuteFn>(execute));
        }

        template<typename TAddGBufferPassFn, typename TAddLightingPassFn>
        void DispatchDeferredCompiledGraphPasses(
            ::FrameGraph& frameGraph,
            const PreparedComputeDispatchSource& preparedComputeSource,
            const std::vector<CompiledThreadedRenderSceneGraphPass>& compiledPasses,
            TAddGBufferPassFn&& addGBufferPass,
            TAddLightingPassFn&& addLightingPass)
        {
            const DeferredGBufferGraphPassData* gBufferPassData = nullptr;
            ScenePassOutputResourceState outputState;
            for (const auto& compiledPass : compiledPasses)
            {
                if (TryAddPreparedComputeDispatchCompiledGraphPass(
                    frameGraph,
                    preparedComputeSource,
                    compiledPass))
                {
                    continue;
                }

                SeedScenePassOutputResourceState(outputState, compiledPass.outputChain);

                switch (GetDeferredScenePassExecutionKind(compiledPass.metadata.commandKind))
                {
                case DeferredScenePassExecutionKind::GBuffer:
                    gBufferPassData = &addGBufferPass(compiledPass);
                    break;
                case DeferredScenePassExecutionKind::Lighting:
                    addLightingPass(compiledPass, outputState, gBufferPassData);
                    break;
                default:
                    break;
                }
            }
        }

        DeferredGraphSceneResources ImportDeferredSceneGraphResources(
            const DeferredGraphSceneResourceRequest& request)
        {
            DeferredGraphSceneResources resources;
            if (request.frameGraph == nullptr || request.frameDescriptor == nullptr)
                return resources;

            if (request.blackboard != nullptr)
            {
                ImportSceneRenderTargets(
                    *request.frameGraph,
                    *request.blackboard,
                    *request.frameDescriptor,
                    request.outputColorResourceName,
                    request.outputDepthResourceName);
                resources.sceneTargets = ResolveImportedSceneRenderTargets(*request.blackboard);
            }

            if (request.preparedResources.gBuffer == nullptr)
                return resources;

            const auto width = request.frameDescriptor->renderWidth;
            const auto height = request.frameDescriptor->renderHeight;
            const auto& colorHandles = request.preparedResources.gBuffer->GetExplicitColorTextureHandles();

            if (request.preparedResources.gbufferAlbedoTexture != nullptr &&
                colorHandles.size() > 0u &&
                colorHandles[0] != nullptr)
            {
                resources.gbufferAlbedo = request.frameGraph->import<NLS::Render::FrameGraph::FrameGraphTexture>(
                    request.gbufferAlbedoResourceName,
                    MakeGBufferColorDesc(width, height, "GBufferAlbedo"),
                    NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
                        colorHandles[0],
                        request.preparedResources.gBuffer->GetOrCreateExplicitColorView(0, request.gbufferAlbedoViewName)));
            }

            if (request.preparedResources.gbufferNormalTexture != nullptr &&
                colorHandles.size() > 1u &&
                colorHandles[1] != nullptr)
            {
                resources.gbufferNormal = request.frameGraph->import<NLS::Render::FrameGraph::FrameGraphTexture>(
                    request.gbufferNormalResourceName,
                    MakeGBufferColorDesc(width, height, "GBufferNormal"),
                    NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
                        colorHandles[1],
                        request.preparedResources.gBuffer->GetOrCreateExplicitColorView(1, request.gbufferNormalViewName)));
            }

            if (request.preparedResources.gbufferMaterialTexture != nullptr &&
                colorHandles.size() > 2u &&
                colorHandles[2] != nullptr)
            {
                resources.gbufferMaterial = request.frameGraph->import<NLS::Render::FrameGraph::FrameGraphTexture>(
                    request.gbufferMaterialResourceName,
                    MakeGBufferColorDesc(width, height, "GBufferMaterial"),
                    NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
                        colorHandles[2],
                        request.preparedResources.gBuffer->GetOrCreateExplicitColorView(2, request.gbufferMaterialViewName)));
            }

            const auto depthHandle = request.preparedResources.gBuffer->GetExplicitDepthTextureHandle();
            if (request.preparedResources.gbufferDepthTexture != nullptr && depthHandle != nullptr)
            {
                resources.gbufferDepth = request.frameGraph->import<NLS::Render::FrameGraph::FrameGraphTexture>(
                    request.gbufferDepthResourceName,
                    MakeGBufferDepthDesc(width, height),
                    NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
                        depthHandle,
                        request.preparedResources.gBuffer->GetOrCreateExplicitDepthView(request.gbufferDepthViewName)));
            }

            return resources;
        }

        NLS::Render::Context::RenderPassCommandInput BuildDeferredScenePassInput(
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            const NLS::Render::Context::RenderScenePackage& package,
            const PreparedComputeDispatchSource& preparedComputeSource,
            const uint64_t opaqueDrawCount,
            const uint64_t lightingDrawCount,
            const std::vector<NLS::Render::Context::RecordedDrawCommandInput>& recordedDrawCommands,
            const DeferredPreparedSceneResources& resources)
        {
            NLS::Render::Context::RenderPassCommandInput passInput;
            if (TryBuildPreparedComputeDispatchThreadedPassInput(
                preparedComputeSource,
                compiledPass,
                passInput))
            {
                return passInput;
            }

            passInput = MakeCompiledThreadedRenderPassCommandInput(compiledPass);
            switch (GetDeferredScenePassExecutionKind(compiledPass.metadata.commandKind))
            {
            case DeferredScenePassExecutionKind::GBuffer:
                passInput.drawCount = opaqueDrawCount;
                if (recordedDrawCommands.size() >= opaqueDrawCount)
                {
                    passInput.recordedDrawCommands.assign(
                        recordedDrawCommands.begin(),
                        recordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(opaqueDrawCount));
                }
                passInput.requiresFrameData = true;
                passInput.requiresObjectData = true;
                passInput.targetsSwapchain = false;
                passInput.usesColorAttachment = true;
                passInput.usesDepthStencilAttachment = true;
                passInput.gbufferTextures = resources.gbufferTextures;
                passInput.colorAttachmentViews = resources.gbufferColorViews;
                passInput.depthStencilAttachmentView = resources.gbufferDepthView;
                for (const auto& colorView : resources.gbufferColorViews)
                {
                    const auto& texture = colorView != nullptr ? colorView->GetTexture() : nullptr;
                    AddFullTextureResourceAccess(
                        passInput,
                        texture,
                        NLS::Render::Context::ResourceAccessMode::Write,
                        NLS::Render::RHI::ResourceState::RenderTarget,
                        NLS::Render::RHI::PipelineStageMask::AllCommands,
                        NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite);
                }
                if (resources.gbufferDepthView != nullptr &&
                    resources.gbufferDepthView->GetTexture() != nullptr)
                {
                    AddFullTextureResourceAccess(
                        passInput,
                        resources.gbufferDepthView->GetTexture(),
                        NLS::Render::Context::ResourceAccessMode::Write,
                        NLS::Render::RHI::ResourceState::DepthWrite,
                        NLS::Render::RHI::PipelineStageMask::AllCommands,
                        NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite);
                }
                break;
            case DeferredScenePassExecutionKind::Lighting:
                passInput.drawCount = lightingDrawCount;
                if (recordedDrawCommands.size() > opaqueDrawCount && lightingDrawCount > 0u)
                {
                    const auto lightingBegin =
                        recordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(opaqueDrawCount);
                    const auto lightingEndOffset = static_cast<size_t>(
                        std::min<uint64_t>(
                            opaqueDrawCount + lightingDrawCount,
                            static_cast<uint64_t>(recordedDrawCommands.size())));
                    passInput.recordedDrawCommands.assign(
                        lightingBegin,
                        recordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(lightingEndOffset));
                }
                passInput.requiresFrameData = true;
                passInput.requiresLightingData = true;
                passInput.targetsSwapchain = package.targetsSwapchain;
                passInput.usesColorAttachment = true;
                passInput.usesDepthStencilAttachment = false;
                passInput.gbufferTextures = resources.gbufferTextures;
                for (const auto& texture : resources.gbufferTextures)
                {
                    AddFullTextureResourceAccess(
                        passInput,
                        texture,
                        NLS::Render::Context::ResourceAccessMode::Read,
                        NLS::Render::RHI::ResourceState::ShaderRead,
                        NLS::Render::RHI::PipelineStageMask::FragmentShader | NLS::Render::RHI::PipelineStageMask::ComputeShader,
                        NLS::Render::RHI::AccessMask::ShaderRead);
                }
                break;
            default:
                break;
            }

            return passInput;
        }

        std::vector<NLS::Render::Context::RenderPassCommandInput> BuildDeferredScenePassInputs(
            const std::vector<CompiledThreadedRenderSceneGraphPass>& compiledPasses,
            const NLS::Render::Context::RenderScenePackage& package,
            const PreparedComputeDispatchSource& preparedComputeSource,
            const uint64_t opaqueDrawCount,
            const uint64_t lightingDrawCount,
            const std::vector<NLS::Render::Context::RecordedDrawCommandInput>& recordedDrawCommands,
            const DeferredPreparedSceneResources& resources)
        {
            std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;
            passInputs.reserve(compiledPasses.size());

            for (const auto& compiledPass : compiledPasses)
            {
                passInputs.push_back(BuildDeferredScenePassInput(
                    compiledPass,
                    package,
                    preparedComputeSource,
                    opaqueDrawCount,
                    lightingDrawCount,
                    recordedDrawCommands,
                    resources));
            }

            return passInputs;
        }

        std::optional<NLS::Render::Context::RenderPassCommandInput> BuildDeferredAggregateHelperPassInput(
            const NLS::Render::Context::RenderScenePackage& package,
            const uint64_t opaqueDrawCount,
            const uint64_t lightingDrawCount)
        {
            const auto helperStart = static_cast<size_t>(
                std::min<uint64_t>(
                    opaqueDrawCount + lightingDrawCount,
                    static_cast<uint64_t>(package.recordedDrawCommands.size())));
            const auto helperDrawCount =
                package.recordedDrawCommands.size() > helperStart
                    ? package.recordedDrawCommands.size() - helperStart
                    : 0u;
            if (helperDrawCount == 0u)
                return std::nullopt;

            NLS::Render::Context::RenderPassCommandInput passInput;
            passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
            passInput.debugName = "EditorHelperPass";
            passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
            passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            passInput.drawCount = static_cast<uint64_t>(helperDrawCount);
            passInput.requiresFrameData = true;
            passInput.requiresObjectData = true;
            passInput.targetsSwapchain = package.targetsSwapchain;
            passInput.renderWidth = package.renderWidth;
            passInput.renderHeight = package.renderHeight;
            passInput.usesColorAttachment = true;
            passInput.usesDepthStencilAttachment = true;
            passInput.recordedDrawCommands.assign(
                package.recordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(helperStart),
                package.recordedDrawCommands.end());
            return passInput;
        }

        uint64_t ResolveDeferredLightingDrawCount(
            const NLS::Render::Context::RenderScenePackage& package,
            const uint64_t opaqueDrawCount,
            const std::vector<NLS::Render::Context::RenderPassCommandInput>& appendedPassInputs,
            const std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata>& appendedPassMetadata,
            const std::optional<uint64_t> queuedLightingDrawCount = std::nullopt)
        {
            if (queuedLightingDrawCount.has_value())
            {
                const auto recordedDrawCount = static_cast<uint64_t>(package.recordedDrawCommands.size());
                if (recordedDrawCount <= opaqueDrawCount)
                    return 0u;

                return std::min<uint64_t>(*queuedLightingDrawCount, recordedDrawCount - opaqueDrawCount);
            }

            const auto recordedDrawCount = static_cast<uint64_t>(package.recordedDrawCommands.size());
            if (recordedDrawCount <= opaqueDrawCount)
                return 0u;

            const auto drawsAfterOpaque = recordedDrawCount - opaqueDrawCount;
            uint64_t explicitAppendedHelperContribution = 0u;
            for (const auto& metadata : appendedPassMetadata)
            {
                if (metadata.commandKind != NLS::Render::Context::RenderPassCommandKind::Helper ||
                    metadata.role != NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper)
                {
                    continue;
                }

                const auto metadataName = metadata.graphPassName != nullptr
                    ? std::string_view(metadata.graphPassName)
                    : std::string_view{};
                const auto appendedInput = std::find_if(
                    appendedPassInputs.begin(),
                    appendedPassInputs.end(),
                    [metadataName](const NLS::Render::Context::RenderPassCommandInput& input)
                    {
                        return metadataName.empty() || input.debugName == metadataName;
                    });
                if (appendedInput == appendedPassInputs.end())
                    continue;

                explicitAppendedHelperContribution +=
                    metadata.visibleDrawCountContribution == NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount
                        ? appendedInput->drawCount
                        : metadata.visibleDrawCountContribution;
            }

            const auto aggregateHelperDrawCount =
                package.helperDrawCount > explicitAppendedHelperContribution
                    ? package.helperDrawCount - explicitAppendedHelperContribution
                    : 0u;
            if (aggregateHelperDrawCount >= drawsAfterOpaque)
                return 0u;

            return 1u;
        }

        void AppendDeferredHelperPassInputForMetadata(
            std::vector<NLS::Render::Context::RenderPassCommandInput>& passInputs,
            const NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata& metadata,
            const std::vector<NLS::Render::Context::RenderPassCommandInput>& appendedPassInputs,
            const std::optional<NLS::Render::Context::RenderPassCommandInput>& aggregateHelperPassInput)
        {
            if (metadata.commandKind != NLS::Render::Context::RenderPassCommandKind::Helper)
                return;

            const auto metadataName = metadata.graphPassName != nullptr
                ? std::string_view(metadata.graphPassName)
                : std::string_view{};
            for (const auto& appendedPassInput : appendedPassInputs)
            {
                if (!metadataName.empty() && appendedPassInput.debugName != metadataName)
                    continue;

                passInputs.push_back(appendedPassInput);
                return;
            }

            if (aggregateHelperPassInput.has_value() &&
                (metadataName.empty() || aggregateHelperPassInput->debugName == metadataName))
            {
                passInputs.push_back(*aggregateHelperPassInput);
            }
        }
    }

    DeferredScenePassExecutionKind GetDeferredScenePassExecutionKind(
        NLS::Render::Context::RenderPassCommandKind kind)
    {
        switch (kind)
        {
        case NLS::Render::Context::RenderPassCommandKind::GBuffer:
            return DeferredScenePassExecutionKind::GBuffer;
        case NLS::Render::Context::RenderPassCommandKind::Lighting:
            return DeferredScenePassExecutionKind::Lighting;
        default:
            return DeferredScenePassExecutionKind::Unknown;
        }
    }

    DeferredPreparedSceneResources CaptureDeferredPreparedSceneResources(
        const DeferredPreparedSceneResourceRequest& request)
    {
        NLS_PROFILE_SCOPE();
        DeferredPreparedSceneResources resources;
        if (request.gBuffer == nullptr)
            return resources;
        if (request.gbufferAlbedoTexture != nullptr)
            resources.gbufferColorViews.push_back(request.gBuffer->GetOrCreateExplicitColorView(0, "GBufferAlbedoView"));
        if (request.gbufferNormalTexture != nullptr)
            resources.gbufferColorViews.push_back(request.gBuffer->GetOrCreateExplicitColorView(1, "GBufferNormalView"));
        if (request.gbufferMaterialTexture != nullptr)
            resources.gbufferColorViews.push_back(request.gBuffer->GetOrCreateExplicitColorView(2, "GBufferMaterialView"));
        if (request.gbufferDepthTexture != nullptr)
            resources.gbufferDepthView = request.gBuffer->GetOrCreateExplicitDepthView("GBufferDepthView");

        if (request.gbufferAlbedoTexture != nullptr && request.gbufferAlbedoTexture->GetExplicitRHITextureHandle())
            resources.gbufferTextures.push_back(request.gbufferAlbedoTexture->GetExplicitRHITextureHandle());
        if (request.gbufferNormalTexture != nullptr && request.gbufferNormalTexture->GetExplicitRHITextureHandle())
            resources.gbufferTextures.push_back(request.gbufferNormalTexture->GetExplicitRHITextureHandle());
        if (request.gbufferMaterialTexture != nullptr && request.gbufferMaterialTexture->GetExplicitRHITextureHandle())
            resources.gbufferTextures.push_back(request.gbufferMaterialTexture->GetExplicitRHITextureHandle());
        if (request.gbufferDepthTexture != nullptr && request.gbufferDepthTexture->GetExplicitRHITextureHandle())
            resources.gbufferTextures.push_back(request.gbufferDepthTexture->GetExplicitRHITextureHandle());
        return resources;
    }

    DeferredGraphSceneResourceRequest BuildDeferredGraphSceneResourceRequest(
        ::FrameGraph& frameGraph,
        FrameGraphBlackboard& blackboard,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const DeferredPreparedSceneResourceRequest& preparedResources)
    {
        DeferredGraphSceneResourceRequest request;
        request.frameGraph = &frameGraph;
        request.blackboard = &blackboard;
        request.frameDescriptor = &frameDescriptor;
        request.preparedResources = preparedResources;
        return request;
    }

    std::array<DeferredScenePassDescriptor, 2> GetDeferredScenePassDescriptors()
    {
        return { {
            {
                {
                    NLS::Render::Context::RenderPassCommandKind::GBuffer,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded,
                    NLS::Render::RHI::QueueType::Graphics,
                    NLS::Render::Context::QueueDependencyPolicy::Previous,
                    "DeferredGBuffer",
                    NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount,
                    false,
                    false,
                    {
                        false,
                        true,
                        true,
                        true,
                        NLS::Maths::Vector4{ 0.0f, 0.0f, 0.0f, 1.0f }
                    }
                }
            },
            {
                {
                    NLS::Render::Context::RenderPassCommandKind::Lighting,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Output,
                    NLS::Render::RHI::QueueType::Graphics,
                    NLS::Render::Context::QueueDependencyPolicy::Previous,
                    "DeferredLighting",
                    1u,
                    true,
                    true,
                    {
                        true,
                        false,
                        false,
                        false,
                        NLS::Maths::Vector4::Zero
                    }
                }
            }
        } };
    }

    void ReserveDeferredSceneGraph(
        ::FrameGraph& frameGraph,
        const DeferredGraphSceneResourceRequest& resourceRequest)
    {
        NLS_PROFILE_SCOPE();
        const bool hasExternalOutput =
            resourceRequest.frameDescriptor != nullptr &&
            ResolveExternalSceneOutputFramebuffer(*resourceRequest.frameDescriptor) != nullptr;
        frameGraph.reserve(2, hasExternalOutput ? 6 : 4);
    }

    PreparedDeferredSceneGraph PrepareDeferredSceneGraph(
        const DeferredGraphSceneResourceRequest& resourceRequest,
        const LightGridCompileContext& lightGridContext)
    {
        NLS_PROFILE_SCOPE();
        PreparedDeferredSceneGraph preparedGraph;
        preparedGraph.resources = ImportDeferredSceneGraphResources(resourceRequest);
        preparedGraph.execution = Detail::CompilePreparedSceneGraphExecution(
            lightGridContext,
            preparedGraph.resources.sceneTargets.color,
            preparedGraph.resources.sceneTargets.depth,
            GetDeferredScenePassDescriptors());
        return preparedGraph;
    }

    void ExecutePreparedDeferredSceneGraph(
        ::FrameGraph& frameGraph,
        const PreparedDeferredSceneGraph& preparedGraph,
        const DeferredSceneGraphExecutionCallbacks& callbacks)
    {
        NLS_PROFILE_SCOPE();
        const auto capturedCallbacks = callbacks;
        const auto addCompiledGBufferPass = [&frameGraph, &preparedGraph, capturedCallbacks](
            const CompiledThreadedRenderSceneGraphPass& compiledPass) -> const DeferredGBufferGraphPassData&
        {
            return AddDeferredCompiledGBufferPass(
                frameGraph,
                compiledPass,
                preparedGraph.resources.gbufferAlbedo,
                preparedGraph.resources.gbufferNormal,
                preparedGraph.resources.gbufferMaterial,
                preparedGraph.resources.gbufferDepth,
                [capturedCallbacks](const DeferredGBufferGraphPassData&, FrameGraphPassResources&, void*, const auto& desc)
                {
                    ExecuteRecordedRenderPass(
                        desc,
                        [&capturedCallbacks](const auto& beginDesc) -> bool
                        {
                            return capturedCallbacks.beginGBufferPass ? capturedCallbacks.beginGBufferPass(beginDesc) : false;
                        },
                        [&capturedCallbacks]()
                        {
                            if (capturedCallbacks.executeGBufferPass)
                                capturedCallbacks.executeGBufferPass();
                        },
                        [&capturedCallbacks]()
                        {
                            if (capturedCallbacks.endGBufferPass)
                                capturedCallbacks.endGBufferPass();
                        });
                });
        };
        const auto addCompiledLightingPass = [&frameGraph, capturedCallbacks](
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            ScenePassOutputResourceState& outputState,
            const DeferredGBufferGraphPassData* gBufferPassData)
        {
            AddDeferredCompiledLightingPass(
                frameGraph,
                compiledPass,
                outputState,
                gBufferPassData,
                [capturedCallbacks](const DeferredLightingGraphPassData&, FrameGraphPassResources&, void*, const auto& desc)
                {
                    ExecuteOutputRenderPass(
                        desc,
                        [&capturedCallbacks](const auto& beginDesc) -> bool
                        {
                            return capturedCallbacks.beginLightingPass ? capturedCallbacks.beginLightingPass(beginDesc) : false;
                        },
                        [&capturedCallbacks]()
                        {
                            if (capturedCallbacks.executeLightingPass)
                                capturedCallbacks.executeLightingPass();
                        },
                        [&capturedCallbacks](bool startedRenderPass, const auto& endDesc)
                        {
                            if (capturedCallbacks.endLightingPass)
                                capturedCallbacks.endLightingPass(startedRenderPass, endDesc);
                        });
                });
        };
        DispatchDeferredCompiledGraphPasses(
            frameGraph,
            preparedGraph.execution.preparedComputeSource,
            preparedGraph.execution.compiledExecution.graphPasses,
            addCompiledGBufferPass,
            addCompiledLightingPass);
    }

    CompiledThreadedRenderSceneExecution CompileAndApplyPreparedDeferredLightGridSceneExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridCompileContext& lightGridContext,
        const DeferredPreparedSceneResources& resources,
        const std::vector<NLS::Render::Context::RenderPassCommandInput>& appendedPassInputs,
        const std::vector<ThreadedRenderScenePassMetadata>& appendedPassMetadata,
        const std::optional<uint64_t> queuedLightingDrawCount)
    {
        NLS_PROFILE_SCOPE();
        const auto opaqueDrawCount = package.opaqueDrawCount;
        const auto lightingDrawCount = ResolveDeferredLightingDrawCount(
            package,
            opaqueDrawCount,
            appendedPassInputs,
            appendedPassMetadata,
            queuedLightingDrawCount);
        std::vector<ThreadedRenderScenePassMetadata> scenePassMetadata;
        const auto deferredPassDescriptors = GetDeferredScenePassDescriptors();
        scenePassMetadata.reserve(deferredPassDescriptors.size() + appendedPassMetadata.size());
        for (const auto& descriptor : deferredPassDescriptors)
            scenePassMetadata.push_back(descriptor.metadata);
        scenePassMetadata.insert(
            scenePassMetadata.end(),
            appendedPassMetadata.begin(),
            appendedPassMetadata.end());
        scenePassMetadata.erase(
            std::remove_if(
                scenePassMetadata.begin() + static_cast<std::ptrdiff_t>(deferredPassDescriptors.size()),
                scenePassMetadata.end(),
                [](const ThreadedRenderScenePassMetadata& metadata)
                {
                    return IsDeferredScenePassKind(metadata.commandKind);
                }),
            scenePassMetadata.end());

        return CompileAndApplyThreadedRenderSceneExecution(
            package,
            lightGridContext.frameDescriptor,
            -1,
            -1,
            [&lightGridContext]()
            {
                return Detail::BuildPreparedLightGridDispatchSource(lightGridContext);
            },
            [&lightGridContext](NLS::Render::Context::RenderScenePackage& scenePackage)
            {
                Detail::ResolvePreparedLightGridPassBindings(scenePackage, lightGridContext);
            },
            scenePassMetadata,
            [&package, &resources, &appendedPassInputs, &appendedPassMetadata, &lightGridContext, opaqueDrawCount, lightingDrawCount](const auto& lightGridComputeSource, const auto& compiledPasses)
            {
                std::vector<CompiledThreadedRenderSceneGraphPass> deferredCompiledPasses;
                deferredCompiledPasses.reserve(2u);
                for (const auto& compiledPass : compiledPasses)
                {
                    if (GetDeferredScenePassExecutionKind(compiledPass.metadata.commandKind) != DeferredScenePassExecutionKind::Unknown)
                        deferredCompiledPasses.push_back(compiledPass);
                }
                auto passInputs = BuildDeferredScenePassInputs(
                    deferredCompiledPasses,
                    package,
                    lightGridComputeSource,
                    opaqueDrawCount,
                    lightingDrawCount,
                    package.recordedDrawCommands,
                    resources);
                const auto aggregateHelperPassInput =
                    BuildDeferredAggregateHelperPassInput(package, opaqueDrawCount, lightingDrawCount);
                for (const auto& metadata : appendedPassMetadata)
                {
                    AppendDeferredHelperPassInputForMetadata(
                        passInputs,
                        metadata,
                        appendedPassInputs,
                        aggregateHelperPassInput);
                }
                for (auto& passInput : passInputs)
                    AttachDeferredGBufferDepthForHelperPasses(passInput, resources);
                if (!package.targetsSwapchain)
                {
                    ApplyExternalSceneOutputAttachments(
                        passInputs,
                        ResolveExternalSceneOutputAttachments(
                            lightGridContext.frameDescriptor,
                            "DeferredOutputColorView",
                            "DeferredOutputDepthView"),
                        {
                            NLS::Render::Context::RenderPassCommandKind::Lighting,
                            NLS::Render::Context::RenderPassCommandKind::Helper
                        });
                    for (auto& passInput : passInputs)
                        AddColorAttachmentWriteResourceAccesses(passInput);
                }
                return passInputs;
            });
    }

    void FinalizePreparedDeferredScenePackage(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor)
    {
        NLS_PROFILE_SCOPE();
        ApplyExternalSceneOutputAttachments(
            package,
            frameDescriptor,
            "DeferredOutputColorView",
            "DeferredOutputDepthView",
            {
                NLS::Render::Context::RenderPassCommandKind::Lighting
            });
        RegisterExternalSceneOutputExtractions(package, frameDescriptor);
    }
}
