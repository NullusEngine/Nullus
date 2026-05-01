#include <gtest/gtest.h>

#include <memory>
#include <string_view>

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/Settings/DriverSettings.h"

namespace
{
    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        std::string_view GetDebugName() const override { return "TestTexture"; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Present; }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc{};
    };

    class TestTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        TestTextureView() = default;
        explicit TestTextureView(std::shared_ptr<NLS::Render::RHI::RHITexture> texture)
            : m_texture(std::move(texture))
        {
        }

        std::string_view GetDebugName() const override { return "TestTextureView"; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        NLS::Render::RHI::RHITextureViewDesc m_desc{};
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
    };

    class TestSwapchain final : public NLS::Render::RHI::RHISwapchain
    {
    public:
        explicit TestSwapchain(std::weak_ptr<NLS::Render::RHI::RHITextureView> trackedView)
            : m_trackedView(std::move(trackedView))
        {
        }

        std::string_view GetDebugName() const override { return "TestSwapchain"; }
        const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_desc; }
        uint32_t GetImageCount() const override { return 2u; }

        std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
            const std::shared_ptr<NLS::Render::RHI::RHISemaphore>&,
            const std::shared_ptr<NLS::Render::RHI::RHIFence>&) override
        {
            return std::nullopt;
        }

        bool Resize(uint32_t width, uint32_t height) override
        {
            resizeCalled = true;
            ++resizeCalls;
            resizeWidth = width;
            resizeHeight = height;
            backbufferReleasedBeforeResize = m_trackedView.expired();
            backbufferTextureReleasedBeforeResize = m_trackedTexture.expired();
            if (!resizeResult)
                return false;
            m_desc.width = width;
            m_desc.height = height;
            return true;
        }

        void SetTrackedView(std::weak_ptr<NLS::Render::RHI::RHITextureView> trackedView)
        {
            m_trackedView = std::move(trackedView);
        }

        void SetTrackedTexture(std::weak_ptr<NLS::Render::RHI::RHITexture> trackedTexture)
        {
            m_trackedTexture = std::move(trackedTexture);
        }

        bool resizeCalled = false;
        uint32_t resizeCalls = 0u;
        bool resizeResult = true;
        bool backbufferReleasedBeforeResize = false;
        bool backbufferTextureReleasedBeforeResize = false;
        uint32_t resizeWidth = 0;
        uint32_t resizeHeight = 0;

    private:
        NLS::Render::RHI::SwapchainDesc m_desc{};
        std::weak_ptr<NLS::Render::RHI::RHITextureView> m_trackedView;
        std::weak_ptr<NLS::Render::RHI::RHITexture> m_trackedTexture;
    };

    class TestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "TestCommandBuffer"; }
        void Begin() override { recording = true; }
        void End() override { recording = false; }
        void Reset() override
        {
            ++resetCalls;
            recording = false;
        }
        bool IsRecording() const override { return recording; }
        void* GetNativeCommandBuffer() const override { return nullptr; }
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
        uint32_t resetCalls = 0u;
    };

    class TestCommandPool final : public NLS::Render::RHI::RHICommandPool
    {
    public:
        std::string_view GetDebugName() const override { return "TestCommandPool"; }
        NLS::Render::RHI::QueueType GetQueueType() const override { return NLS::Render::RHI::QueueType::Graphics; }
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string) override { return commandBuffer; }
        void Reset() override { ++resetCalls; }

        std::shared_ptr<TestCommandBuffer> commandBuffer;
        uint32_t resetCalls = 0u;
    };
}

TEST(DriverSwapchainResizeTests, ReleasesFrameContextBackbufferViewsBeforeResizingSwapchain)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;

    NLS::Render::Context::Driver driver(settings);

    auto view = std::make_shared<TestTextureView>();
    std::weak_ptr<NLS::Render::RHI::RHITextureView> weakView = view;
    auto swapchain = std::make_shared<TestSwapchain>(weakView);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.swapchainBackbufferView = view;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    view.reset();

    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    driver.ResizePlatformSwapchain(1664u, 941u);

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_EQ(swapchain->resizeWidth, 1664u);
    EXPECT_EQ(swapchain->resizeHeight, 941u);
    EXPECT_TRUE(swapchain->backbufferReleasedBeforeResize);
    EXPECT_EQ(frameContext.swapchainBackbufferView, nullptr);
    EXPECT_EQ(commandBuffer->resetCalls, 1u);
    EXPECT_EQ(commandPool->resetCalls, 1u);
}

TEST(DriverSwapchainResizeTests, ClearsTrackedBackbufferTextureStatesBeforeResizingSwapchain)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;

    NLS::Render::Context::Driver driver(settings);

    auto texture = std::make_shared<TestTexture>();
    auto view = std::make_shared<TestTextureView>(texture);
    std::weak_ptr<NLS::Render::RHI::RHITextureView> weakView = view;
    std::weak_ptr<NLS::Render::RHI::RHITexture> weakTexture = texture;
    auto swapchain = std::make_shared<TestSwapchain>(weakView);
    swapchain->SetTrackedTexture(weakTexture);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0);
    frameContext.swapchainBackbufferView = view;
    frameContext.explicitReadbackTexture = texture;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHIBarrierDesc trackedBackbufferState;
    NLS::Render::RHI::RHITextureBarrier barrier;
    barrier.texture = texture;
    barrier.after = NLS::Render::RHI::ResourceState::Present;
    trackedBackbufferState.textureBarriers.push_back(barrier);
    frameContext.resourceStateTracker->Commit(trackedBackbufferState);

    barrier.texture.reset();
    trackedBackbufferState.textureBarriers.clear();
    texture.reset();
    view.reset();

    NLS::Render::Context::DriverTestAccess::SetCompletedReadbackTexture(driver, weakTexture.lock());
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    driver.ResizePlatformSwapchain(1920u, 1080u);

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_TRUE(swapchain->backbufferReleasedBeforeResize);
    EXPECT_TRUE(swapchain->backbufferTextureReleasedBeforeResize);
    EXPECT_EQ(frameContext.swapchainBackbufferView, nullptr);
    EXPECT_EQ(frameContext.explicitReadbackTexture, nullptr);
}

TEST(DriverSwapchainResizeTests, NotifiesWillResizeAgainAtActualResizeToReleaseReacquiredUiBackbuffers)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;

    NLS::Render::Context::Driver driver(settings);

    auto heldBackbufferView = std::make_shared<TestTextureView>();
    auto swapchain = std::make_shared<TestSwapchain>(std::weak_ptr<NLS::Render::RHI::RHITextureView>(heldBackbufferView));
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    uint32_t willResizeNotifications = 0u;
    driver.SetSwapchainWillResizeCallback([&]()
    {
        ++willResizeNotifications;
        heldBackbufferView.reset();
    });

    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);
    driver.ResizePlatformSwapchain(1600u, 900u);

    EXPECT_FALSE(swapchain->resizeCalled);
    EXPECT_EQ(willResizeNotifications, 1u);

    heldBackbufferView = std::make_shared<TestTextureView>();
    swapchain->SetTrackedView(heldBackbufferView);

    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_EQ(willResizeNotifications, 2u);
    EXPECT_TRUE(swapchain->backbufferReleasedBeforeResize);
}

TEST(DriverSwapchainResizeTests, ThreadedResizeWaitsForInFlightFrameRetirementBeforeResizingSwapchain)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 17u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    auto view = std::make_shared<TestTextureView>();
    std::weak_ptr<NLS::Render::RHI::RHITextureView> weakView = view;
    auto swapchain = std::make_shared<TestSwapchain>(weakView);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0);
    frameContext.swapchainBackbufferView = view;
    view.reset();

    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    driver.ResizePlatformSwapchain(1280u, 720u);

    EXPECT_FALSE(swapchain->resizeCalled);
    EXPECT_NE(frameContext.swapchainBackbufferView, nullptr);

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    NLS::Render::Context::RenderScenePackage package;
    package.frameId = 17u;
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = 17u;
    ASSERT_TRUE(lifecycle->TryBeginRenderScene(0u));
    ASSERT_TRUE(lifecycle->CompleteRenderScene(0u, package));
    ASSERT_TRUE(lifecycle->TryBeginRhiSubmission(0u));
    ASSERT_TRUE(lifecycle->CompleteRhiSubmission(0u, submissionFrame));
    ASSERT_TRUE(lifecycle->RetireFrame(0u));

    driver.ResizePlatformSwapchain(1280u, 720u);

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_EQ(swapchain->resizeWidth, 1280u);
    EXPECT_EQ(swapchain->resizeHeight, 720u);
}

TEST(DriverSwapchainResizeTests, ThreadedResizeAppliesPendingResizeBeforeRecoveredSwapchainPublish)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot firstSnapshot;
    firstSnapshot.frameId = 23u;
    firstSnapshot.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, firstSnapshot));

    auto view = std::make_shared<TestTextureView>();
    std::weak_ptr<NLS::Render::RHI::RHITextureView> weakView = view;
    auto swapchain = std::make_shared<TestSwapchain>(weakView);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.swapchainBackbufferView = view;
    view.reset();

    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    driver.ResizePlatformSwapchain(1440u, 810u);

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    NLS::Render::Context::RenderScenePackage package;
    package.frameId = 23u;
    package.targetsSwapchain = true;
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = 23u;
    ASSERT_TRUE(lifecycle->TryBeginRenderScene(0u));
    ASSERT_TRUE(lifecycle->CompleteRenderScene(0u, package));
    ASSERT_TRUE(lifecycle->TryBeginRhiSubmission(0u));
    ASSERT_TRUE(lifecycle->CompleteRhiSubmission(0u, submissionFrame));
    ASSERT_TRUE(lifecycle->RetireFrame(0u));

    NLS::Render::Context::FrameSnapshot secondSnapshot;
    secondSnapshot.frameId = 24u;
    secondSnapshot.targetsSwapchain = true;
    EXPECT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, secondSnapshot));

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_EQ(swapchain->resizeWidth, 1440u);
    EXPECT_EQ(swapchain->resizeHeight, 810u);
    EXPECT_TRUE(swapchain->backbufferReleasedBeforeResize);
    EXPECT_EQ(frameContext.swapchainBackbufferView, nullptr);
}

TEST(DriverSwapchainResizeTests, RetriesPendingSwapchainResizeAfterBackendResizeFailure)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;

    NLS::Render::Context::Driver driver(settings);

    auto swapchain = std::make_shared<TestSwapchain>(
        std::weak_ptr<NLS::Render::RHI::RHITextureView>{});
    swapchain->resizeResult = false;

    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    driver.ResizePlatformSwapchain(1483u, 9003u);

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_EQ(swapchain->resizeCalls, 1u);
    EXPECT_EQ(swapchain->GetDesc().width, 0u);
    EXPECT_EQ(swapchain->GetDesc().height, 0u);

    swapchain->resizeResult = true;
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(swapchain->resizeCalls, 2u);
    EXPECT_EQ(swapchain->resizeWidth, 1483u);
    EXPECT_EQ(swapchain->resizeHeight, 9003u);
    EXPECT_EQ(swapchain->GetDesc().width, 1483u);
    EXPECT_EQ(swapchain->GetDesc().height, 9003u);
}
