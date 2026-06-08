#include <gtest/gtest.h>

#if defined(_WIN32)
#include <memory>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include "Rendering/RHI/Backends/DX12/DX12RenderPassUtils.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace
{
    std::string ReadRepoFile(const std::filesystem::path& relativePath)
    {
        std::ifstream input(std::filesystem::path(NLS_ROOT_DIR) / relativePath);
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

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

TEST(DX12RenderPassUtilsTests, BackbufferClearWarningFilterSerializesDeviceWideInfoQueueMutation)
{
    const auto header = ReadRepoFile("Runtime/Rendering/RHI/Backends/DX12/DX12InfoQueueUtils.h");

    EXPECT_NE(header.find("#include <mutex>"), std::string::npos);
    EXPECT_NE(header.find("Dx12InfoQueueMessageFilterMutex"), std::string::npos);
    EXPECT_NE(header.find("ScopedDx12InfoQueueMessageScope"), std::string::npos);
    EXPECT_NE(header.find("D3D12 info-queue filters are device-wide"), std::string::npos);

    const auto commandSource = ReadRepoFile("Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");
    EXPECT_NE(commandSource.find("ScopedDx12InfoQueueMessageScope"), std::string::npos);
}

TEST(DX12RenderPassUtilsTests, HZBOcclusionResourceStateContractsStayExplicitAndSubresourceAware)
{
    const auto commandSource = ReadRepoFile("Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto toState = commandSource.find("NativeDX12CommandBuffer::ToD3D12ResourceState");
    ASSERT_NE(toState, std::string::npos);
    const auto shaderWrite = commandSource.find("ResourceState::ShaderWrite", toState);
    ASSERT_NE(shaderWrite, std::string::npos);
    EXPECT_NE(commandSource.find("D3D12_RESOURCE_STATE_UNORDERED_ACCESS", shaderWrite), std::string::npos);
    const auto shaderRead = commandSource.find("ResourceState::ShaderRead", toState);
    ASSERT_NE(shaderRead, std::string::npos);
    EXPECT_NE(commandSource.find("D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE", shaderRead), std::string::npos);
    EXPECT_NE(commandSource.find("D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE", shaderRead), std::string::npos);
    EXPECT_NE(commandSource.find("ResourceState::DepthRead", toState), std::string::npos);
    EXPECT_NE(commandSource.find("D3D12_RESOURCE_STATE_DEPTH_READ", toState), std::string::npos);
    EXPECT_NE(commandSource.find("ResourceState::DepthWrite", toState), std::string::npos);
    EXPECT_NE(commandSource.find("D3D12_RESOURCE_STATE_DEPTH_WRITE", toState), std::string::npos);

    const auto barrierChecked = commandSource.find("NativeDX12CommandBuffer::BarrierChecked");
    ASSERT_NE(barrierChecked, std::string::npos);
    const auto textureBarrierLoop = commandSource.find("for (const auto& textureBarrier", barrierChecked);
    ASSERT_NE(textureBarrierLoop, std::string::npos);
    const auto sameState = commandSource.find("stateBefore == stateAfter", textureBarrierLoop);
    ASSERT_NE(sameState, std::string::npos);
    const auto uavBarrier = commandSource.find("D3D12_RESOURCE_BARRIER_TYPE_UAV", sameState);
    ASSERT_NE(uavBarrier, std::string::npos);
    EXPECT_LT(uavBarrier, commandSource.find("barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION", sameState));

    const auto subresourceSplit = commandSource.find("BuildDX12BarrierSubresourceIndices", textureBarrierLoop);
    ASSERT_NE(subresourceSplit, std::string::npos);
    EXPECT_NE(commandSource.find("barrier.Transition.Subresource = subresourceIndex", subresourceSplit), std::string::npos);
    EXPECT_NE(commandSource.find("nativeTexture->SetState(textureBarrier.after)", textureBarrierLoop), std::string::npos);
    EXPECT_NE(commandSource.find("nativeTexture->MarkPartialStateDirty()", textureBarrierLoop), std::string::npos);

    const auto beginRenderPass = commandSource.find("NativeDX12CommandBuffer::BeginRenderPass");
    ASSERT_NE(beginRenderPass, std::string::npos);
    const auto readOnlyDepth = commandSource.find("readOnlyDepthStencil", beginRenderPass);
    ASSERT_NE(readOnlyDepth, std::string::npos);
    EXPECT_NE(commandSource.find("D3D12_RESOURCE_STATE_DEPTH_READ", readOnlyDepth), std::string::npos);
    EXPECT_NE(commandSource.find("RHIDepthStencilViewAccess::ReadOnlyDepthStencil", readOnlyDepth), std::string::npos);
}

TEST(DX12RenderPassUtilsTests, PartialUnknownTextureBarrierFilteringKeepsHZBStorageWrites)
{
    const auto commandSource = ReadRepoFile("Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

    const auto filter = commandSource.find("bool ShouldFilterUnresolvedPartialTextureBarrier");
    ASSERT_NE(filter, std::string::npos);
    const auto filterEnd = commandSource.find("bool IsValidDX12BufferCopyEndpoint", filter);
    ASSERT_NE(filterEnd, std::string::npos);
    const auto filterBody = commandSource.substr(filter, filterEnd - filter);

    EXPECT_NE(filterBody.find("barrier.before == NLS::Render::RHI::ResourceState::Unknown"), std::string::npos);
    EXPECT_NE(filterBody.find("!coversWholeTexture"), std::string::npos);
    EXPECT_NE(filterBody.find("IsD3D12CommonPromotionAllowedForTexture(barrier.after)"), std::string::npos);
}
#endif
