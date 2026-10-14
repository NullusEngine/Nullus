#include <gtest/gtest.h>

#include <memory>
#include <future>
#include <atomic>
#include <stdexcept>
#include <string_view>

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderCommandPipe.h"
#include "Rendering/Context/RenderingThreadSystem.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/Context/SwapchainResizePolicy.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Tests/RenderingThreadTestHelpers.h"

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

        void SetTrackedTexture(std::weak_ptr<NLS::Render::RHI::RHITexture> trackedTexture)
        {
            m_trackedTexture = std::move(trackedTexture);
        }

        std::atomic_bool resizeCalled{ false };
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

    class BlockingGpuFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        std::string_view GetDebugName() const override { return "ResizeGpuFence"; }
        bool IsSignaled() const override { return signaled.load(); }
        void Reset() override { signaled = false; }
        bool Wait(uint64_t timeoutNanoseconds) override
        {
            waitStarted.Signal();
            if (!release.WaitFor(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::nanoseconds(timeoutNanoseconds))))
                return false;
            signaled = true;
            return true;
        }
        NLS::Render::Context::RenderCommandEvent waitStarted;
        NLS::Render::Context::RenderCommandEvent release;
        std::atomic_bool signaled{ false };
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
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

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
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_TRUE(swapchain->backbufferReleasedBeforeResize);
    EXPECT_TRUE(swapchain->backbufferTextureReleasedBeforeResize);
    EXPECT_EQ(frameContext.swapchainBackbufferView, nullptr);
    EXPECT_EQ(frameContext.explicitReadbackTexture, nullptr);
}

TEST(DriverSwapchainResizeTests, CanInvalidateSpecificCompletedReadbackTexture)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;

    NLS::Render::Context::Driver driver(settings);
    auto firstTexture = std::make_shared<TestTexture>();
    auto secondTexture = std::make_shared<TestTexture>();

    NLS::Render::Context::DriverTestAccess::SetCompletedReadbackTexture(driver, firstTexture);
    NLS::Render::Context::DriverTestAccess::SetCompletedReadbackTexture(driver, secondTexture);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(driver, firstTexture));
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(driver, secondTexture));

    NLS::Render::Context::DriverRendererAccess::InvalidateCompletedReadbackTexture(driver, firstTexture);

    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(driver, firstTexture));
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(driver, secondTexture));
}

TEST(DriverSwapchainResizeTests, CompletedReadbackGenerationDoesNotPromoteLateOldSubmission)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;

    NLS::Render::Context::Driver driver(settings);
    auto texture = std::make_shared<TestTexture>();

    const uint64_t oldGeneration =
        NLS::Render::Context::DriverRendererAccess::BeginReadbackTextureSubmission(driver, texture);
    const uint64_t newGeneration =
        NLS::Render::Context::DriverRendererAccess::BeginReadbackTextureSubmission(driver, texture);
    ASSERT_NE(oldGeneration, 0u);
    ASSERT_NE(newGeneration, 0u);
    ASSERT_NE(oldGeneration, newGeneration);

    NLS::Render::Context::DriverTestAccess::SetCompletedReadbackTexture(driver, texture);

    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
        driver,
        texture,
        oldGeneration));
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
        driver,
        texture,
        newGeneration));

    NLS::Render::Context::DriverTestAccess::SetCompletedReadbackTexture(driver, texture);

    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
        driver,
        texture,
        newGeneration));
}

TEST(DriverSwapchainResizeTests, CompletedReadbackGenerationUsesExplicitFrameTokenWhenProvided)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;

    NLS::Render::Context::Driver driver(settings);
    auto texture = std::make_shared<TestTexture>();

    const uint64_t oldGeneration =
        NLS::Render::Context::DriverRendererAccess::BeginReadbackTextureSubmission(driver, texture);
    const uint64_t newGeneration =
        NLS::Render::Context::DriverRendererAccess::BeginReadbackTextureSubmission(driver, texture);
    ASSERT_NE(oldGeneration, newGeneration);

    NLS::Render::Context::DriverTestAccess::SetCompletedReadbackTexture(
        driver,
        texture,
        newGeneration);

    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
        driver,
        texture,
        oldGeneration));
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
        driver,
        texture,
        newGeneration));
}

TEST(DriverSwapchainResizeTests, NotifiesWillResizeOnlyWhenResizeActuallyApplies)
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

    NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    NLS::Tests::RunOnRenderThread([&]
    {
        const NLS::Render::Context::ScopedFrameResourceContext frameResources(driver, 0u);
        driver.ResizePlatformSwapchain(1600u, 900u);
        EXPECT_FALSE(swapchain->resizeCalled);
        EXPECT_EQ(willResizeNotifications, 0u);
    });
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_EQ(willResizeNotifications, 1u);
    EXPECT_TRUE(swapchain->backbufferReleasedBeforeResize);
}

TEST(DriverSwapchainResizeTests, IgnoresResizeRequestsMatchingCurrentSwapchainSize)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;

    NLS::Render::Context::Driver driver(settings);

    auto swapchain = std::make_shared<TestSwapchain>(
        std::weak_ptr<NLS::Render::RHI::RHITextureView>{});
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    driver.ResizePlatformSwapchain(1600u, 900u);

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_EQ(swapchain->resizeCalls, 1u);

    driver.ResizePlatformSwapchain(1600u, 900u);

    EXPECT_EQ(swapchain->resizeCalls, 1u);
    EXPECT_EQ(swapchain->resizeWidth, 1600u);
    EXPECT_EQ(swapchain->resizeHeight, 900u);
}

TEST(DriverSwapchainResizeTests, ThreadedResizeWaitsForInFlightFrameRetirementBeforeResizingSwapchain)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 17u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot,
        [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));

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

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));

    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
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
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::RenderFramePacket firstSnapshot;
    firstSnapshot.frameId = 23u;
    firstSnapshot.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, firstSnapshot,
        [firstSnapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(firstSnapshot); }));

    auto view = std::make_shared<TestTextureView>();
    std::weak_ptr<NLS::Render::RHI::RHITextureView> weakView = view;
    auto swapchain = std::make_shared<TestSwapchain>(weakView);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.swapchainBackbufferView = view;
    view.reset();

    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    driver.ResizePlatformSwapchain(1440u, 810u);

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));

    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::RenderFramePacket secondSnapshot;
    secondSnapshot.frameId = 24u;
    secondSnapshot.targetsSwapchain = true;
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, secondSnapshot,
        [secondSnapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(secondSnapshot); }));

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_EQ(swapchain->resizeWidth, 1440u);
    EXPECT_EQ(swapchain->resizeHeight, 810u);
    EXPECT_TRUE(swapchain->backbufferReleasedBeforeResize);
    EXPECT_EQ(frameContext.swapchainBackbufferView, nullptr);
}

TEST(DriverSwapchainResizeTests, PendingResizePausesOffscreenProducersAndRecoversAtLatestSize)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto swapchain = std::make_shared<TestSwapchain>(std::weak_ptr<RHI::RHITextureView> {});
    Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    Context::RenderFramePacket packet;
    packet.frameId = 100u;
    ASSERT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, packet,
        [packet](size_t) { return Context::BuildRenderFramePackage(packet); }));

    driver.ResizePlatformSwapchain(1280u, 720u);
    driver.ResizePlatformSwapchain(1600u, 900u);
    EXPECT_FALSE(swapchain->resizeCalled);
    uint64_t frameId = 1u;
    EXPECT_FALSE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, packet,
        [packet](size_t) { return Context::BuildRenderFramePackage(packet); }, nullptr, &frameId));
    EXPECT_EQ(frameId, 0u);
    EXPECT_FALSE(Context::RenderThreadCoordinator::BeginRendererFrame(driver, false, false));
    EXPECT_EQ(Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, 1u);

    ASSERT_TRUE(Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    packet.frameId = 101u;
    ASSERT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, packet,
        [packet](size_t) { return Context::BuildRenderFramePackage(packet); }));
    EXPECT_EQ(swapchain->resizeCalls, 1u);
    EXPECT_EQ(swapchain->GetDesc().width, 1600u);
    EXPECT_EQ(swapchain->GetDesc().height, 900u);
}

TEST(DriverSwapchainResizeTests, ThreadedDrainJoinsQueuedResizeBeforeReturning)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    bool resizedOnRenderThread = false;
    NLS::Render::Context::Driver driver(settings);
    auto swapchain = std::make_shared<TestSwapchain>(std::weak_ptr<NLS::Render::RHI::RHITextureView> {});
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    driver.SetSwapchainWillResizeCallback([&resizedOnRenderThread]
    {
        resizedOnRenderThread = NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread();
    });
    NLS::Render::Context::RenderCommandEvent resizeStarted;
    NLS::Render::Context::RenderCommandEvent allowResize;
    NLS::Render::Context::RenderCommandDispatcher::Enqueue([&driver, resizeStarted, allowResize]
    {
        resizeStarted.Signal();
        allowResize.Wait();
        driver.ResizePlatformSwapchain(1600u, 900u);
    });
    const bool started = resizeStarted.WaitFor(std::chrono::seconds(5));
    if (!started)
        allowResize.Signal();
    ASSERT_TRUE(started);
    auto drain = std::async(std::launch::async, [&driver]
    {
        return NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver);
    });
    const auto beforeResize = drain.wait_for(std::chrono::milliseconds(50));
    allowResize.Signal();
    EXPECT_EQ(beforeResize, std::future_status::timeout)
        << "Drain must not return while the queued resize is still blocked.";
    ASSERT_TRUE(drain.get());
    EXPECT_TRUE(resizedOnRenderThread);
    EXPECT_EQ(swapchain->resizeCalls, 1u);
    EXPECT_EQ(swapchain->GetDesc().width, 1600u);
    EXPECT_EQ(swapchain->GetDesc().height, 900u);
}

TEST(DriverSwapchainResizeTests, WindowResizeExecutesAfterEarlierMainPipeCommands)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    auto swapchain = std::make_shared<TestSwapchain>(std::weak_ptr<RHI::RHITextureView> {});
    Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    std::atomic_bool resizedOnRenderThread = false;
    driver.SetSwapchainWillResizeCallback([&]
    {
        resizedOnRenderThread = Context::RenderingThreadSystem::IsInRenderingThread();
    });

    Context::RenderCommandEvent earlierStarted;
    Context::RenderCommandEvent releaseEarlier;
    Context::EnqueueRenderCommand<struct EarlierResizeCommand>([=]
    {
        earlierStarted.Signal();
        releaseEarlier.Wait();
    });
    const bool started = earlierStarted.WaitFor(std::chrono::seconds(5));
    if (!started)
        releaseEarlier.Signal();
    ASSERT_TRUE(started);
    auto resize = std::async(std::launch::async, [&] { driver.ResizePlatformSwapchain(1420u, 840u); });
    const auto beforeEarlierFinished = resize.wait_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(swapchain->resizeCalled);
    releaseEarlier.Signal();
    EXPECT_EQ(beforeEarlierFinished, std::future_status::timeout);
    resize.get();
    EXPECT_TRUE(resizedOnRenderThread);
    EXPECT_EQ(swapchain->resizeCalls, 1u);
    EXPECT_EQ(swapchain->GetDesc().width, 1420u);
    EXPECT_EQ(swapchain->GetDesc().height, 840u);
}

TEST(DriverSwapchainResizeTests, ResizeFlushesEarlierPresentBeforeRebuildingSwapchain)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    auto swapchain = std::make_shared<TestSwapchain>(std::weak_ptr<RHI::RHITextureView> {});
    Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    std::atomic_bool presentFinished = false;
    std::atomic_bool callbackCalled = false;
    driver.SetSwapchainWillResizeCallback([&]
    {
        EXPECT_TRUE(presentFinished.load());
        EXPECT_TRUE(Context::RenderingThreadSystem::IsInRenderingThread());
        callbackCalled = true;
    });
    Context::RenderCommandEvent presentStarted;
    Context::RenderCommandEvent releasePresent;
    auto& executor = Context::RhiCommandListExecutor::Get();
    auto present = std::async(std::launch::async, [&]
    {
        return executor.RunOnSubmitLane("BlockedPresentBeforeResize", [&, presentStarted, releasePresent]
        {
            presentStarted.Signal();
            releasePresent.Wait();
            presentFinished = true;
            return RHI::RHIQueueOperationResult {};
        }, true);
    });
    const bool started = presentStarted.WaitFor(std::chrono::seconds(5));
    if (!started)
        releasePresent.Signal();
    ASSERT_TRUE(started);
    auto resize = std::async(std::launch::async, [&] { driver.ResizePlatformSwapchain(1720u, 960u); });
    const auto beforePresentFinished = resize.wait_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(callbackCalled.load());
    releasePresent.Signal();
    EXPECT_EQ(beforePresentFinished, std::future_status::timeout);
    ASSERT_TRUE(present.get().Succeeded());
    resize.get();
    EXPECT_TRUE(callbackCalled.load());
    EXPECT_EQ(swapchain->resizeCalls, 1u);
    EXPECT_EQ(executor.GetPresentSequence(), executor.GetPresentRequestSequence());
}

TEST(DriverSwapchainResizeTests, ClearingResizeCallbackJoinsItsActiveInvocation)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    auto swapchain = std::make_shared<TestSwapchain>(std::weak_ptr<RHI::RHITextureView> {});
    Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    Context::RenderCommandEvent callbackStarted;
    Context::RenderCommandEvent releaseCallback;
    std::atomic_uint32_t calls = 0u;
    driver.SetSwapchainWillResizeCallback([&]
    {
        ++calls;
        callbackStarted.Signal();
        releaseCallback.Wait();
    });
    auto resize = std::async(std::launch::async, [&] { driver.ResizePlatformSwapchain(1440u, 810u); });
    const bool started = callbackStarted.WaitFor(std::chrono::seconds(5));
    if (!started)
        releaseCallback.Signal();
    ASSERT_TRUE(started);
    auto clear = std::async(std::launch::async, [&] { driver.SetSwapchainWillResizeCallback(nullptr); });
    const auto beforeCallbackFinished = clear.wait_for(std::chrono::milliseconds(50));
    releaseCallback.Signal();
    EXPECT_EQ(beforeCallbackFinished, std::future_status::timeout);
    resize.get();
    clear.get();
    driver.ResizePlatformSwapchain(1500u, 850u);
    EXPECT_EQ(calls.load(), 1u);
    EXPECT_EQ(swapchain->resizeCalls, 2u);
}

TEST(DriverSwapchainResizeTests, ResizeCallbackCanClearItself)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    auto swapchain = std::make_shared<TestSwapchain>(std::weak_ptr<RHI::RHITextureView> {});
    Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    uint32_t calls = 0u;
    driver.SetSwapchainWillResizeCallback([&]
    {
        ++calls;
        driver.SetSwapchainWillResizeCallback(nullptr);
    });
    driver.ResizePlatformSwapchain(1440u, 810u);
    driver.ResizePlatformSwapchain(1500u, 850u);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(swapchain->resizeCalls, 2u);
}

TEST(DriverSwapchainResizeTests, FailedPresentPreventsResizeAndAllocatorReset)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    {
        Context::Driver driver(settings);
        auto swapchain = std::make_shared<TestSwapchain>(std::weak_ptr<RHI::RHITextureView> {});
        Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
        auto commandBuffer = std::make_shared<TestCommandBuffer>();
        auto& frame = Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
        frame.commandBuffer = commandBuffer;
        auto& executor = Context::RhiCommandListExecutor::Get();
        const auto failed = executor.RunOnSubmitLane("FailedPresentBeforeResize", []
        {
            RHI::RHIQueueOperationResult result;
            result.code = RHI::RHIQueueOperationStatusCode::BackendFailure;
            result.message = "Present rejected during resize test";
            return result;
        }, true);
        ASSERT_FALSE(failed.Succeeded());
        EXPECT_THROW(driver.ResizePlatformSwapchain(1440u, 810u), std::runtime_error);
        EXPECT_FALSE(swapchain->resizeCalled);
        EXPECT_EQ(commandBuffer->resetCalls, 0u);
        EXPECT_TRUE(Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).unsafeGpuWorkQuarantined);
    }
    Context::ResetRenderFatalErrorForTesting();
    Context::RhiCommandListExecutor::Get().ResetForTesting();
}

TEST(DriverSwapchainResizeTests, RhiSubmitCallbackDefersResizeUntilItsOwnSubmissionCompletes)
{
    using namespace NLS::Render;
    for (const auto mode : {Settings::RhiThreadModePreference::ForceNone,
        Settings::RhiThreadModePreference::ForceDedicatedThread})
    {
        Settings::DriverSettings settings;
        settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.rhiThreadMode = mode;
        Context::Driver driver(settings);
        auto swapchain = std::make_shared<TestSwapchain>(std::weak_ptr<RHI::RHITextureView> {});
        Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
        driver.SetSwapchainWillResizeCallback([]
        {
            EXPECT_TRUE(Context::RenderingThreadSystem::IsInRenderingThread());
            EXPECT_FALSE(Context::RenderingThreadSystem::IsInRhiThread());
        });
        const auto submit = Context::EnqueueRenderCommand<struct ResizeFromSubmitCommand>([&]
        {
            const auto result = Context::RhiCommandListExecutor::Get().RunOnSubmitLane("ResizeDuringSubmit", [&]
            {
                EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
                driver.ResizePlatformSwapchain(1440u, 810u);
                EXPECT_FALSE(swapchain->resizeCalled.load());
                return RHI::RHIQueueOperationResult {};
            }, true);
            EXPECT_TRUE(result.Succeeded()) << result.message;
        });
        ASSERT_TRUE(submit.WaitFor(std::chrono::seconds(5)));
        ASSERT_TRUE(Context::DriverRendererAccess::TryDrainThreadedRendering(driver));
        EXPECT_EQ(swapchain->resizeCalls, 1u);
        EXPECT_EQ(swapchain->GetDesc().width, 1440u);
    }
}

TEST(DriverSwapchainResizeTests, GpuFenceCompletesBeforeAllocatorResetAndResize)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    auto swapchain = std::make_shared<TestSwapchain>(std::weak_ptr<RHI::RHITextureView> {});
    Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    auto fence = std::make_shared<BlockingGpuFence>();
    auto buffer = std::make_shared<TestCommandBuffer>();
    auto pool = std::make_shared<TestCommandPool>();
    auto& frame = Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frame.frameFence = fence;
    frame.commandBuffer = buffer;
    frame.commandPool = pool;
    Context::DriverTestAccess::GetImplForTesting(driver)->frameResourceSlots->RetireSlot(0u, fence);
    auto resize = std::async(std::launch::async, [&] { driver.ResizePlatformSwapchain(1440u, 810u); });
    const bool waiting = fence->waitStarted.WaitFor(std::chrono::seconds(5));
    EXPECT_FALSE(swapchain->resizeCalled.load());
    EXPECT_EQ(buffer->resetCalls, 0u);
    EXPECT_EQ(pool->resetCalls, 0u);
    fence->release.Signal();
    ASSERT_TRUE(waiting);
    resize.get();
    EXPECT_TRUE(fence->IsSignaled());
    EXPECT_EQ(buffer->resetCalls, 1u);
    EXPECT_EQ(pool->resetCalls, 1u);
    EXPECT_EQ(swapchain->resizeCalls, 1u);
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
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_EQ(swapchain->resizeCalls, 2u);
    EXPECT_EQ(swapchain->GetDesc().width, 0u);
    EXPECT_EQ(swapchain->GetDesc().height, 0u);

    swapchain->resizeResult = true;
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(swapchain->resizeCalls, 3u);
    EXPECT_EQ(swapchain->resizeWidth, 1483u);
    EXPECT_EQ(swapchain->resizeHeight, 9003u);
    EXPECT_EQ(swapchain->GetDesc().width, 1483u);
    EXPECT_EQ(swapchain->GetDesc().height, 9003u);
}
