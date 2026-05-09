#pragma once

#include <algorithm>
#include <vector>

#include "Profiling/Profiler.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderLightGrid.h"

namespace NLS::Render::FrameGraph::Detail
{
    inline bool IsPreparedPassBindingPlaceholder(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
    {
        return bindingSet != nullptr && bindingSet->GetDebugName() == "PreparedPassBindingPlaceholder";
    }

    inline void ResolvePreparedPassBindingPlaceholders(
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& drawCommands,
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet)
    {
        for (auto& drawCommand : drawCommands)
        {
            if (IsPreparedPassBindingPlaceholder(drawCommand.passBindingSet))
                drawCommand.passBindingSet = resolvedBindingSet;
        }
    }

    inline PreparedComputeDispatchSource BuildPreparedLightGridDispatchSource(
        const LightGridCompileContext& context)
    {
        return context.preparedComputeSource;
    }

    inline void ResolvePreparedLightGridPassBindings(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridCompileContext& context)
    {
        ResolvePreparedPassBindingPlaceholders(package.recordedDrawCommands, context.graphicsPassBindingSet);
        for (auto& passInput : package.passCommandInputs)
            ResolvePreparedPassBindingPlaceholders(passInput.recordedDrawCommands, context.graphicsPassBindingSet);
    }

    template<typename TMetadataRange>
    PreparedSceneGraphExecution CompilePreparedSceneGraphExecution(
        const LightGridCompileContext& lightGridContext,
        const int32_t importedColor,
        const int32_t importedDepth,
        const TMetadataRange& scenePassMetadataRange)
    {
        NLS_PROFILE_SCOPE();
        PreparedSceneGraphExecution execution;
        execution.preparedComputeSource = BuildPreparedLightGridDispatchSource(lightGridContext);
        execution.compiledExecution = CompileThreadedRenderSceneExecution(
            lightGridContext.frameDescriptor,
            importedColor,
            importedDepth,
            execution.preparedComputeSource,
            scenePassMetadataRange);
        return execution;
    }
}
