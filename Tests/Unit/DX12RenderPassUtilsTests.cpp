#include <gtest/gtest.h>

#if defined(_WIN32)
#include <memory>
#include <string_view>
#include <utility>

#include "Rendering/RHI/Backends/DX12/DX12RenderPassUtils.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace
{
    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        TestTexture(
            NLS::Render::RHI::RHITextureDesc desc,
            const bool requiresExternalClearValueMessageFilter = false)
            : m_desc(std::move(desc))
            , m_requiresExternalClearValueMessageFilter(requiresExternalClearValueMessageFilter)
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }
        bool RequiresExternalClearValueMessageFilter() const override
        {
            return m_requiresExternalClearValueMessageFilter;
        }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc;
        bool m_requiresExternalClearValueMessageFilter = false;
    };

    class TestTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        explicit TestTextureView(std::shared_ptr<NLS::Render::RHI::RHITexture> texture)
            : m_texture(std::move(texture))
        {
        }

        std::string_view GetDebugName() const override { return "TestTextureView"; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
        NLS::Render::RHI::RHITextureViewDesc m_desc;
    };

    std::shared_ptr<NLS::Render::RHI::RHITextureView> MakeTextureView(
        const NLS::Render::RHI::TextureUsageFlags usage,
        const bool requiresExternalClearValueMessageFilter = false)
    {
        NLS::Render::RHI::RHITextureDesc textureDesc;
        textureDesc.usage = usage;
        auto texture = std::make_shared<TestTexture>(
            textureDesc,
            requiresExternalClearValueMessageFilter);
        return std::make_shared<TestTextureView>(texture);
    }
}

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

    ASSERT_EQ(clearPlan.colorClearRequests.size(), 1u);
    EXPECT_EQ(clearPlan.colorClearRequests[0].attachmentIndex, 1u);
    EXPECT_FALSE(clearPlan.colorClearRequests[0].suppressClearValueMismatchWarning);
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

    EXPECT_TRUE(clearPlan.colorClearRequests.empty());
    EXPECT_FALSE(clearPlan.clearDepth);
    EXPECT_FALSE(clearPlan.clearStencil);
}

TEST(DX12RenderPassUtilsTests, SkipsReadOnlyDepthStencilClearRequests)
{
    NLS::Render::RHI::RHIRenderPassDesc renderPassDesc;
    NLS::Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthStencilAttachment;
    depthStencilAttachment.depthLoadOp = NLS::Render::RHI::LoadOp::Clear;
    depthStencilAttachment.stencilLoadOp = NLS::Render::RHI::LoadOp::Clear;
    depthStencilAttachment.readOnlyDepthStencil = true;
    renderPassDesc.depthStencilAttachment = depthStencilAttachment;

    const auto clearPlan = NLS::Render::RHI::DX12::BuildDX12RenderPassClearPlan(renderPassDesc);

    EXPECT_FALSE(clearPlan.clearDepth);
    EXPECT_FALSE(clearPlan.clearStencil);
}

TEST(DX12RenderPassUtilsTests, TracksExternalBackbufferClearRequestsSeparatelyFromOwnedRenderTargets)
{
    NLS::Render::RHI::RHIRenderPassDesc renderPassDesc;
    renderPassDesc.colorAttachments = {
        {
            MakeTextureView(NLS::Render::RHI::TextureUsageFlags::ColorAttachment),
            NLS::Render::RHI::LoadOp::Clear,
            NLS::Render::RHI::StoreOp::Store,
            {}
        },
        {
            MakeTextureView(
                NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
                NLS::Render::RHI::TextureUsageFlags::Present,
                true),
            NLS::Render::RHI::LoadOp::Clear,
            NLS::Render::RHI::StoreOp::Store,
            {}
        }
    };

    const auto clearPlan = NLS::Render::RHI::DX12::BuildDX12RenderPassClearPlan(renderPassDesc);

    ASSERT_EQ(clearPlan.colorClearRequests.size(), 2u);
    EXPECT_EQ(clearPlan.colorClearRequests[0].attachmentIndex, 0u);
    EXPECT_FALSE(clearPlan.colorClearRequests[0].suppressClearValueMismatchWarning);
    EXPECT_EQ(clearPlan.colorClearRequests[1].attachmentIndex, 1u);
    EXPECT_TRUE(clearPlan.colorClearRequests[1].suppressClearValueMismatchWarning);
}

TEST(DX12RenderPassUtilsTests, PresentUsageAloneDoesNotSuppressOwnedRenderTargetClearWarnings)
{
    NLS::Render::RHI::RHIRenderPassDesc renderPassDesc;
    renderPassDesc.colorAttachments = {
        {
            MakeTextureView(
                NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
                NLS::Render::RHI::TextureUsageFlags::Present),
            NLS::Render::RHI::LoadOp::Clear,
            NLS::Render::RHI::StoreOp::Store,
            {}
        }
    };

    const auto clearPlan = NLS::Render::RHI::DX12::BuildDX12RenderPassClearPlan(renderPassDesc);

    ASSERT_EQ(clearPlan.colorClearRequests.size(), 1u);
    EXPECT_EQ(clearPlan.colorClearRequests[0].attachmentIndex, 0u);
    EXPECT_FALSE(clearPlan.colorClearRequests[0].suppressClearValueMismatchWarning);
}

#endif
