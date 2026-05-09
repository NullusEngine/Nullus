#include "Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h"

#include <utility>

#include "Profiling/Profiler.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderInternal.h"

namespace NLS::Render::FrameGraph
{
    namespace
    {
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

                if (outputState.color < 0 && compiledPass.outputChain.color >= 0)
                    outputState.color = compiledPass.outputChain.color;
                if (outputState.depth < 0 && compiledPass.outputChain.depth >= 0)
                    outputState.depth = compiledPass.outputChain.depth;

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
                for (const auto& texture : resources.gbufferTextures)
                {
                    if (texture == nullptr)
                        continue;

                    NLS::Render::RHI::RHISubresourceRange fullRange;
                    fullRange.baseMipLevel = 0u;
                    fullRange.mipLevelCount = texture->GetDesc().mipLevels;
                    fullRange.baseArrayLayer = 0u;
                    fullRange.arrayLayerCount = texture->GetDesc().arrayLayers;
                    passInput.textureResourceAccesses.push_back({
                        texture,
                        fullRange,
                        NLS::Render::Context::ResourceAccessMode::Write,
                        NLS::Render::RHI::ResourceState::RenderTarget,
                        NLS::Render::RHI::PipelineStageMask::AllCommands,
                        NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite
                    });
                }
                break;
            case DeferredScenePassExecutionKind::Lighting:
                passInput.drawCount = 1u;
                if (recordedDrawCommands.size() > opaqueDrawCount)
                {
                    passInput.recordedDrawCommands.assign(
                        recordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(opaqueDrawCount),
                        recordedDrawCommands.end());
                }
                passInput.requiresFrameData = true;
                passInput.requiresLightingData = true;
                passInput.targetsSwapchain = package.targetsSwapchain;
                passInput.usesColorAttachment = true;
                passInput.usesDepthStencilAttachment = false;
                passInput.gbufferTextures = resources.gbufferTextures;
                for (const auto& texture : resources.gbufferTextures)
                {
                    if (texture == nullptr)
                        continue;

                    NLS::Render::RHI::RHISubresourceRange fullRange;
                    fullRange.baseMipLevel = 0u;
                    fullRange.mipLevelCount = texture->GetDesc().mipLevels;
                    fullRange.baseArrayLayer = 0u;
                    fullRange.arrayLayerCount = texture->GetDesc().arrayLayers;
                    passInput.textureResourceAccesses.push_back({
                        texture,
                        fullRange,
                        NLS::Render::Context::ResourceAccessMode::Read,
                        NLS::Render::RHI::ResourceState::ShaderRead,
                        NLS::Render::RHI::PipelineStageMask::FragmentShader | NLS::Render::RHI::PipelineStageMask::ComputeShader,
                        NLS::Render::RHI::AccessMask::ShaderRead
                    });
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
                    recordedDrawCommands,
                    resources));
            }

            return passInputs;
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
        const auto addCompiledGBufferPass = [&frameGraph, &preparedGraph, &callbacks](
            const CompiledThreadedRenderSceneGraphPass& compiledPass) -> const DeferredGBufferGraphPassData&
        {
            return AddDeferredCompiledGBufferPass(
                frameGraph,
                compiledPass,
                preparedGraph.resources.gbufferAlbedo,
                preparedGraph.resources.gbufferNormal,
                preparedGraph.resources.gbufferMaterial,
                preparedGraph.resources.gbufferDepth,
                [&callbacks](const DeferredGBufferGraphPassData&, FrameGraphPassResources&, void*, const auto& desc)
                {
                    ExecuteRecordedRenderPass(
                        desc,
                        [&callbacks](const auto& beginDesc) -> bool
                        {
                            return callbacks.beginGBufferPass ? callbacks.beginGBufferPass(beginDesc) : false;
                        },
                        [&callbacks]()
                        {
                            if (callbacks.executeGBufferPass)
                                callbacks.executeGBufferPass();
                        },
                        [&callbacks]()
                        {
                            if (callbacks.endGBufferPass)
                                callbacks.endGBufferPass();
                        });
                });
        };
        const auto addCompiledLightingPass = [&frameGraph, &callbacks](
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            ScenePassOutputResourceState& outputState,
            const DeferredGBufferGraphPassData* gBufferPassData)
        {
            AddDeferredCompiledLightingPass(
                frameGraph,
                compiledPass,
                outputState,
                gBufferPassData,
                [&callbacks](const DeferredLightingGraphPassData&, FrameGraphPassResources&, void*, const auto& desc)
                {
                    ExecuteOutputRenderPass(
                        desc,
                        [&callbacks](const auto& beginDesc) -> bool
                        {
                            return callbacks.beginLightingPass ? callbacks.beginLightingPass(beginDesc) : false;
                        },
                        [&callbacks]()
                        {
                            if (callbacks.executeLightingPass)
                                callbacks.executeLightingPass();
                        },
                        [&callbacks](bool startedRenderPass, const auto& endDesc)
                        {
                            if (callbacks.endLightingPass)
                                callbacks.endLightingPass(startedRenderPass, endDesc);
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
        const DeferredPreparedSceneResources& resources)
    {
        NLS_PROFILE_SCOPE();
        const auto opaqueDrawCount = package.opaqueDrawCount;
        const auto recordedDrawCommands = package.recordedDrawCommands;
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
            GetDeferredScenePassDescriptors(),
            [&package, &resources, opaqueDrawCount, &recordedDrawCommands](const auto& lightGridComputeSource, const auto& compiledPasses)
            {
                return BuildDeferredScenePassInputs(
                    compiledPasses,
                    package,
                    lightGridComputeSource,
                    opaqueDrawCount,
                    recordedDrawCommands,
                    resources);
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
