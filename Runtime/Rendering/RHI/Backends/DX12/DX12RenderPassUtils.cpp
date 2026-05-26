#include "Rendering/RHI/Backends/DX12/DX12RenderPassUtils.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI::DX12
{
    namespace
    {
        bool IsExternalBackbufferClear(const RHIRenderPassColorAttachmentDesc& attachment)
        {
            if (attachment.view == nullptr || attachment.view->GetTexture() == nullptr)
                return false;

            return attachment.view->GetTexture()->RequiresExternalClearValueMessageFilter();
        }
    }

    DX12RenderPassClearPlan BuildDX12RenderPassClearPlan(const RHIRenderPassDesc& desc)
    {
        DX12RenderPassClearPlan clearPlan;

        for (uint32_t attachmentIndex = 0; attachmentIndex < static_cast<uint32_t>(desc.colorAttachments.size()); ++attachmentIndex)
        {
            const auto& colorAttachment = desc.colorAttachments[attachmentIndex];
            if (colorAttachment.loadOp == LoadOp::Clear)
            {
                clearPlan.colorClearRequests.push_back({
                    attachmentIndex,
                    IsExternalBackbufferClear(colorAttachment)
                });
            }
        }

        if (desc.depthStencilAttachment.has_value())
        {
            clearPlan.clearDepth = desc.depthStencilAttachment->depthLoadOp == LoadOp::Clear;
            clearPlan.clearStencil = desc.depthStencilAttachment->stencilLoadOp == LoadOp::Clear;
        }

        return clearPlan;
    }
}
