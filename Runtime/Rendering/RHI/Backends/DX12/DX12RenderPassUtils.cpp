#include "Rendering/RHI/Backends/DX12/DX12RenderPassUtils.h"

namespace NLS::Render::RHI::DX12
{
    DX12RenderPassClearPlan BuildDX12RenderPassClearPlan(const RHIRenderPassDesc& desc)
    {
        DX12RenderPassClearPlan clearPlan;

        for (uint32_t attachmentIndex = 0; attachmentIndex < static_cast<uint32_t>(desc.colorAttachments.size()); ++attachmentIndex)
        {
            if (desc.colorAttachments[attachmentIndex].loadOp == LoadOp::Clear)
                clearPlan.colorAttachmentIndices.push_back(attachmentIndex);
        }

        if (desc.depthStencilAttachment.has_value())
        {
            clearPlan.clearDepth = desc.depthStencilAttachment->depthLoadOp == LoadOp::Clear;
            clearPlan.clearStencil = desc.depthStencilAttachment->stencilLoadOp == LoadOp::Clear;
        }

        return clearPlan;
    }
}
