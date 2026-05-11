#pragma once

#include <utility>

#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"
#include "Rendering/LightGridPrepass.h"

namespace NLS::Engine::Rendering
{
    template<typename TMetadataRange, typename TMutatePackageFn, typename TBuildPassInputsFn>
    inline NLS::Render::FrameGraph::CompiledThreadedRenderSceneExecution CompileAndApplyPreparedLightGridThreadedExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridPrepass::PreparedComputeRequest& preparedComputeRequest,
        TMutatePackageFn&& mutatePackageForPreparedCompute,
        const TMetadataRange& scenePassMetadataRange,
        TBuildPassInputsFn&& buildPassInputs)
    {
        return NLS::Render::FrameGraph::CompileAndApplyThreadedRenderSceneExecution(
            package,
            preparedComputeRequest.frameDescriptor,
            -1,
            -1,
            [&]()
            {
                return LightGridPrepass::BuildPreparedComputeDispatchSource(preparedComputeRequest);
            },
            std::forward<TMutatePackageFn>(mutatePackageForPreparedCompute),
            scenePassMetadataRange,
            std::forward<TBuildPassInputsFn>(buildPassInputs));
    }

    template<typename TMetadataRange, typename TMutatePackageFn>
    inline NLS::Render::FrameGraph::CompiledThreadedRenderSceneExecution CompileAndApplyPreparedLightGridThreadedExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridPrepass::PreparedComputeRequest& preparedComputeRequest,
        TMutatePackageFn&& mutatePackageForPreparedCompute,
        const TMetadataRange& scenePassMetadataRange)
    {
        return NLS::Render::FrameGraph::CompileAndApplyThreadedRenderSceneExecution(
            package,
            preparedComputeRequest.frameDescriptor,
            -1,
            -1,
            [&]()
            {
                return LightGridPrepass::BuildPreparedComputeDispatchSource(preparedComputeRequest);
            },
            std::forward<TMutatePackageFn>(mutatePackageForPreparedCompute),
            scenePassMetadataRange);
    }
}
