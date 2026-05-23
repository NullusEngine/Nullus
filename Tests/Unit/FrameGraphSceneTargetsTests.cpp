#include <gtest/gtest.h>

#include <fg/GraphvizWriter.hpp>

#include <array>
#include <sstream>

#include "Core/ServiceLocator.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Buffers/MultiFramebuffer.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/FrameGraph/FrameGraphBuffer.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"
#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/FrameGraphSceneTargets.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilder.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/Settings/DriverSettings.h"

namespace
{
    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata MakeThreadedPassMetadata(
        const NLS::Render::Context::RenderPassCommandKind commandKind,
        const NLS::Render::FrameGraph::ThreadedRenderScenePassRole role,
        const char* graphPassName,
        const uint64_t visibleDrawCountContribution = NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount,
        const NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode executionMode =
            NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Output,
        const NLS::Render::RHI::QueueType queueType = NLS::Render::RHI::QueueType::Graphics,
        const NLS::Render::Context::QueueDependencyPolicy queueDependencyPolicy =
            NLS::Render::Context::QueueDependencyPolicy::Previous)
    {
        NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata metadata;
        metadata.commandKind = commandKind;
        metadata.role = role;
        metadata.executionMode = executionMode;
        metadata.queueType = queueType;
        metadata.queueDependencyPolicy = queueDependencyPolicy;
        metadata.graphPassName = graphPassName;
        metadata.visibleDrawCountContribution = visibleDrawCountContribution;
        return metadata;
    }

    NLS::Render::FrameGraph::ThreadedRenderScenePassPlan MakeThreadedPassPlan(
        NLS::Render::Context::RenderPassCommandInput commandInput,
        const NLS::Render::FrameGraph::ThreadedRenderScenePassRole role,
        const std::string_view graphPassName,
        const uint64_t visibleDrawCountContribution,
        const NLS::Render::RHI::QueueType queueType = NLS::Render::RHI::QueueType::Graphics,
        const NLS::Render::Context::QueueDependencyPolicy queueDependencyPolicy =
            NLS::Render::Context::QueueDependencyPolicy::Previous)
    {
        NLS::Render::FrameGraph::ThreadedRenderScenePassPlan plan;
        commandInput.queueDependencyPolicy = queueDependencyPolicy;
        plan.commandInput = std::move(commandInput);
        plan.role = role;
        plan.queueType = queueType;
        plan.queueDependencyPolicy = queueDependencyPolicy;
        plan.graphPassName = graphPassName;
        plan.visibleDrawCountContribution = visibleDrawCountContribution;
        return plan;
    }

    class TestAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "FrameGraphSceneTargetsTestsAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::DX12; }
        std::string_view GetVendor() const override { return "TestVendor"; }
        std::string_view GetHardware() const override { return "TestHardware"; }
    };

    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        explicit TestTexture(NLS::Render::RHI::RHITextureDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return reportedState; }

        NLS::Render::RHI::ResourceState reportedState = NLS::Render::RHI::ResourceState::Unknown;

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    class TestBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        explicit TestBuffer(NLS::Render::RHI::RHIBufferDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }
        uint64_t GetGPUAddress() const override { return 0u; }

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc {};
    };

    class TestBindingSet final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        explicit TestBindingSet(std::string debugName)
        {
            m_desc.debugName = std::move(debugName);
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingSetDesc m_desc {};
    };

    class TestTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        TestTextureView(
            std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
            NLS::Render::RHI::RHITextureViewDesc desc)
            : m_texture(std::move(texture))
            , m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
        NLS::Render::RHI::RHITextureViewDesc m_desc {};
    };

    class TestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "FrameGraphSceneTargetsTestsCommandBuffer"; }
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        bool IsRecording() const override { return true; }
        NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
        void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc) override
        {
            ++beginRenderPassCalls;
            lastRenderPassDesc = desc;
        }
        void EndRenderPass() override { ++endRenderPassCalls; }
        void SetViewport(const NLS::Render::RHI::RHIViewport&) override {}
        void SetScissor(const NLS::Render::RHI::RHIRect2D&) override {}
        void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>&) override {}
        void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}
        void BindBindingSet(uint32_t, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>&) override {}
        void PushConstants(NLS::Render::RHI::ShaderStageMask, uint32_t, uint32_t, const void*) override {}
        void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override {}
        void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override {}
        void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
        void Dispatch(uint32_t, uint32_t, uint32_t) override {}
        void CopyBuffer(
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const NLS::Render::RHI::RHIBufferCopyRegion&) override {}
        void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc&) override {}
        void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc& desc) override
        {
            ++barrierCalls;
            barrierHistory.push_back(desc);
        }

        size_t barrierCalls = 0u;
        size_t beginRenderPassCalls = 0u;
        size_t endRenderPassCalls = 0u;
        NLS::Render::RHI::RHIRenderPassDesc lastRenderPassDesc {};
        std::vector<NLS::Render::RHI::RHIBarrierDesc> barrierHistory;
    };

    class TestQueue final : public NLS::Render::RHI::RHIQueue
    {
    public:
        std::string_view GetDebugName() const override { return "FrameGraphSceneTargetsTestsQueue"; }
        NLS::Render::RHI::QueueType GetType() const override { return NLS::Render::RHI::QueueType::Graphics; }
        void Submit(const NLS::Render::RHI::RHISubmitDesc&) override {}
        NLS::Render::RHI::RHIQueueOperationResult SubmitChecked(
            const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
        {
            Submit(submitDesc);
            return {};
        }
        void Present(const NLS::Render::RHI::RHIPresentDesc&) override {}
        NLS::Render::RHI::RHIQueueOperationResult PresentChecked(
            const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
        {
            Present(presentDesc);
            return {};
        }
    };

    class TestExplicitDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        using NLS::Render::RHI::RHIDevice::CreateBuffer;
        using NLS::Render::RHI::RHIDevice::CreateTexture;

        TestExplicitDevice()
            : m_adapter(std::make_shared<TestAdapter>())
            , m_queue(std::make_shared<TestQueue>())
        {
            m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
            m_capabilities.backendReady = true;
            m_capabilities.supportsGraphics = true;
            m_capabilities.supportsCompute = true;
            m_capabilities.supportsSwapchain = true;
            m_capabilities.supportsCurrentSceneRenderer = true;
            m_capabilities.supportsOffscreenFramebuffers = true;
            m_capabilities.supportsMultiRenderTargets = true;
            m_capabilities.supportsExplicitBarriers = true;
            m_capabilities.supportsCentralizedDescriptorManagement = true;
            m_capabilities.supportsPipelineStateCache = true;
        }

        std::string_view GetDebugName() const override { return "FrameGraphSceneTargetsTestsDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return m_queue; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc& desc,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override
        {
            bufferDescs.push_back(desc);
            return std::make_shared<TestBuffer>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const NLS::Render::RHI::RHITextureUploadDesc&) override
        {
            ++textureCreateCalls;
            if (failTextureCreateCall != 0u && textureCreateCalls == failTextureCreateCall)
                return nullptr;

            return std::make_shared<TestTexture>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHITextureViewDesc& desc) override
        {
            return std::make_shared<TestTextureView>(texture, desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc&, std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(NLS::Render::RHI::QueueType, std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string = {}) override { return nullptr; }
        void ReadPixels(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
            uint32_t,
            uint32_t,
            uint32_t,
            uint32_t,
            NLS::Render::Settings::EPixelDataFormat,
            NLS::Render::Settings::EPixelDataType,
            void*) override {}

        size_t textureCreateCalls = 0u;
        size_t failTextureCreateCall = 0u;
        std::vector<NLS::Render::RHI::RHIBufferDesc> bufferDescs;

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        std::shared_ptr<NLS::Render::RHI::RHIQueue> m_queue;
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };
}

TEST(FrameGraphSceneTargetsTests, SceneColorTargetSupportsSamplingForEditorViews)
{
    const auto desc = NLS::Engine::Rendering::MakeSceneColorTargetDesc(1280, 720);

    EXPECT_EQ(desc.debugName, "SceneColor");
    EXPECT_EQ(desc.extent.width, 1280u);
    EXPECT_EQ(desc.extent.height, 720u);
    EXPECT_EQ(desc.format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_TRUE(NLS::Render::RHI::HasTextureUsage(
        desc.usage,
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment));
    EXPECT_TRUE(NLS::Render::RHI::HasTextureUsage(
        desc.usage,
        NLS::Render::RHI::TextureUsageFlags::Sampled));

    const auto depthDesc = NLS::Engine::Rendering::MakeSceneDepthTargetDesc(1280, 720);
    EXPECT_EQ(depthDesc.debugName, "SceneDepth");
}

TEST(FrameGraphSceneTargetsTests, FramebufferResizeRetainsPreviousExplicitResourcesTemporarily)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    std::weak_ptr<NLS::Render::RHI::RHITexture> previousColorTexture;
    std::weak_ptr<NLS::Render::RHI::RHITexture> previousDepthTexture;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> previousColorView;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> previousDepthView;

    {
        const auto colorTexture = outputBuffer.GetExplicitTextureHandle();
        const auto depthTexture = outputBuffer.GetExplicitDepthStencilTextureHandle();
        const auto colorView = outputBuffer.GetOrCreateExplicitColorView("OldSceneColorView");
        const auto depthView = outputBuffer.GetOrCreateExplicitDepthStencilView("OldSceneDepthView");
        previousColorTexture = colorTexture;
        previousDepthTexture = depthTexture;
        previousColorView = colorView;
        previousDepthView = depthView;
    }

    outputBuffer.Resize(640u, 360u);

    EXPECT_FALSE(previousColorTexture.expired());
    EXPECT_FALSE(previousDepthTexture.expired());
    EXPECT_FALSE(previousColorView.expired());
    EXPECT_FALSE(previousDepthView.expired());
    EXPECT_NE(outputBuffer.GetExplicitTextureHandle(), previousColorTexture.lock());
    EXPECT_NE(outputBuffer.GetExplicitDepthStencilTextureHandle(), previousDepthTexture.lock());
}

TEST(FrameGraphSceneTargetsTests, FramebufferClearValueRebuildRetainsPreviousExplicitResourcesTemporarily)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    std::weak_ptr<NLS::Render::RHI::RHITexture> previousColorTexture;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> previousColorView;

    {
        const auto colorTexture = outputBuffer.GetExplicitTextureHandle();
        const auto colorView = outputBuffer.GetOrCreateExplicitColorView("OldSceneColorView");
        previousColorTexture = colorTexture;
        previousColorView = colorView;
    }

    outputBuffer.SetOptimizedColorClearValue(0.25f, 0.5f, 0.75f, 1.0f);

    EXPECT_FALSE(previousColorTexture.expired());
    EXPECT_FALSE(previousColorView.expired());
    EXPECT_NE(outputBuffer.GetExplicitTextureHandle(), previousColorTexture.lock());
    ASSERT_NE(outputBuffer.GetExplicitTextureHandle(), nullptr);
    const auto& optimizedClearValue = outputBuffer.GetExplicitTextureHandle()->GetDesc().optimizedClearValue;
    EXPECT_TRUE(optimizedClearValue.enabled);
    EXPECT_FLOAT_EQ(optimizedClearValue.color[0], 0.25f);
    EXPECT_FLOAT_EQ(optimizedClearValue.color[1], 0.5f);
    EXPECT_FLOAT_EQ(optimizedClearValue.color[2], 0.75f);
    EXPECT_FLOAT_EQ(optimizedClearValue.color[3], 1.0f);
}

TEST(FrameGraphSceneTargetsTests, MultiFramebufferResizeRetainsPreviousExplicitResourcesTemporarily)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> attachments(2u);
    attachments[0].format = NLS::Render::RHI::TextureFormat::RGBA8;
    attachments[1].format = NLS::Render::RHI::TextureFormat::RGB8;

    NLS::Render::Buffers::MultiFramebuffer gBuffer(320u, 180u, attachments, true);
    std::weak_ptr<NLS::Render::RHI::RHITexture> previousColorTexture0;
    std::weak_ptr<NLS::Render::RHI::RHITexture> previousColorTexture1;
    std::weak_ptr<NLS::Render::RHI::RHITexture> previousDepthTexture;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> previousColorView0;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> previousColorView1;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> previousDepthView;

    {
        const auto colorTextures = gBuffer.GetExplicitColorTextureHandles();
        ASSERT_EQ(colorTextures.size(), 2u);
        const auto colorView0 = gBuffer.GetOrCreateExplicitColorView(0u, "OldGBufferColorView0");
        const auto colorView1 = gBuffer.GetOrCreateExplicitColorView(1u, "OldGBufferColorView1");
        const auto depthTexture = gBuffer.GetExplicitDepthTextureHandle();
        const auto depthView = gBuffer.GetOrCreateExplicitDepthView("OldGBufferDepthView");

        previousColorTexture0 = colorTextures[0];
        previousColorTexture1 = colorTextures[1];
        previousDepthTexture = depthTexture;
        previousColorView0 = colorView0;
        previousColorView1 = colorView1;
        previousDepthView = depthView;
    }

    gBuffer.Resize(640u, 360u);

    EXPECT_FALSE(previousColorTexture0.expired());
    EXPECT_FALSE(previousColorTexture1.expired());
    EXPECT_FALSE(previousDepthTexture.expired());
    EXPECT_FALSE(previousColorView0.expired());
    EXPECT_FALSE(previousColorView1.expired());
    EXPECT_FALSE(previousDepthView.expired());

    const auto resizedColorTextures = gBuffer.GetExplicitColorTextureHandles();
    ASSERT_EQ(resizedColorTextures.size(), 2u);
    EXPECT_NE(resizedColorTextures[0], previousColorTexture0.lock());
    EXPECT_NE(resizedColorTextures[1], previousColorTexture1.lock());
    EXPECT_NE(gBuffer.GetExplicitDepthTextureHandle(), previousDepthTexture.lock());
}

TEST(FrameGraphSceneTargetsTests, MultiFramebufferFailedResizeCanRetrySameTargetSize)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> attachments(1u);
    attachments[0].format = NLS::Render::RHI::TextureFormat::RGBA8;

    NLS::Render::Buffers::MultiFramebuffer gBuffer(320u, 180u, attachments, true);
    const auto initialColorTexture = gBuffer.GetExplicitColorTextureHandles().front();
    const auto initialDepthTexture = gBuffer.GetExplicitDepthTextureHandle();

    explicitDevice->failTextureCreateCall = explicitDevice->textureCreateCalls + 1u;
    gBuffer.Resize(640u, 360u);

    ASSERT_EQ(gBuffer.GetExplicitColorTextureHandles().size(), 1u);
    ASSERT_NE(gBuffer.GetExplicitColorTextureHandles().front(), nullptr);
    ASSERT_NE(gBuffer.GetExplicitDepthTextureHandle(), nullptr);
    EXPECT_EQ(gBuffer.GetExplicitColorTextureHandles().front(), initialColorTexture);
    EXPECT_EQ(gBuffer.GetExplicitDepthTextureHandle(), initialDepthTexture);
    EXPECT_EQ(gBuffer.GetExplicitColorTextureHandles().front()->GetDesc().extent.width, 320u);
    EXPECT_EQ(gBuffer.GetExplicitColorTextureHandles().front()->GetDesc().extent.height, 180u);

    explicitDevice->failTextureCreateCall = 0u;
    gBuffer.Resize(640u, 360u);

    ASSERT_EQ(gBuffer.GetExplicitColorTextureHandles().size(), 1u);
    ASSERT_NE(gBuffer.GetExplicitColorTextureHandles().front(), nullptr);
    ASSERT_NE(gBuffer.GetExplicitDepthTextureHandle(), nullptr);
    EXPECT_NE(gBuffer.GetExplicitColorTextureHandles().front(), initialColorTexture);
    EXPECT_NE(gBuffer.GetExplicitDepthTextureHandle(), initialDepthTexture);
    EXPECT_EQ(gBuffer.GetExplicitColorTextureHandles().front()->GetDesc().extent.width, 640u);
    EXPECT_EQ(gBuffer.GetExplicitColorTextureHandles().front()->GetDesc().extent.height, 360u);
}

TEST(FrameGraphSceneTargetsTests, MultiFramebufferZeroSizeResizeRetiresPreviousExplicitResources)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> attachments(1u);
    attachments[0].format = NLS::Render::RHI::TextureFormat::RGBA8;

    NLS::Render::Buffers::MultiFramebuffer gBuffer(320u, 180u, attachments, true);
    std::weak_ptr<NLS::Render::RHI::RHITexture> previousColorTexture;
    std::weak_ptr<NLS::Render::RHI::RHITexture> previousDepthTexture;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> previousColorView;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> previousDepthView;

    {
        const auto colorTexture = gBuffer.GetExplicitColorTextureHandles().front();
        const auto depthTexture = gBuffer.GetExplicitDepthTextureHandle();
        const auto colorView = gBuffer.GetOrCreateExplicitColorView(0u, "OldZeroSizeGBufferColorView");
        const auto depthView = gBuffer.GetOrCreateExplicitDepthView("OldZeroSizeGBufferDepthView");

        previousColorTexture = colorTexture;
        previousDepthTexture = depthTexture;
        previousColorView = colorView;
        previousDepthView = depthView;
    }

    const auto textureCreateCallsBeforeResize = explicitDevice->textureCreateCalls;
    gBuffer.Resize(0u, 0u);

    EXPECT_FALSE(previousColorTexture.expired());
    EXPECT_FALSE(previousDepthTexture.expired());
    EXPECT_FALSE(previousColorView.expired());
    EXPECT_FALSE(previousDepthView.expired());
    EXPECT_FALSE(gBuffer.IsInitialized());
    EXPECT_TRUE(gBuffer.GetExplicitColorTextureHandles().empty());
    EXPECT_EQ(gBuffer.GetExplicitDepthTextureHandle(), nullptr);
    EXPECT_EQ(explicitDevice->textureCreateCalls, textureCreateCallsBeforeResize);
}

TEST(FrameGraphSceneTargetsTests, ImportSceneRenderTargetsSkipsFramesWithoutOutputBuffer)
{
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;

    NLS::Render::FrameGraph::ImportSceneRenderTargets(
        frameGraph,
        blackboard,
        frameDescriptor,
        "SceneColor",
        "SceneDepth");

    EXPECT_EQ(blackboard.try_get<NLS::Render::FrameGraph::SceneRenderTargetsData>(), nullptr);
}

TEST(FrameGraphSceneTargetsTests, ImportSceneRenderTargetsAddsExternalFramebufferResourcesToBlackboard)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;

    NLS::Render::FrameGraph::ImportSceneRenderTargets(
        frameGraph,
        blackboard,
        frameDescriptor,
        "SceneColor",
        "SceneDepth");

    const auto* targets = blackboard.try_get<NLS::Render::FrameGraph::SceneRenderTargetsData>();
    ASSERT_NE(targets, nullptr);
    EXPECT_GE(targets->color, 0);
    EXPECT_GE(targets->depth, 0);
    EXPECT_NE(outputBuffer.GetExplicitTextureHandle(), nullptr);
    EXPECT_NE(outputBuffer.GetOrCreateExplicitColorView("SceneOutputColorView"), nullptr);
    EXPECT_NE(outputBuffer.GetExplicitDepthStencilTextureHandle(), nullptr);
    EXPECT_NE(outputBuffer.GetOrCreateExplicitDepthStencilView("SceneOutputDepthView"), nullptr);
}

TEST(FrameGraphSceneTargetsTests, FrameGraphCreatedResourcesRegisterAndRetireTransientLifetime)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 7u;
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::FrameGraph::FrameGraphExecutionContext executionContext{
        driver,
        explicitDevice.get(),
        frameContext.commandBuffer.get(),
        &frameContext
    };

    NLS::Render::FrameGraph::FrameGraphTexture texture;
    NLS::Render::FrameGraph::FrameGraphTexture::Desc textureDesc;
    textureDesc.extent.width = 256u;
    textureDesc.extent.height = 144u;
    textureDesc.extent.depth = 1u;
    textureDesc.arrayLayers = 1u;
    textureDesc.mipLevels = 1u;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment | NLS::Render::RHI::TextureUsageFlags::Sampled;
    textureDesc.debugName = "TransientSceneColor";
    texture.create(textureDesc, &executionContext);

    NLS::Render::FrameGraph::FrameGraphBuffer buffer;
    NLS::Render::FrameGraph::FrameGraphBuffer::Desc bufferDesc;
    bufferDesc.size = 1024u;
    bufferDesc.type = NLS::Render::RHI::BufferType::ShaderStorage;
    bufferDesc.usage = NLS::Render::RHI::BufferUsage::DynamicDraw;
    bufferDesc.debugName = "TransientLightList";
    buffer.create(bufferDesc, &executionContext);

    std::weak_ptr<NLS::Render::RHI::RHITexture> transientTexture = texture.explicitTexture;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> transientTextureView = texture.explicitView;
    std::weak_ptr<NLS::Render::RHI::RHIBuffer> transientBuffer = buffer.explicitBuffer;

    auto trackerStats = frameContext.resourceStateTracker->GetStats();
    EXPECT_EQ(trackerStats.currentFrameIndex, 7u);
    EXPECT_EQ(trackerStats.transientTextureRegistrations, 1u);
    EXPECT_EQ(trackerStats.transientBufferRegistrations, 1u);
    EXPECT_NE(texture.explicitTexture, nullptr);
    EXPECT_NE(texture.explicitView, nullptr);
    EXPECT_NE(buffer.explicitBuffer, nullptr);
    EXPECT_EQ(texture.explicitTexture->GetDebugName(), "TransientSceneColor");
    EXPECT_EQ(texture.explicitView->GetDebugName(), "TransientSceneColorView");
    EXPECT_EQ(buffer.explicitBuffer->GetDebugName(), "TransientLightList");

    texture.destroy(textureDesc, &executionContext);
    buffer.destroy(bufferDesc, &executionContext);
    EXPECT_EQ(texture.explicitTexture, nullptr);
    EXPECT_EQ(texture.explicitView, nullptr);
    EXPECT_EQ(buffer.explicitBuffer, nullptr);
    EXPECT_FALSE(transientTexture.expired());
    EXPECT_FALSE(transientTextureView.expired());
    EXPECT_FALSE(transientBuffer.expired());

    frameContext.resourceStateTracker->RetireTransientResources(frameContext.frameIndex - 1u);
    EXPECT_FALSE(transientTexture.expired());
    EXPECT_FALSE(transientTextureView.expired());
    EXPECT_FALSE(transientBuffer.expired());

    frameContext.resourceStateTracker->RetireTransientResources(frameContext.frameIndex);
    trackerStats = frameContext.resourceStateTracker->GetStats();
    EXPECT_EQ(trackerStats.retiredTransientTextures, 1u);
    EXPECT_EQ(trackerStats.retiredTransientBuffers, 1u);
    EXPECT_TRUE(transientTexture.expired());
    EXPECT_TRUE(transientTextureView.expired());
    EXPECT_TRUE(transientBuffer.expired());
}

TEST(FrameGraphSceneTargetsTests, DynamicUniformFrameGraphBuffersDoNotRequestCopyDstUsage)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 8u;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::FrameGraph::FrameGraphExecutionContext executionContext{
        driver,
        explicitDevice.get(),
        nullptr,
        &frameContext
    };

    NLS::Render::FrameGraph::FrameGraphBuffer uniformBuffer;
    NLS::Render::FrameGraph::FrameGraphBuffer::Desc uniformDesc;
    uniformDesc.size = 256u;
    uniformDesc.type = NLS::Render::RHI::BufferType::Uniform;
    uniformDesc.usage = NLS::Render::RHI::BufferUsage::DynamicDraw;
    uniformDesc.debugName = "DynamicPassConstants";

    uniformBuffer.create(uniformDesc, &executionContext);

    ASSERT_EQ(explicitDevice->bufferDescs.size(), 1u);
    EXPECT_TRUE(NLS::Render::RHI::HasBufferUsage(
        explicitDevice->bufferDescs.front().usage,
        NLS::Render::RHI::BufferUsageFlags::Uniform));
    EXPECT_FALSE(NLS::Render::RHI::HasBufferUsage(
        explicitDevice->bufferDescs.front().usage,
        NLS::Render::RHI::BufferUsageFlags::CopyDst));
    EXPECT_EQ(
        explicitDevice->bufferDescs.front().memoryUsage,
        NLS::Render::RHI::MemoryUsage::CPUToGPU);

    uniformBuffer.destroy(uniformDesc, &executionContext);
}

TEST(FrameGraphSceneTargetsTests, DeferredSceneGraphImportsGBufferResourcesWithStableDebugNames)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> attachments(3u);
    attachments[0].format = NLS::Render::RHI::TextureFormat::RGBA8;
    attachments[1].format = NLS::Render::RHI::TextureFormat::RGBA8;
    attachments[2].format = NLS::Render::RHI::TextureFormat::RGBA8;
    NLS::Render::Buffers::MultiFramebuffer gBuffer(320u, 180u, attachments, true);

    auto albedoTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        gBuffer.GetExplicitColorTextureHandles()[0],
        320u,
        180u);
    auto normalTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        gBuffer.GetExplicitColorTextureHandles()[1],
        320u,
        180u);
    auto materialTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        gBuffer.GetExplicitColorTextureHandles()[2],
        320u,
        180u);
    auto depthTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        gBuffer.GetExplicitDepthTextureHandle(),
        320u,
        180u);

    NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest preparedResources;
    preparedResources.gBuffer = &gBuffer;
    preparedResources.gbufferAlbedoTexture = albedoTexture.get();
    preparedResources.gbufferNormalTexture = normalTexture.get();
    preparedResources.gbufferMaterialTexture = materialTexture.get();
    preparedResources.gbufferDepthTexture = depthTexture.get();

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;
    const auto resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
        frameGraph,
        blackboard,
        frameDescriptor,
        preparedResources);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    const auto preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
        resourceRequest,
        lightGridContext);

    EXPECT_EQ(
        frameGraph.getDescriptor<NLS::Render::FrameGraph::FrameGraphTexture>(
            preparedGraph.resources.gbufferAlbedo).debugName,
        "GBufferAlbedo");
    EXPECT_EQ(
        frameGraph.getDescriptor<NLS::Render::FrameGraph::FrameGraphTexture>(
            preparedGraph.resources.gbufferNormal).debugName,
        "GBufferNormal");
    EXPECT_EQ(
        frameGraph.getDescriptor<NLS::Render::FrameGraph::FrameGraphTexture>(
            preparedGraph.resources.gbufferMaterial).debugName,
        "GBufferMaterial");
    EXPECT_EQ(
        frameGraph.getDescriptor<NLS::Render::FrameGraph::FrameGraphTexture>(
            preparedGraph.resources.gbufferDepth).debugName,
        "GBufferDepth");
}

TEST(FrameGraphSceneTargetsTests, FrameGraphExecutionContextCommitsTrackedBarriersOnlyWhenStateChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 23u;
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::FrameGraph::FrameGraphExecutionContext executionContext{
        driver,
        explicitDevice.get(),
        commandBuffer.get(),
        &frameContext
    };

    NLS::Render::FrameGraph::FrameGraphTexture texture;
    NLS::Render::FrameGraph::FrameGraphTexture::Desc textureDesc;
    textureDesc.extent.width = 128u;
    textureDesc.extent.height = 72u;
    textureDesc.extent.depth = 1u;
    textureDesc.arrayLayers = 1u;
    textureDesc.mipLevels = 1u;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    texture.create(textureDesc, &executionContext);

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 1u;

    texture.preRead(textureDesc, 0u, &executionContext);
    ASSERT_EQ(commandBuffer->barrierCalls, 1u);
    auto trackedTextureState = frameContext.resourceStateTracker->GetTextureState(
        texture.explicitTexture,
        fullRange);
    ASSERT_TRUE(trackedTextureState.has_value());
    EXPECT_EQ(trackedTextureState->state, NLS::Render::RHI::ResourceState::ShaderRead);

    texture.preRead(textureDesc, 0u, &executionContext);
    EXPECT_EQ(commandBuffer->barrierCalls, 1u);

    texture.preWrite(textureDesc, 0u, &executionContext);
    ASSERT_EQ(commandBuffer->barrierCalls, 2u);
    trackedTextureState = frameContext.resourceStateTracker->GetTextureState(
        texture.explicitTexture,
        fullRange);
    ASSERT_TRUE(trackedTextureState.has_value());
    EXPECT_EQ(trackedTextureState->state, NLS::Render::RHI::ResourceState::RenderTarget);
}

TEST(FrameGraphSceneTargetsTests, VisibilityTransitionsPreferTrackerStateOverResourceReportedState)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 41u;
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "TrackedVisibilityTexture";
    textureDesc.extent = { 64u, 64u, 1u };
    auto texture = std::make_shared<TestTexture>(textureDesc);
    texture->reportedState = NLS::Render::RHI::ResourceState::ShaderRead;

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHIBarrierDesc trackedState;
    trackedState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::RenderTarget,
        fullRange,
        NLS::Render::RHI::PipelineStageMask::None,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::AccessMask::None,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });
    frameContext.resourceStateTracker->Commit(trackedState);

    NLS::Render::Context::RenderPassCommandInput visibilityInput;
    visibilityInput.textureVisibilityTransitions.push_back({
        texture,
        fullRange,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    ASSERT_TRUE(NLS::Render::Context::Detail::RecordResourceVisibilityTransitions(
        *commandBuffer,
        visibilityInput,
        &frameContext));

    ASSERT_EQ(commandBuffer->barrierCalls, 1u);
    ASSERT_EQ(commandBuffer->barrierHistory.size(), 1u);
    ASSERT_EQ(commandBuffer->barrierHistory[0].textureBarriers.size(), 1u);
    EXPECT_EQ(
        commandBuffer->barrierHistory[0].textureBarriers[0].before,
        NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(
        commandBuffer->barrierHistory[0].textureBarriers[0].after,
        NLS::Render::RHI::ResourceState::ShaderRead);
}

TEST(FrameGraphSceneTargetsTests, BeginPassAttachmentTransitionsPreferTrackerStateOverResourceReportedState)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 42u;
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "TrackedAttachmentTexture";
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.mipLevels = 1u;
    textureDesc.arrayLayers = 1u;
    auto texture = std::make_shared<TestTexture>(textureDesc);
    texture->reportedState = NLS::Render::RHI::ResourceState::ShaderRead;

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.debugName = "TrackedAttachmentView";
    viewDesc.subresourceRange.baseMipLevel = 0u;
    viewDesc.subresourceRange.mipLevelCount = 1u;
    viewDesc.subresourceRange.baseArrayLayer = 0u;
    viewDesc.subresourceRange.arrayLayerCount = 1u;
    auto view = std::make_shared<TestTextureView>(texture, viewDesc);

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHIBarrierDesc trackedState;
    trackedState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::Present,
        fullRange,
        NLS::Render::RHI::PipelineStageMask::Present,
        NLS::Render::RHI::PipelineStageMask::Present,
        NLS::Render::RHI::AccessMask::Present,
        NLS::Render::RHI::AccessMask::Present
    });
    frameContext.resourceStateTracker->Commit(trackedState);

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.debugName = "TrackedAttachmentPass";
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.usesColorAttachment = true;
    passInput.colorAttachmentViews.push_back(view);

    ASSERT_TRUE(NLS::Render::Context::Detail::BeginPassCommandPlan(
        *commandBuffer,
        nullptr,
        nullptr,
        passInput,
        &frameContext));

    ASSERT_EQ(commandBuffer->barrierCalls, 1u);
    ASSERT_EQ(commandBuffer->barrierHistory.size(), 1u);
    ASSERT_EQ(commandBuffer->barrierHistory[0].textureBarriers.size(), 1u);
    EXPECT_EQ(
        commandBuffer->barrierHistory[0].textureBarriers[0].before,
        NLS::Render::RHI::ResourceState::Present);
    EXPECT_EQ(
        commandBuffer->barrierHistory[0].textureBarriers[0].after,
        NLS::Render::RHI::ResourceState::RenderTarget);

    const auto trackedTextureState = frameContext.resourceStateTracker->GetTextureState(texture, fullRange);
    ASSERT_TRUE(trackedTextureState.has_value());
    EXPECT_EQ(trackedTextureState->state, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(commandBuffer->beginRenderPassCalls, 1u);
    EXPECT_TRUE(commandBuffer->lastRenderPassDesc.attachmentsRequireExternalStateTransitions);
}

TEST(FrameGraphSceneTargetsTests, EndSwapchainPassTransitionsTrackedBackbufferToPresent)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 43u;
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "TrackedBackbufferTexture";
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.mipLevels = 1u;
    textureDesc.arrayLayers = 1u;
    auto texture = std::make_shared<TestTexture>(textureDesc);

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.debugName = "TrackedBackbufferView";
    viewDesc.subresourceRange.baseMipLevel = 0u;
    viewDesc.subresourceRange.mipLevelCount = 1u;
    viewDesc.subresourceRange.baseArrayLayer = 0u;
    viewDesc.subresourceRange.arrayLayerCount = 1u;
    frameContext.swapchainBackbufferView = std::make_shared<TestTextureView>(texture, viewDesc);

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHIBarrierDesc trackedState;
    trackedState.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::RenderTarget,
        fullRange,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });
    frameContext.resourceStateTracker->Commit(trackedState);

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.targetsSwapchain = true;

    NLS::Render::Context::Detail::EndPassCommandPlan(
        *commandBuffer,
        &passInput,
        &frameContext);

    ASSERT_EQ(commandBuffer->barrierCalls, 1u);
    ASSERT_EQ(commandBuffer->barrierHistory.size(), 1u);
    ASSERT_EQ(commandBuffer->barrierHistory[0].textureBarriers.size(), 1u);
    EXPECT_EQ(
        commandBuffer->barrierHistory[0].textureBarriers[0].before,
        NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(
        commandBuffer->barrierHistory[0].textureBarriers[0].after,
        NLS::Render::RHI::ResourceState::Present);

    const auto trackedTextureState = frameContext.resourceStateTracker->GetTextureState(texture, fullRange);
    ASSERT_TRUE(trackedTextureState.has_value());
    EXPECT_EQ(trackedTextureState->state, NLS::Render::RHI::ResourceState::Present);
}

TEST(FrameGraphSceneTargetsTests, EndExternalAttachmentPassTransitionsTrackedColorToShaderRead)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 46u;
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "ExternalSceneColor";
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.mipLevels = 1u;
    textureDesc.arrayLayers = 1u;
    textureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto texture = std::make_shared<TestTexture>(textureDesc);

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.debugName = "ExternalSceneColorView";
    viewDesc.format = textureDesc.format;
    viewDesc.subresourceRange.baseMipLevel = 0u;
    viewDesc.subresourceRange.mipLevelCount = 1u;
    viewDesc.subresourceRange.baseArrayLayer = 0u;
    viewDesc.subresourceRange.arrayLayerCount = 1u;
    auto view = std::make_shared<TestTextureView>(texture, viewDesc);

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.debugName = "TrackedExternalColorPass";
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.usesColorAttachment = true;
    passInput.colorAttachmentViews.push_back(view);

    ASSERT_TRUE(NLS::Render::Context::Detail::BeginPassCommandPlan(
        *commandBuffer,
        nullptr,
        nullptr,
        passInput,
        &frameContext));
    NLS::Render::Context::Detail::EndPassCommandPlan(
        *commandBuffer,
        &passInput,
        &frameContext);

    ASSERT_GE(commandBuffer->barrierHistory.size(), 2u);
    const auto& endBarrier = commandBuffer->barrierHistory.back();
    ASSERT_EQ(endBarrier.textureBarriers.size(), 1u);
    EXPECT_EQ(endBarrier.textureBarriers[0].texture, texture);
    EXPECT_EQ(endBarrier.textureBarriers[0].before, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(endBarrier.textureBarriers[0].after, NLS::Render::RHI::ResourceState::ShaderRead);

    const auto trackedTextureState =
        frameContext.resourceStateTracker->GetTextureState(texture, viewDesc.subresourceRange);
    ASSERT_TRUE(trackedTextureState.has_value());
    EXPECT_EQ(trackedTextureState->state, NLS::Render::RHI::ResourceState::ShaderRead);
}

TEST(FrameGraphSceneTargetsTests, RendererOutputPassTransitionsExternalFramebufferColorToShaderRead)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 47u;
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::Buffers::Framebuffer outputBuffer(64u, 64u);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    NLS::Render::Core::CompositeRenderer renderer(driver);
    renderer.BeginFrame(frameDescriptor);
    const bool startedPass = renderer.BeginOutputRenderPass(
        frameDescriptor.renderWidth,
        frameDescriptor.renderHeight,
        true,
        true,
        true);
    ASSERT_TRUE(startedPass);
    renderer.EndOutputRenderPass(startedPass);
    renderer.EndFrame();

    const auto colorTexture = outputBuffer.GetExplicitTextureHandle();
    ASSERT_NE(colorTexture, nullptr);
    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = colorTexture->GetDesc().mipLevels;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = colorTexture->GetDesc().arrayLayers;
    const auto trackedTextureState =
        frameContext.resourceStateTracker->GetTextureState(colorTexture, fullRange);
    ASSERT_TRUE(trackedTextureState.has_value());
    EXPECT_EQ(trackedTextureState->state, NLS::Render::RHI::ResourceState::ShaderRead);
}

TEST(FrameGraphSceneTargetsTests, ApplyExternalSceneOutputAttachmentsOnlyTouchesSelectedPassKinds)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;

    NLS::Render::Context::RenderPassCommandInput gbufferPass;
    gbufferPass.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    gbufferPass.usesColorAttachment = true;
    gbufferPass.usesDepthStencilAttachment = true;

    NLS::Render::Context::RenderPassCommandInput lightingPass;
    lightingPass.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPass.usesColorAttachment = true;

    package.passCommandInputs.push_back(gbufferPass);
    package.passCommandInputs.push_back(lightingPass);

    const auto applied = NLS::Render::FrameGraph::ApplyExternalSceneOutputAttachments(
        package,
        frameDescriptor,
        "SceneOutputColorView",
        "SceneOutputDepthView",
        { NLS::Render::Context::RenderPassCommandKind::Lighting });

    EXPECT_TRUE(applied);
    EXPECT_TRUE(package.passCommandInputs[0].colorAttachmentViews.empty());
    EXPECT_EQ(package.passCommandInputs[0].depthStencilAttachmentView, nullptr);
    ASSERT_FALSE(package.passCommandInputs[1].colorAttachmentViews.empty());
    EXPECT_NE(package.passCommandInputs[1].colorAttachmentViews.front(), nullptr);
    EXPECT_EQ(package.passCommandInputs[1].depthStencilAttachmentView, nullptr);
}

TEST(FrameGraphSceneTargetsTests, ApplyThreadedExecutionPlanRebuildsPackageCountsFromPassRoles)
{
    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    package.recordedDrawCommands.resize(3u);

    NLS::Render::FrameGraph::ThreadedRenderSceneExecutionPlan plan;

    NLS::Render::Context::RenderPassCommandInput gbufferPass;
    gbufferPass.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    gbufferPass.drawCount = 2u;
    gbufferPass.usesColorAttachment = true;
    gbufferPass.usesDepthStencilAttachment = true;
    plan.passes.push_back(MakeThreadedPassPlan(
        gbufferPass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
        "",
        2u));

    NLS::Render::Context::RenderPassCommandInput lightingPass;
    lightingPass.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPass.drawCount = 1u;
    lightingPass.usesColorAttachment = true;
    plan.passes.push_back(MakeThreadedPassPlan(
        lightingPass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "",
        1u));

    NLS::Render::FrameGraph::ApplyThreadedRenderSceneExecutionPlan(package, plan);

    EXPECT_EQ(package.passPlanCount, 2u);
    EXPECT_EQ(package.opaqueDrawCount, 2u);
    EXPECT_EQ(package.visibleDrawCount, 3u);
    EXPECT_TRUE(package.hasOpaquePass);
    EXPECT_FALSE(package.hasTransparentPass);
    EXPECT_EQ(package.drawCommandCount, 3u);
    ASSERT_EQ(package.passCommandInputs.size(), 2u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
}

TEST(FrameGraphSceneTargetsTests, ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits)
{
    NLS::Render::Context::RenderScenePackage package;

    NLS::Render::FrameGraph::ThreadedRenderSceneExecutionPlan plan;

    NLS::Render::Context::RenderPassCommandInput opaquePass;
    opaquePass.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePass.debugName = "ForwardOpaque";
    opaquePass.drawCount = 1u;

    NLS::Render::Context::RenderPassCommandInput computePlannedPass;
    computePlannedPass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePlannedPass.debugName = "LightGridCompact";

    plan.passes.push_back(MakeThreadedPassPlan(
        computePlannedPass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "LightGridCompact",
        0u,
        NLS::Render::RHI::QueueType::Compute,
        NLS::Render::Context::QueueDependencyPolicy::None));
    plan.passes.push_back(MakeThreadedPassPlan(
        opaquePass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
        "ForwardOpaque",
        1u));
    plan.dependencies = NLS::Render::FrameGraph::BuildThreadedRenderSceneDependencyEdges(plan.passes);

    NLS::Render::Context::RenderPassCommandInput computePass;
    computePass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePass.debugName = "LightGridInjection";

    NLS::Render::Context::ParallelCommandWorkUnit computeWorkUnit;
    computeWorkUnit.debugName = computePass.debugName;
    computeWorkUnit.commandInput = computePass;

    std::vector<NLS::Render::Context::ParallelCommandWorkUnit> additionalWorkUnits;
    additionalWorkUnits.push_back(std::move(computeWorkUnit));

    NLS::Render::FrameGraph::ApplyThreadedRenderSceneExecutionPlan(
        package,
        plan,
        std::move(additionalWorkUnits));

    ASSERT_EQ(package.parallelCommandWorkUnits.size(), 3u);
    EXPECT_EQ(package.parallelCommandWorkUnits[0].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Compute);
    EXPECT_EQ(package.parallelCommandWorkUnits[1].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Compute);
    EXPECT_EQ(package.parallelCommandWorkUnits[2].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(package.parallelCommandWorkUnits[0].workUnitIndex, 0u);
    EXPECT_EQ(package.parallelCommandWorkUnits[1].workUnitIndex, 1u);
    EXPECT_EQ(package.parallelCommandWorkUnits[2].workUnitIndex, 2u);
    EXPECT_EQ(package.parallelCommandWorkUnits[0].submissionOrder, 0u);
    EXPECT_EQ(package.parallelCommandWorkUnits[1].submissionOrder, 1u);
    EXPECT_EQ(package.parallelCommandWorkUnits[2].submissionOrder, 2u);
    EXPECT_FALSE(package.parallelCommandWorkUnits[2].commandInput.dependencySourceWorkUnitIndex.has_value());
    ASSERT_EQ(package.workUnitDependencyEdges.size(), 1u);
    EXPECT_EQ(package.workUnitDependencyEdges[0].sourceWorkUnitIndex, 1u);
    EXPECT_EQ(package.workUnitDependencyEdges[0].targetWorkUnitIndex, 2u);
    ASSERT_EQ(package.parallelCommandWorkUnits[2].incomingDependencyEdges.size(), 1u);
    EXPECT_EQ(package.parallelCommandWorkUnits[2].incomingDependencyEdges[0].sourceWorkUnitIndex, 1u);
    EXPECT_EQ(package.parallelCommandWorkUnits[2].incomingDependencyEdges[0].targetWorkUnitIndex, 2u);
}

TEST(FrameGraphSceneTargetsTests, BuildThreadedExecutionPlanMapsPassMetadataByKind)
{
    std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;

    NLS::Render::Context::RenderPassCommandInput opaquePass;
    opaquePass.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePass.drawCount = 2u;
    passInputs.push_back(opaquePass);

    NLS::Render::Context::RenderPassCommandInput lightingPass;
    lightingPass.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPass.drawCount = 1u;
    passInputs.push_back(lightingPass);

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 2> metadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Opaque,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "ForwardOpaque"),
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Lighting,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
            "DeferredLighting",
            1u)
    }};

    const auto plan = NLS::Render::FrameGraph::BuildThreadedRenderSceneExecutionPlan(passInputs, metadata);

    ASSERT_EQ(plan.passes.size(), 2u);
    EXPECT_EQ(plan.passes[0].role, NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque);
    EXPECT_EQ(plan.passes[0].graphPassName, std::string_view("ForwardOpaque"));
    EXPECT_EQ(plan.passes[0].visibleDrawCountContribution, 2u);
    EXPECT_EQ(plan.passes[1].role, NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary);
    EXPECT_EQ(plan.passes[1].graphPassName, std::string_view("DeferredLighting"));
    EXPECT_EQ(plan.passes[1].visibleDrawCountContribution, 1u);
    ASSERT_EQ(plan.dependencies.size(), 1u);
    EXPECT_EQ(plan.dependencies[0].sourcePassIndex, 0u);
    EXPECT_EQ(plan.dependencies[0].targetPassIndex, 1u);
    EXPECT_EQ(
        plan.dependencies[0].kind,
        NLS::Render::FrameGraph::ThreadedRenderSceneDependencyKind::ResourceVisibility);
}

TEST(FrameGraphSceneTargetsTests, BuildThreadedPassMetadataSequencePreservesPrefixComputePassesWithoutPromotingDependencies)
{
    std::vector<NLS::Render::Context::RecordedComputeDispatchInput> preparedDispatchInputs(2u);
    preparedDispatchInputs[0].debugName = "LightGridInjection";
    preparedDispatchInputs[1].debugName = "LightGridCompact";

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 2> sceneMetadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::GBuffer,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "DeferredGBuffer"),
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Lighting,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
            "DeferredLighting")
    }};

    const auto combinedMetadata = NLS::Render::FrameGraph::BuildThreadedRenderScenePassMetadataSequence(
        NLS::Render::FrameGraph::BuildPreparedComputeDispatchPassMetadata(preparedDispatchInputs),
        sceneMetadata);

    ASSERT_EQ(combinedMetadata.size(), 4u);
    EXPECT_STREQ(combinedMetadata[0].graphPassName, "LightGridInjection");
    EXPECT_STREQ(combinedMetadata[1].graphPassName, "LightGridCompact");
    EXPECT_STREQ(combinedMetadata[2].graphPassName, "DeferredGBuffer");
    EXPECT_STREQ(combinedMetadata[3].graphPassName, "DeferredLighting");
    EXPECT_EQ(combinedMetadata[0].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::None);
    EXPECT_EQ(combinedMetadata[1].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::None);
    EXPECT_EQ(combinedMetadata[2].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::Previous);
}

TEST(FrameGraphSceneTargetsTests, BuildPreparedComputeAndScenePassMetadataCombinesDispatchAndSceneOrder)
{
    std::vector<NLS::Render::Context::RecordedComputeDispatchInput> preparedDispatchInputs(2u);
    preparedDispatchInputs[0].debugName = "LightGridInjection";
    preparedDispatchInputs[1].debugName = "LightGridCompact";

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 2> sceneMetadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Opaque,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "ForwardOpaque"),
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Transparent,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Transparent,
            "ForwardTransparent")
    }};

    const auto combinedMetadata = NLS::Render::FrameGraph::BuildPreparedComputeAndScenePassMetadata(
        preparedDispatchInputs,
        sceneMetadata);

    ASSERT_EQ(combinedMetadata.size(), 4u);
    EXPECT_STREQ(combinedMetadata[0].graphPassName, "LightGridInjection");
    EXPECT_STREQ(combinedMetadata[1].graphPassName, "LightGridCompact");
    EXPECT_STREQ(combinedMetadata[2].graphPassName, "ForwardOpaque");
    EXPECT_STREQ(combinedMetadata[3].graphPassName, "ForwardTransparent");
    EXPECT_EQ(combinedMetadata[2].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::LastCompute);
    EXPECT_EQ(combinedMetadata[3].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::Previous);
}

TEST(FrameGraphSceneTargetsTests, PreparedComputeDispatchMetadataDefaultsToIndependentQueueTransition)
{
    std::vector<NLS::Render::Context::RecordedComputeDispatchInput> preparedDispatchInputs(2u);
    preparedDispatchInputs[0].debugName = "LightGridInjection";
    preparedDispatchInputs[1].debugName = "LightGridCompact";

    const auto metadata = NLS::Render::FrameGraph::BuildPreparedComputeDispatchPassMetadata(preparedDispatchInputs);

    ASSERT_EQ(metadata.size(), 2u);
    EXPECT_EQ(metadata[0].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::None);
    EXPECT_EQ(metadata[1].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::None);
    EXPECT_EQ(metadata[0].queueType, NLS::Render::RHI::QueueType::Compute);
    EXPECT_EQ(metadata[1].queueType, NLS::Render::RHI::QueueType::Compute);
}

TEST(FrameGraphSceneTargetsTests, PreparedComputeThreadedPassInputCarriesShaderReadAccessExports)
{
    auto testBuffer = std::make_shared<TestBuffer>(NLS::Render::RHI::RHIBufferDesc{});
    NLS::Render::Context::RecordedComputeDispatchInput dispatchInput;
    dispatchInput.debugName = "LightGridCompact";
    dispatchInput.shaderReadBuffersAfter.push_back(testBuffer);

    const auto preparedSource = NLS::Render::FrameGraph::BuildPreparedComputeDispatchSource({ dispatchInput });
    NLS::Render::FrameGraph::CompiledThreadedRenderSceneGraphPass compiledPass;
    compiledPass.metadata = preparedSource.metadata.front();
    compiledPass.outputExecution.renderWidth = 64u;
    compiledPass.outputExecution.renderHeight = 64u;

    NLS::Render::Context::RenderPassCommandInput passInput;
    ASSERT_TRUE(NLS::Render::FrameGraph::TryBuildPreparedComputeDispatchThreadedPassInput(
        preparedSource,
        compiledPass,
        passInput));
    EXPECT_TRUE(passInput.bufferResourceAccesses.empty());
    ASSERT_EQ(passInput.exportedBufferVisibilityTransitions.size(), 1u);
    EXPECT_EQ(passInput.exportedBufferVisibilityTransitions[0].buffer, testBuffer);
    EXPECT_EQ(passInput.exportedBufferVisibilityTransitions[0].before, NLS::Render::RHI::ResourceState::ShaderWrite);
    EXPECT_EQ(passInput.exportedBufferVisibilityTransitions[0].after, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(passInput.exportedBufferVisibilityTransitions[0].destinationAccess, NLS::Render::RHI::AccessMask::ShaderRead);
}

TEST(FrameGraphSceneTargetsTests, PreparedComputeLifecycleBufferAccessesDoNotTripConflictValidation)
{
    auto testBuffer = std::make_shared<TestBuffer>(NLS::Render::RHI::RHIBufferDesc{});
    NLS::Render::Context::RecordedComputeDispatchInput dispatchInput;
    dispatchInput.debugName = "LightGridCompact";
    dispatchInput.shaderWriteBuffersBefore.push_back(testBuffer);
    dispatchInput.uavBarrierBuffersAfter.push_back(testBuffer);
    dispatchInput.shaderReadBuffersAfter.push_back(testBuffer);

    const auto preparedSource = NLS::Render::FrameGraph::BuildPreparedComputeDispatchSource({ dispatchInput });
    NLS::Render::FrameGraph::CompiledThreadedRenderSceneGraphPass compiledPass;
    compiledPass.metadata = preparedSource.metadata.front();
    compiledPass.outputExecution.renderWidth = 64u;
    compiledPass.outputExecution.renderHeight = 64u;

    NLS::Render::Context::RenderPassCommandInput passInput;
    ASSERT_TRUE(NLS::Render::FrameGraph::TryBuildPreparedComputeDispatchThreadedPassInput(
        preparedSource,
        compiledPass,
        passInput));
    EXPECT_TRUE(passInput.bufferResourceAccesses.empty());
    ASSERT_EQ(passInput.bufferVisibilityTransitions.size(), 2u);
    ASSERT_EQ(passInput.exportedBufferVisibilityTransitions.size(), 1u);
    EXPECT_EQ(passInput.bufferVisibilityTransitions[0].buffer, testBuffer);
    EXPECT_EQ(passInput.bufferVisibilityTransitions[0].after, NLS::Render::RHI::ResourceState::ShaderWrite);
    EXPECT_EQ(passInput.bufferVisibilityTransitions[1].buffer, testBuffer);
    EXPECT_EQ(passInput.bufferVisibilityTransitions[1].before, NLS::Render::RHI::ResourceState::ShaderWrite);
    EXPECT_EQ(passInput.bufferVisibilityTransitions[1].after, NLS::Render::RHI::ResourceState::ShaderWrite);
    EXPECT_EQ(passInput.exportedBufferVisibilityTransitions[0].buffer, testBuffer);
    EXPECT_EQ(passInput.exportedBufferVisibilityTransitions[0].before, NLS::Render::RHI::ResourceState::ShaderWrite);
    EXPECT_EQ(passInput.exportedBufferVisibilityTransitions[0].after, NLS::Render::RHI::ResourceState::ShaderRead);

    const auto validation = NLS::Render::FrameGraph::ValidateThreadedRenderSceneExecutionInputs(
        std::vector<NLS::Render::Context::RenderPassCommandInput>{ passInput },
        preparedSource.metadata);

    EXPECT_FALSE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::ConflictingBufferResourceAccess));
}

TEST(FrameGraphSceneTargetsTests, ThreadedResourceDependencyUsesLastWriteAccessState)
{
    auto testBuffer = std::make_shared<TestBuffer>(NLS::Render::RHI::RHIBufferDesc{});

    NLS::Render::Context::RenderPassCommandInput computePass;
    computePass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePass.debugName = "LightGridCompact";
    computePass.bufferResourceAccesses.push_back({
        testBuffer,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });
    computePass.bufferResourceAccesses.push_back({
        testBuffer,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    NLS::Render::Context::RenderPassCommandInput graphicsPass;
    graphicsPass.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    graphicsPass.debugName = "ForwardOpaque";
    graphicsPass.bufferResourceAccesses.push_back({
        testBuffer,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    NLS::Render::FrameGraph::ThreadedRenderSceneExecutionPlan plan;
    plan.passes.push_back(MakeThreadedPassPlan(
        computePass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "LightGridCompact",
        0u,
        NLS::Render::RHI::QueueType::Compute,
        NLS::Render::Context::QueueDependencyPolicy::None));
    plan.passes.push_back(MakeThreadedPassPlan(
        graphicsPass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
        "ForwardOpaque",
        1u,
        NLS::Render::RHI::QueueType::Graphics,
        NLS::Render::Context::QueueDependencyPolicy::LastCompute));

    plan.dependencies = NLS::Render::FrameGraph::BuildThreadedRenderSceneDependencyEdges(plan.passes);

    ASSERT_EQ(plan.dependencies.size(), 1u);
    ASSERT_TRUE(plan.dependencies[0].sourceBufferAccess.has_value());
    EXPECT_EQ(plan.dependencies[0].sourceBufferAccess->state, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(plan.dependencies[0].sourceBufferAccess->access, NLS::Render::RHI::AccessMask::ShaderRead);
}

TEST(FrameGraphSceneTargetsTests, BuildThreadedExecutionPlanEmitsResourceAccessDependencyEdges)
{
    auto testTexture = std::make_shared<TestTexture>(NLS::Render::RHI::RHITextureDesc{});
    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 1u;

    std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;

    NLS::Render::Context::RenderPassCommandInput gbufferPass;
    gbufferPass.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    gbufferPass.drawCount = 1u;
    gbufferPass.textureResourceAccesses.push_back({
        testTexture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::RenderTarget,
        NLS::Render::RHI::PipelineStageMask::AllGraphics,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });
    passInputs.push_back(gbufferPass);

    NLS::Render::Context::RenderPassCommandInput lightingPass;
    lightingPass.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPass.drawCount = 1u;
    lightingPass.textureResourceAccesses.push_back({
        testTexture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    passInputs.push_back(lightingPass);

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 2> metadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::GBuffer,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "DeferredGBuffer"),
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Lighting,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
            "DeferredLighting",
            1u)
    }};

    const auto plan = NLS::Render::FrameGraph::BuildThreadedRenderSceneExecutionPlan(passInputs, metadata);

    ASSERT_EQ(plan.dependencies.size(), 1u);
    EXPECT_EQ(plan.dependencies[0].sourcePassIndex, 0u);
    EXPECT_EQ(plan.dependencies[0].targetPassIndex, 1u);
    EXPECT_EQ(
        plan.dependencies[0].kind,
        NLS::Render::FrameGraph::ThreadedRenderSceneDependencyKind::ResourceVisibility);
    EXPECT_EQ(
        plan.dependencies[0].resourceKind,
        NLS::Render::FrameGraph::ThreadedRenderSceneDependencyResourceKind::Texture);
    ASSERT_TRUE(plan.dependencies[0].sourceTextureAccess.has_value());
    ASSERT_TRUE(plan.dependencies[0].targetTextureAccess.has_value());
    EXPECT_EQ(plan.dependencies[0].sourceTextureAccess->texture, testTexture);
    EXPECT_EQ(plan.dependencies[0].targetTextureAccess->texture, testTexture);
}

TEST(FrameGraphSceneTargetsTests, DerivedVisibilityUsesExportedAttachmentEndStateForSampledDepth)
{
    auto depthTexture = std::make_shared<TestTexture>(NLS::Render::RHI::RHITextureDesc{});
    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 1u;

    std::vector<NLS::Render::Context::ParallelCommandWorkUnit> workUnits(2);

    auto& gbuffer = workUnits[0];
    gbuffer.workUnitIndex = 0u;
    gbuffer.commandInput.textureResourceAccesses.push_back({
        depthTexture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::DepthWrite,
        NLS::Render::RHI::PipelineStageMask::DepthStencil,
        NLS::Render::RHI::AccessMask::DepthStencilWrite
    });
    gbuffer.commandInput.exportedTextureVisibilityTransitions.push_back({
        depthTexture,
        fullRange,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::DepthRead,
        NLS::Render::RHI::PipelineStageMask::DepthStencil,
        NLS::Render::RHI::PipelineStageMask::DepthStencil | NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::DepthStencilRead | NLS::Render::RHI::AccessMask::DepthStencilWrite,
        NLS::Render::RHI::AccessMask::DepthStencilRead | NLS::Render::RHI::AccessMask::ShaderRead
    });

    auto& lighting = workUnits[1];
    lighting.workUnitIndex = 1u;
    lighting.commandInput.dependencySourceWorkUnitIndex = 0u;
    lighting.commandInput.textureResourceAccesses.push_back({
        depthTexture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    NLS::Render::Context::Detail::PopulateVisibilityTransitionsFromResourceUsage(workUnits);

    ASSERT_EQ(lighting.commandInput.textureVisibilityTransitions.size(), 1u);
    const auto& transition = lighting.commandInput.textureVisibilityTransitions.front();
    EXPECT_EQ(transition.before, NLS::Render::RHI::ResourceState::DepthRead);
    EXPECT_EQ(transition.after, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(
        transition.sourceStages,
        NLS::Render::RHI::PipelineStageMask::DepthStencil | NLS::Render::RHI::PipelineStageMask::FragmentShader);
    EXPECT_EQ(
        transition.sourceAccess,
        NLS::Render::RHI::AccessMask::DepthStencilRead | NLS::Render::RHI::AccessMask::ShaderRead);
    EXPECT_TRUE(lighting.commandInput.requiresDependencyVisibility);
}

TEST(FrameGraphSceneTargetsTests, ValidateThreadedExecutionInputsReportsPassAndResourceContractErrors)
{
    std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;

    NLS::Render::Context::RenderPassCommandInput gbufferPass;
    gbufferPass.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    gbufferPass.bufferResourceAccesses.push_back({
        nullptr,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    passInputs.push_back(gbufferPass);

    NLS::Render::Context::RenderPassCommandInput lightingPass;
    lightingPass.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPass.textureResourceAccesses.push_back({
        nullptr,
        {},
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    passInputs.push_back(lightingPass);

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> metadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::GBuffer,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "")
    }};

    const auto validation =
        NLS::Render::FrameGraph::ValidateThreadedRenderSceneExecutionInputs(passInputs, metadata);

    EXPECT_TRUE(validation.HasErrors());
    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::EmptyGraphPassName));
    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::NullBufferResourceAccess));
    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::NullTextureResourceAccess));
    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::MissingPassMetadata));
}

TEST(FrameGraphSceneTargetsTests, ValidateThreadedExecutionInputsAcceptsCompletePassAndResourceContracts)
{
    auto testTexture = std::make_shared<TestTexture>(NLS::Render::RHI::RHITextureDesc{});
    auto testBuffer = std::make_shared<TestBuffer>(NLS::Render::RHI::RHIBufferDesc{});
    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 1u;

    std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;

    NLS::Render::Context::RenderPassCommandInput computePass;
    computePass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePass.bufferResourceAccesses.push_back({
        testBuffer,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });
    passInputs.push_back(computePass);

    NLS::Render::Context::RenderPassCommandInput lightingPass;
    lightingPass.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPass.bufferResourceAccesses.push_back({
        testBuffer,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    lightingPass.textureResourceAccesses.push_back({
        testTexture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    passInputs.push_back(lightingPass);

    std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 2> metadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Compute,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
            "LightGridCompact",
            0u,
            NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded,
            NLS::Render::RHI::QueueType::Compute,
            NLS::Render::Context::QueueDependencyPolicy::None),
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Lighting,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
            "DeferredLighting")
    }};
    metadata[0].propagatesColorOutput = false;
    metadata[0].propagatesDepthOutput = false;

    const auto validation =
        NLS::Render::FrameGraph::ValidateThreadedRenderSceneExecutionInputs(passInputs, metadata);

    EXPECT_FALSE(validation.HasErrors());
    EXPECT_TRUE(validation.diagnostics.empty());
}

TEST(FrameGraphSceneTargetsTests, UE427RdgPassContractRetainsSideEffectsAndReportsUndeclaredResourceUse)
{
    NLS::Render::FrameGraph::RDGPassContract sideEffectPass;
    sideEffectPass.name = "LightGridInjection";
    sideEffectPass.queueType = NLS::Render::RHI::QueueType::Compute;
    sideEffectPass.hasSideEffect = true;

    EXPECT_TRUE(NLS::Render::FrameGraph::RDGPassMustExecute(sideEffectPass));
    EXPECT_FALSE(NLS::Render::FrameGraph::RDGPassCanCull(sideEffectPass));

    NLS::Render::FrameGraph::RDGPassContract invalidPass;
    invalidPass.name = "DeferredLighting";
    invalidPass.queueType = NLS::Render::RHI::QueueType::Graphics;
    invalidPass.declaredResourceNames.push_back("SceneColor");
    invalidPass.usedResourceNames.push_back("SceneColor");
    invalidPass.usedResourceNames.push_back("LightGridLinks");

    const auto validation = NLS::Render::FrameGraph::ValidateRDGPassContract(invalidPass);

    EXPECT_TRUE(validation.HasErrors());
    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::UndeclaredRDGResourceUse));
    ASSERT_FALSE(validation.diagnostics.empty());
    EXPECT_NE(validation.diagnostics.front().message.find("LightGridLinks"), std::string::npos);
}

TEST(FrameGraphSceneTargetsTests, UE427RdgPassContractCanBeBuiltFromThreadedPassDeclaredResourceAccesses)
{
    auto sceneColor = std::make_shared<TestTexture>(NLS::Render::RHI::RHITextureDesc{});
    auto lightLinks = std::make_shared<TestBuffer>(NLS::Render::RHI::RHIBufferDesc{});

    NLS::Render::Context::RenderPassCommandInput lightingPass;
    lightingPass.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPass.textureResourceAccesses.push_back({
        sceneColor,
        {},
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    lightingPass.bufferResourceAccesses.push_back({
        lightLinks,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    auto metadata = MakeThreadedPassMetadata(
        NLS::Render::Context::RenderPassCommandKind::Lighting,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "DeferredLighting");
    metadata.propagatesColorOutput = false;
    metadata.propagatesDepthOutput = false;

    const auto contract = NLS::Render::FrameGraph::BuildRDGPassContract(lightingPass, metadata);

    EXPECT_EQ(contract.name, "DeferredLighting");
    EXPECT_TRUE(contract.hasSideEffect);
    EXPECT_FALSE(NLS::Render::FrameGraph::RDGPassCanCull(contract));
    EXPECT_EQ(contract.declaredResourceNames.size(), 2u);
    EXPECT_EQ(contract.usedResourceNames, contract.declaredResourceNames);
    EXPECT_FALSE(NLS::Render::FrameGraph::ValidateRDGPassContract(contract).HasErrors());
}

TEST(FrameGraphSceneTargetsTests, UE427ParallelDrawCommandBatchesPromoteComputeToGraphicsDependencyEdges)
{
    auto visibilityBuffer = std::make_shared<TestBuffer>(NLS::Render::RHI::RHIBufferDesc{});

    NLS::Render::FrameGraph::ThreadedRenderSceneExecutionPlan plan;
    NLS::Render::Context::RenderPassCommandInput computePass;
    computePass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePass.queueType = NLS::Render::RHI::QueueType::Compute;
    computePass.debugName = "VisibilityCompute";
    computePass.bufferResourceAccesses.push_back({
        visibilityBuffer,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });

    NLS::Render::Context::RenderPassCommandInput opaquePass;
    opaquePass.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePass.queueType = NLS::Render::RHI::QueueType::Graphics;
    opaquePass.debugName = "ForwardOpaque";
    opaquePass.drawCount = 1u;
    opaquePass.bufferResourceAccesses.push_back({
        visibilityBuffer,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    plan.passes.push_back(MakeThreadedPassPlan(
        computePass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "VisibilityCompute",
        0u,
        NLS::Render::RHI::QueueType::Compute,
        NLS::Render::Context::QueueDependencyPolicy::None));
    plan.passes.push_back(MakeThreadedPassPlan(
        opaquePass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
        "ForwardOpaque",
        1u,
        NLS::Render::RHI::QueueType::Graphics,
        NLS::Render::Context::QueueDependencyPolicy::LastCompute));
    plan.dependencies = NLS::Render::FrameGraph::BuildThreadedRenderSceneDependencyEdges(plan.passes);

    NLS::Render::Context::RenderScenePackage package;
    NLS::Render::FrameGraph::ApplyThreadedRenderSceneExecutionPlan(package, plan);

    ASSERT_EQ(package.parallelDrawCommandBatches.size(), 2u);
    EXPECT_EQ(
        package.parallelDrawCommandBatches[0].passRole,
        NLS::Render::Context::ParallelDrawCommandPassRole::Compute);
    EXPECT_EQ(
        package.parallelDrawCommandBatches[1].passRole,
        NLS::Render::Context::ParallelDrawCommandPassRole::Opaque);
    ASSERT_EQ(package.parallelDrawCommandBatches[1].incomingDependencyEdges.size(), 1u);
    EXPECT_EQ(package.parallelDrawCommandBatches[1].incomingDependencyEdges[0].sourceWorkUnitIndex, 0u);
    EXPECT_EQ(package.parallelDrawCommandBatches[1].incomingDependencyEdges[0].targetWorkUnitIndex, 1u);
    EXPECT_EQ(
        package.parallelDrawCommandBatches[1].incomingDependencyEdges[0].resourceKind,
        NLS::Render::Context::ThreadedDependencyResourceKind::Buffer);
}

TEST(FrameGraphSceneTargetsTests, ValidateThreadedExecutionInputsReportsIllegalMetadataAndQueueContracts)
{
    std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;

    NLS::Render::Context::RenderPassCommandInput gbufferPass;
    gbufferPass.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    passInputs.push_back(gbufferPass);

    NLS::Render::Context::RenderPassCommandInput computePass;
    computePass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    passInputs.push_back(computePass);

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 2> metadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::GBuffer,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "InvalidRecordedGBuffer",
            NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount,
            NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded,
            NLS::Render::RHI::QueueType::Graphics,
            NLS::Render::Context::QueueDependencyPolicy::LastCompute),
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Compute,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
            "InvalidCompute",
            0u,
            NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Output,
            NLS::Render::RHI::QueueType::Graphics,
            NLS::Render::Context::QueueDependencyPolicy::None)
    }};

    const auto validation =
        NLS::Render::FrameGraph::ValidateThreadedRenderSceneExecutionInputs(passInputs, metadata);

    EXPECT_TRUE(validation.HasErrors());
    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::RecordedPassPropagatesOutput));
    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::ComputePassUsesNonComputeQueue));
    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::ComputePassPropagatesOutput));
    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::MissingQueueDependencySource));
}

TEST(FrameGraphSceneTargetsTests, ValidateThreadedExecutionInputsReportsResourceConflictsAndInvalidDependencies)
{
    auto testTexture = std::make_shared<TestTexture>(NLS::Render::RHI::RHITextureDesc{});
    auto testBuffer = std::make_shared<TestBuffer>(NLS::Render::RHI::RHIBufferDesc{});

    std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;

    NLS::Render::Context::RenderPassCommandInput computePass;
    computePass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePass.bufferResourceAccesses.push_back({
        testBuffer,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    computePass.bufferResourceAccesses.push_back({
        testBuffer,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });
    computePass.textureResourceAccesses.push_back({
        testTexture,
        {},
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    computePass.textureResourceAccesses.push_back({
        testTexture,
        {},
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });
    passInputs.push_back(computePass);

    std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> metadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Compute,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
            "ConflictingCompute",
            0u,
            NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded,
            NLS::Render::RHI::QueueType::Compute,
            NLS::Render::Context::QueueDependencyPolicy::None)
    }};
    metadata[0].propagatesColorOutput = false;
    metadata[0].propagatesDepthOutput = false;

    const auto validation =
        NLS::Render::FrameGraph::ValidateThreadedRenderSceneExecutionInputs(passInputs, metadata);

    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::ConflictingBufferResourceAccess));
    EXPECT_TRUE(validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::ConflictingTextureResourceAccess));

    NLS::Render::FrameGraph::ThreadedRenderSceneExecutionPlan plan;
    plan.passes.push_back({});
    plan.dependencies.push_back({
        0u,
        0u,
        NLS::Render::FrameGraph::ThreadedRenderSceneDependencyKind::QueueSynchronization,
        NLS::Render::FrameGraph::ThreadedRenderSceneDependencyResourceKind::None
    });
    plan.dependencies.push_back({
        2u,
        0u,
        NLS::Render::FrameGraph::ThreadedRenderSceneDependencyKind::QueueSynchronization,
        NLS::Render::FrameGraph::ThreadedRenderSceneDependencyResourceKind::None
    });

    const auto planValidation =
        NLS::Render::FrameGraph::ValidateThreadedRenderSceneExecutionPlan(plan);

    EXPECT_TRUE(planValidation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::InvalidQueueDependency));
}

TEST(FrameGraphSceneTargetsTests, BuildThreadedExecutionPlanRejectsIllegalMetadataContractsBeforePlanning)
{
    std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;

    NLS::Render::Context::RenderPassCommandInput gbufferPass;
    gbufferPass.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    passInputs.push_back(gbufferPass);

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> metadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::GBuffer,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "InvalidRecordedGBuffer",
            NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount,
            NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded)
    }};

    EXPECT_THROW(
        NLS::Render::FrameGraph::BuildThreadedRenderSceneExecutionPlan(passInputs, metadata),
        std::invalid_argument);
}

TEST(FrameGraphSceneTargetsTests, TryBuildThreadedExecutionPlanReturnsDiagnosticsInsteadOfThrowing)
{
    std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;

    NLS::Render::Context::RenderPassCommandInput gbufferPass;
    gbufferPass.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    passInputs.push_back(gbufferPass);

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> metadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::GBuffer,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "InvalidRecordedGBuffer",
            NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount,
            NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded)
    }};

    const auto result =
        NLS::Render::FrameGraph::TryBuildThreadedRenderSceneExecutionPlan(passInputs, metadata);

    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::RecordedPassPropagatesOutput));
    EXPECT_TRUE(result.plan.passes.empty());
}

TEST(FrameGraphSceneTargetsTests, TryCompileAndApplyThreadedExecutionReportsDiagnosticsWithoutMutatingPackage)
{
    NLS::Render::Context::RenderScenePackage package;
    package.visibleDrawCount = 7u;
    package.hasVisibleDraws = true;

    NLS::Render::Context::RenderPassCommandInput gbufferPass;
    gbufferPass.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    gbufferPass.drawCount = 7u;
    package.passCommandInputs.push_back(gbufferPass);

    NLS::Render::Data::FrameDescriptor frame;
    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> metadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::GBuffer,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "InvalidRecordedGBuffer",
            NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount,
            NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded)
    }};

    const auto result = NLS::Render::FrameGraph::TryCompileAndApplyThreadedRenderSceneExecution(
        package,
        frame,
        -1,
        -1,
        metadata);

    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.validation.ContainsError(
        NLS::Render::FrameGraph::FrameGraphCompileDiagnosticCode::RecordedPassPropagatesOutput));
    EXPECT_EQ(package.visibleDrawCount, 7u);
    EXPECT_TRUE(package.parallelCommandWorkUnits.empty());
}

TEST(FrameGraphSceneTargetsTests, BuildThreadedExecutionPlanRejectsInvalidResourceContractsBeforePlanning)
{
    std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;

    NLS::Render::Context::RenderPassCommandInput lightingPass;
    lightingPass.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPass.textureResourceAccesses.push_back({
        nullptr,
        {},
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    passInputs.push_back(lightingPass);

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> metadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Lighting,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
            "DeferredLighting")
    }};

    EXPECT_THROW(
        NLS::Render::FrameGraph::BuildThreadedRenderSceneExecutionPlan(passInputs, metadata),
        std::invalid_argument);
}

TEST(FrameGraphSceneTargetsTests, ApplyThreadedExecutionPlanMapsMultipleResourceDependencyEdgesToTargetWorkUnit)
{
    NLS::Render::Context::RenderScenePackage package;

    auto testBuffer = std::make_shared<TestBuffer>(NLS::Render::RHI::RHIBufferDesc{});
    auto testTexture = std::make_shared<TestTexture>(NLS::Render::RHI::RHITextureDesc{});
    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 1u;

    NLS::Render::FrameGraph::ThreadedRenderSceneExecutionPlan plan;

    NLS::Render::Context::RenderPassCommandInput computePass;
    computePass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePass.debugName = "ComputeA";
    computePass.queueType = NLS::Render::RHI::QueueType::Compute;
    computePass.bufferResourceAccesses.push_back({
        testBuffer,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });
    plan.passes.push_back(MakeThreadedPassPlan(
        computePass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "ComputeA",
        0u,
        NLS::Render::RHI::QueueType::Compute,
        NLS::Render::Context::QueueDependencyPolicy::None));

    NLS::Render::Context::RenderPassCommandInput gbufferPass;
    gbufferPass.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    gbufferPass.debugName = "GBuffer";
    gbufferPass.textureResourceAccesses.push_back({
        testTexture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::RenderTarget,
        NLS::Render::RHI::PipelineStageMask::AllGraphics,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });
    plan.passes.push_back(MakeThreadedPassPlan(
        gbufferPass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
        "GBuffer",
        1u));

    NLS::Render::Context::RenderPassCommandInput lightingPass;
    lightingPass.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPass.debugName = "Lighting";
    lightingPass.bufferResourceAccesses.push_back({
        testBuffer,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    lightingPass.textureResourceAccesses.push_back({
        testTexture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    plan.passes.push_back(MakeThreadedPassPlan(
        lightingPass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "Lighting",
        1u));

    plan.dependencies = NLS::Render::FrameGraph::BuildThreadedRenderSceneDependencyEdges(plan.passes);

    NLS::Render::FrameGraph::ApplyThreadedRenderSceneExecutionPlan(package, plan);

    ASSERT_EQ(package.parallelCommandWorkUnits.size(), 3u);
    ASSERT_EQ(package.workUnitDependencyEdges.size(), 3u);
    ASSERT_EQ(package.parallelCommandWorkUnits[2].incomingDependencyEdges.size(), 2u);
    EXPECT_EQ(package.parallelCommandWorkUnits[2].incomingDependencyEdges[0].sourceWorkUnitIndex, 0u);
    EXPECT_EQ(package.parallelCommandWorkUnits[2].incomingDependencyEdges[1].sourceWorkUnitIndex, 1u);
    ASSERT_EQ(package.parallelCommandWorkUnits[1].incomingDependencyEdges.size(), 1u);
    EXPECT_EQ(package.parallelCommandWorkUnits[1].incomingDependencyEdges[0].sourceWorkUnitIndex, 0u);
}

TEST(FrameGraphSceneTargetsTests, SceneRenderGraphBuilderExposesExpectedForwardAndDeferredOrdering)
{
    const auto& forwardDescriptors = NLS::Render::FrameGraph::GetForwardScenePassDescriptors();
    ASSERT_EQ(forwardDescriptors.size(), 3u);
    EXPECT_EQ(forwardDescriptors[0].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(forwardDescriptors[1].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(forwardDescriptors[2].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_EQ(forwardDescriptors[0].metadata.role, NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque);
    EXPECT_EQ(forwardDescriptors[1].metadata.role, NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Skybox);

    const auto& deferredDescriptors = NLS::Render::FrameGraph::GetDeferredScenePassDescriptors();
    ASSERT_EQ(deferredDescriptors.size(), 2u);
    EXPECT_EQ(deferredDescriptors[0].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(deferredDescriptors[1].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_FALSE(deferredDescriptors[0].metadata.propagatesColorOutput);
    EXPECT_TRUE(deferredDescriptors[1].metadata.propagatesColorOutput);
}

TEST(FrameGraphSceneTargetsTests, DeferredLightGridCompilationUsesResolvedPassBindingsForPassInputs)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;

    auto placeholder = std::make_shared<TestBindingSet>("PreparedPassBindingPlaceholder");
    auto lightGridBindingSet = std::make_shared<TestBindingSet>("LightGridGraphicsBindingSet");

    package.recordedDrawCommands.resize(2u);
    package.recordedDrawCommands[0].passBindingSet = placeholder;
    package.recordedDrawCommands[1].passBindingSet = placeholder;

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;
    lightGridContext.graphicsPassBindingSet = lightGridBindingSet;

    NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;
    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources);

    ASSERT_EQ(package.passCommandInputs.size(), 2u);
    ASSERT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    ASSERT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    ASSERT_EQ(package.passCommandInputs[0].recordedDrawCommands.size(), 1u);
    ASSERT_EQ(package.passCommandInputs[1].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.recordedDrawCommands[0].passBindingSet, lightGridBindingSet);
    EXPECT_EQ(package.recordedDrawCommands[1].passBindingSet, lightGridBindingSet);
    EXPECT_EQ(package.passCommandInputs[0].recordedDrawCommands[0].passBindingSet, lightGridBindingSet);
    EXPECT_EQ(package.passCommandInputs[1].recordedDrawCommands[0].passBindingSet, lightGridBindingSet);
}

TEST(FrameGraphSceneTargetsTests, LightGridCompileContextCarriesUEForwardLightingResourceContract)
{
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;

    NLS::Render::FrameGraph::PreparedComputeDispatchSource preparedSource;
    preparedSource.dispatchInputs.resize(3u);
    preparedSource.dispatchInputs[0].debugName = "LightGridReset";
    preparedSource.dispatchInputs[1].debugName = "LightGridInjection";
    preparedSource.dispatchInputs[2].debugName = "LightGridCompact";

    auto lightGridBindingSet = std::make_shared<TestBindingSet>("LightGridGraphicsBindingSet");
    const auto context = NLS::Render::FrameGraph::BuildLightGridCompileContext(
        frameDescriptor,
        std::move(preparedSource),
        lightGridBindingSet);

    EXPECT_EQ(context.forwardLightingResources.forwardLightDataUniformBufferName, "ForwardLightDataUniformBuffer");
    EXPECT_EQ(context.forwardLightingResources.forwardLocalLightBufferName, "ForwardLocalLightBuffer");
    EXPECT_EQ(context.forwardLightingResources.numCulledLightsGridName, "NumCulledLightsGrid");
    EXPECT_EQ(context.forwardLightingResources.culledLightDataGridName, "CulledLightDataGrid");
    EXPECT_EQ(context.forwardLightingResources.numCulledLightsGridStride, 2u);
    EXPECT_EQ(context.forwardLightingResources.lightLinkStride, 2u);
    EXPECT_EQ(context.forwardLightingResources.lightGridInjectionGroupSize, 4u);
    EXPECT_EQ(context.graphicsPassBindingSet, lightGridBindingSet);
}

TEST(FrameGraphSceneTargetsTests, BuildPreparedComputeDispatchSourceCarriesDispatchInputsAndMetadataInOrder)
{
    std::vector<NLS::Render::Context::RecordedComputeDispatchInput> preparedDispatchInputs(2u);
    preparedDispatchInputs[0].debugName = "LightGridInjection";
    preparedDispatchInputs[1].debugName = "LightGridCompact";

    const auto preparedSource = NLS::Render::FrameGraph::BuildPreparedComputeDispatchSource(preparedDispatchInputs);

    ASSERT_EQ(preparedSource.dispatchInputs.size(), 2u);
    ASSERT_EQ(preparedSource.metadata.size(), 2u);
    EXPECT_EQ(preparedSource.dispatchInputs[0].debugName, "LightGridInjection");
    EXPECT_EQ(preparedSource.dispatchInputs[1].debugName, "LightGridCompact");
    EXPECT_EQ(preparedSource.metadata[0].commandKind, NLS::Render::Context::RenderPassCommandKind::Compute);
    EXPECT_EQ(preparedSource.metadata[1].commandKind, NLS::Render::Context::RenderPassCommandKind::Compute);
    EXPECT_STREQ(preparedSource.metadata[0].graphPassName, "LightGridInjection");
    EXPECT_STREQ(preparedSource.metadata[1].graphPassName, "LightGridCompact");

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> sceneMetadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Lighting,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
            "DeferredLighting")
    }};

    const auto combinedMetadata = NLS::Render::FrameGraph::BuildPreparedComputeAndScenePassMetadata(
        preparedSource,
        sceneMetadata);

    ASSERT_EQ(combinedMetadata.size(), 3u);
    EXPECT_STREQ(combinedMetadata[0].graphPassName, "LightGridInjection");
    EXPECT_STREQ(combinedMetadata[1].graphPassName, "LightGridCompact");
    EXPECT_STREQ(combinedMetadata[2].graphPassName, "DeferredLighting");
}

TEST(FrameGraphSceneTargetsTests, CompileThreadedExecutionBuildsPreparedComputeSourceFromFactoryOnce)
{
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> sceneMetadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Opaque,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "ForwardOpaque")
    }};

    int factoryCallCount = 0;
    const auto compiledExecution = NLS::Render::FrameGraph::CompileThreadedRenderSceneExecution(
        frameDescriptor,
        -1,
        -1,
        [&]()
        {
            ++factoryCallCount;
            std::vector<NLS::Render::Context::RecordedComputeDispatchInput> dispatchInputs(2u);
            dispatchInputs[0].debugName = "LightGridInjection";
            dispatchInputs[1].debugName = "LightGridCompact";
            return NLS::Render::FrameGraph::BuildPreparedComputeDispatchSource(std::move(dispatchInputs));
        },
        sceneMetadata,
        [](const NLS::Render::FrameGraph::PreparedComputeDispatchSource& preparedSource, const auto& compiledPasses)
        {
            std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;
            passInputs.reserve(compiledPasses.size());

            for (const auto& compiledPass : compiledPasses)
            {
                NLS::Render::Context::RenderPassCommandInput passInput;
                if (NLS::Render::FrameGraph::TryBuildPreparedComputeDispatchThreadedPassInput(
                    preparedSource,
                    compiledPass,
                    passInput))
                {
                    passInputs.push_back(std::move(passInput));
                    continue;
                }

                passInput.kind = compiledPass.metadata.commandKind;
                passInput.debugName = compiledPass.metadata.graphPassName;
                passInput.drawCount = 1u;
                passInputs.push_back(std::move(passInput));
            }

            return passInputs;
        });

    EXPECT_EQ(factoryCallCount, 1);
    ASSERT_EQ(compiledExecution.graphPasses.size(), 3u);
    ASSERT_EQ(compiledExecution.threadedPlan.passes.size(), 3u);
    EXPECT_STREQ(compiledExecution.graphPasses[0].metadata.graphPassName, "LightGridInjection");
    EXPECT_STREQ(compiledExecution.graphPasses[1].metadata.graphPassName, "LightGridCompact");
    EXPECT_STREQ(compiledExecution.graphPasses[2].metadata.graphPassName, "ForwardOpaque");
    EXPECT_EQ(compiledExecution.threadedPlan.passes[0].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Compute);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[1].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Compute);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[2].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[0].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::None);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[1].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::None);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[2].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::LastCompute);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[0].commandInput.queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::None);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[1].commandInput.queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::None);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[2].commandInput.queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::LastCompute);
    ASSERT_EQ(compiledExecution.threadedPlan.dependencies.size(), 1u);
    EXPECT_EQ(compiledExecution.threadedPlan.dependencies[0].sourcePassIndex, 1u);
    EXPECT_EQ(compiledExecution.threadedPlan.dependencies[0].targetPassIndex, 2u);
    EXPECT_EQ(
        compiledExecution.threadedPlan.dependencies[0].kind,
        NLS::Render::FrameGraph::ThreadedRenderSceneDependencyKind::QueueSynchronization);
    ASSERT_TRUE(compiledExecution.threadedPlan.passes[2].dependencySourcePassIndex.has_value());
    EXPECT_EQ(*compiledExecution.threadedPlan.passes[2].dependencySourcePassIndex, 1u);
}

TEST(FrameGraphSceneTargetsTests, CompileAndApplyThreadedExecutionAppliesPreparedComputeInputMutatorBeforePassInputBuild)
{
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> sceneMetadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Opaque,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "ForwardOpaque")
    }};

    bool buildPassInputsSawMutatedPackage = false;
    const auto compiledExecution = NLS::Render::FrameGraph::CompileAndApplyThreadedRenderSceneExecution(
        package,
        frameDescriptor,
        -1,
        -1,
        []()
        {
            std::vector<NLS::Render::Context::RecordedComputeDispatchInput> dispatchInputs(1u);
            dispatchInputs[0].debugName = "LightGridInjection";

            NLS::Render::FrameGraph::PreparedComputeCompilationInput input;
            input.source = NLS::Render::FrameGraph::BuildPreparedComputeDispatchSource(std::move(dispatchInputs));
            input.applyToPackage = [](NLS::Render::Context::RenderScenePackage& packageToMutate)
            {
                packageToMutate.targetsSwapchain = true;
            };
            return input;
        },
        sceneMetadata,
        [&package, &buildPassInputsSawMutatedPackage](
            const NLS::Render::FrameGraph::PreparedComputeDispatchSource& preparedSource,
            const auto& compiledPasses)
        {
            buildPassInputsSawMutatedPackage = package.targetsSwapchain;

            std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;
            passInputs.reserve(compiledPasses.size());

            for (const auto& compiledPass : compiledPasses)
            {
                NLS::Render::Context::RenderPassCommandInput passInput;
                if (NLS::Render::FrameGraph::TryBuildPreparedComputeDispatchThreadedPassInput(
                    preparedSource,
                    compiledPass,
                    passInput))
                {
                    passInputs.push_back(std::move(passInput));
                    continue;
                }

                passInput.kind = compiledPass.metadata.commandKind;
                passInput.debugName = compiledPass.metadata.graphPassName;
                passInput.drawCount = 1u;
                passInputs.push_back(std::move(passInput));
            }

            return passInputs;
        });

    EXPECT_TRUE(buildPassInputsSawMutatedPackage);
    EXPECT_TRUE(package.targetsSwapchain);
    ASSERT_EQ(compiledExecution.threadedPlan.passes.size(), 2u);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[0].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Compute);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[1].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
}

TEST(FrameGraphSceneTargetsTests, CompileAndApplyThreadedExecutionAcceptsPreparedComputeSourceFactoryAndPackageMutatorSeparately)
{
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> sceneMetadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Opaque,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "ForwardOpaque")
    }};

    bool buildPassInputsSawMutatedPackage = false;
    int sourceFactoryCallCount = 0;
    const auto compiledExecution = NLS::Render::FrameGraph::CompileAndApplyThreadedRenderSceneExecution(
        package,
        frameDescriptor,
        -1,
        -1,
        [&]()
        {
            ++sourceFactoryCallCount;
            std::vector<NLS::Render::Context::RecordedComputeDispatchInput> dispatchInputs(1u);
            dispatchInputs[0].debugName = "LightGridInjection";
            return NLS::Render::FrameGraph::BuildPreparedComputeDispatchSource(std::move(dispatchInputs));
        },
        [](NLS::Render::Context::RenderScenePackage& packageToMutate)
        {
            packageToMutate.targetsSwapchain = true;
        },
        sceneMetadata,
        [&package, &buildPassInputsSawMutatedPackage](
            const NLS::Render::FrameGraph::PreparedComputeDispatchSource& preparedSource,
            const auto& compiledPasses)
        {
            buildPassInputsSawMutatedPackage = package.targetsSwapchain;

            std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;
            passInputs.reserve(compiledPasses.size());

            for (const auto& compiledPass : compiledPasses)
            {
                NLS::Render::Context::RenderPassCommandInput passInput;
                if (NLS::Render::FrameGraph::TryBuildPreparedComputeDispatchThreadedPassInput(
                    preparedSource,
                    compiledPass,
                    passInput))
                {
                    passInputs.push_back(std::move(passInput));
                    continue;
                }

                passInput.kind = compiledPass.metadata.commandKind;
                passInput.debugName = compiledPass.metadata.graphPassName;
                passInput.drawCount = 1u;
                passInputs.push_back(std::move(passInput));
            }

            return passInputs;
        });

    EXPECT_EQ(sourceFactoryCallCount, 1);
    EXPECT_TRUE(buildPassInputsSawMutatedPackage);
    EXPECT_TRUE(package.targetsSwapchain);
    ASSERT_EQ(compiledExecution.threadedPlan.passes.size(), 2u);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[0].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Compute);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[1].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
}

TEST(FrameGraphSceneTargetsTests, CompileAndApplyThreadedExecutionBuildsDefaultPassInputsFromPackageAfterPreparedComputeSourceFactory)
{
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;

    NLS::Render::Context::RenderPassCommandInput opaquePassInput;
    opaquePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePassInput.debugName = "ForwardOpaque";
    opaquePassInput.drawCount = 2u;
    package.passCommandInputs.push_back(opaquePassInput);

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 1> sceneMetadata{{
        MakeThreadedPassMetadata(
            NLS::Render::Context::RenderPassCommandKind::Opaque,
            NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
            "ForwardOpaque")
    }};

    int sourceFactoryCallCount = 0;
    const auto compiledExecution = NLS::Render::FrameGraph::CompileAndApplyThreadedRenderSceneExecution(
        package,
        frameDescriptor,
        -1,
        -1,
        [&]()
        {
            ++sourceFactoryCallCount;
            std::vector<NLS::Render::Context::RecordedComputeDispatchInput> dispatchInputs(1u);
            dispatchInputs[0].debugName = "LightGridInjection";
            return NLS::Render::FrameGraph::BuildPreparedComputeDispatchSource(std::move(dispatchInputs));
        },
        [](NLS::Render::Context::RenderScenePackage& packageToMutate)
        {
            packageToMutate.targetsSwapchain = true;
        },
        sceneMetadata);

    EXPECT_EQ(sourceFactoryCallCount, 1);
    EXPECT_TRUE(package.targetsSwapchain);
    ASSERT_EQ(compiledExecution.threadedPlan.passes.size(), 2u);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[0].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Compute);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[1].commandInput.kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(compiledExecution.threadedPlan.passes[1].commandInput.drawCount, 2u);
}

TEST(FrameGraphSceneTargetsTests, ResolveScenePassOutputChainPrefersPreviousPassResources)
{
    const auto accessPlan = NLS::Render::FrameGraph::ResolveScenePassOutputChain(
        11,
        13,
        17,
        19);

    EXPECT_EQ(accessPlan.color, 11);
    EXPECT_EQ(accessPlan.depth, 13);
    EXPECT_FALSE(accessPlan.requiresSideEffect);
}

TEST(FrameGraphSceneTargetsTests, ResolveScenePassOutputChainUsesImportedResourcesOrSideEffectWhenPreviousOutputsAreAbsent)
{
    const auto importedResolution = NLS::Render::FrameGraph::ResolveScenePassOutputChain(
        -1,
        -1,
        17,
        19);
    EXPECT_EQ(importedResolution.color, 17);
    EXPECT_EQ(importedResolution.depth, 19);
    EXPECT_FALSE(importedResolution.requiresSideEffect);

    const auto sideEffectResolution = NLS::Render::FrameGraph::ResolveScenePassOutputChain(
        -1,
        -1,
        -1,
        -1);
    EXPECT_EQ(sideEffectResolution.color, -1);
    EXPECT_EQ(sideEffectResolution.depth, -1);
    EXPECT_TRUE(sideEffectResolution.requiresSideEffect);
}

TEST(FrameGraphSceneTargetsTests, ResolveScenePassOutputChainRespectsMetadataOutputFlags)
{
    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata colorDisabledMetadata;
    colorDisabledMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    colorDisabledMetadata.propagatesColorOutput = false;

    const auto depthOnlyChain = NLS::Render::FrameGraph::ResolveScenePassOutputChain(
        11,
        13,
        17,
        19,
        colorDisabledMetadata);
    EXPECT_EQ(depthOnlyChain.color, -1);
    EXPECT_EQ(depthOnlyChain.depth, 13);
    EXPECT_FALSE(depthOnlyChain.requiresSideEffect);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata noOutputMetadata;
    noOutputMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    noOutputMetadata.propagatesColorOutput = false;
    noOutputMetadata.propagatesDepthOutput = false;

    const auto noOutputChain = NLS::Render::FrameGraph::ResolveScenePassOutputChain(
        11,
        13,
        17,
        19,
        noOutputMetadata);
    EXPECT_EQ(noOutputChain.color, -1);
    EXPECT_EQ(noOutputChain.depth, -1);
    EXPECT_TRUE(noOutputChain.requiresSideEffect);
}

TEST(FrameGraphSceneTargetsTests, AddSceneOutputCallbackPassAppliesMetadataToPassData)
{
    struct TestPassData
    {
        FrameGraphResource color = -1;
        FrameGraphResource depth = -1;
    };

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;
    NLS::Render::FrameGraph::ImportSceneRenderTargets(
        frameGraph,
        blackboard,
        frameDescriptor,
        "SceneColor",
        "SceneDepth");

    const auto* targets = blackboard.try_get<NLS::Render::FrameGraph::SceneRenderTargetsData>();
    ASSERT_NE(targets, nullptr);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata metadata;
    metadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    metadata.graphPassName = "LightingPass";
    metadata.propagatesDepthOutput = false;

    const auto& passData = NLS::Render::FrameGraph::AddSceneOutputCallbackPass<
        TestPassData,
        &TestPassData::color,
        &TestPassData::depth>(
        frameGraph,
        metadata,
        -1,
        -1,
        targets->color,
        targets->depth,
        [](const TestPassData&, FrameGraphPassResources&, void*)
        {
        });

    EXPECT_GE(passData.color, 0);
    EXPECT_EQ(passData.depth, -1);
}

TEST(FrameGraphSceneTargetsTests, AddSceneOutputCallbackPassSupportsAdditionalSetupReads)
{
    struct TestPassData
    {
        FrameGraphResource inputColor = -1;
        FrameGraphResource color = -1;
        FrameGraphResource depth = -1;
    };

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;
    NLS::Render::FrameGraph::ImportSceneRenderTargets(
        frameGraph,
        blackboard,
        frameDescriptor,
        "SceneColor",
        "SceneDepth");

    const auto* targets = blackboard.try_get<NLS::Render::FrameGraph::SceneRenderTargetsData>();
    ASSERT_NE(targets, nullptr);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata metadata;
    metadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    metadata.graphPassName = "LightingWithReadPass";
    metadata.propagatesDepthOutput = false;

    const auto& passData = NLS::Render::FrameGraph::AddSceneOutputCallbackPass<
        TestPassData,
        &TestPassData::color,
        &TestPassData::depth>(
        frameGraph,
        metadata,
        -1,
        -1,
        targets->color,
        targets->depth,
        [=](FrameGraph::Builder& builder, TestPassData& data)
        {
            data.inputColor = builder.read(targets->color);
        },
        [](const TestPassData&, FrameGraphPassResources&, void*)
        {
        });

    EXPECT_GE(passData.inputColor, 0);
    EXPECT_GE(passData.color, 0);
    EXPECT_EQ(passData.depth, -1);
}

TEST(FrameGraphSceneTargetsTests, AddMetadataCallbackPassSupportsPureInternalResourceSetup)
{
    struct TestPassData
    {
        FrameGraphResource color = -1;
        FrameGraphResource depth = -1;
    };

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;
    NLS::Render::FrameGraph::ImportSceneRenderTargets(
        frameGraph,
        blackboard,
        frameDescriptor,
        "SceneColor",
        "SceneDepth");

    const auto* targets = blackboard.try_get<NLS::Render::FrameGraph::SceneRenderTargetsData>();
    ASSERT_NE(targets, nullptr);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata metadata;
    metadata.commandKind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    metadata.graphPassName = "InternalWritePass";

    const auto& passData = NLS::Render::FrameGraph::AddMetadataCallbackPass<TestPassData>(
        frameGraph,
        metadata,
        [=](FrameGraph::Builder& builder, TestPassData& data)
        {
            data.color = builder.write(targets->color);
            data.depth = builder.write(targets->depth);
        },
        [](const TestPassData&, FrameGraphPassResources&, void*)
        {
        });

    EXPECT_GE(passData.color, 0);
    EXPECT_GE(passData.depth, 0);
}

TEST(FrameGraphSceneTargetsTests, ExecuteOutputRenderPassRunsBodyAndEndWithStartedFlag)
{
    std::vector<std::string> events;

    NLS::Render::FrameGraph::OutputRenderPassExecutionDesc desc;
    desc.renderWidth = 320u;
    desc.renderHeight = 180u;
    desc.clearColor = true;

    NLS::Render::FrameGraph::ExecuteOutputRenderPass(
        desc,
        [&](const auto& beginDesc) -> bool
        {
            EXPECT_EQ(beginDesc.renderWidth, 320u);
            EXPECT_EQ(beginDesc.renderHeight, 180u);
            EXPECT_TRUE(beginDesc.clearColor);
            events.push_back("begin");
            return true;
        },
        [&]()
        {
            events.push_back("draw");
        },
        [&](bool started, const auto& endDesc)
        {
            events.push_back(started ? "end-started" : "end-not-started");
            EXPECT_EQ(endDesc.renderWidth, 320u);
            EXPECT_EQ(endDesc.renderHeight, 180u);
        });

    EXPECT_EQ(events, std::vector<std::string>({ "begin", "draw", "end-started" }));
}

TEST(FrameGraphSceneTargetsTests, ExecuteRecordedRenderPassSkipsRecordedBodyWhenBeginFails)
{
    std::vector<std::string> events;
    NLS::Render::FrameGraph::RecordedRenderPassExecutionDesc desc;
    desc.renderWidth = 256u;
    desc.renderHeight = 144u;
    desc.clearDepth = true;

    NLS::Render::FrameGraph::ExecuteRecordedRenderPass(
        desc,
        [&](const auto& beginDesc) -> bool
        {
            EXPECT_EQ(beginDesc.renderWidth, 256u);
            EXPECT_EQ(beginDesc.renderHeight, 144u);
            EXPECT_TRUE(beginDesc.clearDepth);
            events.push_back("begin");
            return false;
        },
        [&]()
        {
            events.push_back("recorded");
        },
        [&]()
        {
            events.push_back("end");
        });

    EXPECT_EQ(events, std::vector<std::string>({ "begin" }));
}

TEST(FrameGraphSceneTargetsTests, RegisterExternalSceneOutputExtractionsAddsSampledColorTextureToPackage)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;

    const auto registered = NLS::Render::FrameGraph::RegisterExternalSceneOutputExtractions(package, frameDescriptor);

    EXPECT_TRUE(registered);
    ASSERT_EQ(package.extractedTextures.size(), 1u);
    EXPECT_NE(package.extractedTextures[0], nullptr);
    EXPECT_EQ(package.extractedTextures[0], outputBuffer.GetExplicitTextureHandle());
    EXPECT_EQ(package.externalSceneOutputTextureCount, 2u);
    EXPECT_EQ(NLS::Render::FrameGraph::CountExternalSceneOutputSampledTextures(package), 1u);
}

TEST(FrameGraphSceneTargetsTests, ExternalSceneOutputBridgeResolvesFramebufferTargetsFromFrameDescriptor)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;

    EXPECT_TRUE(NLS::Render::FrameGraph::FrameTargetsSwapchain(frameDescriptor));
    EXPECT_EQ(NLS::Render::FrameGraph::ResolveExternalSceneOutputFramebuffer(frameDescriptor), nullptr);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    EXPECT_FALSE(NLS::Render::FrameGraph::FrameTargetsSwapchain(frameDescriptor));
    EXPECT_EQ(NLS::Render::FrameGraph::ResolveExternalSceneOutputFramebuffer(frameDescriptor), &outputBuffer);

    const auto summary = NLS::Render::FrameGraph::BuildExternalSceneOutputSummary(frameDescriptor);
    EXPECT_FALSE(summary.targetsSwapchain);
    EXPECT_TRUE(summary.hasExternalOutput);
    EXPECT_EQ(summary.textureCount, 2u);
}

TEST(FrameGraphSceneTargetsTests, ExternalSceneOutputSnapshotDetachesFramebufferPointer)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    const auto snapshot = NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(frameDescriptor);

    EXPECT_EQ(NLS::Render::FrameGraph::ResolveExternalSceneOutputFramebuffer(snapshot), nullptr);
    EXPECT_EQ(snapshot.outputColorTexture, frameDescriptor.outputColorTexture);
    EXPECT_EQ(snapshot.outputDepthStencilTexture, frameDescriptor.outputDepthStencilTexture);
    EXPECT_EQ(snapshot.outputColorView, frameDescriptor.outputColorView);
    EXPECT_EQ(snapshot.outputDepthStencilView, frameDescriptor.outputDepthStencilView);

    const auto summary = NLS::Render::FrameGraph::BuildExternalSceneOutputSummary(snapshot);
    EXPECT_FALSE(summary.targetsSwapchain);
    EXPECT_TRUE(summary.hasExternalOutput);
    EXPECT_EQ(summary.textureCount, 2u);
}

TEST(FrameGraphSceneTargetsTests, PrepareForwardSceneGraphImportsExternalTargetsAndPreservesForwardPassOrder)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    FrameGraph frameGraph;
    NLS::Render::FrameGraph::ReserveForwardSceneGraph(frameGraph, frameDescriptor);
    FrameGraphBlackboard blackboard;

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    const auto preparedGraph = NLS::Render::FrameGraph::PrepareForwardSceneGraph(
        frameGraph,
        blackboard,
        lightGridContext);

    const auto* importedTargets = blackboard.try_get<NLS::Render::FrameGraph::SceneRenderTargetsData>();
    ASSERT_NE(importedTargets, nullptr);
    EXPECT_GE(importedTargets->color, 0);
    EXPECT_GE(importedTargets->depth, 0);

    const auto& graphPasses = preparedGraph.execution.compiledExecution.graphPasses;
    ASSERT_EQ(graphPasses.size(), 3u);
    EXPECT_EQ(graphPasses[0].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(graphPasses[1].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(graphPasses[2].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_STREQ(graphPasses[0].metadata.graphPassName, "ForwardOpaque");
    EXPECT_STREQ(graphPasses[1].metadata.graphPassName, "ForwardSkybox");
    EXPECT_STREQ(graphPasses[2].metadata.graphPassName, "ForwardTransparent");
    EXPECT_TRUE(preparedGraph.execution.compiledExecution.threadedPlan.passes.empty());
}

TEST(FrameGraphSceneTargetsTests, ForwardExternalOutputPassExecutesWithoutDownstreamConsumer)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    FrameGraph frameGraph;
    NLS::Render::FrameGraph::ReserveForwardSceneGraph(frameGraph, frameDescriptor);
    FrameGraphBlackboard blackboard;

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor =
        NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(frameDescriptor);

    const auto preparedGraph = NLS::Render::FrameGraph::PrepareForwardSceneGraph(
        frameGraph,
        blackboard,
        lightGridContext);

    int beginCount = 0;
    int drawCount = 0;
    int endCount = 0;
    NLS::Render::FrameGraph::ExecutePreparedForwardSceneGraph(
        frameGraph,
        preparedGraph,
        {
            [&](const auto&) -> bool
            {
                ++beginCount;
                return true;
            },
            [&](const NLS::Render::FrameGraph::CompiledThreadedRenderSceneGraphPass& compiledPass)
            {
                if (compiledPass.metadata.commandKind == NLS::Render::Context::RenderPassCommandKind::Opaque)
                    ++drawCount;
            },
            [&](bool startedRenderPass, const auto&)
            {
                EXPECT_TRUE(startedRenderPass);
                ++endCount;
            }
        });

    frameGraph.compile();
    std::ostringstream frameGraphDebug;
    frameGraph.debugOutput(frameGraphDebug, graphviz::Writer{});
    NLS::Render::FrameGraph::FrameGraphExecutionContext executionContext{
        driver,
        explicitDevice.get(),
        nullptr,
        nullptr
    };
    frameGraph.execute(&executionContext, &executionContext);

    EXPECT_GE(beginCount, 1) << frameGraphDebug.str();
    EXPECT_GE(drawCount, 1) << frameGraphDebug.str();
    EXPECT_GE(endCount, 1) << frameGraphDebug.str();
}

TEST(FrameGraphSceneTargetsTests, FinalizePreparedForwardScenePackageRegistersExtractionVisibilityAndReadbackSource)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;

    NLS::Render::Context::RenderPassCommandInput opaquePassInput;
    opaquePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePassInput.usesColorAttachment = true;
    opaquePassInput.usesDepthStencilAttachment = true;
    package.passCommandInputs.push_back(opaquePassInput);

    NLS::Render::FrameGraph::FinalizePreparedForwardScenePackage(package, frameDescriptor);

    ASSERT_EQ(package.passCommandInputs.size(), 1u);
    ASSERT_EQ(package.passCommandInputs[0].colorAttachmentViews.size(), 1u);
    EXPECT_NE(package.passCommandInputs[0].colorAttachmentViews[0], nullptr);
    EXPECT_NE(package.passCommandInputs[0].depthStencilAttachmentView, nullptr);

    ASSERT_EQ(package.extractedTextures.size(), 1u);
    EXPECT_NE(package.extractedTextures[0], nullptr);
    EXPECT_EQ(package.extractedTextures[0], outputBuffer.GetExplicitTextureHandle());
    EXPECT_EQ(package.externalSceneOutputTextureCount, 2u);
    EXPECT_EQ(NLS::Render::FrameGraph::CountExternalSceneOutputSampledTextures(package), 1u);

    const auto visibilityPassInput =
        NLS::Render::FrameGraph::BuildExtractionVisibilityPassInput(package);
    EXPECT_TRUE(visibilityPassInput.requiresDependencyVisibility);
    ASSERT_EQ(visibilityPassInput.textureVisibilityTransitions.size(), 1u);
    EXPECT_EQ(
        visibilityPassInput.textureVisibilityTransitions[0].texture,
        package.extractedTextures[0]);
    EXPECT_EQ(
        NLS::Render::FrameGraph::ResolveFrameReadbackTexture(&package, nullptr),
        package.extractedTextures[0]);
}

TEST(FrameGraphSceneTargetsTests, PreferredReadbackRegistrationDeduplicatesAndPromotesReadbackTexture)
{
    NLS::Render::RHI::RHITextureDesc extractedDesc;
    extractedDesc.debugName = "SceneColor";
    extractedDesc.extent = { 320u, 180u, 1u };
    auto extractedTexture = std::make_shared<TestTexture>(extractedDesc);

    NLS::Render::RHI::RHITextureDesc preferredDesc;
    preferredDesc.debugName = "PickingColor";
    preferredDesc.extent = { 320u, 180u, 1u };
    auto preferredTexture = std::make_shared<TestTexture>(preferredDesc);

    NLS::Render::Context::RenderScenePackage package;
    package.extractedTextures.push_back(extractedTexture);
    package.extractedTextures.push_back(preferredTexture);

    EXPECT_TRUE(NLS::Render::FrameGraph::RegisterPreferredReadbackTexture(package, preferredTexture));
    EXPECT_EQ(package.preferredReadbackTexture, preferredTexture);
    ASSERT_EQ(package.extractedTextures.size(), 2u);
    EXPECT_EQ(package.extractedTextures[0], preferredTexture);
    EXPECT_EQ(package.extractedTextures[1], extractedTexture);

    EXPECT_FALSE(NLS::Render::FrameGraph::RegisterPreferredReadbackTexture(package, preferredTexture));
    ASSERT_EQ(package.extractedTextures.size(), 2u);
}

TEST(FrameGraphSceneTargetsTests, PreferredReadbackTextureIsNotCountedAsExternalSceneOutput)
{
    NLS::Render::RHI::RHITextureDesc sceneDesc;
    sceneDesc.debugName = "SceneColor";
    sceneDesc.extent = { 320u, 180u, 1u };
    sceneDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto sceneTexture = std::make_shared<TestTexture>(sceneDesc);

    NLS::Render::RHI::RHITextureDesc pickingDesc;
    pickingDesc.debugName = "PickingColor";
    pickingDesc.extent = { 320u, 180u, 1u };
    pickingDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto pickingTexture = std::make_shared<TestTexture>(pickingDesc);

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    package.extractedTextures.push_back(sceneTexture);
    package.extractedTextures.push_back(pickingTexture);
    package.preferredReadbackTexture = pickingTexture;
    package.externalSceneOutputTextureCount = 1u;

    EXPECT_EQ(NLS::Render::FrameGraph::CountExternalSceneOutputSampledTextures(package), 1u);

    const auto summary = NLS::Render::FrameGraph::BuildExternalSceneOutputSummary(package);
    EXPECT_TRUE(summary.hasExternalOutput);
    EXPECT_EQ(summary.textureCount, 1u);

    const auto visibilityPassInput =
        NLS::Render::FrameGraph::BuildExtractionVisibilityPassInput(package);
    ASSERT_EQ(visibilityPassInput.textureVisibilityTransitions.size(), 2u);
    EXPECT_EQ(visibilityPassInput.textureVisibilityTransitions[0].texture, sceneTexture);
    EXPECT_EQ(visibilityPassInput.textureVisibilityTransitions[1].texture, pickingTexture);
}

TEST(FrameGraphSceneTargetsTests, OrdinaryExtractedTexturesDoNotImplyExternalSceneOutput)
{
    NLS::Render::RHI::RHITextureDesc sceneDesc;
    sceneDesc.debugName = "SceneColor";
    sceneDesc.extent = { 320u, 180u, 1u };
    sceneDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto sceneTexture = std::make_shared<TestTexture>(sceneDesc);

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    package.extractedTextures.push_back(sceneTexture);

    const auto summary = NLS::Render::FrameGraph::BuildExternalSceneOutputSummary(package);
    EXPECT_FALSE(summary.targetsSwapchain);
    EXPECT_FALSE(summary.hasExternalOutput);
    EXPECT_EQ(summary.textureCount, 0u);

    const auto visibilityPassInput =
        NLS::Render::FrameGraph::BuildExtractionVisibilityPassInput(package);
    ASSERT_EQ(visibilityPassInput.textureVisibilityTransitions.size(), 1u);
    EXPECT_EQ(visibilityPassInput.textureVisibilityTransitions[0].texture, sceneTexture);
}

TEST(FrameGraphSceneTargetsTests, ExtractionVisibilityUsesAttachmentViewRangesForExternalOutput)
{
    NLS::Render::RHI::RHITextureDesc sceneDesc;
    sceneDesc.debugName = "SceneColorArray";
    sceneDesc.extent = { 320u, 180u, 1u };
    sceneDesc.mipLevels = 4u;
    sceneDesc.arrayLayers = 3u;
    sceneDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto sceneTexture = std::make_shared<TestTexture>(sceneDesc);

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.subresourceRange.baseMipLevel = 1u;
    viewDesc.subresourceRange.mipLevelCount = 1u;
    viewDesc.subresourceRange.baseArrayLayer = 2u;
    viewDesc.subresourceRange.arrayLayerCount = 1u;
    auto sceneView = std::make_shared<TestTextureView>(sceneTexture, viewDesc);

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.usesColorAttachment = true;
    passInput.colorAttachmentViews.push_back(sceneView);

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    package.passCommandInputs.push_back(passInput);
    package.extractedTextures.push_back(sceneTexture);
    package.externalSceneOutputTextureCount = 1u;

    const auto visibilityPassInput =
        NLS::Render::FrameGraph::BuildExtractionVisibilityPassInput(package);
    ASSERT_EQ(visibilityPassInput.textureVisibilityTransitions.size(), 1u);
    const auto& transition = visibilityPassInput.textureVisibilityTransitions[0];
    EXPECT_EQ(transition.texture, sceneTexture);
    EXPECT_EQ(transition.before, NLS::Render::RHI::ResourceState::Unknown);
    EXPECT_EQ(transition.after, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(transition.subresourceRange.baseMipLevel, viewDesc.subresourceRange.baseMipLevel);
    EXPECT_EQ(transition.subresourceRange.mipLevelCount, viewDesc.subresourceRange.mipLevelCount);
    EXPECT_EQ(transition.subresourceRange.baseArrayLayer, viewDesc.subresourceRange.baseArrayLayer);
    EXPECT_EQ(transition.subresourceRange.arrayLayerCount, viewDesc.subresourceRange.arrayLayerCount);
}
