#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <type_traits>

#include "Core/ServiceLocator.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/RHI/Utils/RHIUIBridge.h"
#include "Rendering/RHI/Utils/RHIUIBridgeInternal.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Tooling/RenderDocCaptureController.h"
#include "Rendering/Tooling/RenderDocEnvironment.h"
#include "UI/UIManager.h"
#include "UI/Widgets/Buttons/ButtonImage.h"
#include "UI/Widgets/Visual/Image.h"
#include "Windowing/Context/Device.h"
#include "Windowing/Settings/WindowSettings.h"
#include "Windowing/Window.h"

namespace
{
    template<typename T, typename = void>
    struct HasDevicePrepareUIRender : std::false_type
    {
    };

    template<typename T>
    struct HasDevicePrepareUIRender<T, std::void_t<decltype(std::declval<T&>().PrepareUIRender())>>
        : std::true_type
    {
    };

    template<typename T, typename = void>
    struct HasDeviceReleaseUITextureHandles : std::false_type
    {
    };

    template<typename T>
    struct HasDeviceReleaseUITextureHandles<T, std::void_t<decltype(std::declval<T&>().ReleaseUITextureHandles())>>
        : std::true_type
    {
    };

    template<typename T, typename = void>
    struct HasDeviceSetCurrentCommandBuffer : std::false_type
    {
    };

    template<typename T>
    struct HasDeviceSetCurrentCommandBuffer<T, std::void_t<decltype(std::declval<T&>().SetCurrentCommandBuffer(NLS::Render::RHI::NativeHandle{}))>>
        : std::true_type
    {
    };

    struct ImGuiContextGuard
    {
        ImGuiContextGuard()
        {
            IMGUI_CHECKVERSION();
            context = ImGui::CreateContext();
            ImGui::GetIO().DisplaySize = ImVec2(320.0f, 200.0f);
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            ImGui::GetIO().Fonts->TexID = reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(0x1));
        }

        ~ImGuiContextGuard()
        {
            ImGui::DestroyContext(context);
        }

        ImGuiContext* context = nullptr;
    };

    bool CanCreateHeadlessGlfwWindow()
    {
#if defined(_WIN32) || defined(__APPLE__)
        return true;
#else
        return std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
#endif
    }

    class TestUiTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        std::string_view GetDebugName() const override { return "TestUiTexture"; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return desc; }
        NLS::Render::RHI::ResourceState GetState() const override
        {
            return NLS::Render::RHI::ResourceState::ShaderRead;
        }

        NLS::Render::RHI::RHITextureDesc desc {};
    };

    class TestUiTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        explicit TestUiTextureView(std::shared_ptr<NLS::Render::RHI::RHITexture> texture)
            : m_texture(std::move(texture))
        {
        }

        std::string_view GetDebugName() const override { return "TestUiTextureView"; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
        NLS::Render::RHI::RHITextureViewDesc m_desc {};
    };

    class TestUiSampler final : public NLS::Render::RHI::RHISampler
    {
    public:
        std::string_view GetDebugName() const override { return "TestUiSampler"; }
        const NLS::Render::RHI::SamplerDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::SamplerDesc m_desc {};
    };

    class TestUiBindingSet final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        std::string_view GetDebugName() const override { return "TestUiBindingSet"; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingSetDesc m_desc {};
    };

    class TestUiAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "TestUiAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override
        {
            return NLS::Render::RHI::NativeBackendType::DX12;
        }
        std::string_view GetVendor() const override { return "NullusTest"; }
        std::string_view GetHardware() const override { return "UIOverlayTestDevice"; }
    };

    class TestUiOverlayCapabilityDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        TestUiOverlayCapabilityDevice()
            : m_adapter(std::make_shared<TestUiAdapter>())
        {
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Graphics, true);
            m_capabilities.SetFeature(
                NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
                true,
                "test overlay support");
        }

        std::string_view GetDebugName() const override { return "TestUiOverlayCapabilityDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override
        {
            NLS::Render::RHI::NativeRenderDeviceInfo info;
            info.backend = NLS::Render::RHI::NativeBackendType::DX12;
            return info;
        }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(
            const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc&,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override { return nullptr; }
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
            const NLS::Render::RHI::RHIBindingLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(
            const NLS::Render::RHI::RHIBindingSetDesc&) override { return nullptr; }
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

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities;
    };

    int CountDrawElementsWithTextureId(const ImDrawList& commandList, ImTextureID textureId)
    {
        int elementCount = 0;
        for (int commandIndex = 0; commandIndex < commandList.CmdBuffer.Size; ++commandIndex)
        {
            const ImDrawCmd& command = commandList.CmdBuffer[commandIndex];
            if (command.GetTexID() == textureId && command.ElemCount > 0)
                elementCount += static_cast<int>(command.ElemCount);
        }

        return elementCount;
    }
}

TEST(UIAndToolingBackendAwarenessTests, ResolvesImGuiGlfwInitBackendByGraphicsBackend)
{
    using NLS::Render::Settings::EGraphicsBackend;
    using NLS::UI::ImGuiGlfwInitBackend;

    const auto expectedOpenGlBackend =
        NLS::Render::Settings::SupportsImGuiRendererBackend(EGraphicsBackend::OPENGL)
        ? ImGuiGlfwInitBackend::OpenGL
        : ImGuiGlfwInitBackend::Other;

    EXPECT_EQ(NLS::UI::ResolveImGuiGlfwInitBackend(EGraphicsBackend::OPENGL), expectedOpenGlBackend);
    EXPECT_EQ(NLS::UI::ResolveImGuiGlfwInitBackend(EGraphicsBackend::VULKAN), ImGuiGlfwInitBackend::Vulkan);
    EXPECT_EQ(NLS::UI::ResolveImGuiGlfwInitBackend(EGraphicsBackend::DX12), ImGuiGlfwInitBackend::Other);
    EXPECT_EQ(NLS::UI::ResolveImGuiGlfwInitBackend(EGraphicsBackend::DX11), ImGuiGlfwInitBackend::Other);
    EXPECT_EQ(NLS::UI::ResolveImGuiGlfwInitBackend(EGraphicsBackend::METAL), ImGuiGlfwInitBackend::Other);
    EXPECT_EQ(NLS::UI::ResolveImGuiGlfwInitBackend(EGraphicsBackend::NONE), ImGuiGlfwInitBackend::Other);
}

TEST(UIAndToolingBackendAwarenessTests, ResolvesRenderDocCaptureDeviceByNativeBackend)
{
    NLS::Render::RHI::NativeRenderDeviceInfo info;
    void* vulkanDispatchPointer = reinterpret_cast<void*>(0x33);
    info.device = reinterpret_cast<void*>(0x11);
    info.graphicsQueue = reinterpret_cast<void*>(0x22);
    info.instance = &vulkanDispatchPointer;

    info.backend = NLS::Render::RHI::NativeBackendType::DX12;
    EXPECT_EQ(NLS::Render::Tooling::ResolveRenderDocCaptureDevice(info), info.device);

    info.backend = NLS::Render::RHI::NativeBackendType::Vulkan;
#if defined(_WIN32)
    EXPECT_EQ(NLS::Render::Tooling::ResolveRenderDocCaptureDevice(info), vulkanDispatchPointer);
#else
    EXPECT_EQ(NLS::Render::Tooling::ResolveRenderDocCaptureDevice(info), info.device);
#endif

    info.backend = NLS::Render::RHI::NativeBackendType::DX11;
    EXPECT_EQ(NLS::Render::Tooling::ResolveRenderDocCaptureDevice(info), info.device);

    info.backend = NLS::Render::RHI::NativeBackendType::OpenGL;
    EXPECT_EQ(NLS::Render::Tooling::ResolveRenderDocCaptureDevice(info), info.device);
}

TEST(UIAndToolingBackendAwarenessTests, NativeRenderDeviceInfoExposesTaggedHandlesForToolingBoundaries)
{
    NLS::Render::RHI::NativeRenderDeviceInfo info;
    info.backend = NLS::Render::RHI::NativeBackendType::DX12;
    info.device = reinterpret_cast<void*>(0x11);
    info.graphicsQueue = reinterpret_cast<void*>(0x22);
    info.currentCommandBuffer = reinterpret_cast<void*>(0x33);

    const auto deviceHandle = info.GetDeviceHandle();
    const auto graphicsQueueHandle = info.GetGraphicsQueueHandle();
    const auto commandBufferHandle = info.GetCurrentCommandBufferHandle();

    EXPECT_EQ(deviceHandle.backend, NLS::Render::RHI::NativeRenderDeviceHandleKind::DX12);
    EXPECT_EQ(deviceHandle.handle, info.device);
    EXPECT_EQ(graphicsQueueHandle.backend, NLS::Render::RHI::NativeRenderDeviceHandleKind::DX12);
    EXPECT_EQ(graphicsQueueHandle.handle, info.graphicsQueue);
    EXPECT_EQ(commandBufferHandle.backend, NLS::Render::RHI::NativeRenderDeviceHandleKind::DX12);
    EXPECT_EQ(commandBufferHandle.handle, info.currentCommandBuffer);

    EXPECT_EQ(NLS::Render::Tooling::ResolveRenderDocCaptureDeviceHandle(info).backend, NLS::Render::RHI::NativeRenderDeviceHandleKind::DX12);
    EXPECT_EQ(NLS::Render::Tooling::ResolveRenderDocCaptureDeviceHandle(info).handle, info.device);
}

TEST(UIAndToolingBackendAwarenessTests, QueuedRenderDocStartupCaptureUsesExplicitFrameBoundary)
{
    using NLS::Render::Tooling::CanQueueRenderDocCapture;
    using NLS::Render::Tooling::RenderDocQueuedCaptureAction;
    using NLS::Render::Tooling::ResolveRenderDocQueuedCaptureInitialCountdown;
    using NLS::Render::Tooling::ResolveRenderDocQueuedCapturePreFrameAction;

    EXPECT_EQ(ResolveRenderDocQueuedCaptureInitialCountdown(), 2u);
    EXPECT_EQ(ResolveRenderDocQueuedCaptureInitialCountdown(true), 1u);
    EXPECT_TRUE(CanQueueRenderDocCapture(true, false, false, false, false));
    EXPECT_FALSE(CanQueueRenderDocCapture(false, false, false, false, false));
    EXPECT_FALSE(CanQueueRenderDocCapture(true, true, false, false, false));
    EXPECT_FALSE(CanQueueRenderDocCapture(true, false, true, false, false));
    EXPECT_FALSE(CanQueueRenderDocCapture(true, false, false, true, false));
    EXPECT_FALSE(CanQueueRenderDocCapture(true, false, false, false, true));

    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(false, true, true, false, false, false, 1u),
        RenderDocQueuedCaptureAction::None);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, false, true, false, false, false, 1u),
        RenderDocQueuedCaptureAction::None);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, false, false, false, false, 2u),
        RenderDocQueuedCaptureAction::None);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, true, false, false, false, 2u),
        RenderDocQueuedCaptureAction::WaitForFutureFrame);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, false, false, false, false, 1u),
        RenderDocQueuedCaptureAction::StartExplicitFrameCapture);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, true, false, false, false, 1u),
        RenderDocQueuedCaptureAction::StartExplicitFrameCapture);
}

TEST(UIAndToolingBackendAwarenessTests, QueuedRenderDocCaptureForcesStaticViewRenderUntilCaptureStarts)
{
    using NLS::Render::Tooling::ShouldForceRenderDocCaptureFrameRender;

    EXPECT_FALSE(ShouldForceRenderDocCaptureFrameRender(true, true, false, false, false, false, false, 300u));
    EXPECT_FALSE(ShouldForceRenderDocCaptureFrameRender(true, true, false, false, false, false, false, 2u));
    EXPECT_TRUE(ShouldForceRenderDocCaptureFrameRender(true, true, false, false, false, false, false, 1u));
    EXPECT_FALSE(ShouldForceRenderDocCaptureFrameRender(false, true, false, false, false, false, false, 1u));
    EXPECT_FALSE(ShouldForceRenderDocCaptureFrameRender(true, false, false, false, false, false, false, 1u));
    EXPECT_FALSE(ShouldForceRenderDocCaptureFrameRender(true, true, true, false, false, false, false, 1u));
    EXPECT_FALSE(ShouldForceRenderDocCaptureFrameRender(true, true, false, true, false, false, false, 1u));
    EXPECT_FALSE(ShouldForceRenderDocCaptureFrameRender(true, true, false, false, true, false, false, 1u));
    EXPECT_FALSE(ShouldForceRenderDocCaptureFrameRender(true, true, false, false, false, true, false, 1u));
    EXPECT_TRUE(ShouldForceRenderDocCaptureFrameRender(true, true, false, false, false, true, true, 1u));
}

TEST(UIAndToolingBackendAwarenessTests, QueuedRenderDocCaptureStartsOnExternalSceneOutputBeforeUiOverlay)
{
    using NLS::Render::Tooling::RenderDocQueuedCaptureAction;
    using NLS::Render::Tooling::ResolveRenderDocQueuedCapturePreFrameAction;

    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, false, true, false, false, 2u),
        RenderDocQueuedCaptureAction::WaitForFutureFrame);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, true, false, true, false, 1u),
        RenderDocQueuedCaptureAction::None);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, false, false, true, false, 1u),
        RenderDocQueuedCaptureAction::None);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, false, true, true, false, 1u),
        RenderDocQueuedCaptureAction::None);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, false, true, false, false, 1u),
        RenderDocQueuedCaptureAction::StartExplicitFrameCapture);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, true, false, true, true, 1u),
        RenderDocQueuedCaptureAction::None);
    EXPECT_EQ(
        ResolveRenderDocQueuedCapturePreFrameAction(true, true, false, true, true, true, 1u),
        RenderDocQueuedCaptureAction::StartExplicitFrameCapture);
}

TEST(UIAndToolingBackendAwarenessTests, RenderDocDefaultCaptureDirectoryUsesProjectLogs)
{
    const std::filesystem::path projectRoot = std::filesystem::path(NLS_ROOT_DIR) / "TestProject";
    const std::filesystem::path projectFile = projectRoot / "TestProject.nullus";

    EXPECT_EQ(
        std::filesystem::path(NLS::Render::Tooling::ResolveRenderDocDefaultCaptureDirectory(projectRoot, "Editor")),
        projectRoot / "Logs" / "RenderDoc" / "Editor");
    EXPECT_EQ(
        std::filesystem::path(NLS::Render::Tooling::ResolveRenderDocDefaultCaptureDirectory(projectFile, "Game")),
        projectRoot / "Logs" / "RenderDoc" / "Game");
}

TEST(UIAndToolingBackendAwarenessTests, OffscreenRenderDocCaptureEndWaitsForPathAndUsesFallbackOnlyOnFailure)
{
    const std::filesystem::path controllerPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Tooling/RenderDocCaptureController.cpp";

    std::ifstream stream(controllerPath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    const auto onPostFrame = source.find("void RenderDocCaptureController::OnPostFrame");
    ASSERT_NE(onPostFrame, std::string::npos);
    const auto getLatestCapturePath = source.find("std::string RenderDocCaptureController::GetLatestCapturePath", onPostFrame);
    ASSERT_NE(getLatestCapturePath, std::string::npos);
    const auto body = source.substr(onPostFrame, getLatestCapturePath - onPostFrame);

    EXPECT_NE(body.find("EndFrameCapture(m_impl->captureDevice, m_impl->captureWindow)"), std::string::npos);
    EXPECT_NE(body.find("WaitForEndedCapturePath("), std::string::npos);
    const auto waitForEndedCapture = body.find("WaitForEndedCapturePath(");
    const auto triggerFallback = body.find("TriggerCaptureFallback(");
    ASSERT_NE(waitForEndedCapture, std::string::npos);
    ASSERT_NE(triggerFallback, std::string::npos);
    EXPECT_LT(waitForEndedCapture, triggerFallback);
    EXPECT_NE(body.find("m_impl->queuedCaptureActive = false;"), std::string::npos);
}

TEST(UIAndToolingBackendAwarenessTests, OffscreenRenderDocTriggeredFallbackCanResolveWithoutPresent)
{
    const std::filesystem::path controllerPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Tooling/RenderDocCaptureController.cpp";

    std::ifstream stream(controllerPath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto helper = source.find("ResolveTriggeredCaptureIfAvailable");
    ASSERT_NE(helper, std::string::npos);
    const auto onPostFrame = source.find("void RenderDocCaptureController::OnPostFrame");
    ASSERT_NE(onPostFrame, std::string::npos);
    const auto getLatestCapturePath = source.find("std::string RenderDocCaptureController::GetLatestCapturePath", onPostFrame);
    ASSERT_NE(getLatestCapturePath, std::string::npos);
    const auto body = source.substr(onPostFrame, getLatestCapturePath - onPostFrame);

    const auto fallbackPoll = body.find("ResolveTriggeredCaptureIfAvailable(\"after post-frame\")");
    const auto queuedCaptureCheck = body.find("!m_impl->queuedCaptureActive");
    ASSERT_NE(fallbackPoll, std::string::npos);
    ASSERT_NE(queuedCaptureCheck, std::string::npos);
    EXPECT_LT(fallbackPoll, queuedCaptureCheck);
}

TEST(UIAndToolingBackendAwarenessTests, RhiDeviceBaseDoesNotExposeDefaultUiBridgeHooks)
{
    EXPECT_FALSE(HasDevicePrepareUIRender<NLS::Render::RHI::RHIDevice>::value);
    EXPECT_FALSE(HasDeviceReleaseUITextureHandles<NLS::Render::RHI::RHIDevice>::value);
    EXPECT_FALSE(HasDeviceSetCurrentCommandBuffer<NLS::Render::RHI::RHIDevice>::value);
}

TEST(UIAndToolingBackendAwarenessTests, NullRendererBridgeAcceptsTransientTextureRetirement)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;
    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    NLS::Render::RHI::NativeRenderDeviceInfo nativeInfo;
    nativeInfo.backend = NLS::Render::RHI::NativeBackendType::DX11;

    const auto bridge = NLS::Render::RHI::CreateRHIUIBridge(nullptr, "#version 150", &nativeInfo);

    ASSERT_NE(bridge, nullptr);
    EXPECT_NO_THROW(bridge->RetireTextureViewHandle(nullptr));
}

TEST(UIAndToolingBackendAwarenessTests, CurrentFrameTextureRetirementReleasesOnDiscardOnlyBeforeSubmit)
{
    using NLS::Render::RHI::RHIUICurrentFrameTextureRetirementTracker;
    using NLS::Render::RHI::RHIUITextureHandleUse;

    RHIUICurrentFrameTextureRetirementTracker tracker;
    const RHIUITextureHandleUse use{0x101u, 7u};

    EXPECT_TRUE(tracker.RetireCurrentFrameUse(use, true));
    EXPECT_TRUE(tracker.RetireCurrentFrameUse(use, true));
    EXPECT_TRUE(tracker.IsRetiredCurrentFrameUse(use));

    const auto discarded = tracker.DiscardCurrentFrame();
    ASSERT_EQ(discarded.size(), 1u);
    EXPECT_EQ(discarded.front(), use);
    EXPECT_TRUE(tracker.DiscardCurrentFrame().empty());

    EXPECT_TRUE(tracker.RetireCurrentFrameUse(use, true));
    tracker.RetainCurrentFrame();
    EXPECT_TRUE(tracker.DiscardCurrentFrame().empty());
}

TEST(UIAndToolingBackendAwarenessTests, CurrentFrameTextureRetirementIgnoresNonCurrentOrInvalidUses)
{
    using NLS::Render::RHI::RHIUICurrentFrameTextureRetirementTracker;
    using NLS::Render::RHI::RHIUITextureHandleUse;

    RHIUICurrentFrameTextureRetirementTracker tracker;
    EXPECT_FALSE(tracker.RetireCurrentFrameUse({0x101u, 7u}, false));
    EXPECT_FALSE(tracker.RetireCurrentFrameUse({0u, 7u}, true));
    EXPECT_FALSE(tracker.RetireCurrentFrameUse({0x101u, 0u}, true));
    EXPECT_TRUE(tracker.DiscardCurrentFrame().empty());

    const RHIUITextureHandleUse first{0x101u, 7u};
    const RHIUITextureHandleUse second{0x202u, 8u};
    EXPECT_TRUE(tracker.RetireCurrentFrameUse(first, true));
    EXPECT_TRUE(tracker.RetireCurrentFrameUse(second, true));
    tracker.RemoveViewKey(first.textureViewKey);

    const auto discarded = tracker.DiscardCurrentFrame();
    ASSERT_EQ(discarded.size(), 1u);
    EXPECT_EQ(discarded.front(), second);
}

TEST(UIAndToolingBackendAwarenessTests, ImageWithUnresolvedTextureDoesNotSubmitNullTextureDrawCommand)
{
    ImGuiContextGuard guard;

    ImGui::NewFrame();
    ImGui::Begin("Unresolved Image Test");
    const ImDrawList* drawList = ImGui::GetWindowDrawList();
    ASSERT_NE(drawList, nullptr);
    const int nullTextureElementsBeforeImage = CountDrawElementsWithTextureId(*drawList, nullptr);
    {
        NLS::UI::Widgets::Image image(nullptr, { 16.0f, 16.0f });
        image.Draw();
        EXPECT_TRUE(image.HasLastDrawBounds());
    }
    EXPECT_EQ(CountDrawElementsWithTextureId(*drawList, nullptr), nullTextureElementsBeforeImage);
    ImGui::End();
    ImGui::Render();
}

TEST(UIAndToolingBackendAwarenessTests, ButtonImageWithUnresolvedTextureDoesNotSubmitNullTextureDrawCommand)
{
    ImGuiContextGuard guard;

    ImGui::NewFrame();
    ImGui::Begin("Unresolved Button Image Test");
    const ImDrawList* drawList = ImGui::GetWindowDrawList();
    ASSERT_NE(drawList, nullptr);
    const int nullTextureElementsBeforeButtonImage = CountDrawElementsWithTextureId(*drawList, nullptr);
    {
        NLS::UI::Widgets::ButtonImage button(nullptr, { 20.0f, 20.0f });
        button.Draw();
    }
    EXPECT_EQ(CountDrawElementsWithTextureId(*drawList, nullptr), nullTextureElementsBeforeButtonImage);
    ImGui::End();
    ImGui::Render();
}

TEST(UIAndToolingBackendAwarenessTests, CreatesNullRendererBridgeForDX11Backend)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;
    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    NLS::Render::RHI::NativeRenderDeviceInfo nativeInfo;
    nativeInfo.backend = NLS::Render::RHI::NativeBackendType::DX11;

    const auto bridge = NLS::Render::RHI::CreateRHIUIBridge(nullptr, "#version 150", &nativeInfo);

    ASSERT_NE(bridge, nullptr);
    EXPECT_FALSE(bridge->HasRendererBackend());
    EXPECT_EQ(bridge->GetNativeBackendType(), NLS::Render::RHI::NativeBackendType::None);
    EXPECT_FALSE(bridge->ResolveTextureView(nullptr).IsValid());
}

TEST(UIAndToolingBackendAwarenessTests, UIManagerFontLoadInvalidatesDriverOwnedFrameGraphFontAtlas)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (!CanCreateHeadlessGlfwWindow())
        GTEST_SKIP() << "GLFW display is not available in this environment.";

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestUiOverlayCapabilityDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->uiOverlayRenderer.FontAtlas().SetUploadedResourcesForTesting(
        std::make_shared<TestUiTexture>(),
        std::make_shared<TestUiTextureView>(std::make_shared<TestUiTexture>()),
        std::make_shared<TestUiSampler>(),
        std::make_shared<TestUiBindingSet>());
    ASSERT_EQ(impl->uiOverlayRenderer.FontAtlas().GetRetiredResourceCountForTesting(), 0u);

    NLS::Windowing::Settings::DeviceSettings deviceSettings;
    NLS::Context::Device device(deviceSettings);
    NLS::Windowing::Settings::WindowSettings windowSettings;
    windowSettings.title = "UIManagerFontAtlasInvalidationTests";
    windowSettings.width = 64;
    windowSettings.height = 64;
    windowSettings.visible = false;
    windowSettings.clientAPI = NLS::Windowing::Settings::WindowClientAPI::NoAPI;
    NLS::Windowing::Window window(device, windowSettings);

    NLS::UI::UIManager uiManager(
        window.GetGlfwWindow(),
        NLS::Render::Settings::EGraphicsBackend::DX12,
        NLS::UI::EStyle::IM_DARK_STYLE,
        "#version 150");

    const std::filesystem::path fontPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Editor/Fonts/Ruda-Bold.ttf";
    ASSERT_TRUE(uiManager.LoadFont("framegraph-font-atlas-test", fontPath.string(), 12.0f));

    EXPECT_EQ(impl->uiOverlayRenderer.FontAtlas().GetRetiredResourceCountForTesting(), 1u)
        << "UIManager font mutations must invalidate the driver-owned RHI ImGui font atlas on the migrated path.";
#else
    GTEST_SKIP() << "Requires NLS_ENABLE_TEST_HOOKS.";
#endif
}
