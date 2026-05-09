#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/SwapchainResizePolicy.h"
#include "Rendering/FrameGraph/FrameGraphBuffer.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"
#include "Rendering/FrameGraph/FrameGraphTexture.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/Settings/DriverSettings.h"

namespace
{
    class ContractAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "ContractAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override
        {
            return NLS::Render::RHI::NativeBackendType::DX12;
        }
        std::string_view GetVendor() const override { return "ContractVendor"; }
        std::string_view GetHardware() const override { return "ContractHardware"; }
    };

    class ContractCompletionToken final : public NLS::Render::RHI::RHICompletionToken
    {
    public:
        explicit ContractCompletionToken(NLS::Render::RHI::RHICompletionStatus status)
            : m_status(std::move(status))
        {
        }

        std::string_view GetDebugName() const override { return "ContractCompletionToken"; }
        bool IsComplete() const override { return m_status.IsComplete(); }
        NLS::Render::RHI::RHICompletionStatus GetStatus() const override { return m_status; }
        NLS::Render::RHI::RHICompletionStatus Wait(uint64_t = 0) override
        {
            ++waitCalls;
            return m_status;
        }

        size_t waitCalls = 0u;

    private:
        NLS::Render::RHI::RHICompletionStatus m_status;
    };

    class ContractFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        std::string_view GetDebugName() const override { return "ContractFence"; }
        bool IsSignaled() const override { return signaled; }
        void Reset() override
        {
            signaled = false;
            ++resetCalls;
        }
        bool Wait(uint64_t = 0) override
        {
            signaled = true;
            ++waitCalls;
            return true;
        }
        NLS::Render::RHI::NativeHandle GetNativeFenceHandle() override
        {
            return { NLS::Render::RHI::BackendType::DX12, this };
        }

        bool signaled = false;
        size_t resetCalls = 0u;
        size_t waitCalls = 0u;
    };

    class ContractSemaphore final : public NLS::Render::RHI::RHISemaphore
    {
    public:
        std::string_view GetDebugName() const override { return "ContractSemaphore"; }
        bool IsSignaled() const override { return signaled; }
        void Reset() override
        {
            signaled = false;
            ++resetCalls;
        }
        NLS::Render::RHI::NativeHandle GetNativeSemaphoreHandle() override
        {
            return { NLS::Render::RHI::BackendType::DX12, this };
        }

        bool signaled = false;
        size_t resetCalls = 0u;
    };

    class ContractTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        ContractTexture() = default;

        explicit ContractTexture(NLS::Render::RHI::RHITextureDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override
        {
            return m_desc.debugName.empty() ? std::string_view("ContractTexture") : std::string_view(m_desc.debugName);
        }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return reportedState; }
        NLS::Render::RHI::NativeHandle GetNativeImageHandle() override
        {
            return { NLS::Render::RHI::BackendType::DX12, this };
        }

        NLS::Render::RHI::ResourceState reportedState = NLS::Render::RHI::ResourceState::Unknown;

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    class ContractTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        ContractTextureView() = default;

        ContractTextureView(
            std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
            NLS::Render::RHI::RHITextureViewDesc desc)
            : m_texture(std::move(texture))
            , m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override
        {
            return m_desc.debugName.empty()
                ? std::string_view("ContractTextureView")
                : std::string_view(m_desc.debugName);
        }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
        NLS::Render::RHI::RHITextureViewDesc m_desc {};
    };

    class ContractBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        explicit ContractBuffer(NLS::Render::RHI::RHIBufferDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override
        {
            return m_desc.debugName.empty() ? std::string_view("ContractBuffer") : std::string_view(m_desc.debugName);
        }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }
        uint64_t GetGPUAddress() const override { return 0u; }
        NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override
        {
            return { NLS::Render::RHI::BackendType::DX12, this };
        }

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc {};
    };

    class ContractCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "ContractCommandBuffer"; }
        void Begin() override
        {
            recording = true;
            ++beginCalls;
        }
        void End() override
        {
            recording = false;
            ++endCalls;
        }
        void Reset() override
        {
            recording = false;
            ++resetCalls;
        }
        bool IsRecording() const override { return recording; }
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

        bool recording = false;
        size_t beginCalls = 0u;
        size_t endCalls = 0u;
        size_t resetCalls = 0u;
    };

    class ContractCommandPool final : public NLS::Render::RHI::RHICommandPool
    {
    public:
        std::string_view GetDebugName() const override { return "ContractCommandPool"; }
        NLS::Render::RHI::QueueType GetQueueType() const override { return NLS::Render::RHI::QueueType::Graphics; }
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string = {}) override
        {
            return commandBuffer;
        }
        void Reset() override { ++resetCalls; }

        std::shared_ptr<ContractCommandBuffer> commandBuffer;
        size_t resetCalls = 0u;
    };

    class ContractQueue final : public NLS::Render::RHI::RHIQueue
    {
    public:
        std::string_view GetDebugName() const override { return "ContractQueue"; }
        NLS::Render::RHI::QueueType GetType() const override { return NLS::Render::RHI::QueueType::Graphics; }
        void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
        {
            ++submitCalls;
            lastSubmitDesc = submitDesc;
        }
        void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
        {
            ++presentCalls;
            lastPresentDesc = presentDesc;
        }

        size_t submitCalls = 0u;
        size_t presentCalls = 0u;
        NLS::Render::RHI::RHISubmitDesc lastSubmitDesc {};
        NLS::Render::RHI::RHIPresentDesc lastPresentDesc {};
    };

    class ContractSwapchain final : public NLS::Render::RHI::RHISwapchain
    {
    public:
        ContractSwapchain()
            : backbufferView(std::make_shared<ContractTextureView>())
        {
            m_desc.width = 800u;
            m_desc.height = 600u;
        }

        explicit ContractSwapchain(std::weak_ptr<NLS::Render::RHI::RHITextureView> trackedView)
            : ContractSwapchain()
        {
            this->trackedView = std::move(trackedView);
        }

        std::string_view GetDebugName() const override { return "ContractSwapchain"; }
        const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_desc; }
        uint32_t GetImageCount() const override { return 2u; }
        std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
            const std::shared_ptr<NLS::Render::RHI::RHISemaphore>&,
            const std::shared_ptr<NLS::Render::RHI::RHIFence>&) override
        {
            ++acquireCalls;
            return NLS::Render::RHI::RHIAcquiredImage{ acquiredImageIndex, backbufferView, false };
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> GetBackbufferView(uint32_t index) override
        {
            lastBackbufferIndex = index;
            return backbufferView;
        }
        bool Resize(uint32_t width, uint32_t height) override
        {
            ++resizeCalls;
            resizeWidth = width;
            resizeHeight = height;
            trackedViewReleasedBeforeResize = trackedView.expired();
            fenceWaitsObservedAtResize = observedFence != nullptr ? observedFence->waitCalls : 0u;
            commandResetsObservedAtResize = observedCommandBuffer != nullptr ? observedCommandBuffer->resetCalls : 0u;
            commandPoolResetsObservedAtResize = observedCommandPool != nullptr ? observedCommandPool->resetCalls : 0u;
            m_desc.width = width;
            m_desc.height = height;
            return true;
        }

        std::weak_ptr<NLS::Render::RHI::RHITextureView> trackedView;
        std::shared_ptr<ContractTextureView> backbufferView;
        ContractFence* observedFence = nullptr;
        ContractCommandBuffer* observedCommandBuffer = nullptr;
        ContractCommandPool* observedCommandPool = nullptr;
        size_t resizeCalls = 0u;
        size_t acquireCalls = 0u;
        size_t fenceWaitsObservedAtResize = 0u;
        size_t commandResetsObservedAtResize = 0u;
        size_t commandPoolResetsObservedAtResize = 0u;
        uint32_t acquiredImageIndex = 1u;
        uint32_t lastBackbufferIndex = 0u;
        uint32_t resizeWidth = 0u;
        uint32_t resizeHeight = 0u;
        bool trackedViewReleasedBeforeResize = false;

    private:
        NLS::Render::RHI::SwapchainDesc m_desc {};
    };

    class ContractDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        using NLS::Render::RHI::RHIDevice::CreateBuffer;
        using NLS::Render::RHI::RHIDevice::CreateTexture;

        ContractDevice()
            : m_adapter(std::make_shared<ContractAdapter>())
            , m_queue(std::make_shared<ContractQueue>())
        {
            m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
            m_capabilities.backendReady = true;
            m_capabilities.supportsGraphics = true;
            m_capabilities.supportsSwapchain = true;
            m_capabilities.supportsExplicitBarriers = true;
            m_capabilities.supportsCentralizedDescriptorManagement = true;
        }

        std::string_view GetDebugName() const override { return "ContractDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override
        {
            return m_queue;
        }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(
            const NLS::Render::RHI::SwapchainDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc& desc,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override
        {
            return std::make_shared<ContractBuffer>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const NLS::Render::RHI::RHITextureUploadDesc&) override
        {
            return std::make_shared<ContractTexture>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHITextureViewDesc& desc) override
        {
            return std::make_shared<ContractTextureView>(texture, desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(
            const NLS::Render::RHI::SamplerDesc&,
            std::string = {}) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(
            const NLS::Render::RHI::RHIBindingLayoutDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(
            const NLS::Render::RHI::RHIBindingSetDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(
            const NLS::Render::RHI::RHIPipelineLayoutDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(
            const NLS::Render::RHI::RHIShaderModuleDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(
            const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(
            const NLS::Render::RHI::RHIComputePipelineDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
            NLS::Render::RHI::QueueType,
            std::string = {}) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string = {}) override
        {
            return std::make_shared<ContractFence>();
        }
        std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string = {}) override
        {
            return std::make_shared<ContractSemaphore>();
        }
        void ReadPixels(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            uint32_t,
            uint32_t,
            uint32_t,
            uint32_t,
            NLS::Render::Settings::EPixelDataFormat,
            NLS::Render::Settings::EPixelDataType,
            void*) override
        {
            lastReadPixelsTexture = texture;
        }
        NLS::Render::RHI::RHIReadbackResult BeginReadPixels(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            uint32_t,
            uint32_t,
            uint32_t,
            uint32_t,
            NLS::Render::Settings::EPixelDataFormat,
            NLS::Render::Settings::EPixelDataType,
            void*) override
        {
            ++beginReadPixelsCalls;
            lastReadPixelsTexture = texture;
            return nextBeginReadPixelsResult;
        }

        std::shared_ptr<ContractQueue> GetContractQueue() const { return m_queue; }

        size_t beginReadPixelsCalls = 0u;
        std::shared_ptr<NLS::Render::RHI::RHITexture> lastReadPixelsTexture;
        NLS::Render::RHI::RHIReadbackResult nextBeginReadPixelsResult {
            NLS::Render::RHI::RHIReadbackStatusCode::Success,
            {},
            std::make_shared<ContractCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
                NLS::Render::RHI::RHICompletionStatusCode::Success,
                {}
            })
        };

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        std::shared_ptr<ContractQueue> m_queue;
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    NLS::Render::Settings::DriverSettings MakeContractDriverSettings(bool threaded = false)
    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.enableThreadedRendering = threaded;
        settings.threadedFrameSlotCount = 1u;
        settings.framesInFlight = 1u;
        return settings;
    }
}

TEST(RenderFrameworkContractTests, SwapchainResizeContractDrainsThreadedFramesAndReleasesBackbufferBeforeResize)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings(true));
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 101u;
    snapshot.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    auto retainedBackbufferView = std::make_shared<ContractTextureView>();
    std::weak_ptr<NLS::Render::RHI::RHITextureView> weakBackbufferView = retainedBackbufferView;
    auto swapchain = std::make_shared<ContractSwapchain>(weakBackbufferView);
    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    auto commandPool = std::make_shared<ContractCommandPool>();
    auto frameFence = std::make_shared<ContractFence>();
    commandPool->commandBuffer = commandBuffer;
    swapchain->observedFence = frameFence.get();
    swapchain->observedCommandBuffer = commandBuffer.get();
    swapchain->observedCommandPool = commandPool.get();

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.swapchainBackbufferView = retainedBackbufferView;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    retainedBackbufferView.reset();

    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    driver.ResizePlatformSwapchain(1280u, 720u);

    EXPECT_EQ(swapchain->resizeCalls, 0u);
    EXPECT_FALSE(weakBackbufferView.expired());
    EXPECT_EQ(frameFence->waitCalls, 0u);

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = true;
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = snapshot.frameId;
    ASSERT_TRUE(lifecycle->TryBeginRenderScene(0u));
    ASSERT_TRUE(lifecycle->CompleteRenderScene(0u, package));
    ASSERT_TRUE(lifecycle->TryBeginRhiSubmission(0u));
    ASSERT_TRUE(lifecycle->CompleteRhiSubmission(0u, submissionFrame));
    ASSERT_TRUE(lifecycle->RetireFrame(0u));

    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    driver.ResizePlatformSwapchain(1280u, 720u);

    EXPECT_EQ(swapchain->resizeCalls, 1u);
    EXPECT_TRUE(swapchain->trackedViewReleasedBeforeResize);
    EXPECT_GE(swapchain->fenceWaitsObservedAtResize, 1u);
    EXPECT_GE(swapchain->commandResetsObservedAtResize, 1u);
    EXPECT_GE(swapchain->commandPoolResetsObservedAtResize, 1u);
    EXPECT_EQ(frameContext.swapchainBackbufferView, nullptr);
    EXPECT_EQ(swapchain->resizeWidth, 1280u);
    EXPECT_EQ(swapchain->resizeHeight, 720u);
}

TEST(RenderFrameworkContractTests, ReadbackContractSeparatesBeginTokenFromCheckedWaitAndDiagnostics)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "ContractReadbackTexture";
    textureDesc.extent = { 4u, 4u, 1u };
    auto texture = std::make_shared<ContractTexture>(textureDesc);
    std::array<uint8_t, 16> pixels {};

    auto pendingCompletion = std::make_shared<ContractCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
        NLS::Render::RHI::RHICompletionStatusCode::Pending,
        {}
    });
    explicitDevice->nextBeginReadPixelsResult = {
        NLS::Render::RHI::RHIReadbackStatusCode::Success,
        {},
        pendingCompletion
    };

    const auto beginResult = NLS::Render::Context::DriverRendererAccess::BeginReadPixels(
        driver,
        texture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixels.data());

    EXPECT_TRUE(beginResult.Succeeded());
    EXPECT_EQ(beginResult.completion, pendingCompletion);
    EXPECT_EQ(pendingCompletion->waitCalls, 0u);
    EXPECT_EQ(explicitDevice->lastReadPixelsTexture, texture);

    auto failedCompletion = std::make_shared<ContractCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
        NLS::Render::RHI::RHICompletionStatusCode::Failed,
        "readback fence failed"
    });
    explicitDevice->nextBeginReadPixelsResult = {
        NLS::Render::RHI::RHIReadbackStatusCode::Success,
        {},
        failedCompletion
    };

    const auto checkedResult = NLS::Render::Context::DriverRendererAccess::ReadPixelsChecked(
        driver,
        texture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixels.data());

    EXPECT_EQ(failedCompletion->waitCalls, 1u);
    EXPECT_EQ(checkedResult.code, NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure);
    EXPECT_EQ(checkedResult.message, "readback fence failed");
}

TEST(RenderFrameworkContractTests, UiCompositionContractPropagatesSignalToPresentAndClearsAfterSubmit)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    auto swapchain = std::make_shared<ContractSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    auto commandPool = std::make_shared<ContractCommandPool>();
    auto frameFence = std::make_shared<ContractFence>();
    auto imageAcquiredSemaphore = std::make_shared<ContractSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<ContractSemaphore>();
    commandPool->commandBuffer = commandBuffer;

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::CanBeginStandaloneExplicitFrame(driver));
    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);

    auto boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    ASSERT_TRUE(boundary.sceneToUiWaitSemaphore.IsValid());
    EXPECT_EQ(boundary.sceneToUiWaitSemaphore.backend, NLS::Render::RHI::BackendType::DX12);
    EXPECT_EQ(boundary.sceneToUiWaitSemaphore.handle, renderFinishedSemaphore.get());
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());

    const NLS::Render::RHI::NativeHandle uiSignalSemaphore{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x9876)
    };
    constexpr uint64_t uiSignalValue = 77u;
    NLS::Render::Context::DriverUIAccess::SetUICompositionSignal(
        driver,
        uiSignalSemaphore,
        uiSignalValue);

    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, true);

    const auto queue = explicitDevice->GetContractQueue();
    ASSERT_EQ(queue->submitCalls, 1u);
    ASSERT_EQ(queue->presentCalls, 1u);
    ASSERT_EQ(queue->lastPresentDesc.waitSemaphores.size(), 1u);
    EXPECT_EQ(queue->lastPresentDesc.waitSemaphores.front(), renderFinishedSemaphore);
    EXPECT_EQ(queue->lastPresentDesc.uiSignalSemaphore.backend, NLS::Render::RHI::BackendType::DX12);
    EXPECT_EQ(queue->lastPresentDesc.uiSignalSemaphore.handle, uiSignalSemaphore.handle);
    EXPECT_EQ(queue->lastPresentDesc.uiSignalValue, uiSignalValue);

    boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);
}

TEST(RenderFrameworkContractTests, FrameGraphResourceLifetimeContractKeepsTransientResourcesUntilRetiredFrame)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 9u;
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
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    textureDesc.debugName = "ContractTransientColor";

    NLS::Render::FrameGraph::FrameGraphBuffer buffer;
    NLS::Render::FrameGraph::FrameGraphBuffer::Desc bufferDesc;
    bufferDesc.size = 256u;
    bufferDesc.type = NLS::Render::RHI::BufferType::ShaderStorage;
    bufferDesc.usage = NLS::Render::RHI::BufferUsage::DynamicDraw;
    bufferDesc.debugName = "ContractTransientLightList";

    texture.create(textureDesc, &executionContext);
    buffer.create(bufferDesc, &executionContext);

    std::weak_ptr<NLS::Render::RHI::RHITexture> weakTexture = texture.explicitTexture;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> weakTextureView = texture.explicitView;
    std::weak_ptr<NLS::Render::RHI::RHIBuffer> weakBuffer = buffer.explicitBuffer;

    auto stats = frameContext.resourceStateTracker->GetStats();
    EXPECT_EQ(stats.transientTextureRegistrations, 1u);
    EXPECT_EQ(stats.transientTextureViewRegistrations, 1u);
    EXPECT_EQ(stats.transientBufferRegistrations, 1u);

    texture.destroy(textureDesc, &executionContext);
    buffer.destroy(bufferDesc, &executionContext);
    EXPECT_EQ(texture.explicitTexture, nullptr);
    EXPECT_EQ(texture.explicitView, nullptr);
    EXPECT_EQ(buffer.explicitBuffer, nullptr);
    EXPECT_FALSE(weakTexture.expired());
    EXPECT_FALSE(weakTextureView.expired());
    EXPECT_FALSE(weakBuffer.expired());

    frameContext.resourceStateTracker->RetireTransientResources(frameContext.frameIndex - 1u);
    EXPECT_FALSE(weakTexture.expired());
    EXPECT_FALSE(weakTextureView.expired());
    EXPECT_FALSE(weakBuffer.expired());

    frameContext.resourceStateTracker->RetireTransientResources(frameContext.frameIndex);
    stats = frameContext.resourceStateTracker->GetStats();
    EXPECT_EQ(stats.retiredTransientTextures, 1u);
    EXPECT_EQ(stats.retiredTransientTextureViews, 1u);
    EXPECT_EQ(stats.retiredTransientBuffers, 1u);
    EXPECT_TRUE(weakTexture.expired());
    EXPECT_TRUE(weakTextureView.expired());
    EXPECT_TRUE(weakBuffer.expired());
}
