#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Core/FrameObjectBindingProvider.h"
#include "Rendering/Core/RendererStats.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Math/Matrix4.h"

namespace
{
    NLS::Render::Context::Driver& EnsureTestDriver()
    {
        static auto driver = std::make_unique<NLS::Render::Context::Driver>([]()
        {
            NLS::Render::Settings::DriverSettings settings;
            settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
            settings.enableExplicitRHI = false;
            return settings;
        }());
        NLS::Core::ServiceLocator::Provide(*driver);
        return *driver;
    }

    class TestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "StatsOnlyTestCommandBuffer"; }
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        bool IsRecording() const override { return true; }
        NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
        void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
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
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc&) override {}
    };

    class TestBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        explicit TestBuffer(NLS::Render::RHI::RHIBufferDesc desc)
            : m_desc(std::move(desc))
        {
        }

        TestBuffer(
            NLS::Render::RHI::RHIBufferDesc desc,
            const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc)
            : m_desc(std::move(desc))
        {
            if (uploadDesc.HasData())
            {
                uploadData.resize(uploadDesc.dataSize);
                std::memcpy(uploadData.data(), uploadDesc.data, uploadDesc.dataSize);
            }
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }
        uint64_t GetGPUAddress() const override { return 0u; }
        NLS::Render::RHI::RHIUpdateResult UpdateData(const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
        {
            if (!uploadDesc.HasData())
                return { NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument, "missing upload data" };
            if (uploadDesc.destinationOffset + uploadDesc.dataSize > m_desc.size)
                return { NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument, "upload exceeds buffer size" };
            if (uploadData.size() < m_desc.size)
                uploadData.resize(m_desc.size, 0u);
            std::memcpy(
                uploadData.data() + static_cast<size_t>(uploadDesc.destinationOffset),
                uploadDesc.data,
                uploadDesc.dataSize);
            ++updateCalls;
            return { NLS::Render::RHI::RHIUpdateStatusCode::Success, {} };
        }

        std::vector<uint8_t> uploadData;
        uint32_t updateCalls = 0u;

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc {};
    };

    class TestBindingLayout final : public NLS::Render::RHI::RHIBindingLayout
    {
    public:
        explicit TestBindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingLayoutDesc m_desc {};
    };

    class TestBindingSet final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        explicit TestBindingSet(NLS::Render::RHI::RHIBindingSetDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingSetDesc m_desc {};
    };

    class TestAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "RendererStatsTestsAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::DX12; }
        std::string_view GetVendor() const override { return "TestVendor"; }
        std::string_view GetHardware() const override { return "TestHardware"; }
    };

    class TestQueue final : public NLS::Render::RHI::RHIQueue
    {
    public:
        std::string_view GetDebugName() const override { return "RendererStatsTestsQueue"; }
        NLS::Render::RHI::QueueType GetType() const override { return NLS::Render::RHI::QueueType::Graphics; }
        void Submit(const NLS::Render::RHI::RHISubmitDesc&) override {}
        NLS::Render::RHI::RHIQueueOperationResult SubmitChecked(const NLS::Render::RHI::RHISubmitDesc&) override { return {}; }
        void Present(const NLS::Render::RHI::RHIPresentDesc&) override {}
        NLS::Render::RHI::RHIQueueOperationResult PresentChecked(const NLS::Render::RHI::RHIPresentDesc&) override { return {}; }
    };

    class TestExplicitDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        TestExplicitDevice()
            : m_adapter(std::make_shared<TestAdapter>())
            , m_queue(std::make_shared<TestQueue>())
        {
            m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
            m_capabilities.backendReady = true;
            m_capabilities.supportsGraphics = true;
            m_capabilities.supportsCurrentSceneRenderer = true;
            m_capabilities.supportsCentralizedDescriptorManagement = true;
        }

        std::string_view GetDebugName() const override { return "RendererStatsTestsDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return m_queue; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc& desc,
            const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
        {
            auto buffer = std::make_shared<TestBuffer>(desc, uploadDesc);
            buffers.push_back(buffer);
            return buffer;
        }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
            const NLS::Render::RHI::RHITextureDesc&,
            const NLS::Render::RHI::RHITextureUploadDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
            const NLS::Render::RHI::RHITextureViewDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(
            const NLS::Render::RHI::SamplerDesc&,
            std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(
            const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override
        {
            ++bindingLayoutCreateCalls;
            return std::make_shared<TestBindingLayout>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(
            const NLS::Render::RHI::RHIBindingSetDesc& desc) override
        {
            ++bindingSetCreateCalls;
            bindingSetDescs.push_back(desc);
            return std::make_shared<TestBindingSet>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(
            const NLS::Render::RHI::RHIPipelineLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(
            const NLS::Render::RHI::RHIShaderModuleDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(
            const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(
            const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
            NLS::Render::RHI::QueueType,
            std::string = {}) override { return nullptr; }
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

        uint32_t bindingLayoutCreateCalls = 0u;
        uint32_t bindingSetCreateCalls = 0u;
        std::vector<NLS::Render::RHI::RHIBindingSetDesc> bindingSetDescs;
        std::vector<std::shared_ptr<TestBuffer>> buffers;

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        std::shared_ptr<NLS::Render::RHI::RHIQueue> m_queue;
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    class StatsOnlyRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit StatsOnlyRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
        {
        }

    protected:
        bool PrepareRecordedDraw(
            PipelineState,
            const NLS::Render::Entities::Drawable& drawable,
            PreparedRecordedDraw& outDraw) const override
        {
            if (drawable.material == nullptr)
                return false;

            outDraw.commandBuffer = std::make_shared<TestCommandBuffer>();
            outDraw.instanceCount = static_cast<uint32_t>(std::max(drawable.material->GetGPUInstances(), 0));
            return outDraw.instanceCount > 0u;
        }

        void BindPreparedGraphicsPipeline(const PreparedRecordedDraw&) const override {}
        void BindPreparedMaterialBindingSet(const PreparedRecordedDraw&) const override {}
        void SubmitPreparedDraw(const PreparedRecordedDraw&) const override {}
    };

    std::unique_ptr<NLS::Render::Resources::Mesh> CreateTriangleMesh()
    {
        EnsureTestDriver();
        std::vector<NLS::Render::Geometry::Vertex> vertices(3);
        vertices[0].position[0] = 0.0f;
        vertices[1].position[0] = 1.0f;
        vertices[2].position[1] = 1.0f;
        std::vector<uint32_t> indices { 0u, 1u, 2u };
        return std::make_unique<NLS::Render::Resources::Mesh>(vertices, indices, 0u);
    }
}

TEST(RendererStatsTests, RendererStatsTracksSubmittedDrawCounts)
{
    NLS::Render::Core::RendererStats stats;
    NLS::Render::Resources::Material material;
    material.SetGPUInstances(2);
    const auto mesh = CreateTriangleMesh();

    NLS::Render::Entities::Drawable drawable;
    drawable.mesh = mesh.get();
    drawable.material = &material;

    stats.BeginFrame();
    stats.RecordSubmittedDraw(drawable, static_cast<uint32_t>(material.GetGPUInstances()));
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.batchCount, 1u);
    EXPECT_EQ(frameInfo.instanceCount, 2u);
    EXPECT_EQ(frameInfo.polyCount, 2u);
    EXPECT_EQ(frameInfo.vertexCount, 6u);
}

TEST(RendererStatsTests, RendererStatsTracksRenderPreparationCounters)
{
    NLS::Render::Core::RendererStats stats;

    stats.BeginFrame();
    stats.RecordSceneParse(8u, 3u, 1u);
    stats.RecordGBufferMaterialSync();
    stats.RecordGBufferMaterialSync();
    stats.RecordRenderBindingSetCreation();
    stats.RecordRenderBindingSetCreation();
    stats.RecordRenderSnapshotBufferCreation();
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.parseSceneCallCount, 1u);
    EXPECT_EQ(frameInfo.parsedOpaqueDrawableCount, 8u);
    EXPECT_EQ(frameInfo.parsedTransparentDrawableCount, 3u);
    EXPECT_EQ(frameInfo.parsedSkyboxDrawableCount, 1u);
    EXPECT_EQ(frameInfo.gBufferMaterialSyncCount, 2u);
    EXPECT_EQ(frameInfo.renderBindingSetCreationCount, 2u);
    EXPECT_EQ(frameInfo.renderSnapshotBufferCreationCount, 1u);
}

TEST(RendererStatsTests, CompositeRendererExposesFinalizedFrameInfoWithoutFeatureRegistration)
{
    auto& driver = EnsureTestDriver();

    StatsOnlyRenderer renderer(driver);
    NLS::Render::Resources::Material material;
    material.SetGPUInstances(3);
    const auto mesh = CreateTriangleMesh();

    NLS::Render::Entities::Drawable drawable;
    drawable.mesh = mesh.get();
    drawable.material = &material;

    renderer.ResetFrameStatistics();
    renderer.DrawEntity(NLS::Render::Data::PipelineState {}, drawable);
    renderer.FinalizeFrameStatistics();

    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.batchCount, 1u);
    EXPECT_EQ(frameInfo.instanceCount, 3u);
    EXPECT_EQ(frameInfo.polyCount, 3u);
    EXPECT_EQ(frameInfo.vertexCount, 9u);
}

TEST(RendererStatsTests, CompositeRendererFinalizedFrameInfoIsZeroWhenNoDrawsWereSubmitted)
{
    auto& driver = EnsureTestDriver();

    StatsOnlyRenderer renderer(driver);
    renderer.ResetFrameStatistics();
    renderer.FinalizeFrameStatistics();

    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.batchCount, 0u);
    EXPECT_EQ(frameInfo.instanceCount, 0u);
    EXPECT_EQ(frameInfo.polyCount, 0u);
    EXPECT_EQ(frameInfo.vertexCount, 0u);
}

TEST(RendererStatsTests, IndexedObjectDrawsCreateOneObjectBindingSetForRepeatedVisibleDraws)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 44u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);

    StatsOnlyRenderer renderer(driver);
    auto providerOwner = std::make_unique<NLS::Engine::Rendering::EngineFrameObjectBindingProvider>(renderer);
    renderer.SetFrameObjectBindingProvider(std::move(providerOwner));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    auto* provider = renderer.GetFrameObjectBindingProvider();
    ASSERT_NE(provider, nullptr);
    renderer.ResetFrameStatistics();
    provider->BeginFrame(frameDescriptor);

    NLS::Render::Resources::Material material;
    material.SetGPUInstances(1);
    const auto mesh = CreateTriangleMesh();

    for (uint32_t drawIndex = 0u; drawIndex < 3u; ++drawIndex)
    {
        NLS::Render::Entities::Drawable drawable;
        drawable.mesh = mesh.get();
        drawable.material = &material;
        drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
            NLS::Maths::Matrix4::Translation({ static_cast<float>(drawIndex), 0.0f, 0.0f }),
            NLS::Maths::Matrix4::Identity,
            drawIndex
        });
        renderer.DrawEntity(NLS::Render::Data::PipelineState {}, drawable);
    }
    provider->EndFrame();
    renderer.FinalizeFrameStatistics();

    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.renderBindingSetCreationCount, 1u);
    EXPECT_EQ(frameInfo.renderSnapshotBufferCreationCount, 0u);

    const auto objectBindingSets = static_cast<size_t>(std::count_if(
        explicitDevice->bindingSetDescs.begin(),
        explicitDevice->bindingSetDescs.end(),
        [](const NLS::Render::RHI::RHIBindingSetDesc& desc)
        {
            return desc.debugName == "EngineObjectBindingSet";
        }));
    EXPECT_EQ(objectBindingSets, 1u);
    EXPECT_LT(frameInfo.renderBindingSetCreationCount, 3u);
    EXPECT_LT(frameInfo.renderSnapshotBufferCreationCount, 3u);

    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererStatsTests, RendererStatsTracksThreadedFrameTelemetryFields)
{
    NLS::Render::Core::RendererStats stats;

    stats.BeginFrame();
    NLS::Render::Context::ThreadedFrameTelemetry telemetry;
    telemetry.inFlightFrameCount = 2u;
    telemetry.blockedPublishCount = 1u;
    telemetry.publishState = NLS::Render::Data::FramePublishState::BackPressured;
    telemetry.stageSummary = NLS::Render::Data::ThreadedFrameStageSummary::Rhi;
    telemetry.retirementState = NLS::Render::Data::FrameRetirementState::Pending;
    stats.SetThreadedFrameTelemetry(telemetry);
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.inFlightFrameCount, 2u);
    EXPECT_EQ(frameInfo.blockedFrameCount, 1u);
    EXPECT_EQ(frameInfo.publishState, NLS::Render::Data::FramePublishState::BackPressured);
    EXPECT_EQ(frameInfo.stageSummary, NLS::Render::Data::ThreadedFrameStageSummary::Rhi);
    EXPECT_EQ(frameInfo.retirementState, NLS::Render::Data::FrameRetirementState::Pending);
}

TEST(RendererStatsTests, RendererStatsTracksInfrastructureMainlineDiagnostics)
{
    NLS::Render::Core::RendererStats stats;

    NLS::Render::Context::ThreadedFrameTelemetry telemetry;
    telemetry.descriptorMainlineActive = true;
    telemetry.pipelineMainlineActive = true;
    telemetry.transientLifetimeMainlineActive = true;
    telemetry.retirementMainlineActive = true;
    telemetry.descriptorBypassCount = 0u;
    telemetry.pipelineBypassCount = 0u;
    telemetry.transientLifetimeBypassCount = 0u;
    telemetry.retirementBypassCount = 0u;
    telemetry.transientTextureRegistrationCount = 3u;
    telemetry.transientBufferRegistrationCount = 2u;
    telemetry.retiredTransientTextureCount = 3u;
    telemetry.retiredTransientBufferCount = 2u;
    telemetry.descriptorTransientPeak = 7u;
    telemetry.descriptorAllocationFailures = 1u;
    telemetry.pipelineCacheGraphicsHits = 5u;
    telemetry.pipelineCacheGraphicsMisses = 2u;
    telemetry.pipelineCacheGraphicsStores = 2u;
    telemetry.pipelineCacheGraphicsEntries = 2u;
    telemetry.pipelineCacheComputeHits = 4u;
    telemetry.pipelineCacheComputeMisses = 1u;
    telemetry.pipelineCacheComputeStores = 1u;
    telemetry.pipelineCacheComputeEntries = 1u;

    stats.BeginFrame();
    stats.SetThreadedFrameTelemetry(telemetry);
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_TRUE(frameInfo.descriptorMainlineActive);
    EXPECT_TRUE(frameInfo.pipelineMainlineActive);
    EXPECT_TRUE(frameInfo.transientLifetimeMainlineActive);
    EXPECT_TRUE(frameInfo.retirementMainlineActive);
    EXPECT_EQ(frameInfo.descriptorBypassCount, 0u);
    EXPECT_EQ(frameInfo.pipelineBypassCount, 0u);
    EXPECT_EQ(frameInfo.transientLifetimeBypassCount, 0u);
    EXPECT_EQ(frameInfo.retirementBypassCount, 0u);
    EXPECT_EQ(frameInfo.transientTextureRegistrationCount, 3u);
    EXPECT_EQ(frameInfo.transientBufferRegistrationCount, 2u);
    EXPECT_EQ(frameInfo.retiredTransientTextureCount, 3u);
    EXPECT_EQ(frameInfo.retiredTransientBufferCount, 2u);
    EXPECT_EQ(frameInfo.descriptorTransientPeak, 7u);
    EXPECT_EQ(frameInfo.descriptorAllocationFailures, 1u);
    EXPECT_EQ(frameInfo.pipelineCacheGraphicsHits, 5u);
    EXPECT_EQ(frameInfo.pipelineCacheGraphicsMisses, 2u);
    EXPECT_EQ(frameInfo.pipelineCacheGraphicsStores, 2u);
    EXPECT_EQ(frameInfo.pipelineCacheGraphicsEntries, 2u);
    EXPECT_EQ(frameInfo.pipelineCacheComputeHits, 4u);
    EXPECT_EQ(frameInfo.pipelineCacheComputeMisses, 1u);
    EXPECT_EQ(frameInfo.pipelineCacheComputeStores, 1u);
    EXPECT_EQ(frameInfo.pipelineCacheComputeEntries, 1u);
}
