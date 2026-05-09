#pragma once

#include <memory>

#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"

namespace NLS::Render::FrameGraph
{
    struct LightGridCompileContext
    {
        NLS::Render::Data::FrameDescriptor frameDescriptor{};
        PreparedComputeDispatchSource preparedComputeSource;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> graphicsPassBindingSet;
    };

    struct PreparedSceneGraphExecution
    {
        PreparedComputeDispatchSource preparedComputeSource;
        CompiledThreadedRenderSceneExecution compiledExecution;
    };

    NLS_RENDER_API LightGridCompileContext BuildLightGridCompileContext(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        PreparedComputeDispatchSource preparedComputeSource,
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> graphicsPassBindingSet);
}
