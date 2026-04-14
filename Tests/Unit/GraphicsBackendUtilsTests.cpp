#include <gtest/gtest.h>

#include "Rendering/Settings/GraphicsBackendUtils.h"

TEST(GraphicsBackendUtilsTests, ParsesAllBackendAliases)
{
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("opengl"), NLS::Render::Settings::EGraphicsBackend::OPENGL);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("OpenGL"), NLS::Render::Settings::EGraphicsBackend::OPENGL);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("OPENGL"), NLS::Render::Settings::EGraphicsBackend::OPENGL);

    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("vulkan"), NLS::Render::Settings::EGraphicsBackend::VULKAN);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("Vulkan"), NLS::Render::Settings::EGraphicsBackend::VULKAN);

    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("dx12"), NLS::Render::Settings::EGraphicsBackend::DX12);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("DX12"), NLS::Render::Settings::EGraphicsBackend::DX12);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("directx12"), NLS::Render::Settings::EGraphicsBackend::DX12);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("d3d12"), NLS::Render::Settings::EGraphicsBackend::DX12);

    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("dx11"), NLS::Render::Settings::EGraphicsBackend::DX11);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("Dx11"), NLS::Render::Settings::EGraphicsBackend::DX11);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("directx11"), NLS::Render::Settings::EGraphicsBackend::DX11);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("d3d11"), NLS::Render::Settings::EGraphicsBackend::DX11);

    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("metal"), NLS::Render::Settings::EGraphicsBackend::METAL);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("Metal"), NLS::Render::Settings::EGraphicsBackend::METAL);

    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("none"), NLS::Render::Settings::EGraphicsBackend::NONE);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("null"), NLS::Render::Settings::EGraphicsBackend::NONE);

    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("not-a-backend"), std::nullopt);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("unknown"), std::nullopt);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend(""), std::nullopt);
}

TEST(GraphicsBackendUtilsTests, StringifiesAllBackends)
{
    EXPECT_STREQ(NLS::Render::Settings::ToString(NLS::Render::Settings::EGraphicsBackend::OPENGL), "OpenGL");
    EXPECT_STREQ(NLS::Render::Settings::ToString(NLS::Render::Settings::EGraphicsBackend::VULKAN), "Vulkan");
    EXPECT_STREQ(NLS::Render::Settings::ToString(NLS::Render::Settings::EGraphicsBackend::DX12), "DX12");
    EXPECT_STREQ(NLS::Render::Settings::ToString(NLS::Render::Settings::EGraphicsBackend::DX11), "DX11");
    EXPECT_STREQ(NLS::Render::Settings::ToString(NLS::Render::Settings::EGraphicsBackend::METAL), "Metal");
    EXPECT_STREQ(NLS::Render::Settings::ToString(NLS::Render::Settings::EGraphicsBackend::NONE), "None");
}

TEST(GraphicsBackendUtilsTests, StringifiesDX11Backend)
{
    EXPECT_STREQ(NLS::Render::Settings::ToString(NLS::Render::Settings::EGraphicsBackend::DX11), "DX11");
}

TEST(GraphicsBackendUtilsTests, SceneRendererSupportDescriptionsMatchCurrentSupportMatrix)
{
    EXPECT_NE(
        std::string(NLS::Render::Settings::SceneRendererSupportDescription(
            NLS::Render::Settings::EGraphicsBackend::DX12)).find("formal RHI mainline"),
        std::string::npos);
    EXPECT_NE(
        std::string(NLS::Render::Settings::SceneRendererSupportDescription(
            NLS::Render::Settings::EGraphicsBackend::VULKAN)).find("formal RHI mainline"),
        std::string::npos);

    EXPECT_NE(
        std::string(NLS::Render::Settings::SceneRendererSupportDescription(
            NLS::Render::Settings::EGraphicsBackend::DX11)).find("unsupported"),
        std::string::npos);
    EXPECT_NE(
        std::string(NLS::Render::Settings::SceneRendererSupportDescription(
            NLS::Render::Settings::EGraphicsBackend::OPENGL)).find("unsupported"),
        std::string::npos);
    EXPECT_NE(
        std::string(NLS::Render::Settings::SceneRendererSupportDescription(
            NLS::Render::Settings::EGraphicsBackend::METAL)).find("unsupported"),
        std::string::npos);
}

TEST(GraphicsBackendUtilsTests, EditorMainRuntimeDoesNotRequireFramebufferReadback)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsFramebufferReadback = false;
    capabilities.supportsUITextureHandles = true;
    capabilities.supportsDepthBlit = true;
    capabilities.supportsCubemaps = true;

    EXPECT_TRUE(NLS::Render::Settings::SupportsEditorMainRuntime(capabilities));
    EXPECT_FALSE(NLS::Render::Settings::SupportsEditorPickingReadback(capabilities));
}

TEST(GraphicsBackendUtilsTests, EditorMainRuntimeStillRequiresCoreOffscreenCapabilities)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = false;
    capabilities.supportsFramebufferReadback = true;
    capabilities.supportsEditorPickingReadback = true;
    capabilities.supportsUITextureHandles = true;
    capabilities.supportsDepthBlit = true;
    capabilities.supportsCubemaps = true;

    EXPECT_FALSE(NLS::Render::Settings::SupportsEditorMainRuntime(capabilities));
    EXPECT_TRUE(NLS::Render::Settings::SupportsEditorPickingReadback(capabilities));
}

TEST(GraphicsBackendUtilsTests, EditorPickingReadbackWarningIsEmptyWhenSupported)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.supportsFramebufferReadback = true;
    capabilities.supportsEditorPickingReadback = true;

    EXPECT_FALSE(NLS::Render::Settings::GetEditorPickingReadbackWarning(capabilities).has_value());
}

TEST(GraphicsBackendUtilsTests, EditorPickingReadbackWarningExplainsSceneViewDegradeWhenUnsupported)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.supportsFramebufferReadback = false;

    const auto warning = NLS::Render::Settings::GetEditorPickingReadbackWarning(capabilities);

    ASSERT_TRUE(warning.has_value());
    EXPECT_NE(warning->find("Scene view picking readback is unavailable"), std::string::npos);
    EXPECT_NE(warning->find("hover picking"), std::string::npos);
    EXPECT_NE(warning->find("click selection"), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, EditorPickingReadbackRequiresDedicatedPickingSupport)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.supportsFramebufferReadback = true;

    EXPECT_FALSE(NLS::Render::Settings::SupportsEditorPickingReadback(capabilities));
    EXPECT_TRUE(NLS::Render::Settings::GetEditorPickingReadbackWarning(capabilities).has_value());
}

TEST(GraphicsBackendUtilsTests, GameMainRuntimeRequiresSwapchainAndSceneRenderer)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsSwapchain = false;

    EXPECT_FALSE(NLS::Render::Settings::SupportsGameMainRuntime(capabilities));

    capabilities.supportsSwapchain = true;
    EXPECT_TRUE(NLS::Render::Settings::SupportsGameMainRuntime(capabilities));
}

TEST(GraphicsBackendUtilsTests, GameMainRuntimeDoesNotRequireEditorOnlyReadbackOrUICapabilities)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsSwapchain = true;
    capabilities.supportsFramebufferReadback = false;
    capabilities.supportsOffscreenFramebuffers = false;
    capabilities.supportsUITextureHandles = false;
    capabilities.supportsDepthBlit = false;

    EXPECT_TRUE(NLS::Render::Settings::SupportsGameMainRuntime(capabilities));
}

TEST(GraphicsBackendUtilsTests, EditorRuntimeFallbackDecisionStaysOnRequestedBackendWhenCapabilitiesAreSufficient)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsUITextureHandles = true;
    capabilities.supportsDepthBlit = true;
    capabilities.supportsCubemaps = true;

    const auto decision = NLS::Render::Settings::EvaluateEditorMainRuntimeFallback(
        NLS::Render::Settings::EGraphicsBackend::VULKAN,
        capabilities);

    EXPECT_FALSE(decision.shouldFallbackToOpenGL);
    EXPECT_FALSE(decision.primaryWarning.has_value());
    EXPECT_FALSE(decision.detailWarning.has_value());
}

TEST(GraphicsBackendUtilsTests, EditorRuntimeFallbackDecisionExplainsBackendNotReady)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = false;

    const auto decision = NLS::Render::Settings::EvaluateEditorMainRuntimeFallback(
        NLS::Render::Settings::EGraphicsBackend::DX12,
        capabilities);

    EXPECT_FALSE(decision.shouldFallbackToOpenGL);
    ASSERT_TRUE(decision.primaryWarning.has_value());
    EXPECT_NE(decision.primaryWarning->find("no validated fallback backend"), std::string::npos);
    ASSERT_TRUE(decision.detailWarning.has_value());
    EXPECT_NE(decision.detailWarning->find("formal RHI mainline"), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, EditorRuntimeFallbackDecisionExplainsCapabilityGapWhenBackendIsReady)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = false;
    capabilities.supportsUITextureHandles = true;
    capabilities.supportsDepthBlit = true;
    capabilities.supportsCubemaps = true;

    const auto decision = NLS::Render::Settings::EvaluateEditorMainRuntimeFallback(
        NLS::Render::Settings::EGraphicsBackend::DX12,
        capabilities);

    EXPECT_FALSE(decision.shouldFallbackToOpenGL);
    ASSERT_TRUE(decision.primaryWarning.has_value());
    EXPECT_NE(decision.primaryWarning->find("no validated fallback backend"), std::string::npos);
    ASSERT_TRUE(decision.detailWarning.has_value());
    EXPECT_NE(decision.detailWarning->find("formal RHI mainline"), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, GameRuntimeFallbackDecisionExplainsBackendNotReady)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = false;

    const auto decision = NLS::Render::Settings::EvaluateGameMainRuntimeFallback(
        NLS::Render::Settings::EGraphicsBackend::VULKAN,
        capabilities);

    EXPECT_FALSE(decision.shouldFallbackToOpenGL);
    ASSERT_TRUE(decision.primaryWarning.has_value());
    EXPECT_NE(decision.primaryWarning->find("no validated fallback backend"), std::string::npos);
    ASSERT_TRUE(decision.detailWarning.has_value());
    EXPECT_NE(decision.detailWarning->find("formal RHI mainline"), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, GameRuntimeFallbackDecisionReportsUnsupportedBackendsExplicitly)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsSwapchain = true;

    const auto decision = NLS::Render::Settings::EvaluateGameMainRuntimeFallback(
        NLS::Render::Settings::EGraphicsBackend::DX11,
        capabilities);

    EXPECT_FALSE(decision.shouldFallbackToOpenGL);
    ASSERT_TRUE(decision.primaryWarning.has_value());
    EXPECT_NE(decision.primaryWarning->find("unsupported"), std::string::npos);
    ASSERT_TRUE(decision.detailWarning.has_value());
    EXPECT_NE(decision.detailWarning->find("unsupported"), std::string::npos);
}
