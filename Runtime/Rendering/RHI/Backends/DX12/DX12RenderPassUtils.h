#pragma once

#include <cstdint>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHICommand.h"

namespace NLS::Render::RHI::DX12
{
    struct NLS_RENDER_API DX12ColorClearRequest
    {
        uint32_t attachmentIndex = 0u;
        bool suppressClearValueMismatchWarning = false;
    };

    struct NLS_RENDER_API DX12RenderPassClearPlan
    {
        std::vector<DX12ColorClearRequest> colorClearRequests;
        bool clearDepth = false;
        bool clearStencil = false;
    };

    NLS_RENDER_API DX12RenderPassClearPlan BuildDX12RenderPassClearPlan(
        const RHIRenderPassDesc& desc);
}
