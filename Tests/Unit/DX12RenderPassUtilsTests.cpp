#include <gtest/gtest.h>

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX12/DX12RenderPassUtils.h"
#include "Rendering/RHI/Core/RHICommand.h"

TEST(DX12RenderPassUtilsTests, CollectsColorAndDepthStencilClearRequests)
{
    NLS::Render::RHI::RHIRenderPassDesc renderPassDesc;
    renderPassDesc.colorAttachments = {
        { nullptr, NLS::Render::RHI::LoadOp::Load, NLS::Render::RHI::StoreOp::Store, {} },
        { nullptr, NLS::Render::RHI::LoadOp::Clear, NLS::Render::RHI::StoreOp::Store, {} },
        { nullptr, NLS::Render::RHI::LoadOp::DontCare, NLS::Render::RHI::StoreOp::Store, {} }
    };

    NLS::Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthStencilAttachment;
    depthStencilAttachment.depthLoadOp = NLS::Render::RHI::LoadOp::Clear;
    depthStencilAttachment.stencilLoadOp = NLS::Render::RHI::LoadOp::Clear;
    renderPassDesc.depthStencilAttachment = depthStencilAttachment;

    const auto clearPlan = NLS::Render::RHI::DX12::BuildDX12RenderPassClearPlan(renderPassDesc);

    ASSERT_EQ(clearPlan.colorAttachmentIndices.size(), 1u);
    EXPECT_EQ(clearPlan.colorAttachmentIndices[0], 1u);
    EXPECT_TRUE(clearPlan.clearDepth);
    EXPECT_TRUE(clearPlan.clearStencil);
}

TEST(DX12RenderPassUtilsTests, SkipsAttachmentsThatDoNotRequestClear)
{
    NLS::Render::RHI::RHIRenderPassDesc renderPassDesc;
    renderPassDesc.colorAttachments = {
        { nullptr, NLS::Render::RHI::LoadOp::Load, NLS::Render::RHI::StoreOp::Store, {} },
        { nullptr, NLS::Render::RHI::LoadOp::DontCare, NLS::Render::RHI::StoreOp::Store, {} }
    };

    NLS::Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthStencilAttachment;
    depthStencilAttachment.depthLoadOp = NLS::Render::RHI::LoadOp::Load;
    depthStencilAttachment.stencilLoadOp = NLS::Render::RHI::LoadOp::DontCare;
    renderPassDesc.depthStencilAttachment = depthStencilAttachment;

    const auto clearPlan = NLS::Render::RHI::DX12::BuildDX12RenderPassClearPlan(renderPassDesc);

    EXPECT_TRUE(clearPlan.colorAttachmentIndices.empty());
    EXPECT_FALSE(clearPlan.clearDepth);
    EXPECT_FALSE(clearPlan.clearStencil);
}
#endif
