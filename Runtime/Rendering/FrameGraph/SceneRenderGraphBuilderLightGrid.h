#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Rendering/FrameGraph/FrameGraphExecutionTypes.h"

namespace NLS::Render::FrameGraph
{
    struct ForwardLightingResources
    {
        std::string forwardLightDataUniformBufferName;
        std::string forwardLocalLightBufferName;
        std::string numCulledLightsGridName;
        std::string culledLightDataGridName;
        uint32_t numCulledLightsGridStride = 0u;
        uint32_t lightLinkStride = 0u;
        uint32_t lightGridInjectionGroupSize = 0u;
    };

    struct LightGridCompileContext
    {
        NLS::Render::Data::FrameDescriptor frameDescriptor{};
        PreparedComputeDispatchSource preparedComputeSource;
        ForwardLightingResources forwardLightingResources;
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
