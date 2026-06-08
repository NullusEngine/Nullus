#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Core/FrameObjectBindingProvider.h"
#include "Rendering/Core/RendererStats.h"
#include "Rendering/Data/DrawCallOptimizationStats.h"
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

    class TestCommandPool final : public NLS::Render::RHI::RHICommandPool
    {
    public:
        explicit TestCommandPool(
            const NLS::Render::RHI::QueueType queueType,
            std::string debugName)
            : m_queueType(queueType)
            , m_debugName(std::move(debugName))
        {
        }

        std::string_view GetDebugName() const override { return m_debugName; }
        NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string = {}) override
        {
            return std::make_shared<TestCommandBuffer>();
        }
        void Reset() override {}

    private:
        NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;
        std::string m_debugName;
    };

    class TestFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        explicit TestFence(std::string debugName)
            : m_debugName(std::move(debugName))
        {
        }

        std::string_view GetDebugName() const override { return m_debugName; }
        bool IsSignaled() const override { return m_signaled; }
        void Reset() override { m_signaled = false; }
        bool Wait(uint64_t = 0u) override
        {
            m_signaled = true;
            return true;
        }

    private:
        std::string m_debugName;
        bool m_signaled = true;
    };

    class TestSemaphore final : public NLS::Render::RHI::RHISemaphore
    {
    public:
        explicit TestSemaphore(std::string debugName)
            : m_debugName(std::move(debugName))
        {
        }

        std::string_view GetDebugName() const override { return m_debugName; }
        bool IsSignaled() const override { return false; }
        void Reset() override {}

    private:
        std::string m_debugName;
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
            m_capabilities.supportsCompute = true;
            m_capabilities.supportsSwapchain = true;
            m_capabilities.supportsCurrentSceneRenderer = true;
            m_capabilities.supportsOffscreenFramebuffers = true;
            m_capabilities.supportsMultiRenderTargets = true;
            m_capabilities.supportsExplicitBarriers = true;
            m_capabilities.supportsCentralizedDescriptorManagement = true;
            m_capabilities.supportsPipelineStateCache = true;
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
            NLS::Render::RHI::QueueType queueType,
            std::string debugName = {}) override
        {
            return std::make_shared<TestCommandPool>(queueType, std::move(debugName));
        }
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName = {}) override
        {
            return std::make_shared<TestFence>(std::move(debugName));
        }
        std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName = {}) override
        {
            return std::make_shared<TestSemaphore>(std::move(debugName));
        }
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

    NLS::Render::Data::FrameDescriptor MakeFrameDescriptor(NLS::Render::Entities::Camera& camera)
    {
        NLS::Render::Data::FrameDescriptor frameDescriptor;
        frameDescriptor.renderWidth = 64u;
        frameDescriptor.renderHeight = 64u;
        frameDescriptor.camera = &camera;
        return frameDescriptor;
    }

    NLS::Render::Settings::DriverSettings MakeThreadedRendererStatsSettings()
    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.enableThreadedRendering = true;
        settings.threadedFrameSlotCount = 1u;
        return settings;
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
    stats.RecordGBufferMaterialResolve(true);
    stats.RecordGBufferMaterialResolve(false);
    stats.RecordGBufferMaterialResolve(true);
    stats.RecordPreparedRecordedDrawStaticBaseCache(false);
    stats.RecordPreparedRecordedDrawStaticBaseCache(true);
    stats.RecordPreparedRecordedDrawStaticBaseCache(true);
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
    EXPECT_EQ(frameInfo.gBufferMaterialResolveHitCount, 2u);
    EXPECT_EQ(frameInfo.gBufferMaterialResolveMissCount, 1u);
    EXPECT_EQ(frameInfo.preparedRecordedDrawStaticBaseCacheHitCount, 2u);
    EXPECT_EQ(frameInfo.preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(frameInfo.renderBindingSetCreationCount, 2u);
    EXPECT_EQ(frameInfo.renderSnapshotBufferCreationCount, 1u);
}

TEST(RendererStatsTests, CompositeRendererExposesFinalizedFrameInfoWithoutFeatureRegistration)
{
    NLS::Render::Context::Driver driver(MakeThreadedRendererStatsSettings());
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    StatsOnlyRenderer renderer(driver);
    NLS::Render::Resources::Material material;
    material.SetGPUInstances(3);
    const auto mesh = CreateTriangleMesh();

    NLS::Render::Entities::Drawable drawable;
    drawable.mesh = mesh.get();
    drawable.material = &material;
    NLS::Render::Entities::Camera camera;
    const auto frameDescriptor = MakeFrameDescriptor(camera);

    renderer.BeginFrame(frameDescriptor);
    renderer.DrawEntity(NLS::Render::Data::PipelineState {}, drawable);
    renderer.EndFrame();

    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.batchCount, 1u);
    EXPECT_EQ(frameInfo.instanceCount, 3u);
    EXPECT_EQ(frameInfo.polyCount, 3u);
    EXPECT_EQ(frameInfo.vertexCount, 9u);
}

TEST(RendererStatsTests, CompositeRendererFinalizedFrameInfoIsZeroWhenNoDrawsWereSubmitted)
{
    NLS::Render::Context::Driver driver(MakeThreadedRendererStatsSettings());
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    StatsOnlyRenderer renderer(driver);
    NLS::Render::Entities::Camera camera;
    const auto frameDescriptor = MakeFrameDescriptor(camera);

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

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
    renderer.BeginFrame(frameDescriptor);

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
    renderer.EndFrame();

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
    telemetry.unsafeGpuWorkQuarantined = true;
    telemetry.unsafeGpuWorkQuarantineReason = "queued work without retirement fence";
    stats.SetThreadedFrameTelemetry(telemetry);
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.inFlightFrameCount, 2u);
    EXPECT_EQ(frameInfo.blockedFrameCount, 1u);
    EXPECT_EQ(frameInfo.publishState, NLS::Render::Data::FramePublishState::BackPressured);
    EXPECT_EQ(frameInfo.stageSummary, NLS::Render::Data::ThreadedFrameStageSummary::Rhi);
    EXPECT_EQ(frameInfo.retirementState, NLS::Render::Data::FrameRetirementState::Pending);
    EXPECT_TRUE(frameInfo.unsafeGpuWorkQuarantined);
    EXPECT_EQ(frameInfo.unsafeGpuWorkQuarantineReason, "queued work without retirement fence");
}

TEST(RendererStatsTests, RendererStatsReusesLastThreadedTelemetryWhenRefreshIsUnavailable)
{
    NLS::Render::Core::RendererStats stats;

    NLS::Render::Context::ThreadedFrameTelemetry telemetry;
    telemetry.inFlightFrameCount = 2u;
    telemetry.blockedPublishCount = 1u;
    telemetry.reservedSlotWaitCount = 3u;
    telemetry.reservedSlotWaitTimeoutCount = 1u;
    telemetry.reservedSlotWaitTotalNs = 2400000u;
    telemetry.publishState = NLS::Render::Data::FramePublishState::BackPressured;
    telemetry.stageSummary = NLS::Render::Data::ThreadedFrameStageSummary::Rhi;
    telemetry.retirementState = NLS::Render::Data::FrameRetirementState::Pending;
    telemetry.pipelineMainlineActive = true;
    telemetry.pipelineCacheGraphicsHits = 4u;

    stats.BeginFrame();
    stats.SetThreadedFrameTelemetry(telemetry);
    stats.EndFrame();

    stats.BeginFrame();
    stats.RecordSceneParse(9u, 8u, 7u);
    EXPECT_TRUE(stats.ReuseLastThreadedFrameTelemetry());
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.parseSceneCallCount, 1u);
    EXPECT_EQ(frameInfo.parsedOpaqueDrawableCount, 9u);
    EXPECT_EQ(frameInfo.parsedTransparentDrawableCount, 8u);
    EXPECT_EQ(frameInfo.parsedSkyboxDrawableCount, 7u);
    EXPECT_EQ(frameInfo.inFlightFrameCount, 2u);
    EXPECT_EQ(frameInfo.blockedFrameCount, 1u);
    EXPECT_EQ(frameInfo.reservedSlotWaitCount, 3u);
    EXPECT_EQ(frameInfo.reservedSlotWaitTimeoutCount, 1u);
    EXPECT_EQ(frameInfo.reservedSlotWaitTotalNs, 2400000u);
    EXPECT_EQ(frameInfo.publishState, NLS::Render::Data::FramePublishState::BackPressured);
    EXPECT_EQ(frameInfo.stageSummary, NLS::Render::Data::ThreadedFrameStageSummary::Rhi);
    EXPECT_EQ(frameInfo.retirementState, NLS::Render::Data::FrameRetirementState::Pending);
    EXPECT_TRUE(frameInfo.pipelineMainlineActive);
    EXPECT_EQ(frameInfo.pipelineCacheGraphicsHits, 4u);
}

TEST(RendererStatsTests, RendererStatsDoesNotReuseThreadedTelemetryBeforeFirstSample)
{
    NLS::Render::Core::RendererStats stats;

    stats.BeginFrame();
    EXPECT_FALSE(stats.ReuseLastThreadedFrameTelemetry());
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.inFlightFrameCount, 0u);
    EXPECT_EQ(frameInfo.publishState, NLS::Render::Data::FramePublishState::Direct);
    EXPECT_EQ(frameInfo.stageSummary, NLS::Render::Data::ThreadedFrameStageSummary::Direct);
    EXPECT_EQ(frameInfo.retirementState, NLS::Render::Data::FrameRetirementState::Direct);
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
    telemetry.parallelCommandWorkUnitCount = 5u;
    telemetry.parallelRecordingWorkerCount = 3u;
    telemetry.parallelFallbackReason = "ordered sliced submission unavailable";

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
    EXPECT_EQ(frameInfo.parallelCommandWorkUnitCount, 5u);
    EXPECT_EQ(frameInfo.parallelRecordingWorkerCount, 3u);
    EXPECT_EQ(frameInfo.parallelFallbackReason, "ordered sliced submission unavailable");
}

TEST(RendererStatsTests, RendererStatsTracksDrawCallOptimizationCounters)
{
    NLS::Render::Core::RendererStats stats;

    stats.BeginFrame();
    stats.RecordDrawCallOptimizationStats({
        .rawVisibleObjectCount = 1000u,
        .submittedSceneDrawCount = 96u,
        .dynamicInstanceGroupCount = 12u,
        .largestInstanceGroupSize = 128u,
        .cachedCommandRebuildCount = 4u,
        .objectDataOverflowDroppedObjectCount = 7u
    });
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.rawVisibleObjectCount, 1000u);
    EXPECT_EQ(frameInfo.submittedSceneDrawCount, 96u);
    EXPECT_EQ(frameInfo.dynamicInstanceGroupCount, 12u);
    EXPECT_EQ(frameInfo.largestInstanceGroupSize, 128u);
    EXPECT_EQ(frameInfo.cachedCommandRebuildCount, 4u);
    EXPECT_EQ(frameInfo.objectDataOverflowDroppedObjectCount, 7u);
}

TEST(RendererStatsTests, DrawCallOptimizationStatsDoesNotOverwriteThreadedTelemetry)
{
    NLS::Render::Core::RendererStats stats;

    NLS::Render::Context::ThreadedFrameTelemetry telemetry;
    telemetry.parallelCommandWorkUnitCount = 3u;
    telemetry.parallelRecordingWorkerCount = 2u;
    telemetry.parallelFallbackReason = "unsafe ordered slice fallback";

    stats.BeginFrame();
    stats.SetThreadedFrameTelemetry(telemetry);
    stats.RecordDrawCallOptimizationStats({
        .rawVisibleObjectCount = 1000u,
        .submittedSceneDrawCount = 96u,
        .dynamicInstanceGroupCount = 12u,
        .largestInstanceGroupSize = 128u,
        .cachedCommandRebuildCount = 4u,
        .objectDataOverflowDroppedObjectCount = 7u
    });
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.rawVisibleObjectCount, 1000u);
    EXPECT_EQ(frameInfo.parallelCommandWorkUnitCount, 3u);
    EXPECT_EQ(frameInfo.parallelRecordingWorkerCount, 2u);
    EXPECT_EQ(frameInfo.parallelFallbackReason, "unsafe ordered slice fallback");
}

TEST(RendererStatsTests, RendererStatsAggregatesLargeSceneTelemetryAndResetsPerFrame)
{
    NLS::Render::Core::RendererStats stats;

    NLS::Render::Data::LargeSceneTelemetry first;
    first.registeredPrimitiveCount = 1000u;
    first.staticPrimitiveCount = 800u;
    first.dynamicPrimitiveCount = 200u;
    first.unclassifiedPrimitiveCount = 0u;
    first.spatialCandidateCount = 120u;
    first.fullScanCandidateCount = 1000u;
    first.visiblePrimitiveCount = 80u;
    first.visibleMeshCount = 76u;
    first.culledByReason[2] = 5u;
    first.lodSelectionCount[3] = 6u;
    first.activeHLODClusterCount = 7u;
    first.occlusionTestCount = 8u;
    first.occlusionCulledCount = 9u;
    first.streamingRequestCount = 10u;
    first.streamingCommitCount = 11u;
    first.streamingEvictCount = 12u;
    first.syncTouchedPrimitiveCount = 12u;
    first.syncFullSweepCount = 1u;
    first.boundsDirtyPrimitiveCount = 4u;
    first.primitiveSlotReuseCount = 2u;
    first.primitiveRecordsTouched = 125u;
    first.allocatedPrimitiveSlotCount = 1005u;
    first.tombstonedPrimitiveSlotCount = 5u;
    first.syncSweepTouchedSlotCount = 1005u;
    first.visibilityTestedPrimitiveCount = 130u;
    first.visibilityBitsetWordCount = 24u;
    first.finalizationTouchedPrimitiveCount = 80u;
    first.finalizationTouchedCommandCount = 90u;
    first.commandOffsetRebuildCount = 2u;
    first.rawVisibleDrawCount = 88u;
    first.submittedDrawCount = 44u;
    first.dynamicInstanceGroupCount = 12u;
    first.streamingDependencyCount = 14u;
    first.residencyTicketCount = 9u;
    first.residentCpuBytes = 1024u;
    first.residentGpuBytes = 2048u;
    first.requestedCpuBytes = 4096u;
    first.requestedGpuBytes = 8192u;
    first.dynamicCandidateCount = 15u;
    first.dynamicRecordsTouched = 16u;
    first.staticIndexRefitCount = 17u;
    first.staticIndexRebuildCount = 18u;
    first.staticIndexLastGoodQueryCount = 19u;
    first.staticIndexDirtyOverlayCount = 20u;
    first.spatialRebuildFallbackCount = 21u;
    first.dynamicIndexUpdateCount = 22u;
    first.syncTimeNs = 300u;
    first.serialVisibilityTimeNs = 400u;
    first.queueFinalizationTimeNs = 500u;
    first.hzbBuildTimeNs = 600u;
    first.hzbHistoryPruneTouchedHandleCount = 7u;
    first.hzbHistoryPruneRemovedHandleCount = 3u;
    first.hzbHistoryPruneRemovedKeyCount = 5u;
    first.hzbHistoryPruneTimeNs = 40u;
    first.streamingCommitTimeNs = 700u;

    NLS::Render::Data::LargeSceneTelemetry second;
    second.registeredPrimitiveCount = 1000u;
    second.unclassifiedPrimitiveCount = 1000u;
    second.spatialCandidateCount = 30u;
    second.fullScanCandidateCount = 500u;
    second.visiblePrimitiveCount = 10u;
    second.visibleMeshCount = 9u;
    second.culledByReason[2] = 1u;
    second.culledByReason[4] = 2u;
    second.lodSelectionCount[3] = 3u;
    second.lodSelectionCount[5] = 4u;
    second.activeHLODClusterCount = 5u;
    second.occlusionTestCount = 6u;
    second.occlusionCulledCount = 7u;
    second.streamingRequestCount = 8u;
    second.streamingCommitCount = 9u;
    second.streamingEvictCount = 10u;
    second.syncTouchedPrimitiveCount = 2u;
    second.boundsDirtyPrimitiveCount = 1u;
    second.primitiveSlotReuseCount = 3u;
    second.primitiveRecordsTouched = 35u;
    second.allocatedPrimitiveSlotCount = 1002u;
    second.tombstonedPrimitiveSlotCount = 2u;
    second.syncSweepTouchedSlotCount = 1002u;
    second.visibilityTestedPrimitiveCount = 40u;
    second.visibilityBitsetWordCount = 4u;
    second.finalizationTouchedPrimitiveCount = 10u;
    second.finalizationTouchedCommandCount = 11u;
    second.rawVisibleDrawCount = 12u;
    second.submittedDrawCount = 6u;
    second.dynamicInstanceGroupCount = 1u;
    second.streamingDependencyCount = 3u;
    second.residencyTicketCount = 1u;
    second.residentCpuBytes = 512u;
    second.residentGpuBytes = 256u;
    second.requestedCpuBytes = 2048u;
    second.requestedGpuBytes = 1024u;
    second.dynamicCandidateCount = 1u;
    second.dynamicRecordsTouched = 2u;
    second.staticIndexRefitCount = 3u;
    second.staticIndexRebuildCount = 4u;
    second.staticIndexLastGoodQueryCount = 5u;
    second.staticIndexDirtyOverlayCount = 6u;
    second.spatialRebuildFallbackCount = 7u;
    second.dynamicIndexUpdateCount = 8u;
    second.syncTimeNs = 50u;
    second.parallelVisibilityTimeNs = 60u;
    second.queueFinalizationTimeNs = 70u;
    second.hzbBuildTimeNs = 80u;
    second.hzbHistoryPruneTouchedHandleCount = 2u;
    second.hzbHistoryPruneRemovedHandleCount = 1u;
    second.hzbHistoryPruneRemovedKeyCount = 4u;
    second.hzbHistoryPruneTimeNs = 6u;
    second.streamingCommitTimeNs = 90u;

    stats.BeginFrame();
    stats.RecordLargeSceneTelemetry(first);
    stats.RecordLargeSceneTelemetry(second);
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.largeScene.registeredPrimitiveCount, 2000u);
    EXPECT_EQ(frameInfo.largeScene.staticPrimitiveCount, 800u);
    EXPECT_EQ(frameInfo.largeScene.dynamicPrimitiveCount, 200u);
    EXPECT_EQ(frameInfo.largeScene.unclassifiedPrimitiveCount, 1000u);
    EXPECT_EQ(frameInfo.largeScene.spatialCandidateCount, 150u);
    EXPECT_EQ(frameInfo.largeScene.fullScanCandidateCount, 1500u);
    EXPECT_EQ(frameInfo.largeScene.visiblePrimitiveCount, 90u);
    EXPECT_EQ(frameInfo.largeScene.visibleMeshCount, 85u);
    EXPECT_EQ(frameInfo.largeScene.culledByReason[2], 6u);
    EXPECT_EQ(frameInfo.largeScene.culledByReason[4], 2u);
    EXPECT_EQ(frameInfo.largeScene.lodSelectionCount[3], 9u);
    EXPECT_EQ(frameInfo.largeScene.lodSelectionCount[5], 4u);
    EXPECT_EQ(frameInfo.largeScene.activeHLODClusterCount, 12u);
    EXPECT_EQ(frameInfo.largeScene.occlusionTestCount, 14u);
    EXPECT_EQ(frameInfo.largeScene.occlusionCulledCount, 16u);
    EXPECT_EQ(frameInfo.largeScene.streamingRequestCount, 18u);
    EXPECT_EQ(frameInfo.largeScene.streamingCommitCount, 20u);
    EXPECT_EQ(frameInfo.largeScene.streamingEvictCount, 22u);
    EXPECT_EQ(frameInfo.largeScene.syncTouchedPrimitiveCount, 14u);
    EXPECT_EQ(frameInfo.largeScene.syncFullSweepCount, 1u);
    EXPECT_EQ(frameInfo.largeScene.boundsDirtyPrimitiveCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.primitiveSlotReuseCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.primitiveRecordsTouched, 160u);
    EXPECT_EQ(frameInfo.largeScene.allocatedPrimitiveSlotCount, 2007u);
    EXPECT_EQ(frameInfo.largeScene.tombstonedPrimitiveSlotCount, 7u);
    EXPECT_EQ(frameInfo.largeScene.syncSweepTouchedSlotCount, 2007u);
    EXPECT_EQ(frameInfo.largeScene.visibilityTestedPrimitiveCount, 170u);
    EXPECT_EQ(frameInfo.largeScene.visibilityBitsetWordCount, 28u);
    EXPECT_EQ(frameInfo.largeScene.finalizationTouchedPrimitiveCount, 90u);
    EXPECT_EQ(frameInfo.largeScene.finalizationTouchedCommandCount, 101u);
    EXPECT_EQ(frameInfo.largeScene.commandOffsetRebuildCount, 2u);
    EXPECT_EQ(frameInfo.largeScene.rawVisibleDrawCount, 100u);
    EXPECT_EQ(frameInfo.largeScene.submittedDrawCount, 50u);
    EXPECT_EQ(frameInfo.largeScene.dynamicInstanceGroupCount, 13u);
    EXPECT_EQ(frameInfo.largeScene.streamingDependencyCount, 17u);
    EXPECT_EQ(frameInfo.largeScene.residencyTicketCount, second.residencyTicketCount);
    EXPECT_EQ(frameInfo.largeScene.residentCpuBytes, second.residentCpuBytes);
    EXPECT_EQ(frameInfo.largeScene.residentGpuBytes, second.residentGpuBytes);
    EXPECT_EQ(frameInfo.largeScene.requestedCpuBytes, second.requestedCpuBytes);
    EXPECT_EQ(frameInfo.largeScene.requestedGpuBytes, second.requestedGpuBytes);
    EXPECT_EQ(frameInfo.largeScene.dynamicCandidateCount, 16u);
    EXPECT_EQ(frameInfo.largeScene.dynamicRecordsTouched, 18u);
    EXPECT_EQ(frameInfo.largeScene.staticIndexRefitCount, 20u);
    EXPECT_EQ(frameInfo.largeScene.staticIndexRebuildCount, 22u);
    EXPECT_EQ(frameInfo.largeScene.staticIndexLastGoodQueryCount, 24u);
    EXPECT_EQ(frameInfo.largeScene.staticIndexDirtyOverlayCount, 26u);
    EXPECT_EQ(frameInfo.largeScene.spatialRebuildFallbackCount, 28u);
    EXPECT_EQ(frameInfo.largeScene.dynamicIndexUpdateCount, 30u);
    EXPECT_EQ(frameInfo.largeScene.syncTimeNs, 350u);
    EXPECT_EQ(frameInfo.largeScene.serialVisibilityTimeNs, 400u);
    EXPECT_EQ(frameInfo.largeScene.parallelVisibilityTimeNs, 60u);
    EXPECT_EQ(frameInfo.largeScene.queueFinalizationTimeNs, 570u);
    EXPECT_EQ(frameInfo.largeScene.hzbBuildTimeNs, 680u);
    EXPECT_EQ(frameInfo.largeScene.hzbHistoryPruneTouchedHandleCount, 9u);
    EXPECT_EQ(frameInfo.largeScene.hzbHistoryPruneRemovedHandleCount, 4u);
    EXPECT_EQ(frameInfo.largeScene.hzbHistoryPruneRemovedKeyCount, 9u);
    EXPECT_EQ(frameInfo.largeScene.hzbHistoryPruneTimeNs, 46u);
    EXPECT_EQ(frameInfo.largeScene.streamingCommitTimeNs, 790u);

    stats.BeginFrame();
    stats.EndFrame();

    const auto& resetFrameInfo = stats.GetFrameInfo();
    EXPECT_EQ(resetFrameInfo.largeScene.registeredPrimitiveCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.spatialCandidateCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.fullScanCandidateCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.visiblePrimitiveCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.primitiveRecordsTouched, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.allocatedPrimitiveSlotCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.syncSweepTouchedSlotCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.boundsDirtyPrimitiveCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.rawVisibleDrawCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.residentCpuBytes, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.requestedGpuBytes, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.culledByReason[2], 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.lodSelectionCount[3], 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.streamingRequestCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.dynamicCandidateCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.syncTimeNs, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.hzbBuildTimeNs, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.hzbHistoryPruneTouchedHandleCount, 0u);
    EXPECT_EQ(resetFrameInfo.largeScene.hzbHistoryPruneRemovedKeyCount, 0u);
}

TEST(RendererStatsTests, RendererStatsSumsLargeScenePrimitiveCountsAcrossRenderScenes)
{
    NLS::Render::Core::RendererStats stats;

    NLS::Render::Data::LargeSceneTelemetry mainScene;
    mainScene.registeredPrimitiveCount = 3u;
    mainScene.staticPrimitiveCount = 2u;
    mainScene.dynamicPrimitiveCount = 1u;
    mainScene.unclassifiedPrimitiveCount = 0u;

    NLS::Render::Data::LargeSceneTelemetry additiveScene;
    additiveScene.registeredPrimitiveCount = 5u;
    additiveScene.staticPrimitiveCount = 4u;
    additiveScene.dynamicPrimitiveCount = 1u;
    additiveScene.unclassifiedPrimitiveCount = 0u;

    stats.BeginFrame();
    stats.RecordLargeSceneTelemetry(mainScene);
    stats.RecordLargeSceneTelemetry(additiveScene);
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.largeScene.registeredPrimitiveCount, 8u);
    EXPECT_EQ(frameInfo.largeScene.staticPrimitiveCount, 6u);
    EXPECT_EQ(frameInfo.largeScene.dynamicPrimitiveCount, 2u);
    EXPECT_EQ(frameInfo.largeScene.unclassifiedPrimitiveCount, 0u);
}

TEST(RendererStatsTests, RendererStatsUsesLatestLargeSceneResidencyByteGauge)
{
    NLS::Render::Core::RendererStats stats;

    NLS::Render::Data::LargeSceneTelemetry lower;
    lower.residentCpuBytes = 1024u;
    lower.residentGpuBytes = 2048u;
    lower.requestedCpuBytes = 4096u;
    lower.requestedGpuBytes = 8192u;

    NLS::Render::Data::LargeSceneTelemetry higher;
    higher.residentCpuBytes = 2048u;
    higher.residentGpuBytes = 4096u;
    higher.requestedCpuBytes = 8192u;
    higher.requestedGpuBytes = 16384u;

    stats.BeginFrame();
    stats.RecordLargeSceneTelemetry(lower);
    stats.RecordLargeSceneTelemetry(higher);
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.largeScene.residentCpuBytes, higher.residentCpuBytes);
    EXPECT_EQ(frameInfo.largeScene.residentGpuBytes, higher.residentGpuBytes);
    EXPECT_EQ(frameInfo.largeScene.requestedCpuBytes, higher.requestedCpuBytes);
    EXPECT_EQ(frameInfo.largeScene.requestedGpuBytes, higher.requestedGpuBytes);
}
