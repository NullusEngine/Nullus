#pragma once

#include "RenderDef.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/RHI/Core/RHIPipeline.h"

namespace NLS::Render::RHI
{
    NLS_RENDER_API void ApplyPipelineStateToGraphicsPipelineDesc(
        const NLS::Render::Data::PipelineState& pipelineState,
        RHIGraphicsPipelineDesc& desc);
}
