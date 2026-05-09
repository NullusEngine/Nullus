#include "Rendering/FrameGraph/SceneRenderGraphBuilderLightGrid.h"

#include <utility>

namespace NLS::Render::FrameGraph
{
    LightGridCompileContext BuildLightGridCompileContext(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        PreparedComputeDispatchSource preparedComputeSource,
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> graphicsPassBindingSet)
    {
        LightGridCompileContext context;
        context.frameDescriptor = frameDescriptor;
        context.preparedComputeSource = std::move(preparedComputeSource);
        context.graphicsPassBindingSet = std::move(graphicsPassBindingSet);
        return context;
    }
}
