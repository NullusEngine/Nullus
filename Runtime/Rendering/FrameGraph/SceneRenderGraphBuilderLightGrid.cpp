#include "Rendering/FrameGraph/SceneRenderGraphBuilderLightGrid.h"

#include <utility>

namespace NLS::Render::FrameGraph
{
    namespace
    {
        constexpr uint32_t kForwardLightGridInjectionGroupSize = 4u;
        constexpr uint32_t kForwardNumCulledLightsGridStride = 2u;
        constexpr uint32_t kForwardLightLinkStride = 2u;

        ForwardLightingResources BuildForwardLightingResources()
        {
            ForwardLightingResources resources;
            resources.forwardLightDataUniformBufferName = "ForwardLightDataUniformBuffer";
            resources.forwardLocalLightBufferName = "ForwardLocalLightBuffer";
            resources.numCulledLightsGridName = "NumCulledLightsGrid";
            resources.culledLightDataGridName = "CulledLightDataGrid";
            resources.numCulledLightsGridStride = kForwardNumCulledLightsGridStride;
            resources.lightLinkStride = kForwardLightLinkStride;
            resources.lightGridInjectionGroupSize = kForwardLightGridInjectionGroupSize;
            return resources;
        }
    }

    LightGridCompileContext BuildLightGridCompileContext(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        PreparedComputeDispatchSource preparedComputeSource,
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> graphicsPassBindingSet)
    {
        NLS::Render::Data::FrameDescriptor lightGridFrameDescriptor;
        lightGridFrameDescriptor.renderWidth = frameDescriptor.renderWidth;
        lightGridFrameDescriptor.renderHeight = frameDescriptor.renderHeight;
        lightGridFrameDescriptor.camera = frameDescriptor.camera;
        lightGridFrameDescriptor.clearColorOverride = frameDescriptor.clearColorOverride;

        LightGridCompileContext context;
        context.frameDescriptor = lightGridFrameDescriptor;
        context.preparedComputeSource = std::move(preparedComputeSource);
        context.forwardLightingResources = BuildForwardLightingResources();
        context.graphicsPassBindingSet = std::move(graphicsPassBindingSet);
        return context;
    }
}
