#include <gtest/gtest.h>

#include <fg/GraphvizWriter.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>

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
#include "Rendering/RHI/Core/RHIPipeline.h"
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

    NLS::Render::RHI::RHITextureDesc MakeTestTextureDesc(
        const char* debugName,
        const NLS::Render::RHI::TextureFormat format = NLS::Render::FrameGraph::kDeferredGBufferColorFormats[0],
        const NLS::Render::RHI::TextureUsageFlags usage = NLS::Render::FrameGraph::kDeferredGBufferColorUsage)
    {
        NLS::Render::RHI::RHITextureDesc desc;
        desc.debugName = debugName;
        desc.extent = { 320u, 180u, 1u };
        desc.format = format;
        desc.usage = usage;
        desc.mipLevels = 1u;
        desc.arrayLayers = 1u;
        return desc;
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

    class TestGraphicsPipeline final : public NLS::Render::RHI::RHIGraphicsPipeline
    {
    public:
        explicit TestGraphicsPipeline(std::string debugName)
        {
            m_desc.debugName = std::move(debugName);
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc {};
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

    NLS::Render::FrameGraph::DeferredPreparedSceneResources MakeCompleteDeferredPreparedSceneResources(
        const NLS::Render::RHI::TextureUsageFlags normalUsage = NLS::Render::FrameGraph::kDeferredGBufferColorUsage,
        const NLS::Render::RHI::TextureUsageFlags depthUsage = NLS::Render::FrameGraph::kDeferredGBufferDepthUsage)
    {
        NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;
        auto albedoTexture = std::make_shared<TestTexture>(
            MakeTestTextureDesc("DeferredGBufferAlbedo", NLS::Render::FrameGraph::kDeferredGBufferColorFormats[0]));
        auto normalTexture = std::make_shared<TestTexture>(
            MakeTestTextureDesc(
                "DeferredGBufferNormal",
                NLS::Render::FrameGraph::kDeferredGBufferColorFormats[1],
                normalUsage));
        auto materialTexture = std::make_shared<TestTexture>(
            MakeTestTextureDesc("DeferredGBufferMaterial", NLS::Render::FrameGraph::kDeferredGBufferColorFormats[2]));
        auto depthTexture = std::make_shared<TestTexture>(
            MakeTestTextureDesc(
                "DeferredGBufferDepth",
                NLS::Render::FrameGraph::kDeferredGBufferDepthFormat,
                depthUsage));

        auto makeView = [](const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const char* debugName)
        {
            NLS::Render::RHI::RHITextureViewDesc desc;
            desc.debugName = debugName;
            desc.format = texture->GetDesc().format;
            return std::make_shared<TestTextureView>(texture, desc);
        };

        resources.gbufferColorViews.push_back(makeView(albedoTexture, "DeferredGBufferAlbedoView"));
        resources.gbufferColorViews.push_back(makeView(normalTexture, "DeferredGBufferNormalView"));
        resources.gbufferColorViews.push_back(makeView(materialTexture, "DeferredGBufferMaterialView"));
        resources.gbufferDepthView = makeView(depthTexture, "DeferredGBufferDepthView");
        resources.gbufferTextures = {
            resources.gbufferColorViews[0]->GetTexture(),
            resources.gbufferColorViews[1]->GetTexture(),
            resources.gbufferColorViews[2]->GetTexture(),
            resources.gbufferDepthView->GetTexture()
        };
        return resources;
    }

    NLS::Render::FrameGraph::DeferredPreparedQueuedDrawCounts MakeDeferredQueuedDrawCounts(
        std::optional<uint64_t> lightingDrawCount = std::nullopt,
        std::optional<uint64_t> decalDrawCount = std::nullopt,
        std::optional<uint64_t> transparentDrawCount = std::nullopt)
    {
        NLS::Render::FrameGraph::DeferredPreparedQueuedDrawCounts counts;
        counts.lightingDrawCount = lightingDrawCount;
        counts.decalDrawCount = decalDrawCount;
        counts.transparentDrawCount = transparentDrawCount;
        return counts;
    }

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
        NLS::Render::RHI::RHIBarrierDesc FilterBarrierDesc(const NLS::Render::RHI::RHIBarrierDesc& barrier) const override
        {
            lastFilteredBarrierDesc = barrier;
            if (!dropLastTextureBarrier || barrier.textureBarriers.empty())
                return barrier;

            NLS::Render::RHI::RHIBarrierDesc filtered = barrier;
            filtered.textureBarriers.pop_back();
            return filtered;
        }
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc& desc) override
        {
            ++barrierCalls;
            barrierHistory.push_back(desc);
        }
        NLS::Render::RHI::RHICommandRecordingResult BarrierChecked(
            const NLS::Render::RHI::RHIBarrierDesc& desc) override
        {
            ++barrierCheckedCalls;
            if (failBarrierChecked)
            {
                return {
                    NLS::Render::RHI::RHICommandRecordingStatusCode::BackendFailure,
                    "test barrier failure"
                };
            }

            Barrier(desc);
            return {};
        }

        size_t barrierCalls = 0u;
        size_t barrierCheckedCalls = 0u;
        size_t beginRenderPassCalls = 0u;
        size_t endRenderPassCalls = 0u;
        bool dropLastTextureBarrier = false;
        bool failBarrierChecked = false;
        mutable NLS::Render::RHI::RHIBarrierDesc lastFilteredBarrierDesc;
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
            textureDescs.push_back(desc);
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
        std::vector<NLS::Render::RHI::RHITextureDesc> textureDescs;
        std::vector<NLS::Render::RHI::RHIBufferDesc> bufferDescs;

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        std::shared_ptr<NLS::Render::RHI::RHIQueue> m_queue;
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    void ExpectDefaultOptimizedColorClearValue(
        const NLS::Render::RHI::RHITextureDesc::OptimizedClearValue& clearValue)
    {
        EXPECT_TRUE(clearValue.enabled);
        EXPECT_FLOAT_EQ(clearValue.color[0], 0.0f);
        EXPECT_FLOAT_EQ(clearValue.color[1], 0.0f);
        EXPECT_FLOAT_EQ(clearValue.color[2], 0.0f);
        EXPECT_FLOAT_EQ(clearValue.color[3], 1.0f);
    }

    void ExpectDefaultOpaqueColorClearValue(
        const NLS::Render::RHI::RHIColorClearValue& clearValue)
    {
        EXPECT_FLOAT_EQ(clearValue.r, 0.0f);
        EXPECT_FLOAT_EQ(clearValue.g, 0.0f);
        EXPECT_FLOAT_EQ(clearValue.b, 0.0f);
        EXPECT_FLOAT_EQ(clearValue.a, 1.0f);
    }
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

TEST(FrameGraphSceneTargetsTests, FramebufferDefaultColorAttachmentUsesMatchingOptimizedClearValue)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    ASSERT_NE(outputBuffer.GetExplicitTextureHandle(), nullptr);
    ExpectDefaultOptimizedColorClearValue(outputBuffer.GetExplicitTextureHandle()->GetDesc().optimizedClearValue);
    ASSERT_NE(outputBuffer.GetExplicitDepthStencilTextureHandle(), nullptr);
    EXPECT_TRUE(outputBuffer.GetExplicitDepthStencilTextureHandle()->GetDesc().optimizedClearValue.enabled);
}

TEST(FrameGraphSceneTargetsTests, OptimizedClearValueAggregateEnableDefaultsToOpaqueColor)
{
    const NLS::Render::RHI::RHITextureDesc::OptimizedClearValue clearValue{ true };

    ExpectDefaultOptimizedColorClearValue(clearValue);
}

TEST(FrameGraphSceneTargetsTests, RecordedFramebufferPassDefaultClearMatchesOptimizedClearValue)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 13u;
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    NLS::Render::Entities::Camera camera;
    frameDescriptor.camera = &camera;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;

    NLS::Render::Core::CompositeRenderer renderer(driver);
    renderer.BeginFrame(frameDescriptor);
    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);

    ASSERT_TRUE(renderer.BeginRecordedRenderPass(
        &outputBuffer,
        320u,
        180u,
        true,
        true,
        true));
    renderer.EndRecordedRenderPass();
    renderer.EndFrame();

    ASSERT_EQ(commandBuffer->beginRenderPassCalls, 1u);
    ASSERT_EQ(commandBuffer->lastRenderPassDesc.colorAttachments.size(), 1u);
    ExpectDefaultOpaqueColorClearValue(commandBuffer->lastRenderPassDesc.colorAttachments[0].clearValue);
    ExpectDefaultOptimizedColorClearValue(outputBuffer.GetExplicitTextureHandle()->GetDesc().optimizedClearValue);
}

TEST(FrameGraphSceneTargetsTests, FrameGraphColorAttachmentUsesMatchingOptimizedClearValue)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 11u;
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
    textureDesc.extent.width = 128u;
    textureDesc.extent.height = 72u;
    textureDesc.extent.depth = 1u;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    textureDesc.debugName = "TransientSceneColor";

    texture.create(textureDesc, &executionContext);

    ASSERT_EQ(explicitDevice->textureDescs.size(), 1u);
    ExpectDefaultOptimizedColorClearValue(explicitDevice->textureDescs[0].optimizedClearValue);
}

TEST(FrameGraphSceneTargetsTests, FrameGraphDepthAttachmentPreservesExplicitOptimizedClearValue)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 12u;
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
    textureDesc.extent.width = 128u;
    textureDesc.extent.height = 72u;
    textureDesc.extent.depth = 1u;
    textureDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    textureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    textureDesc.optimizedClearValue.enabled = true;
    textureDesc.optimizedClearValue.depth = 0.5f;
    textureDesc.optimizedClearValue.stencil = 7u;
    textureDesc.debugName = "TransientSceneDepth";

    texture.create(textureDesc, &executionContext);

    ASSERT_EQ(explicitDevice->textureDescs.size(), 1u);
    const auto& optimizedClearValue = explicitDevice->textureDescs[0].optimizedClearValue;
    EXPECT_TRUE(optimizedClearValue.enabled);
    EXPECT_FLOAT_EQ(optimizedClearValue.depth, 0.5f);
    EXPECT_EQ(optimizedClearValue.stencil, 7u);
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

TEST(FrameGraphSceneTargetsTests, MultiFramebufferOverloadsSupportZeroSizedInitializationWithoutExplicitDepthDesc)
{
    std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> attachments(1u);
    attachments[0].format = NLS::Render::RHI::TextureFormat::RGBA8;

    NLS::Render::Buffers::MultiFramebuffer framebuffer(0u, 0u, attachments, true);

    EXPECT_FALSE(framebuffer.IsInitialized());
    EXPECT_TRUE(framebuffer.GetExplicitColorTextureHandles().empty());
    EXPECT_EQ(framebuffer.GetExplicitDepthTextureHandle(), nullptr);

    NLS::Render::Buffers::MultiFramebuffer::DepthAttachmentDesc customDepthAttachment;
    customDepthAttachment.format = NLS::Render::RHI::TextureFormat::Depth32F;
    customDepthAttachment.usage = NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment;

    framebuffer.Init(0u, 0u, attachments, true, customDepthAttachment);

    EXPECT_FALSE(framebuffer.IsInitialized());
    EXPECT_TRUE(framebuffer.GetExplicitColorTextureHandles().empty());
    EXPECT_EQ(framebuffer.GetExplicitDepthTextureHandle(), nullptr);
}

TEST(FrameGraphSceneTargetsTests, MultiFramebufferColorAttachmentsUseMatchingOptimizedClearValue)
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

    const auto colorTextures = gBuffer.GetExplicitColorTextureHandles();
    ASSERT_EQ(colorTextures.size(), 2u);
    ASSERT_NE(colorTextures[0], nullptr);
    ASSERT_NE(colorTextures[1], nullptr);
    ASSERT_NE(gBuffer.GetExplicitDepthTextureHandle(), nullptr);

    ExpectDefaultOptimizedColorClearValue(colorTextures[0]->GetDesc().optimizedClearValue);
    ExpectDefaultOptimizedColorClearValue(colorTextures[1]->GetDesc().optimizedClearValue);

    const auto& depthClearValue = gBuffer.GetExplicitDepthTextureHandle()->GetDesc().optimizedClearValue;
    EXPECT_TRUE(depthClearValue.enabled);
    EXPECT_FLOAT_EQ(depthClearValue.depth, 1.0f);
    EXPECT_EQ(depthClearValue.stencil, 0u);
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

TEST(FrameGraphSceneTargetsTests, DeferredSceneGraphImportsGBufferResourcesAndCompilesGBufferBeforeLighting)
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

    const auto& graphPasses = preparedGraph.execution.compiledExecution.graphPasses;
    ASSERT_EQ(graphPasses.size(), 2u);
    EXPECT_EQ(graphPasses[0].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(graphPasses[1].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_STREQ(graphPasses[0].metadata.graphPassName, "DeferredGBuffer");
    EXPECT_STREQ(graphPasses[1].metadata.graphPassName, "DeferredLighting");
    EXPECT_TRUE(preparedGraph.execution.compiledExecution.threadedPlan.passes.empty());
}

TEST(FrameGraphSceneTargetsTests, DeferredSceneGraphUsesGBufferSlotColorViewNames)
{
    static_assert(
        NLS::Render::FrameGraph::kDeferredGBufferColorSlots.size() ==
        NLS::Render::FrameGraph::kDeferredGBufferColorFormats.size());
    for (size_t i = 0u; i < NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount; ++i)
    {
        EXPECT_EQ(
            NLS::Render::FrameGraph::kDeferredGBufferColorFormats[i],
            NLS::Render::FrameGraph::kDeferredGBufferColorSlots[i].format);
    }

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> attachments(
        NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount);
    for (size_t i = 0u; i < NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount; ++i)
        attachments[i].format = NLS::Render::FrameGraph::kDeferredGBufferColorSlots[i].format;
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
    auto resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
        frameGraph,
        blackboard,
        frameDescriptor,
        preparedResources);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    const auto preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
        resourceRequest,
        lightGridContext);

    ASSERT_GE(preparedGraph.resources.gbufferAlbedo, 0);
    ASSERT_GE(preparedGraph.resources.gbufferNormal, 0);
    ASSERT_GE(preparedGraph.resources.gbufferMaterial, 0);
    ASSERT_EQ(
        gBuffer.GetOrCreateExplicitColorView(0)->GetDebugName(),
        NLS::Render::FrameGraph::kDeferredGBufferColorSlots[0].graphViewName);
    ASSERT_EQ(
        gBuffer.GetOrCreateExplicitColorView(1)->GetDebugName(),
        NLS::Render::FrameGraph::kDeferredGBufferColorSlots[1].graphViewName);
    ASSERT_EQ(
        gBuffer.GetOrCreateExplicitColorView(2)->GetDebugName(),
        NLS::Render::FrameGraph::kDeferredGBufferColorSlots[2].graphViewName);
}

TEST(FrameGraphSceneTargetsTests, DeferredTransparentGraphPassPropagatesColorButNotDepth)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> attachments(
        NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount);
    for (size_t i = 0u; i < NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount; ++i)
        attachments[i].format = NLS::Render::FrameGraph::kDeferredGBufferColorSlots[i].format;
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
    auto resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
        frameGraph,
        blackboard,
        frameDescriptor,
        preparedResources);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    const auto preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
        resourceRequest,
        lightGridContext,
        true);

    const auto& graphPasses = preparedGraph.execution.compiledExecution.graphPasses;
    ASSERT_EQ(graphPasses.size(), 3u);
    EXPECT_EQ(graphPasses[2].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_TRUE(graphPasses[2].metadata.propagatesColorOutput);
    EXPECT_FALSE(graphPasses[2].metadata.propagatesDepthOutput);
    EXPECT_FALSE(graphPasses[2].metadata.execution.useFrameClearState);
    EXPECT_FALSE(graphPasses[2].metadata.execution.clearColor);
    EXPECT_FALSE(graphPasses[2].metadata.execution.clearDepth);
    EXPECT_FALSE(graphPasses[2].metadata.execution.clearStencil);
    EXPECT_GE(graphPasses[2].outputChain.color, 0);
    EXPECT_LT(graphPasses[2].outputChain.depth, 0);
}

TEST(FrameGraphSceneTargetsTests, DeferredTransparentGraphPassReadsGBufferDepthAndExecutesCallbacks)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> attachments(
        NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount);
    for (size_t i = 0u; i < NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount; ++i)
        attachments[i].format = NLS::Render::FrameGraph::kDeferredGBufferColorSlots[i].format;
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
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;
    auto resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
        frameGraph,
        blackboard,
        frameDescriptor,
        preparedResources);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor =
        NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(frameDescriptor);

    const auto preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
        resourceRequest,
        lightGridContext,
        true);

    int beginTransparentCount = 0;
    int executeTransparentCount = 0;
    int endTransparentCount = 0;
    NLS::Render::FrameGraph::ExecutePreparedDeferredSceneGraph(
        frameGraph,
        preparedGraph,
        {
            [](const auto&) -> bool { return true; },
            []() {},
            []() {},
            [](const auto&) -> bool { return true; },
            []() {},
            [](bool, const auto&) {},
            [&](const auto&) -> bool
            {
                ++beginTransparentCount;
                return true;
            },
            [&]()
            {
                ++executeTransparentCount;
            },
            [&](bool startedRenderPass, const auto&)
            {
                EXPECT_TRUE(startedRenderPass);
                ++endTransparentCount;
            }
        });

    frameGraph.compile();
    std::ostringstream frameGraphDebug;
    frameGraph.debugOutput(frameGraphDebug, graphviz::Writer{});

    const auto graphvizOutput = frameGraphDebug.str();
    const auto transparentPassLabelPos = graphvizOutput.find("<B>DeferredTransparent</B>");
    ASSERT_NE(transparentPassLabelPos, std::string::npos) << graphvizOutput;
    const auto transparentPassLineStart = graphvizOutput.rfind('\n', transparentPassLabelPos);
    const auto transparentPassKeyStart = transparentPassLineStart == std::string::npos
        ? 0u
        : transparentPassLineStart + 1u;
    const auto transparentPassKeyEnd = graphvizOutput.find('[', transparentPassKeyStart);
    ASSERT_NE(transparentPassKeyStart, std::string::npos) << graphvizOutput;
    ASSERT_NE(transparentPassKeyEnd, std::string::npos) << graphvizOutput;
    const auto transparentPassKey = graphvizOutput.substr(
        transparentPassKeyStart,
        transparentPassKeyEnd - transparentPassKeyStart);

    bool transparentReadsGBufferDepth = false;
    size_t depthResourceLabelPos = 0u;
    while ((depthResourceLabelPos = graphvizOutput.find(
        "<B>DeferredGBufferDepth</B>",
        depthResourceLabelPos)) != std::string::npos)
    {
        const auto depthResourceLineStart = graphvizOutput.rfind('\n', depthResourceLabelPos);
        const auto depthResourceKeyStart = depthResourceLineStart == std::string::npos
            ? 0u
            : depthResourceLineStart + 1u;
        const auto depthResourceKeyEnd = graphvizOutput.find('[', depthResourceKeyStart);
        ASSERT_NE(depthResourceKeyEnd, std::string::npos) << graphvizOutput;
        const auto depthResourceKey = graphvizOutput.substr(
            depthResourceKeyStart,
            depthResourceKeyEnd - depthResourceKeyStart);

        const auto depthReadEdgeStart = graphvizOutput.find(depthResourceKey + "->{ ");
        if (depthReadEdgeStart != std::string::npos)
        {
            const auto depthReadEdgeEnd = graphvizOutput.find("} [color=yellowgreen]", depthReadEdgeStart);
            ASSERT_NE(depthReadEdgeEnd, std::string::npos) << graphvizOutput;
            const auto depthReadEdge = graphvizOutput.substr(
                depthReadEdgeStart,
                depthReadEdgeEnd - depthReadEdgeStart);
            transparentReadsGBufferDepth =
                transparentReadsGBufferDepth ||
                depthReadEdge.find(transparentPassKey) != std::string::npos;
        }
        ++depthResourceLabelPos;
    }
    EXPECT_TRUE(transparentReadsGBufferDepth) << graphvizOutput;

    NLS::Render::FrameGraph::FrameGraphExecutionContext executionContext{
        driver,
        explicitDevice.get(),
        nullptr,
        nullptr
    };
    frameGraph.execute(&executionContext, &executionContext);

    EXPECT_EQ(beginTransparentCount, 1);
    EXPECT_EQ(executeTransparentCount, 1);
    EXPECT_EQ(endTransparentCount, 1);
}

TEST(FrameGraphSceneTargetsTests, DeferredSceneGraphSkipsPassesWhenGBufferResourcesAreIncomplete)
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

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    auto expectSkipped = [&frameDescriptor, &lightGridContext](
        const char* label,
        const NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest& resources)
    {
        SCOPED_TRACE(label);

        FrameGraph frameGraph;
        FrameGraphBlackboard blackboard;
        const auto resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
            frameGraph,
            blackboard,
            frameDescriptor,
            resources);

        const auto preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
            resourceRequest,
            lightGridContext);
        EXPECT_TRUE(preparedGraph.execution.compiledExecution.graphPasses.empty());
    };

    auto missingResources = preparedResources;
    missingResources.gbufferAlbedoTexture = nullptr;
    missingResources.gbufferNormalTexture = nullptr;
    missingResources.gbufferMaterialTexture = nullptr;
    missingResources.gbufferDepthTexture = nullptr;
    expectSkipped("all GBuffer wrappers missing", missingResources);

    missingResources = preparedResources;
    missingResources.gbufferAlbedoTexture = nullptr;
    expectSkipped("albedo wrapper missing", missingResources);

    missingResources = preparedResources;
    missingResources.gbufferNormalTexture = nullptr;
    expectSkipped("normal wrapper missing", missingResources);

    missingResources = preparedResources;
    missingResources.gbufferMaterialTexture = nullptr;
    expectSkipped("material wrapper missing", missingResources);

    missingResources = preparedResources;
    missingResources.gbufferDepthTexture = nullptr;
    expectSkipped("depth wrapper missing", missingResources);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionSkipsDeferredPassesWhenGBufferResourcesAreIncomplete)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(2u);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;
    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources);

    EXPECT_TRUE(package.passCommandInputs.empty());
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionRejectsGBufferResourcesWithMissingAttachmentUsage)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(2u);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    auto resources = MakeCompleteDeferredPreparedSceneResources(
        NLS::Render::RHI::TextureUsageFlags::Sampled,
        NLS::Render::FrameGraph::kDeferredGBufferDepthUsage);
    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources);

    EXPECT_TRUE(package.passCommandInputs.empty());
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionPreservesExplicitHelperWhenGBufferResourcesAreInvalid)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(2u);

    NLS::Render::Context::RenderPassCommandInput helperPass;
    helperPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperPass.debugName = "EditorGridPass";
    helperPass.drawCount = 1u;

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata helperMetadata;
    helperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    helperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    helperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(helperMetadata, "EditorGridPass");

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    NLS::Render::FrameGraph::DeferredPreparedSceneResources invalidResources;
    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        invalidResources,
        { helperPass },
        { helperMetadata });

    ASSERT_EQ(package.passCommandInputs.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[0].debugName, "EditorGridPass");
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::Helper);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionDoesNotAttachStaleGBufferDepthToHelperWhenResourcesMismatch)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 1u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(1u);
    package.targetsSwapchain = true;

    NLS::Render::Context::RenderPassCommandInput helperPass;
    helperPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperPass.debugName = "EditorSelectionPass";
    helperPass.drawCount = 1u;
    helperPass.usesDepthStencilAttachment = true;

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata helperMetadata;
    helperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    helperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    helperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(helperMetadata, "EditorSelectionPass");

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    auto resources = MakeCompleteDeferredPreparedSceneResources(
        NLS::Render::RHI::TextureUsageFlags::Sampled,
        NLS::Render::FrameGraph::kDeferredGBufferDepthUsage);
    const auto staleDepthTexture = resources.gbufferDepthView->GetTexture();

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        { helperPass },
        { helperMetadata },
        MakeDeferredQueuedDrawCounts(0u, 0u));

    ASSERT_EQ(package.passCommandInputs.size(), 1u);
    const auto& preparedHelperPass = package.passCommandInputs[0];
    EXPECT_EQ(preparedHelperPass.debugName, "EditorSelectionPass");
    EXPECT_EQ(preparedHelperPass.kind, NLS::Render::Context::RenderPassCommandKind::Helper);
    EXPECT_EQ(preparedHelperPass.depthStencilAttachmentView, nullptr);
    EXPECT_TRUE(std::none_of(
        preparedHelperPass.textureResourceAccesses.begin(),
        preparedHelperPass.textureResourceAccesses.end(),
        [&staleDepthTexture](const NLS::Render::Context::TextureResourceAccess& access)
        {
            return access.texture == staleDepthTexture;
        }));
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionRejectsGBufferResourcesWithMismatchedViewDesc)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(2u);
    package.targetsSwapchain = true;

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    auto resources = MakeCompleteDeferredPreparedSceneResources();
    const auto gbufferDepthView = resources.gbufferDepthView;
    NLS::Render::RHI::RHITextureViewDesc mismatchedColorViewDesc =
        resources.gbufferColorViews[0]->GetDesc();
    mismatchedColorViewDesc.format = NLS::Render::RHI::TextureFormat::RGBA16F;
    resources.gbufferColorViews[0] = std::make_shared<TestTextureView>(
        resources.gbufferTextures[0],
        mismatchedColorViewDesc);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources);

    ASSERT_EQ(package.passCommandInputs.size(), 0u);

    package.passCommandInputs.clear();
    resources = MakeCompleteDeferredPreparedSceneResources();
    NLS::Render::RHI::RHITextureViewDesc mismatchedDepthViewDesc =
        resources.gbufferDepthView->GetDesc();
    mismatchedDepthViewDesc.subresourceRange.baseMipLevel = 1u;
    resources.gbufferDepthView = std::make_shared<TestTextureView>(
        resources.gbufferTextures[NLS::Render::FrameGraph::kDeferredGBufferDepthTextureIndex],
        mismatchedDepthViewDesc);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources);

    ASSERT_EQ(package.passCommandInputs.size(), 0u);
    EXPECT_NE(gbufferDepthView, nullptr);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionDeclaresDepthStencilWriteForWritingHelperPass)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.helperDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(2u);
    package.targetsSwapchain = true;

    NLS::Render::Context::RenderPassCommandInput helperPass;
    helperPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperPass.debugName = "EditorSelectionPass";
    helperPass.drawCount = 1u;
    helperPass.usesDepthStencilAttachment = true;
    helperPass.writesDepthStencilAttachment = true;
    helperPass.recordedDrawCommands.resize(1u);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata helperMetadata;
    helperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    helperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    helperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(helperMetadata, "EditorSelectionPass");

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    auto resources = MakeCompleteDeferredPreparedSceneResources();
    const auto gbufferDepthTexture = resources.gbufferDepthView->GetTexture();

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        { helperPass },
        { helperMetadata },
        MakeDeferredQueuedDrawCounts(0u, 0u));

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    const auto& preparedHelperPass = package.passCommandInputs[2];
    EXPECT_EQ(preparedHelperPass.debugName, "EditorSelectionPass");
    ASSERT_EQ(preparedHelperPass.recordedDrawCommands.size(), 1u);
    EXPECT_EQ(preparedHelperPass.depthStencilAttachmentView, resources.gbufferDepthView);
    const auto depthAccess = std::find_if(
        preparedHelperPass.textureResourceAccesses.begin(),
        preparedHelperPass.textureResourceAccesses.end(),
        [&gbufferDepthTexture](const NLS::Render::Context::TextureResourceAccess& access)
        {
            return access.texture == gbufferDepthTexture;
        });
    ASSERT_NE(depthAccess, preparedHelperPass.textureResourceAccesses.end());
    EXPECT_EQ(depthAccess->mode, NLS::Render::Context::ResourceAccessMode::Write);
    EXPECT_EQ(depthAccess->state, NLS::Render::RHI::ResourceState::DepthWrite);
    EXPECT_EQ(depthAccess->stages, NLS::Render::RHI::PipelineStageMask::DepthStencil);
    EXPECT_EQ(
        depthAccess->access,
        NLS::Render::RHI::AccessMask::DepthStencilRead |
            NLS::Render::RHI::AccessMask::DepthStencilWrite);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionReplacesStaleHelperDepthAccessWithGBufferDepth)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 1u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(1u);
    package.targetsSwapchain = true;

    auto staleDepthTexture = std::make_shared<TestTexture>(
        MakeTestTextureDesc(
            "SceneViewDepth",
            NLS::Render::FrameGraph::kDeferredGBufferDepthFormat,
            NLS::Render::FrameGraph::kDeferredGBufferDepthUsage));
    NLS::Render::RHI::RHITextureViewDesc staleDepthViewDesc;
    staleDepthViewDesc.debugName = "SceneViewDepthView";
    staleDepthViewDesc.format = staleDepthTexture->GetDesc().format;
    auto staleDepthView = std::make_shared<TestTextureView>(staleDepthTexture, staleDepthViewDesc);

    NLS::Render::Context::RenderPassCommandInput helperPass;
    helperPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperPass.debugName = "SelectionOutlineMask::CaptureMask";
    helperPass.drawCount = 1u;
    helperPass.usesDepthStencilAttachment = true;
    helperPass.writesDepthStencilAttachment = false;
    helperPass.depthStencilAttachmentView = staleDepthView;
    NLS::Render::RHI::RHISubresourceRange staleRange;
    staleRange.mipLevelCount = staleDepthTexture->GetDesc().mipLevels;
    staleRange.arrayLayerCount = staleDepthTexture->GetDesc().arrayLayers;
    helperPass.textureResourceAccesses.push_back({
        staleDepthTexture,
        staleRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::DepthRead,
        NLS::Render::RHI::PipelineStageMask::DepthStencil,
        NLS::Render::RHI::AccessMask::DepthStencilRead
    });

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata helperMetadata;
    helperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    helperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    helperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(helperMetadata, "SelectionOutlineMask::CaptureMask");

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    auto resources = MakeCompleteDeferredPreparedSceneResources();
    const auto gbufferDepthTexture = resources.gbufferDepthView->GetTexture();

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        { helperPass },
        { helperMetadata });

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    const auto& preparedHelperPass = package.passCommandInputs[2];
    EXPECT_EQ(preparedHelperPass.depthStencilAttachmentView, resources.gbufferDepthView);
    EXPECT_TRUE(std::none_of(
        preparedHelperPass.textureResourceAccesses.begin(),
        preparedHelperPass.textureResourceAccesses.end(),
        [&staleDepthTexture](const NLS::Render::Context::TextureResourceAccess& access)
        {
            return access.texture == staleDepthTexture;
        }));

    const auto gbufferDepthAccess = std::find_if(
        preparedHelperPass.textureResourceAccesses.begin(),
        preparedHelperPass.textureResourceAccesses.end(),
        [&gbufferDepthTexture](const NLS::Render::Context::TextureResourceAccess& access)
        {
            return access.texture == gbufferDepthTexture;
        });
    ASSERT_NE(gbufferDepthAccess, preparedHelperPass.textureResourceAccesses.end());
    EXPECT_EQ(gbufferDepthAccess->mode, NLS::Render::Context::ResourceAccessMode::Read);
    EXPECT_EQ(gbufferDepthAccess->state, NLS::Render::RHI::ResourceState::DepthRead);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionDeclaresDepthStencilWriteForGBufferPass)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 1u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(1u);
    package.targetsSwapchain = true;

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    auto resources = MakeCompleteDeferredPreparedSceneResources();

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources);

    ASSERT_GE(package.passCommandInputs.size(), 1u);
    const auto& gbufferPass = package.passCommandInputs[0];
    EXPECT_EQ(gbufferPass.kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_TRUE(gbufferPass.usesDepthStencilAttachment);
    EXPECT_TRUE(gbufferPass.writesDepthStencilAttachment);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionSlicesTransparentBeforeHelperAndKeepsExternalColor)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.helperDrawCount = 1u;
    package.visibleDrawCount = 4u;
    package.drawCommandCount = 4u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.targetsSwapchain = false;

    auto gbufferPipeline = std::make_shared<TestGraphicsPipeline>("DeferredGBufferPipeline");
    auto lightingPipeline = std::make_shared<TestGraphicsPipeline>("DeferredLightingPipeline");
    auto transparentPipeline = std::make_shared<TestGraphicsPipeline>("DeferredTransparentPipeline");
    auto helperPipeline = std::make_shared<TestGraphicsPipeline>("DeferredHelperPipeline");
    package.recordedDrawCommands = {
        { gbufferPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { lightingPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { transparentPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { helperPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u }
    };
    const auto helperMetadata = MakeThreadedPassMetadata(
        NLS::Render::Context::RenderPassCommandKind::Helper,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper,
        "EditorHelperPass");

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    auto resources = MakeCompleteDeferredPreparedSceneResources();
    const auto outputColorView = frameDescriptor.outputColorView;
    ASSERT_NE(outputColorView, nullptr);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        {},
        { helperMetadata },
        MakeDeferredQueuedDrawCounts(1u, 0u, 1u));

    ASSERT_EQ(package.passCommandInputs.size(), 4u);
    const auto& gbufferPass = package.passCommandInputs[0];
    const auto& lightingPass = package.passCommandInputs[1];
    const auto& transparentPass = package.passCommandInputs[2];
    const auto& helperPass = package.passCommandInputs[3];

    EXPECT_EQ(gbufferPass.kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(lightingPass.kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_EQ(transparentPass.kind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_EQ(helperPass.kind, NLS::Render::Context::RenderPassCommandKind::Helper);

    ASSERT_EQ(gbufferPass.recordedDrawCommands.size(), 1u);
    ASSERT_EQ(lightingPass.recordedDrawCommands.size(), 1u);
    ASSERT_EQ(transparentPass.recordedDrawCommands.size(), 1u);
    ASSERT_EQ(helperPass.recordedDrawCommands.size(), 1u);
    EXPECT_EQ(gbufferPass.recordedDrawCommands[0].pipeline, gbufferPipeline);
    EXPECT_EQ(lightingPass.recordedDrawCommands[0].pipeline, lightingPipeline);
    EXPECT_EQ(transparentPass.recordedDrawCommands[0].pipeline, transparentPipeline);
    EXPECT_EQ(helperPass.recordedDrawCommands[0].pipeline, helperPipeline);

    ASSERT_EQ(lightingPass.colorAttachmentViews.size(), 1u);
    ASSERT_EQ(transparentPass.colorAttachmentViews.size(), 1u);
    ASSERT_EQ(helperPass.colorAttachmentViews.size(), 1u);
    EXPECT_EQ(lightingPass.colorAttachmentViews[0], outputColorView);
    EXPECT_EQ(transparentPass.colorAttachmentViews[0], outputColorView);
    EXPECT_EQ(helperPass.colorAttachmentViews[0], outputColorView);
    EXPECT_EQ(transparentPass.depthStencilAttachmentView, resources.gbufferDepthView);
    EXPECT_FALSE(transparentPass.writesDepthStencilAttachment);
    EXPECT_FALSE(transparentPass.clearColor);
    EXPECT_FALSE(transparentPass.clearDepth);
    EXPECT_FALSE(transparentPass.clearStencil);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionRejectsPostOpaqueSceneDrawsWithoutQueuedCounts)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.visibleDrawCount = 2u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.targetsSwapchain = false;

    auto gbufferPipeline = std::make_shared<TestGraphicsPipeline>("DeferredGBufferPipeline");
    auto transparentPipeline = std::make_shared<TestGraphicsPipeline>("DeferredTransparentPipeline");
    package.recordedDrawCommands = {
        { gbufferPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { transparentPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u }
    };

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    auto resources = MakeCompleteDeferredPreparedSceneResources();

    EXPECT_THROW(
        NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
            package,
            lightGridContext,
            resources),
        std::invalid_argument);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionDoesNotSliceTransparentAcrossUnknownLightingDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.visibleDrawCount = 3u;
    package.drawCommandCount = 3u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.targetsSwapchain = false;

    auto gbufferPipeline = std::make_shared<TestGraphicsPipeline>("DeferredGBufferPipeline");
    auto lightingPipeline = std::make_shared<TestGraphicsPipeline>("DeferredLightingPipeline");
    auto transparentPipeline = std::make_shared<TestGraphicsPipeline>("DeferredTransparentPipeline");
    package.recordedDrawCommands = {
        { gbufferPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { lightingPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { transparentPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u }
    };

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    auto resources = MakeCompleteDeferredPreparedSceneResources();

    EXPECT_THROW(
        NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
            package,
            lightGridContext,
            resources),
        std::invalid_argument);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionRejectsPartialPostLightingDrawCounts)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.helperDrawCount = 1u;
    package.visibleDrawCount = 4u;
    package.drawCommandCount = 4u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.targetsSwapchain = false;

    auto gbufferPipeline = std::make_shared<TestGraphicsPipeline>("DeferredGBufferPipeline");
    auto lightingPipeline = std::make_shared<TestGraphicsPipeline>("DeferredLightingPipeline");
    auto transparentPipeline = std::make_shared<TestGraphicsPipeline>("DeferredTransparentPipeline");
    auto helperPipeline = std::make_shared<TestGraphicsPipeline>("DeferredHelperPipeline");
    package.recordedDrawCommands = {
        { gbufferPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { lightingPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { transparentPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { helperPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u }
    };

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    auto resources = MakeCompleteDeferredPreparedSceneResources();

    EXPECT_THROW(
        NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
            package,
            lightGridContext,
            resources,
            {},
            {},
            MakeDeferredQueuedDrawCounts(1u)),
        std::invalid_argument);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionRejectsPartialPostLightingCountsWhenPostLightingStreamIsShort)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.helperDrawCount = 1u;
    package.visibleDrawCount = 4u;
    package.drawCommandCount = 4u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.targetsSwapchain = false;

    auto gbufferPipeline = std::make_shared<TestGraphicsPipeline>("DeferredGBufferPipeline");
    auto lightingPipeline = std::make_shared<TestGraphicsPipeline>("DeferredLightingPipeline");
    auto postLightingPipeline = std::make_shared<TestGraphicsPipeline>("DeferredPostLightingPipeline");
    package.recordedDrawCommands = {
        { gbufferPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { lightingPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { postLightingPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u }
    };

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    auto resources = MakeCompleteDeferredPreparedSceneResources();

    EXPECT_THROW(
        NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
            package,
            lightGridContext,
            resources,
            {},
            {},
            MakeDeferredQueuedDrawCounts(1u)),
        std::invalid_argument);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionSlicesDecalBeforeLightingAndTransparentAfterLighting)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.decalDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.helperDrawCount = 1u;
    package.visibleDrawCount = 5u;
    package.drawCommandCount = 5u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.targetsSwapchain = false;

    auto gbufferPipeline = std::make_shared<TestGraphicsPipeline>("DeferredGBufferPipeline");
    auto decalPipeline = std::make_shared<TestGraphicsPipeline>("DeferredDecalPipeline");
    auto lightingPipeline = std::make_shared<TestGraphicsPipeline>("DeferredLightingPipeline");
    auto transparentPipeline = std::make_shared<TestGraphicsPipeline>("DeferredTransparentPipeline");
    auto helperPipeline = std::make_shared<TestGraphicsPipeline>("DeferredHelperPipeline");
    package.recordedDrawCommands = {
        { gbufferPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { decalPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { lightingPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { transparentPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { helperPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u }
    };
    const auto helperMetadata = MakeThreadedPassMetadata(
        NLS::Render::Context::RenderPassCommandKind::Helper,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper,
        "EditorHelperPass");

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    auto resources = MakeCompleteDeferredPreparedSceneResources();
    const auto outputColorView = frameDescriptor.outputColorView;
    ASSERT_NE(outputColorView, nullptr);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        {},
        { helperMetadata },
        MakeDeferredQueuedDrawCounts(1u, 1u, 1u));

    ASSERT_EQ(package.passCommandInputs.size(), 5u);
    const auto& gbufferPass = package.passCommandInputs[0];
    const auto& decalPass = package.passCommandInputs[1];
    const auto& lightingPass = package.passCommandInputs[2];
    const auto& transparentPass = package.passCommandInputs[3];
    const auto& helperPass = package.passCommandInputs[4];

    EXPECT_EQ(gbufferPass.kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(decalPass.kind, NLS::Render::Context::RenderPassCommandKind::Decal);
    EXPECT_EQ(lightingPass.kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_EQ(transparentPass.kind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_EQ(helperPass.kind, NLS::Render::Context::RenderPassCommandKind::Helper);

    ASSERT_EQ(gbufferPass.recordedDrawCommands.size(), 1u);
    ASSERT_EQ(decalPass.recordedDrawCommands.size(), 1u);
    ASSERT_EQ(lightingPass.recordedDrawCommands.size(), 1u);
    ASSERT_EQ(transparentPass.recordedDrawCommands.size(), 1u);
    ASSERT_EQ(helperPass.recordedDrawCommands.size(), 1u);
    EXPECT_EQ(gbufferPass.recordedDrawCommands[0].pipeline, gbufferPipeline);
    EXPECT_EQ(decalPass.recordedDrawCommands[0].pipeline, decalPipeline);
    EXPECT_EQ(lightingPass.recordedDrawCommands[0].pipeline, lightingPipeline);
    EXPECT_EQ(transparentPass.recordedDrawCommands[0].pipeline, transparentPipeline);
    EXPECT_EQ(helperPass.recordedDrawCommands[0].pipeline, helperPipeline);

    ASSERT_EQ(decalPass.colorAttachmentViews.size(), resources.gbufferColorViews.size());
    EXPECT_EQ(decalPass.colorAttachmentViews[0], resources.gbufferColorViews[0]);
    EXPECT_EQ(decalPass.colorAttachmentViews[1], resources.gbufferColorViews[1]);
    EXPECT_EQ(decalPass.colorAttachmentViews[2], resources.gbufferColorViews[2]);
    EXPECT_EQ(decalPass.depthStencilAttachmentView, resources.gbufferDepthView);
    EXPECT_TRUE(decalPass.usesColorAttachment);
    EXPECT_TRUE(decalPass.usesDepthStencilAttachment);
    EXPECT_FALSE(decalPass.writesDepthStencilAttachment);
    EXPECT_FALSE(decalPass.clearColor);
    EXPECT_FALSE(decalPass.clearDepth);
    EXPECT_FALSE(decalPass.clearStencil);
    const auto albedoTexture = resources.gbufferColorViews[0]->GetTexture();
    const auto normalTexture = resources.gbufferColorViews[1]->GetTexture();
    const auto materialTexture = resources.gbufferColorViews[2]->GetTexture();
    const auto findTextureAccess = [&decalPass](const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
    {
        return std::find_if(
            decalPass.textureResourceAccesses.begin(),
            decalPass.textureResourceAccesses.end(),
            [&texture](const NLS::Render::Context::TextureResourceAccess& access)
            {
                return access.texture == texture;
            });
    };
    const auto albedoAccess = findTextureAccess(albedoTexture);
    ASSERT_NE(albedoAccess, decalPass.textureResourceAccesses.end());
    EXPECT_EQ(albedoAccess->mode, NLS::Render::Context::ResourceAccessMode::Write);
    EXPECT_EQ(albedoAccess->state, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(albedoAccess->stages, NLS::Render::RHI::PipelineStageMask::RenderTarget);
    EXPECT_EQ(
        albedoAccess->access,
        NLS::Render::RHI::AccessMask::ColorAttachmentRead |
            NLS::Render::RHI::AccessMask::ColorAttachmentWrite);

    for (const auto& texture : { normalTexture, materialTexture })
    {
        const auto access = findTextureAccess(texture);
        ASSERT_NE(access, decalPass.textureResourceAccesses.end());
        EXPECT_EQ(access->mode, NLS::Render::Context::ResourceAccessMode::Write);
        EXPECT_EQ(access->state, NLS::Render::RHI::ResourceState::RenderTarget);
        EXPECT_EQ(access->stages, NLS::Render::RHI::PipelineStageMask::RenderTarget);
        EXPECT_EQ(
            access->access,
            NLS::Render::RHI::AccessMask::ColorAttachmentRead |
                NLS::Render::RHI::AccessMask::ColorAttachmentWrite);
    }

    ASSERT_EQ(lightingPass.colorAttachmentViews.size(), 1u);
    ASSERT_EQ(transparentPass.colorAttachmentViews.size(), 1u);
    ASSERT_EQ(helperPass.colorAttachmentViews.size(), 1u);
    EXPECT_EQ(lightingPass.colorAttachmentViews[0], outputColorView);
    EXPECT_EQ(transparentPass.colorAttachmentViews[0], outputColorView);
    EXPECT_EQ(helperPass.colorAttachmentViews[0], outputColorView);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedExecutionPrefersTypedPassInputsOverRecordedDrawOrder)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.decalDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.helperDrawCount = 1u;
    package.visibleDrawCount = 5u;
    package.drawCommandCount = 5u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.targetsSwapchain = false;

    auto typedGBufferPipeline = std::make_shared<TestGraphicsPipeline>("TypedDeferredGBufferPipeline");
    auto typedDecalPipeline = std::make_shared<TestGraphicsPipeline>("TypedDeferredDecalPipeline");
    auto typedLightingPipeline = std::make_shared<TestGraphicsPipeline>("TypedDeferredLightingPipeline");
    auto typedTransparentPipeline = std::make_shared<TestGraphicsPipeline>("TypedDeferredTransparentPipeline");
    auto typedHelperPipeline = std::make_shared<TestGraphicsPipeline>("TypedDeferredHelperPipeline");

    auto makePassInput = [](
        const NLS::Render::Context::RenderPassCommandKind kind,
        const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline,
        const char* debugName)
    {
        NLS::Render::Context::RenderPassCommandInput input;
        input.kind = kind;
        input.drawCount = 1u;
        input.debugName = debugName;
        input.recordedDrawCommands.push_back({ pipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u });
        return input;
    };

    package.passCommandInputs = {
        makePassInput(NLS::Render::Context::RenderPassCommandKind::GBuffer, typedGBufferPipeline, "TypedDeferredGBuffer"),
        makePassInput(NLS::Render::Context::RenderPassCommandKind::Decal, typedDecalPipeline, "TypedDeferredDecal"),
        makePassInput(NLS::Render::Context::RenderPassCommandKind::Lighting, typedLightingPipeline, "TypedDeferredLighting"),
        makePassInput(NLS::Render::Context::RenderPassCommandKind::Transparent, typedTransparentPipeline, "TypedDeferredTransparent"),
        makePassInput(NLS::Render::Context::RenderPassCommandKind::Helper, typedHelperPipeline, "TypedDeferredHelper")
    };
    package.containsCommandInputs = true;

    auto wrongGBufferPipeline = std::make_shared<TestGraphicsPipeline>("WrongDeferredGBufferPipeline");
    auto wrongDecalPipeline = std::make_shared<TestGraphicsPipeline>("WrongDeferredDecalPipeline");
    auto wrongLightingPipeline = std::make_shared<TestGraphicsPipeline>("WrongDeferredLightingPipeline");
    auto wrongTransparentPipeline = std::make_shared<TestGraphicsPipeline>("WrongDeferredTransparentPipeline");
    auto wrongHelperPipeline = std::make_shared<TestGraphicsPipeline>("WrongDeferredHelperPipeline");
    package.recordedDrawCommands = {
        { wrongTransparentPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { wrongLightingPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { wrongDecalPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { wrongGBufferPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u },
        { wrongHelperPipeline, nullptr, nullptr, nullptr, nullptr, nullptr, 1u }
    };

    const auto helperMetadata = MakeThreadedPassMetadata(
        NLS::Render::Context::RenderPassCommandKind::Helper,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper,
        "TypedDeferredHelper");

    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    auto resources = MakeCompleteDeferredPreparedSceneResources();
    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        {},
        { helperMetadata },
        MakeDeferredQueuedDrawCounts(1u, 1u, 1u));

    ASSERT_EQ(package.passCommandInputs.size(), 5u);
    ASSERT_EQ(package.passCommandInputs[0].recordedDrawCommands.size(), 1u);
    ASSERT_EQ(package.passCommandInputs[1].recordedDrawCommands.size(), 1u);
    ASSERT_EQ(package.passCommandInputs[2].recordedDrawCommands.size(), 1u);
    ASSERT_EQ(package.passCommandInputs[3].recordedDrawCommands.size(), 1u);
    ASSERT_EQ(package.passCommandInputs[4].recordedDrawCommands.size(), 1u);

    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Decal);
    EXPECT_EQ(package.passCommandInputs[2].kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_EQ(package.passCommandInputs[3].kind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_EQ(package.passCommandInputs[4].kind, NLS::Render::Context::RenderPassCommandKind::Helper);

    EXPECT_EQ(package.passCommandInputs[0].recordedDrawCommands[0].pipeline, typedGBufferPipeline);
    EXPECT_EQ(package.passCommandInputs[1].recordedDrawCommands[0].pipeline, typedDecalPipeline);
    EXPECT_EQ(package.passCommandInputs[2].recordedDrawCommands[0].pipeline, typedLightingPipeline);
    EXPECT_EQ(package.passCommandInputs[3].recordedDrawCommands[0].pipeline, typedTransparentPipeline);
    EXPECT_EQ(package.passCommandInputs[4].recordedDrawCommands[0].pipeline, typedHelperPipeline);
}

TEST(FrameGraphSceneTargetsTests, DeferredPreparedResourcesRejectMismatchedGBufferWrappers)
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
        std::make_shared<TestTexture>(MakeTestTextureDesc("ForeignGBufferAlbedo")),
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

    NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest request;
    request.gBuffer = &gBuffer;
    request.gbufferAlbedoTexture = albedoTexture.get();
    request.gbufferNormalTexture = normalTexture.get();
    request.gbufferMaterialTexture = materialTexture.get();
    request.gbufferDepthTexture = depthTexture.get();

    const auto resources = NLS::Render::FrameGraph::CaptureDeferredPreparedSceneResources(request);

    EXPECT_TRUE(resources.gbufferColorViews.empty());
    EXPECT_EQ(resources.gbufferDepthView, nullptr);
    EXPECT_TRUE(resources.gbufferTextures.empty());

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;

    const auto resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
        frameGraph,
        blackboard,
        frameDescriptor,
        request);
    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;
    const auto preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
        resourceRequest,
        lightGridContext);
    EXPECT_TRUE(preparedGraph.execution.compiledExecution.graphPasses.empty());
}

TEST(FrameGraphSceneTargetsTests, DeferredSceneGraphRejectsGBufferResourcesWithMismatchedFrameExtent)
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

    NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest request;
    request.gBuffer = &gBuffer;
    request.gbufferAlbedoTexture = albedoTexture.get();
    request.gbufferNormalTexture = normalTexture.get();
    request.gbufferMaterialTexture = materialTexture.get();
    request.gbufferDepthTexture = depthTexture.get();

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 640u;
    frameDescriptor.renderHeight = 360u;
    frameDescriptor.camera = &camera;

    const auto resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
        frameGraph,
        blackboard,
        frameDescriptor,
        request);
    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;
    const auto preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
        resourceRequest,
        lightGridContext);

    EXPECT_TRUE(preparedGraph.execution.compiledExecution.graphPasses.empty());
    EXPECT_LT(preparedGraph.resources.gbufferAlbedo, 0);
    EXPECT_LT(preparedGraph.resources.gbufferDepth, 0);
}

TEST(FrameGraphSceneTargetsTests, DeferredSceneGraphRejectsGBufferResourcesWithUnexpectedColorFormat)
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
    attachments[1].format = NLS::Render::RHI::TextureFormat::RGBA16F;
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

    NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest request;
    request.gBuffer = &gBuffer;
    request.gbufferAlbedoTexture = albedoTexture.get();
    request.gbufferNormalTexture = normalTexture.get();
    request.gbufferMaterialTexture = materialTexture.get();
    request.gbufferDepthTexture = depthTexture.get();

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;

    const auto resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
        frameGraph,
        blackboard,
        frameDescriptor,
        request);
    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;
    const auto preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
        resourceRequest,
        lightGridContext);

    EXPECT_TRUE(preparedGraph.execution.compiledExecution.graphPasses.empty());
    EXPECT_LT(preparedGraph.resources.gbufferNormal, 0);
    EXPECT_LT(preparedGraph.resources.gbufferDepth, 0);
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

TEST(FrameGraphSceneTargetsTests, FrameGraphExecutionContextCommitsOnlyFilteredBarriersToResourceTracker)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 24u;
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    commandBuffer->dropLastTextureBarrier = true;
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::FrameGraph::FrameGraphExecutionContext executionContext{
        driver,
        explicitDevice.get(),
        commandBuffer.get(),
        &frameContext
    };

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "FilteredBarrierTexture";
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.arrayLayers = 2u;
    textureDesc.mipLevels = 1u;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto texture = std::make_shared<TestTexture>(textureDesc);

    NLS::Render::RHI::RHIBarrierDesc request;
    request.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::RenderTarget,
        { 0u, 1u, 0u, 1u },
        NLS::Render::RHI::PipelineStageMask::AllCommands,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::AccessMask::MemoryRead,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });
    request.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        { 0u, 1u, 1u, 1u },
        NLS::Render::RHI::PipelineStageMask::AllCommands,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::MemoryRead,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    executionContext.RecordResourceBarriers(request);

    ASSERT_EQ(commandBuffer->barrierCalls, 1u);
    ASSERT_EQ(commandBuffer->barrierHistory.size(), 1u);
    ASSERT_EQ(commandBuffer->barrierHistory[0].textureBarriers.size(), 1u);
    EXPECT_EQ(commandBuffer->barrierHistory[0].textureBarriers[0].after, NLS::Render::RHI::ResourceState::RenderTarget);

    const auto trackedFirstRange = frameContext.resourceStateTracker->GetTextureState(texture, { 0u, 1u, 0u, 1u });
    ASSERT_TRUE(trackedFirstRange.has_value());
    EXPECT_EQ(trackedFirstRange->state, NLS::Render::RHI::ResourceState::RenderTarget);
    const auto trackedSecondRange = frameContext.resourceStateTracker->GetTextureState(texture, { 0u, 1u, 1u, 1u });
    ASSERT_FALSE(trackedSecondRange.has_value());
}

TEST(FrameGraphSceneTargetsTests, FrameGraphExecutionContextDoesNotCommitWhenBarrierRecordingFails)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 25u;
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    commandBuffer->failBarrierChecked = true;
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::FrameGraph::FrameGraphExecutionContext executionContext{
        driver,
        explicitDevice.get(),
        commandBuffer.get(),
        &frameContext
    };

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "RejectedBarrierTexture";
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.arrayLayers = 1u;
    textureDesc.mipLevels = 1u;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto texture = std::make_shared<TestTexture>(textureDesc);

    NLS::Render::RHI::RHIBarrierDesc request;
    request.textureBarriers.push_back({
        texture,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::RenderTarget,
        { 0u, 1u, 0u, 1u },
        NLS::Render::RHI::PipelineStageMask::AllCommands,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::AccessMask::MemoryRead,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });

    EXPECT_FALSE(executionContext.RecordResourceBarriers(request).Succeeded());
    EXPECT_EQ(commandBuffer->barrierCheckedCalls, 1u);
    EXPECT_EQ(commandBuffer->barrierCalls, 0u);
    const auto trackedState =
        frameContext.resourceStateTracker->GetTextureState(texture, { 0u, 1u, 0u, 1u });
    EXPECT_FALSE(trackedState.has_value());
}

TEST(FrameGraphSceneTargetsTests, DepthAttachmentEndTransitionKeepsShaderReadStateForDX12Sampling)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "DepthSampledTransitionTexture";
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.arrayLayers = 1u;
    textureDesc.mipLevels = 1u;
    textureDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    textureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto texture = std::make_shared<TestTexture>(textureDesc);

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.debugName = "DepthSampledTransitionView";
    viewDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
    viewDesc.subresourceRange.baseMipLevel = 0u;
    viewDesc.subresourceRange.mipLevelCount = 1u;
    viewDesc.subresourceRange.baseArrayLayer = 0u;
    viewDesc.subresourceRange.arrayLayerCount = 1u;
    auto view = std::make_shared<TestTextureView>(texture, viewDesc);

    const auto transition = NLS::Render::FrameGraph::BuildSampledAttachmentEndTransition(view, true);
    ASSERT_TRUE(transition.has_value());
    EXPECT_EQ(
        transition->after,
        NLS::Render::RHI::ResourceState::DepthRead | NLS::Render::RHI::ResourceState::ShaderRead);
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

TEST(FrameGraphSceneTargetsTests, PartialVisibilityTransitionDoesNotSuppressUncoveredTextureRange)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "PartialVisibilityCoverageTexture";
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.mipLevels = 1u;
    textureDesc.arrayLayers = 2u;
    auto texture = std::make_shared<TestTexture>(textureDesc);

    NLS::Render::RHI::RHISubresourceRange firstLayerRange;
    firstLayerRange.baseMipLevel = 0u;
    firstLayerRange.mipLevelCount = 1u;
    firstLayerRange.baseArrayLayer = 0u;
    firstLayerRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 2u;

    NLS::Render::Context::RenderPassCommandInput sourceInput;
    sourceInput.textureResourceAccesses.push_back({
        texture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::RenderTarget,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });

    NLS::Render::Context::ParallelCommandWorkUnit sourceWorkUnit;
    sourceWorkUnit.commandInput = sourceInput;
    sourceWorkUnit.workUnitIndex = 0u;

    NLS::Render::Context::RenderPassCommandInput targetInput;
    targetInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    targetInput.dependencySourceWorkUnitIndex = 0u;
    targetInput.textureVisibilityTransitions.push_back({
        texture,
        firstLayerRange,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    targetInput.textureResourceAccesses.push_back({
        texture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    NLS::Render::Context::ParallelCommandWorkUnit targetWorkUnit;
    targetWorkUnit.commandInput = targetInput;
    targetWorkUnit.workUnitIndex = 1u;

    std::vector<NLS::Render::Context::ParallelCommandWorkUnit> workUnits{
        sourceWorkUnit,
        targetWorkUnit
    };
    NLS::Render::Context::Detail::PopulateVisibilityTransitionsFromResourceUsage(workUnits);

    ASSERT_EQ(workUnits[1].commandInput.textureVisibilityTransitions.size(), 2u);
    EXPECT_EQ(workUnits[1].commandInput.textureVisibilityTransitions[1].subresourceRange.baseArrayLayer, 0u);
    EXPECT_EQ(workUnits[1].commandInput.textureVisibilityTransitions[1].subresourceRange.arrayLayerCount, 2u);
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

TEST(FrameGraphSceneTargetsTests, BeginPassDepthStencilAttachmentBarrierFollowsWriteDeclaration)
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

    NLS::Render::RHI::RHITextureDesc depthDesc;
    depthDesc.debugName = "TrackedDepthStencilAttachment";
    depthDesc.extent = { 64u, 64u, 1u };
    depthDesc.mipLevels = 1u;
    depthDesc.arrayLayers = 1u;
    depthDesc.format = NLS::Render::FrameGraph::kDeferredGBufferDepthFormat;
    depthDesc.usage = NLS::Render::FrameGraph::kDeferredGBufferDepthUsage;
    auto depthTexture = std::make_shared<TestTexture>(depthDesc);

    NLS::Render::RHI::RHITextureViewDesc depthViewDesc;
    depthViewDesc.debugName = "TrackedDepthStencilAttachmentView";
    depthViewDesc.format = depthDesc.format;
    depthViewDesc.subresourceRange.baseMipLevel = 0u;
    depthViewDesc.subresourceRange.mipLevelCount = 1u;
    depthViewDesc.subresourceRange.baseArrayLayer = 0u;
    depthViewDesc.subresourceRange.arrayLayerCount = 1u;
    auto depthView = std::make_shared<TestTextureView>(depthTexture, depthViewDesc);

    auto beginPass = [&](const bool writesDepthStencilAttachment) -> NLS::Render::RHI::RHITextureBarrier
    {
        commandBuffer->barrierCalls = 0u;
        commandBuffer->barrierHistory.clear();
        commandBuffer->beginRenderPassCalls = 0u;

        NLS::Render::Context::RenderPassCommandInput passInput;
        passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
        passInput.debugName = writesDepthStencilAttachment
            ? "TrackedDepthStencilWritePass"
            : "TrackedDepthStencilReadPass";
        passInput.renderWidth = 64u;
        passInput.renderHeight = 64u;
        passInput.usesDepthStencilAttachment = true;
        passInput.depthStencilAttachmentView = depthView;
        passInput.writesDepthStencilAttachment = writesDepthStencilAttachment;

        const bool startedPass = NLS::Render::Context::Detail::BeginPassCommandPlan(
            *commandBuffer,
            nullptr,
            nullptr,
            passInput,
            &frameContext);

        EXPECT_TRUE(startedPass);
        EXPECT_EQ(commandBuffer->barrierCalls, 1u);
        EXPECT_EQ(commandBuffer->barrierHistory.size(), 1u);
        if (!startedPass ||
            commandBuffer->barrierHistory.empty() ||
            commandBuffer->barrierHistory[0].textureBarriers.empty())
        {
            return {};
        }
        EXPECT_EQ(commandBuffer->barrierHistory[0].textureBarriers.size(), 1u);
        return commandBuffer->barrierHistory[0].textureBarriers[0];
    };

        const auto readBarrier = beginPass(false);
        EXPECT_EQ(readBarrier.texture, depthTexture);
        EXPECT_EQ(readBarrier.after, NLS::Render::RHI::ResourceState::DepthRead);
        EXPECT_EQ(readBarrier.destinationAccessMask, NLS::Render::RHI::AccessMask::DepthStencilRead);
        ASSERT_TRUE(commandBuffer->lastRenderPassDesc.depthStencilAttachment.has_value());
        EXPECT_TRUE(commandBuffer->lastRenderPassDesc.depthStencilAttachment->readOnlyDepthStencil);

        const auto writeBarrier = beginPass(true);
        EXPECT_EQ(writeBarrier.texture, depthTexture);
        EXPECT_EQ(writeBarrier.after, NLS::Render::RHI::ResourceState::DepthWrite);
        EXPECT_EQ(
            writeBarrier.destinationAccessMask,
            NLS::Render::RHI::AccessMask::DepthStencilRead |
                NLS::Render::RHI::AccessMask::DepthStencilWrite);
        ASSERT_TRUE(commandBuffer->lastRenderPassDesc.depthStencilAttachment.has_value());
        EXPECT_FALSE(commandBuffer->lastRenderPassDesc.depthStencilAttachment->readOnlyDepthStencil);
}

TEST(FrameGraphSceneTargetsTests, CompiledThreadedPassInputInfersDepthStencilWrites)
{
    NLS::Render::FrameGraph::CompiledThreadedRenderSceneGraphPass compiledPass;
    compiledPass.metadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    compiledPass.metadata.executionMode = NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Output;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(compiledPass.metadata, "ForwardOpaque");
    compiledPass.outputExecution.renderWidth = 320u;
    compiledPass.outputExecution.renderHeight = 180u;
    compiledPass.outputExecution.clearDepth = false;
    compiledPass.outputExecution.clearStencil = false;

    auto opaquePass = NLS::Render::FrameGraph::MakeCompiledThreadedRenderPassCommandInput(compiledPass);
    EXPECT_TRUE(opaquePass.writesDepthStencilAttachment);

    compiledPass.metadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(compiledPass.metadata, "EditorGridPass");
    auto helperPass = NLS::Render::FrameGraph::MakeCompiledThreadedRenderPassCommandInput(compiledPass);
    EXPECT_FALSE(helperPass.writesDepthStencilAttachment);

    compiledPass.outputExecution.clearDepth = true;
    auto clearingHelperPass = NLS::Render::FrameGraph::MakeCompiledThreadedRenderPassCommandInput(compiledPass);
    EXPECT_TRUE(clearingHelperPass.writesDepthStencilAttachment);
}

TEST(FrameGraphSceneTargetsTests, ThreadedExecutionPlanReinfersDepthStencilWritesAfterExplicitClearOverride)
{
    NLS::Render::FrameGraph::CompiledThreadedRenderSceneGraphPass compiledPass;
    compiledPass.metadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    compiledPass.metadata.executionMode = NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Output;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(compiledPass.metadata, "ExplicitClearHelper");
    compiledPass.outputExecution.renderWidth = 320u;
    compiledPass.outputExecution.renderHeight = 180u;
    compiledPass.outputExecution.clearDepth = false;
    compiledPass.outputExecution.clearStencil = false;

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    passInput.debugName = "ExplicitClearHelper";
    passInput.clearDepth = true;
    passInput.clearStencil = true;

    const std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs{ passInput };
    const std::array<NLS::Render::FrameGraph::CompiledThreadedRenderSceneGraphPass, 1> metadata{{
        compiledPass
    }};

    const auto plan = NLS::Render::FrameGraph::BuildThreadedRenderSceneExecutionPlan(
        passInputs,
        metadata);

    ASSERT_EQ(plan.passes.size(), 1u);
    const auto& plannedInput = plan.passes[0].commandInput;
    EXPECT_TRUE(plannedInput.clearDepth);
    EXPECT_TRUE(plannedInput.clearStencil);
    EXPECT_TRUE(plannedInput.writesDepthStencilAttachment);
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

TEST(FrameGraphSceneTargetsTests, RendererOutputPassCanUseReadOnlyExternalDepthView)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 49u;
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::Buffers::Framebuffer outputBuffer(64u, 64u);
    auto gbufferDepthTexture = std::make_shared<TestTexture>(
        MakeTestTextureDesc(
            "DeferredGBufferDepthForTransparent",
            NLS::Render::FrameGraph::kDeferredGBufferDepthFormat,
            NLS::Render::FrameGraph::kDeferredGBufferDepthUsage));
    NLS::Render::RHI::RHITextureViewDesc gbufferDepthViewDesc;
    gbufferDepthViewDesc.debugName = "DeferredGBufferDepthForTransparentView";
    gbufferDepthViewDesc.format = gbufferDepthTexture->GetDesc().format;
    gbufferDepthViewDesc.subresourceRange.baseMipLevel = 0u;
    gbufferDepthViewDesc.subresourceRange.mipLevelCount = gbufferDepthTexture->GetDesc().mipLevels;
    gbufferDepthViewDesc.subresourceRange.baseArrayLayer = 0u;
    gbufferDepthViewDesc.subresourceRange.arrayLayerCount = gbufferDepthTexture->GetDesc().arrayLayers;
    auto gbufferDepthView = std::make_shared<TestTextureView>(gbufferDepthTexture, gbufferDepthViewDesc);

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
        false,
        false,
        false,
        gbufferDepthView,
        false);
    ASSERT_TRUE(startedPass);
    renderer.EndOutputRenderPass(startedPass);
    renderer.EndFrame();

    ASSERT_EQ(commandBuffer->beginRenderPassCalls, 1u);
    ASSERT_EQ(commandBuffer->lastRenderPassDesc.colorAttachments.size(), 1u);
    ASSERT_TRUE(commandBuffer->lastRenderPassDesc.depthStencilAttachment.has_value());
    EXPECT_EQ(commandBuffer->lastRenderPassDesc.depthStencilAttachment->view, gbufferDepthView);
    EXPECT_TRUE(commandBuffer->lastRenderPassDesc.depthStencilAttachment->readOnlyDepthStencil);

    auto depthState = frameContext.resourceStateTracker->GetTextureState(
        gbufferDepthTexture,
        gbufferDepthViewDesc.subresourceRange);
    ASSERT_TRUE(depthState.has_value());
    EXPECT_EQ(
        depthState->state,
        NLS::Render::RHI::ResourceState::DepthRead | NLS::Render::RHI::ResourceState::ShaderRead);
}

TEST(FrameGraphSceneTargetsTests, RendererMultiFramebufferPassTransitionsActiveAttachmentsToSampledStates)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 48u;
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> attachments(2u);
    attachments[0].format = NLS::Render::RHI::TextureFormat::RGBA8;
    attachments[1].format = NLS::Render::RHI::TextureFormat::RGBA8;
    NLS::Render::Buffers::MultiFramebuffer gBuffer(64u, 64u, attachments, true);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    NLS::Render::Core::CompositeRenderer renderer(driver);
    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.BeginRecordedRenderPass(
        &gBuffer,
        frameDescriptor.renderWidth,
        frameDescriptor.renderHeight,
        true,
        true,
        true));

    const auto colorView0 = gBuffer.GetOrCreateExplicitColorView(0u, "MultiFramebufferColorView0");
    const auto colorView1 = gBuffer.GetOrCreateExplicitColorView(1u, "MultiFramebufferColorView1");
    const auto depthView = gBuffer.GetOrCreateExplicitDepthView("MultiFramebufferDepthView");
    ASSERT_NE(colorView0, nullptr);
    ASSERT_NE(colorView1, nullptr);
    ASSERT_NE(depthView, nullptr);

    renderer.EndRecordedRenderPass();
    renderer.EndFrame();

    auto colorState0 = frameContext.resourceStateTracker->GetTextureState(
        colorView0->GetTexture(),
        colorView0->GetDesc().subresourceRange);
    ASSERT_TRUE(colorState0.has_value());
    EXPECT_EQ(colorState0->state, NLS::Render::RHI::ResourceState::ShaderRead);

    auto colorState1 = frameContext.resourceStateTracker->GetTextureState(
        colorView1->GetTexture(),
        colorView1->GetDesc().subresourceRange);
    ASSERT_TRUE(colorState1.has_value());
    EXPECT_EQ(colorState1->state, NLS::Render::RHI::ResourceState::ShaderRead);

    auto depthState = frameContext.resourceStateTracker->GetTextureState(
        depthView->GetTexture(),
        depthView->GetDesc().subresourceRange);
    ASSERT_TRUE(depthState.has_value());
    EXPECT_EQ(
        depthState->state,
        NLS::Render::RHI::ResourceState::DepthRead | NLS::Render::RHI::ResourceState::ShaderRead);

    const auto commandBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(frameContext.commandBuffer);
    ASSERT_NE(commandBuffer, nullptr);
    ASSERT_GE(commandBuffer->barrierHistory.size(), 2u);
    const auto& endBarriers = commandBuffer->barrierHistory.back();
    ASSERT_EQ(endBarriers.textureBarriers.size(), 3u);
    EXPECT_EQ(endBarriers.textureBarriers[0].before, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(endBarriers.textureBarriers[0].after, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(endBarriers.textureBarriers[1].before, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(endBarriers.textureBarriers[1].after, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(endBarriers.textureBarriers[2].before, NLS::Render::RHI::ResourceState::DepthWrite);
    EXPECT_EQ(
        endBarriers.textureBarriers[2].after,
        NLS::Render::RHI::ResourceState::DepthRead | NLS::Render::RHI::ResourceState::ShaderRead);
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
    EXPECT_EQ(
        package.parallelCommandWorkUnits[0].sourcePassIndex,
        NLS::Render::Context::kInvalidParallelCommandSourcePassIndex);
    EXPECT_EQ(package.parallelCommandWorkUnits[1].sourcePassIndex, 0u);
    EXPECT_EQ(package.parallelCommandWorkUnits[2].sourcePassIndex, 1u);
    EXPECT_FALSE(package.parallelCommandWorkUnits[2].commandInput.dependencySourceWorkUnitIndex.has_value());
    ASSERT_EQ(package.workUnitDependencyEdges.size(), 1u);
    EXPECT_EQ(package.workUnitDependencyEdges[0].sourceWorkUnitIndex, 1u);
    EXPECT_EQ(package.workUnitDependencyEdges[0].targetWorkUnitIndex, 2u);
    ASSERT_EQ(package.parallelCommandWorkUnits[2].incomingDependencyEdges.size(), 1u);
    EXPECT_EQ(package.parallelCommandWorkUnits[2].incomingDependencyEdges[0].sourceWorkUnitIndex, 1u);
    EXPECT_EQ(package.parallelCommandWorkUnits[2].incomingDependencyEdges[0].targetWorkUnitIndex, 2u);
}

TEST(FrameGraphSceneTargetsTests, ApplyThreadedExecutionPlanSlicesLargeRecordedPassAndRemapsDependencies)
{
    NLS::Render::Context::RenderScenePackage package;
    NLS::Render::FrameGraph::ThreadedRenderSceneExecutionPlan plan;

    NLS::Render::Context::RenderPassCommandInput opaquePass;
    opaquePass.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePass.debugName = "ForwardOpaque";
    opaquePass.drawCount = 2000u;
    opaquePass.clearColor = false;
    opaquePass.clearDepth = false;
    opaquePass.clearStencil = false;
    opaquePass.recordedDrawCommands.resize(2000u);

    NLS::Render::Context::RenderPassCommandInput lightingPass;
    lightingPass.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPass.debugName = "DeferredLighting";
    lightingPass.drawCount = 1u;
    lightingPass.recordedDrawCommands.resize(1u);

    plan.passes.push_back(MakeThreadedPassPlan(
        opaquePass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
        "ForwardOpaque",
        2000u));
    plan.passes.push_back(MakeThreadedPassPlan(
        lightingPass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "DeferredLighting",
        1u));
    plan.dependencies = NLS::Render::FrameGraph::BuildThreadedRenderSceneDependencyEdges(plan.passes);

    NLS::Render::FrameGraph::ApplyThreadedRenderSceneExecutionPlan(package, plan);

    ASSERT_EQ(package.passCommandInputs.size(), 2u);
    EXPECT_EQ(package.passCommandInputs[0].recordedDrawCommands.size(), 2000u);
    ASSERT_GT(package.parallelCommandWorkUnits.size(), 2u);

    const auto opaqueSliceCount = package.parallelCommandWorkUnits[0].sliceCount;
    ASSERT_GT(opaqueSliceCount, 1u);
    uint64_t coveredOpaqueDraws = 0u;
    for (uint32_t sliceIndex = 0u; sliceIndex < opaqueSliceCount; ++sliceIndex)
    {
        const auto& workUnit = package.parallelCommandWorkUnits[sliceIndex];
        EXPECT_EQ(workUnit.sourcePassIndex, 0u);
        EXPECT_EQ(workUnit.sliceIndex, sliceIndex);
        EXPECT_EQ(workUnit.sliceCount, opaqueSliceCount);
        EXPECT_EQ(workUnit.recordedDrawBegin, coveredOpaqueDraws);
        EXPECT_TRUE(workUnit.commandInput.recordedDrawCommands.empty());
        EXPECT_EQ(workUnit.commandInput.drawCount, workUnit.recordedDrawCount);
        EXPECT_TRUE(workUnit.requiresOrderedSlicedSubmission);
        EXPECT_FALSE(workUnit.eligibleForParallelRecording);
        EXPECT_FALSE(workUnit.eligibleForParallelTranslation);
        coveredOpaqueDraws += workUnit.recordedDrawCount;
        EXPECT_FALSE(workUnit.commandInput.clearColor);
        EXPECT_FALSE(workUnit.commandInput.clearDepth);
        EXPECT_FALSE(workUnit.commandInput.clearStencil);
    }
    EXPECT_EQ(coveredOpaqueDraws, 2000u);

    ASSERT_EQ(package.workUnitDependencyEdges.size(), 1u);
    EXPECT_EQ(package.workUnitDependencyEdges[0].sourceWorkUnitIndex, opaqueSliceCount - 1u);
    EXPECT_EQ(package.workUnitDependencyEdges[0].targetWorkUnitIndex, opaqueSliceCount);
    ASSERT_EQ(package.parallelCommandWorkUnits[opaqueSliceCount].incomingDependencyEdges.size(), 1u);
    EXPECT_EQ(
        package.parallelCommandWorkUnits[opaqueSliceCount].incomingDependencyEdges[0].sourceWorkUnitIndex,
        opaqueSliceCount - 1u);
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

TEST(FrameGraphSceneTargetsTests, SelectionOutlineMaskPassChainEmitsOrderedResourceVisibilityEdges)
{
    using NLS::Render::Context::RenderPassCommandInput;
    using NLS::Render::Context::RenderPassCommandKind;
    using NLS::Render::Context::ResourceAccessMode;
    using NLS::Render::FrameGraph::ThreadedRenderSceneDependencyKind;
    using NLS::Render::FrameGraph::ThreadedRenderSceneDependencyResourceKind;
    using NLS::Render::FrameGraph::ThreadedRenderScenePassRole;
    using NLS::Render::RHI::AccessMask;
    using NLS::Render::RHI::PipelineStageMask;
    using NLS::Render::RHI::ResourceState;

    auto maskTexture = std::make_shared<TestTexture>(MakeTestTextureDesc("SelectionOutlineMask.Mask"));
    auto sceneDepthTexture = std::make_shared<TestTexture>(
        MakeTestTextureDesc(
            "SceneViewDepth",
            NLS::Render::FrameGraph::kDeferredGBufferDepthFormat,
            NLS::Render::FrameGraph::kDeferredGBufferDepthUsage));
    auto outputColorTexture = std::make_shared<TestTexture>(MakeTestTextureDesc("SceneViewColor"));

    const auto fullRange = [](const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
    {
        NLS::Render::RHI::RHISubresourceRange range;
        range.mipLevelCount = texture->GetDesc().mipLevels;
        range.arrayLayerCount = texture->GetDesc().arrayLayers;
        return range;
    };
    const auto addAccess = [&fullRange](
        RenderPassCommandInput& pass,
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
        const ResourceAccessMode mode,
        const ResourceState state,
        const PipelineStageMask stages,
        const AccessMask access)
    {
        pass.textureResourceAccesses.push_back({
            texture,
            fullRange(texture),
            mode,
            state,
            stages,
            access
        });
    };
    const auto makePass = [](const char* name)
    {
        RenderPassCommandInput pass;
        pass.kind = RenderPassCommandKind::Helper;
        pass.debugName = name;
        pass.drawCount = 1u;
        return pass;
    };

    std::vector<RenderPassCommandInput> passInputs;
    passInputs.push_back(makePass("SelectionOutlineMask::CaptureMask"));
    passInputs.back().usesDepthStencilAttachment = true;
    passInputs.back().writesDepthStencilAttachment = false;
    addAccess(
        passInputs.back(),
        sceneDepthTexture,
        ResourceAccessMode::Read,
        ResourceState::DepthRead,
        PipelineStageMask::DepthStencil,
        AccessMask::DepthStencilRead);
    addAccess(
        passInputs.back(),
        maskTexture,
        ResourceAccessMode::Write,
        ResourceState::RenderTarget,
        PipelineStageMask::RenderTarget,
        AccessMask::ColorAttachmentRead | AccessMask::ColorAttachmentWrite);

    passInputs.push_back(makePass("SelectionOutlineMask::Composite"));
    addAccess(passInputs.back(), maskTexture, ResourceAccessMode::Read, ResourceState::ShaderRead, PipelineStageMask::FragmentShader, AccessMask::ShaderRead);
    addAccess(
        passInputs.back(),
        outputColorTexture,
        ResourceAccessMode::Write,
        ResourceState::RenderTarget,
        PipelineStageMask::RenderTarget,
        AccessMask::ColorAttachmentRead | AccessMask::ColorAttachmentWrite);

    const std::array<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata, 2> metadata{{
        MakeThreadedPassMetadata(RenderPassCommandKind::Helper, ThreadedRenderScenePassRole::Helper, "SelectionOutlineMask::CaptureMask", 1u),
        MakeThreadedPassMetadata(RenderPassCommandKind::Helper, ThreadedRenderScenePassRole::Helper, "SelectionOutlineMask::Composite", 1u)
    }};

    const auto plan = NLS::Render::FrameGraph::BuildThreadedRenderSceneExecutionPlan(passInputs, metadata);

    const auto expectTextureEdge =
        [&plan](
            const size_t sourcePass,
            const size_t targetPass,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const ResourceState writtenState,
            const ResourceState readState)
    {
        const auto edge = std::find_if(
            plan.dependencies.begin(),
            plan.dependencies.end(),
            [sourcePass, targetPass, &texture](const NLS::Render::FrameGraph::ThreadedRenderSceneDependencyEdge& candidate)
            {
                return candidate.sourcePassIndex == sourcePass &&
                    candidate.targetPassIndex == targetPass &&
                    candidate.resourceKind == ThreadedRenderSceneDependencyResourceKind::Texture &&
                    candidate.sourceTextureAccess.has_value() &&
                    candidate.targetTextureAccess.has_value() &&
                    candidate.sourceTextureAccess->texture == texture &&
                    candidate.targetTextureAccess->texture == texture;
            });
        ASSERT_NE(edge, plan.dependencies.end());
        EXPECT_EQ(edge->kind, ThreadedRenderSceneDependencyKind::ResourceVisibility);
        EXPECT_EQ(edge->sourceTextureAccess->mode, ResourceAccessMode::Write);
        EXPECT_EQ(edge->targetTextureAccess->mode, ResourceAccessMode::Read);
        EXPECT_EQ(edge->sourceTextureAccess->state, writtenState);
        EXPECT_EQ(edge->targetTextureAccess->state, readState);
    };

    ASSERT_EQ(plan.passes.size(), 2u);
    EXPECT_FALSE(plan.passes[0].requiresDependencyVisibility);
    for (size_t passIndex = 1u; passIndex < plan.passes.size(); ++passIndex)
    {
        EXPECT_TRUE(plan.passes[passIndex].requiresDependencyVisibility) << passIndex;
        ASSERT_TRUE(plan.passes[passIndex].dependencySourcePassIndex.has_value()) << passIndex;
        EXPECT_EQ(*plan.passes[passIndex].dependencySourcePassIndex, passIndex - 1u);
    }
    expectTextureEdge(0u, 1u, maskTexture, ResourceState::RenderTarget, ResourceState::ShaderRead);

    const auto staleDepthWrite = std::find_if(
        plan.dependencies.begin(),
        plan.dependencies.end(),
        [&sceneDepthTexture](const NLS::Render::FrameGraph::ThreadedRenderSceneDependencyEdge& edge)
        {
            return edge.sourceTextureAccess.has_value() &&
                edge.sourceTextureAccess->texture == sceneDepthTexture;
        });
    EXPECT_EQ(staleDepthWrite, plan.dependencies.end());
}

TEST(FrameGraphSceneTargetsTests, ThreadedResourceDependencySlicesPartialTextureWritersForFullRangeRead)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "OverlapDependencyTexture";
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.mipLevels = 1u;
    textureDesc.arrayLayers = 2u;
    auto testTexture = std::make_shared<TestTexture>(textureDesc);

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 2u;

    NLS::Render::RHI::RHISubresourceRange partialRange;
    partialRange.baseMipLevel = 0u;
    partialRange.mipLevelCount = 1u;
    partialRange.baseArrayLayer = 1u;
    partialRange.arrayLayerCount = 1u;

    NLS::Render::Context::RenderPassCommandInput firstWrite;
    firstWrite.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    firstWrite.debugName = "FullWrite";
    firstWrite.textureResourceAccesses.push_back({
        testTexture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::RenderTarget,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });

    NLS::Render::Context::RenderPassCommandInput secondWrite;
    secondWrite.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    secondWrite.debugName = "PartialWrite";
    secondWrite.textureResourceAccesses.push_back({
        testTexture,
        partialRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });

    NLS::Render::Context::RenderPassCommandInput fullRead;
    fullRead.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    fullRead.debugName = "FullRead";
    fullRead.textureResourceAccesses.push_back({
        testTexture,
        fullRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    NLS::Render::FrameGraph::ThreadedRenderSceneExecutionPlan plan;
    plan.passes.push_back(MakeThreadedPassPlan(
        firstWrite,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
        "FullWrite",
        1u));
    plan.passes.push_back(MakeThreadedPassPlan(
        secondWrite,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "PartialWrite",
        0u,
        NLS::Render::RHI::QueueType::Compute,
        NLS::Render::Context::QueueDependencyPolicy::None));
    plan.passes.push_back(MakeThreadedPassPlan(
        fullRead,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "FullRead",
        1u,
        NLS::Render::RHI::QueueType::Graphics,
        NLS::Render::Context::QueueDependencyPolicy::None));

    plan.dependencies = NLS::Render::FrameGraph::BuildThreadedRenderSceneDependencyEdges(plan.passes);

    ASSERT_TRUE(plan.passes[2].dependencySourcePassIndex.has_value());
    EXPECT_EQ(*plan.passes[2].dependencySourcePassIndex, 0u);
    ASSERT_EQ(plan.dependencies.size(), 2u);
    const auto fullTextureEdge = std::find_if(
        plan.dependencies.begin(),
        plan.dependencies.end(),
        [](const NLS::Render::FrameGraph::ThreadedRenderSceneDependencyEdge& edge)
        {
            return edge.sourcePassIndex == 0u &&
                edge.targetPassIndex == 2u &&
                edge.resourceKind == NLS::Render::FrameGraph::ThreadedRenderSceneDependencyResourceKind::Texture;
        });
    ASSERT_NE(fullTextureEdge, plan.dependencies.end());
    ASSERT_TRUE(fullTextureEdge->sourceTextureAccess.has_value());
    ASSERT_TRUE(fullTextureEdge->targetTextureAccess.has_value());
    EXPECT_EQ(fullTextureEdge->sourceTextureAccess->texture, testTexture);
    EXPECT_EQ(fullTextureEdge->sourceTextureAccess->state, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(fullTextureEdge->sourceTextureAccess->subresourceRange.baseArrayLayer, 0u);
    EXPECT_EQ(fullTextureEdge->sourceTextureAccess->subresourceRange.arrayLayerCount, 1u);
    EXPECT_EQ(fullTextureEdge->targetTextureAccess->subresourceRange.baseArrayLayer, 0u);
    EXPECT_EQ(fullTextureEdge->targetTextureAccess->subresourceRange.arrayLayerCount, 1u);

    const auto partialTextureEdge = std::find_if(
        plan.dependencies.begin(),
        plan.dependencies.end(),
        [](const NLS::Render::FrameGraph::ThreadedRenderSceneDependencyEdge& edge)
        {
            return edge.sourcePassIndex == 1u &&
                edge.targetPassIndex == 2u &&
                edge.resourceKind == NLS::Render::FrameGraph::ThreadedRenderSceneDependencyResourceKind::Texture;
        });
    ASSERT_NE(partialTextureEdge, plan.dependencies.end());
    ASSERT_TRUE(partialTextureEdge->sourceTextureAccess.has_value());
    ASSERT_TRUE(partialTextureEdge->targetTextureAccess.has_value());
    EXPECT_EQ(partialTextureEdge->sourceTextureAccess->texture, testTexture);
    EXPECT_EQ(partialTextureEdge->sourceTextureAccess->state, NLS::Render::RHI::ResourceState::ShaderWrite);
    EXPECT_EQ(partialTextureEdge->sourceTextureAccess->subresourceRange.baseArrayLayer, partialRange.baseArrayLayer);
    EXPECT_EQ(partialTextureEdge->sourceTextureAccess->subresourceRange.arrayLayerCount, partialRange.arrayLayerCount);
    EXPECT_EQ(partialTextureEdge->targetTextureAccess->subresourceRange.baseArrayLayer, partialRange.baseArrayLayer);
    EXPECT_EQ(partialTextureEdge->targetTextureAccess->subresourceRange.arrayLayerCount, partialRange.arrayLayerCount);
}

TEST(FrameGraphSceneTargetsTests, ThreadedResourceDependencyNormalizesDefaultFullRangeBeforeSlicing)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "DefaultRangeDependencyTexture";
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.mipLevels = 2u;
    textureDesc.arrayLayers = 1u;
    auto testTexture = std::make_shared<TestTexture>(textureDesc);

    NLS::Render::RHI::RHISubresourceRange defaultFullRange;
    defaultFullRange.baseMipLevel = 0u;
    defaultFullRange.mipLevelCount = 2u;
    defaultFullRange.baseArrayLayer = 0u;
    defaultFullRange.arrayLayerCount = 0u;

    NLS::Render::RHI::RHISubresourceRange firstMipRange;
    firstMipRange.baseMipLevel = 0u;
    firstMipRange.mipLevelCount = 1u;
    firstMipRange.baseArrayLayer = 0u;
    firstMipRange.arrayLayerCount = 1u;

    NLS::Render::RHI::RHISubresourceRange secondMipRange;
    secondMipRange.baseMipLevel = 1u;
    secondMipRange.mipLevelCount = 1u;
    secondMipRange.baseArrayLayer = 0u;
    secondMipRange.arrayLayerCount = 1u;

    NLS::Render::Context::RenderPassCommandInput firstMipWrite;
    firstMipWrite.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    firstMipWrite.debugName = "FirstMipWrite";
    firstMipWrite.textureResourceAccesses.push_back({
        testTexture,
        firstMipRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::RenderTarget,
        NLS::Render::RHI::PipelineStageMask::RenderTarget,
        NLS::Render::RHI::AccessMask::ColorAttachmentWrite
    });

    NLS::Render::Context::RenderPassCommandInput secondMipWrite;
    secondMipWrite.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    secondMipWrite.debugName = "SecondMipWrite";
    secondMipWrite.textureResourceAccesses.push_back({
        testTexture,
        secondMipRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });

    NLS::Render::Context::RenderPassCommandInput fullRead;
    fullRead.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    fullRead.debugName = "DefaultFullRead";
    fullRead.textureResourceAccesses.push_back({
        testTexture,
        defaultFullRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassPlan> passes;
    passes.push_back(MakeThreadedPassPlan(
        firstMipWrite,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
        "FirstMipWrite",
        1u));
    passes.push_back(MakeThreadedPassPlan(
        secondMipWrite,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "SecondMipWrite",
        0u,
        NLS::Render::RHI::QueueType::Graphics,
        NLS::Render::Context::QueueDependencyPolicy::None));
    passes.push_back(MakeThreadedPassPlan(
        fullRead,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
        "DefaultFullRead",
        1u,
        NLS::Render::RHI::QueueType::Graphics,
        NLS::Render::Context::QueueDependencyPolicy::None));

    const auto edges = NLS::Render::FrameGraph::BuildThreadedRenderSceneDependencyEdges(passes);

    std::string textureEdgeSummary;
    for (const auto& edge : edges)
    {
        if (edge.resourceKind != NLS::Render::FrameGraph::ThreadedRenderSceneDependencyResourceKind::Texture ||
            !edge.sourceTextureAccess.has_value() ||
            !edge.targetTextureAccess.has_value())
        {
            continue;
        }

        textureEdgeSummary +=
            " source=" + std::to_string(edge.sourcePassIndex) +
            " target=" + std::to_string(edge.targetPassIndex) +
            " mip=" + std::to_string(edge.sourceTextureAccess->subresourceRange.baseMipLevel) +
            " count=" + std::to_string(edge.sourceTextureAccess->subresourceRange.mipLevelCount);
    }
    ASSERT_EQ(edges.size(), 2u) << textureEdgeSummary;
    const auto hasEdgeForMip =
        [&edges](const size_t sourcePassIndex, const uint32_t mip)
    {
        return std::any_of(
            edges.begin(),
            edges.end(),
            [sourcePassIndex, mip](const NLS::Render::FrameGraph::ThreadedRenderSceneDependencyEdge& edge)
            {
                return edge.sourcePassIndex == sourcePassIndex &&
                    edge.targetPassIndex == 2u &&
                    edge.sourceTextureAccess.has_value() &&
                    edge.targetTextureAccess.has_value() &&
                    edge.sourceTextureAccess->subresourceRange.baseMipLevel == mip &&
                    edge.sourceTextureAccess->subresourceRange.mipLevelCount == 1u &&
                    edge.targetTextureAccess->subresourceRange.baseMipLevel == mip &&
                    edge.targetTextureAccess->subresourceRange.mipLevelCount == 1u;
            });
    };
    EXPECT_TRUE(hasEdgeForMip(0u, 0u));
    EXPECT_TRUE(hasEdgeForMip(1u, 1u));
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
    EXPECT_EQ(
        transition.before,
        NLS::Render::RHI::ResourceState::DepthRead | NLS::Render::RHI::ResourceState::ShaderRead);
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

TEST(FrameGraphSceneTargetsTests, ParallelDrawCommandBatchMetadataPromotesComputeToGraphicsDependencyEdges)
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
    ASSERT_EQ(forwardDescriptors.size(), 4u);
    EXPECT_EQ(forwardDescriptors[0].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(forwardDescriptors[1].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Decal);
    EXPECT_EQ(forwardDescriptors[2].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(forwardDescriptors[3].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_EQ(forwardDescriptors[0].metadata.role, NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque);
    EXPECT_EQ(forwardDescriptors[1].metadata.role, NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Decal);
    EXPECT_EQ(forwardDescriptors[2].metadata.role, NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Skybox);
    EXPECT_EQ(forwardDescriptors[3].metadata.role, NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Transparent);

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

    auto resources = MakeCompleteDeferredPreparedSceneResources();
    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        {},
        {},
        MakeDeferredQueuedDrawCounts(1u));

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

    const auto framebufferSummary = NLS::Render::FrameGraph::BuildExternalSceneOutputSummary(frameDescriptor);
    EXPECT_EQ(summary.identity, framebufferSummary.identity);
    EXPECT_EQ(summary.identities, framebufferSummary.identities);
}

TEST(FrameGraphSceneTargetsTests, ExternalSceneOutputSummaryCountsViewOnlyTargets)
{
    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "ViewOnlySceneColor";
    colorDesc.extent = { 320u, 180u, 1u };
    colorDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto colorTexture = std::make_shared<TestTexture>(colorDesc);

    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.debugName = "ViewOnlySceneColorView";
    auto colorView = std::make_shared<TestTextureView>(colorTexture, colorViewDesc);

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.outputColorView = colorView;

    const auto summary = NLS::Render::FrameGraph::BuildExternalSceneOutputSummary(frameDescriptor);
    EXPECT_FALSE(summary.targetsSwapchain);
    EXPECT_TRUE(summary.hasExternalOutput);
    EXPECT_EQ(summary.textureCount, 1u);
    EXPECT_EQ(summary.identity, reinterpret_cast<uint64_t>(colorTexture.get()));
    ASSERT_EQ(summary.identities.size(), 1u);
    EXPECT_EQ(summary.identities[0], reinterpret_cast<uint64_t>(colorTexture.get()));
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
    ASSERT_EQ(graphPasses.size(), 4u);
    EXPECT_EQ(graphPasses[0].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(graphPasses[1].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Decal);
    EXPECT_EQ(graphPasses[2].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(graphPasses[3].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_STREQ(graphPasses[0].metadata.graphPassName, "ForwardOpaque");
    EXPECT_STREQ(graphPasses[1].metadata.graphPassName, "ForwardDecal");
    EXPECT_STREQ(graphPasses[2].metadata.graphPassName, "ForwardSkybox");
    EXPECT_STREQ(graphPasses[3].metadata.graphPassName, "ForwardTransparent");
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

TEST(FrameGraphSceneTargetsTests, ForwardPreparedExecutionDeclaresDepthStencilWriteForOpaquePass)
{
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.opaqueDrawCount = 1u;
    package.decalDrawCount = 1u;
    package.skyboxDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.drawCommandCount = 4u;
    package.recordedDrawCommands.resize(4u);
    package.clearDepthBuffer = true;
    package.clearStencilBuffer = true;

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;

    NLS::Render::FrameGraph::CompileAndApplyPreparedForwardLightGridSceneExecution(
        package,
        lightGridContext);

    ASSERT_EQ(package.passCommandInputs.size(), 4u);
    const auto& opaquePass = package.passCommandInputs[0];
    const auto& decalPass = package.passCommandInputs[1];
    const auto& skyboxPass = package.passCommandInputs[2];
    const auto& transparentPass = package.passCommandInputs[3];
    EXPECT_EQ(opaquePass.kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(decalPass.kind, NLS::Render::Context::RenderPassCommandKind::Decal);
    EXPECT_EQ(skyboxPass.kind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(transparentPass.kind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_TRUE(opaquePass.usesDepthStencilAttachment);
    EXPECT_TRUE(opaquePass.writesDepthStencilAttachment);
    EXPECT_FALSE(decalPass.writesDepthStencilAttachment);
    EXPECT_TRUE(skyboxPass.writesDepthStencilAttachment);
    EXPECT_FALSE(transparentPass.writesDepthStencilAttachment);
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
