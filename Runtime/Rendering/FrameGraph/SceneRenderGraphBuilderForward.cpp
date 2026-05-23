#include "Rendering/FrameGraph/SceneRenderGraphBuilderForward.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "Profiling/Profiler.h"
#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderInternal.h"

namespace NLS::Render::FrameGraph
{
    namespace
    {
        struct ForwardGraphPassData
        {
            FrameGraphResource color = -1;
            FrameGraphResource depth = -1;
        };

        std::vector<NLS::Render::Context::RenderPassCommandInput> BuildForwardScenePassInputs(
            const NLS::Render::Context::RenderScenePackage& package)
        {
            std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;
            passInputs.reserve(GetForwardScenePassDescriptors().size());

            size_t nextRecordedDrawCommandIndex = 0u;
            for (const auto& descriptor : GetForwardScenePassDescriptors())
            {
                const auto& metadata = descriptor.metadata;
                uint64_t drawCount = 0u;
                switch (metadata.commandKind)
                {
                case NLS::Render::Context::RenderPassCommandKind::Opaque:
                    drawCount = package.opaqueDrawCount;
                    break;
                case NLS::Render::Context::RenderPassCommandKind::Skybox:
                    drawCount = package.skyboxDrawCount;
                    break;
                case NLS::Render::Context::RenderPassCommandKind::Transparent:
                    drawCount = package.transparentDrawCount;
                    break;
                default:
                    break;
                }

                if (drawCount == 0u)
                    continue;

                NLS::Render::Context::RenderPassCommandInput input;
                input.kind = metadata.commandKind;
                input.drawCount = drawCount;
                input.requiresFrameData = true;
                input.requiresObjectData = true;
                input.targetsSwapchain = package.targetsSwapchain;
                input.renderWidth = package.renderWidth;
                input.renderHeight = package.renderHeight;
                input.clearColorValue = package.clearColorValue;
                input.clearColor = metadata.commandKind == NLS::Render::Context::RenderPassCommandKind::Opaque && package.clearColorBuffer;
                input.clearDepth = metadata.commandKind == NLS::Render::Context::RenderPassCommandKind::Opaque && package.clearDepthBuffer;
                input.clearStencil = metadata.commandKind == NLS::Render::Context::RenderPassCommandKind::Opaque && package.clearStencilBuffer;
                input.usesColorAttachment = true;
                input.usesDepthStencilAttachment = true;

                if (!package.recordedDrawCommands.empty() &&
                    nextRecordedDrawCommandIndex < package.recordedDrawCommands.size())
                {
                    const auto availableDrawCount = package.recordedDrawCommands.size() - nextRecordedDrawCommandIndex;
                    const auto copiedDrawCount = std::min<size_t>(static_cast<size_t>(drawCount), availableDrawCount);
                    input.recordedDrawCommands.insert(
                        input.recordedDrawCommands.end(),
                        package.recordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(nextRecordedDrawCommandIndex),
                        package.recordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(nextRecordedDrawCommandIndex + copiedDrawCount));
                    nextRecordedDrawCommandIndex += copiedDrawCount;
                }

                passInputs.push_back(std::move(input));
            }

            if (std::none_of(
                passInputs.begin(),
                passInputs.end(),
                [](const NLS::Render::Context::RenderPassCommandInput& input)
                {
                    return input.clearColor || input.clearDepth || input.clearStencil;
                }))
            {
                const auto firstOutputPass = std::find_if(
                    passInputs.begin(),
                    passInputs.end(),
                    [](const NLS::Render::Context::RenderPassCommandInput& input)
                    {
                        return input.usesColorAttachment || input.usesDepthStencilAttachment;
                    });
                if (firstOutputPass != passInputs.end())
                {
                    firstOutputPass->clearColor = firstOutputPass->usesColorAttachment && package.clearColorBuffer;
                    firstOutputPass->clearDepth = firstOutputPass->usesDepthStencilAttachment && package.clearDepthBuffer;
                    firstOutputPass->clearStencil = firstOutputPass->usesDepthStencilAttachment && package.clearStencilBuffer;
                    firstOutputPass->clearColorValue = package.clearColorValue;
                }
            }

            return passInputs;
        }

        template<typename TExecuteFn>
        void AddForwardCompiledScenePass(
            ::FrameGraph& frameGraph,
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            ScenePassOutputResourceState& outputState,
            TExecuteFn&& execute)
        {
            AddExecutingSceneOutputPass<
                ForwardGraphPassData,
                &ForwardGraphPassData::color,
                &ForwardGraphPassData::depth>(
                frameGraph,
                compiledPass,
                outputState,
                [execute = std::forward<TExecuteFn>(execute), compiledPass](
                    const ForwardGraphPassData& data,
                    FrameGraphPassResources& resources,
                    void* context,
                    const auto& desc)
                {
                    execute(compiledPass, data, resources, context, desc);
                });
        }

        template<typename TExecuteFn>
        void DispatchForwardCompiledGraphPasses(
            ::FrameGraph& frameGraph,
            const PreparedComputeDispatchSource& preparedComputeSource,
            const std::vector<CompiledThreadedRenderSceneGraphPass>& compiledPasses,
            TExecuteFn&& execute)
        {
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

                AddForwardCompiledScenePass(
                    frameGraph,
                    compiledPass,
                    outputState,
                    execute);
            }
        }
    }

    std::array<ForwardScenePassDescriptor, 3> GetForwardScenePassDescriptors()
    {
        return { {
            {
                {
                    NLS::Render::Context::RenderPassCommandKind::Opaque,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Output,
                    NLS::Render::RHI::QueueType::Graphics,
                    NLS::Render::Context::QueueDependencyPolicy::Previous,
                    "ForwardOpaque",
                    NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount,
                    true,
                    true,
                    {
                        true
                    }
                }
            },
            {
                {
                    NLS::Render::Context::RenderPassCommandKind::Skybox,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Skybox,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Output,
                    NLS::Render::RHI::QueueType::Graphics,
                    NLS::Render::Context::QueueDependencyPolicy::Previous,
                    "ForwardSkybox"
                }
            },
            {
                {
                    NLS::Render::Context::RenderPassCommandKind::Transparent,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Transparent,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Output,
                    NLS::Render::RHI::QueueType::Graphics,
                    NLS::Render::Context::QueueDependencyPolicy::Previous,
                    "ForwardTransparent"
                }
            }
        } };
    }

    void ReserveForwardSceneGraph(
        ::FrameGraph& frameGraph,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor)
    {
        NLS_PROFILE_SCOPE();
        frameGraph.reserve(
            3,
            !FrameTargetsSwapchain(frameDescriptor) ? 2 : 0);
    }

    PreparedForwardSceneGraph PrepareForwardSceneGraph(
        ::FrameGraph& frameGraph,
        FrameGraphBlackboard& blackboard,
        const LightGridCompileContext& lightGridContext,
        const char* colorResourceName,
        const char* depthResourceName)
    {
        NLS_PROFILE_SCOPE();
        PreparedForwardSceneGraph preparedGraph;
        ImportSceneRenderTargets(
            frameGraph,
            blackboard,
            lightGridContext.frameDescriptor,
            colorResourceName,
            depthResourceName);
        const auto importedTargets = ResolveImportedSceneRenderTargets(blackboard);
        preparedGraph.execution = Detail::CompilePreparedSceneGraphExecution(
            lightGridContext,
            importedTargets.color,
            importedTargets.depth,
            GetForwardScenePassDescriptors());
        return preparedGraph;
    }

    void ExecutePreparedForwardSceneGraph(
        ::FrameGraph& frameGraph,
        const PreparedForwardSceneGraph& preparedGraph,
        const ForwardSceneGraphExecutionCallbacks& callbacks)
    {
        NLS_PROFILE_SCOPE();
        const auto capturedCallbacks = callbacks;
        DispatchForwardCompiledGraphPasses(
            frameGraph,
            preparedGraph.execution.preparedComputeSource,
            preparedGraph.execution.compiledExecution.graphPasses,
            [capturedCallbacks](const auto& compiledPass, const ForwardGraphPassData&, FrameGraphPassResources&, void*, const auto& desc)
            {
                ExecuteOutputRenderPass(
                    desc,
                    [&capturedCallbacks](const auto& beginDesc) -> bool
                    {
                        return capturedCallbacks.beginOutputPass ? capturedCallbacks.beginOutputPass(beginDesc) : false;
                    },
                    [&capturedCallbacks, &compiledPass]()
                    {
                        if (capturedCallbacks.executePass)
                            capturedCallbacks.executePass(compiledPass);
                    },
                    [&capturedCallbacks](bool startedRenderPass, const auto& endDesc)
                    {
                        if (capturedCallbacks.endOutputPass)
                            capturedCallbacks.endOutputPass(startedRenderPass, endDesc);
                    });
            });
    }

    CompiledThreadedRenderSceneExecution CompileAndApplyPreparedForwardLightGridSceneExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridCompileContext& lightGridContext)
    {
        NLS_PROFILE_SCOPE();
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
            GetForwardScenePassDescriptors(),
            [&package](const auto& lightGridComputeSource, const auto& compiledPasses)
            {
                return BuildPreparedComputeAndScenePassInputs(
                    lightGridComputeSource,
                    compiledPasses,
                    BuildForwardScenePassInputs(package));
            });
    }

    void FinalizePreparedForwardScenePackage(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor)
    {
        NLS_PROFILE_SCOPE();
        if (!package.targetsSwapchain &&
            BuildExternalSceneOutputSummary(frameDescriptor).hasExternalOutput)
        {
            ApplyExternalSceneOutputAttachments(
                package,
                frameDescriptor,
                "ForwardOutputColorView",
                "ForwardOutputDepthView",
                {
                    NLS::Render::Context::RenderPassCommandKind::Opaque,
                    NLS::Render::Context::RenderPassCommandKind::Skybox,
                    NLS::Render::Context::RenderPassCommandKind::Transparent,
                    NLS::Render::Context::RenderPassCommandKind::Helper
                });
            RegisterExternalSceneOutputExtractions(package, frameDescriptor);
        }
    }

    ForwardScenePassExecutionKind GetForwardScenePassExecutionKind(
        NLS::Render::Context::RenderPassCommandKind kind)
    {
        switch (kind)
        {
        case NLS::Render::Context::RenderPassCommandKind::Opaque:
            return ForwardScenePassExecutionKind::Opaque;
        case NLS::Render::Context::RenderPassCommandKind::Skybox:
            return ForwardScenePassExecutionKind::Skybox;
        case NLS::Render::Context::RenderPassCommandKind::Transparent:
            return ForwardScenePassExecutionKind::Transparent;
        default:
            return ForwardScenePassExecutionKind::Unknown;
        }
    }

    ForwardScenePassPipelineKind GetForwardScenePassPipelineKind(
        NLS::Render::Context::RenderPassCommandKind kind)
    {
        return GetForwardScenePassExecutionKind(kind) == ForwardScenePassExecutionKind::Skybox
            ? ForwardScenePassPipelineKind::Skybox
            : ForwardScenePassPipelineKind::Default;
    }
}
