#pragma once

#include <cstdint>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHICommand.h"

namespace NLS::Render::RHI::DX12
{
    struct NLS_RENDER_API DX12RenderPassClearPlan
    {
        std::vector<uint32_t> colorAttachmentIndices;
        bool clearDepth = false;
        bool clearStencil = false;
    };

    NLS_RENDER_API DX12RenderPassClearPlan BuildDX12RenderPassClearPlan(
        const RHIRenderPassDesc& desc);
}
