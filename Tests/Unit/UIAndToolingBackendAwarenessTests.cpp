#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/RHI/Utils/RHIUIBridge.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Tooling/RenderDocCaptureController.h"
#include "UI/UIManager.h"

namespace
{
    std::filesystem::path GetRepositoryRoot()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    }

    std::string ReadRepositorySource(std::string_view relativePath)
    {
        std::ifstream file(GetRepositoryRoot() / relativePath);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
}

TEST(UIAndToolingBackendAwarenessTests, ResolvesImGuiGlfwInitBackendByGraphicsBackend)
{
    using NLS::Render::Settings::EGraphicsBackend;
    using NLS::UI::ImGuiGlfwInitBackend;

#if defined(_WIN32)
    EXPECT_EQ(NLS::UI::ResolveImGuiGlfwInitBackend(EGraphicsBackend::OPENGL), ImGuiGlfwInitBackend::Other);
#else
    EXPECT_EQ(NLS::UI::ResolveImGuiGlfwInitBackend(EGraphicsBackend::OPENGL), ImGuiGlfwInitBackend::OpenGL);
#endif
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
    EXPECT_EQ(bridge->ResolveTextureView(nullptr), nullptr);
}

TEST(UIAndToolingBackendAwarenessTests, DX12UIBridgeClearsBackbufferBeforeImGuiDrawData)
{
    const auto source = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp");

    const auto prepareOffset = source.find("DriverUIAccess::PrepareUIRender");
    const auto ensureOffset = source.find("EnsureSwapchainRenderResources(nativeInfo)");
    const auto setRenderTargetOffset = source.find("m_commandList->OMSetRenderTargets");
    const auto clearOffset = source.find("m_commandList->ClearRenderTargetView", setRenderTargetOffset);
    const auto drawOffset = source.find("ImGui_ImplDX12_RenderDrawData", setRenderTargetOffset);

    ASSERT_NE(prepareOffset, std::string::npos);
    ASSERT_NE(ensureOffset, std::string::npos);
    ASSERT_NE(setRenderTargetOffset, std::string::npos);
    ASSERT_NE(clearOffset, std::string::npos);
    ASSERT_NE(drawOffset, std::string::npos);
    EXPECT_LT(prepareOffset, ensureOffset);
    EXPECT_LT(ensureOffset, setRenderTargetOffset);
    EXPECT_LT(setRenderTargetOffset, clearOffset);
    EXPECT_LT(clearOffset, drawOffset);
}
