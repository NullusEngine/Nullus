#include <gtest/gtest.h>

#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/RHI/Utils/RHIUIBridge.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Tooling/RenderDocCaptureController.h"
#include "UI/UIManager.h"

TEST(UIAndToolingBackendAwarenessTests, ResolvesImGuiGlfwInitBackendByGraphicsBackend)
{
    using NLS::Render::Settings::EGraphicsBackend;
    using NLS::UI::ImGuiGlfwInitBackend;

    EXPECT_EQ(NLS::UI::ResolveImGuiGlfwInitBackend(EGraphicsBackend::OPENGL), ImGuiGlfwInitBackend::Other);
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
    EXPECT_EQ(NLS::Render::Tooling::ResolveRenderDocCaptureDevice(info), vulkanDispatchPointer);

    info.backend = NLS::Render::RHI::NativeBackendType::DX11;
    EXPECT_EQ(NLS::Render::Tooling::ResolveRenderDocCaptureDevice(info), info.device);

    info.backend = NLS::Render::RHI::NativeBackendType::OpenGL;
    EXPECT_EQ(NLS::Render::Tooling::ResolveRenderDocCaptureDevice(info), info.device);
}

TEST(UIAndToolingBackendAwarenessTests, CreatesNullRendererBridgeForDX11Backend)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;
    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    const auto bridge = NLS::Render::RHI::CreateRHIUIBridge(
        nullptr,
        NLS::Render::Settings::EGraphicsBackend::DX11,
        "#version 150",
        nullptr);

    ASSERT_NE(bridge, nullptr);
    EXPECT_FALSE(bridge->HasRendererBackend());
    EXPECT_EQ(bridge->ResolveTextureView(nullptr), nullptr);
}
