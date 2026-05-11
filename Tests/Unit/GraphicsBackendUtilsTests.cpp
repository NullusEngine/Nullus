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

TEST(GraphicsBackendUtilsTests, ParsesTruthyEnvironmentValuesCaseInsensitively)
{
    EXPECT_TRUE(NLS::Render::Settings::IsTruthyEnvironmentValue("1"));
    EXPECT_TRUE(NLS::Render::Settings::IsTruthyEnvironmentValue("true"));
    EXPECT_TRUE(NLS::Render::Settings::IsTruthyEnvironmentValue("TRUE"));
    EXPECT_TRUE(NLS::Render::Settings::IsTruthyEnvironmentValue("TrUe"));

    EXPECT_FALSE(NLS::Render::Settings::IsTruthyEnvironmentValue(nullptr));
    EXPECT_FALSE(NLS::Render::Settings::IsTruthyEnvironmentValue(""));
    EXPECT_FALSE(NLS::Render::Settings::IsTruthyEnvironmentValue("0"));
    EXPECT_FALSE(NLS::Render::Settings::IsTruthyEnvironmentValue("false"));
}

TEST(GraphicsBackendUtilsTests, SceneRendererSupportDescriptionsMatchCurrentSupportMatrix)
{
    EXPECT_NE(
        std::string(NLS::Render::Settings::SceneRendererSupportDescription(
            NLS::Render::Settings::EGraphicsBackend::DX12)).find("only active runtime backend"),
        std::string::npos);
    EXPECT_NE(
        std::string(NLS::Render::Settings::SceneRendererSupportDescription(
            NLS::Render::Settings::EGraphicsBackend::VULKAN)).find("future multi-backend"),
        std::string::npos);
    EXPECT_NE(
        std::string(NLS::Render::Settings::SceneRendererSupportDescription(
            NLS::Render::Settings::EGraphicsBackend::DX11)).find("only permits DX12"),
        std::string::npos);
    EXPECT_NE(
        std::string(NLS::Render::Settings::SceneRendererSupportDescription(
            NLS::Render::Settings::EGraphicsBackend::OPENGL)).find("only permits DX12"),
        std::string::npos);
    EXPECT_NE(
        std::string(NLS::Render::Settings::SceneRendererSupportDescription(
            NLS::Render::Settings::EGraphicsBackend::METAL)).find("only permits DX12"),
        std::string::npos);
}

TEST(GraphicsBackendUtilsTests, Phase1BackendSelectionOnlyAcceptsDX12)
{
#if defined(_WIN32)
    EXPECT_TRUE(NLS::Render::Settings::IsBackendSelectableForPhase1(
        NLS::Render::Settings::EGraphicsBackend::DX12));
#else
    EXPECT_FALSE(NLS::Render::Settings::IsBackendSelectableForPhase1(
        NLS::Render::Settings::EGraphicsBackend::DX12));
#endif
    EXPECT_FALSE(NLS::Render::Settings::IsBackendSelectableForPhase1(
        NLS::Render::Settings::EGraphicsBackend::DX11));
    EXPECT_FALSE(NLS::Render::Settings::IsBackendSelectableForPhase1(
        NLS::Render::Settings::EGraphicsBackend::VULKAN));
    EXPECT_FALSE(NLS::Render::Settings::IsBackendSelectableForPhase1(
        NLS::Render::Settings::EGraphicsBackend::OPENGL));
    EXPECT_FALSE(NLS::Render::Settings::IsBackendSelectableForPhase1(
        NLS::Render::Settings::EGraphicsBackend::METAL));
}

TEST(GraphicsBackendUtilsTests, Phase1BackendRestrictionMessageExplainsExplicitDX12Requirement)
{
    const auto dx12Restriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        NLS::Render::Settings::EGraphicsBackend::DX12,
        "Editor runtime");
    if (NLS::Render::Settings::IsBackendSelectableForPhase1(NLS::Render::Settings::EGraphicsBackend::DX12))
        EXPECT_FALSE(dx12Restriction.has_value());
    else
    {
        ASSERT_TRUE(dx12Restriction.has_value());
        EXPECT_NE(dx12Restriction->find("Editor runtime"), std::string::npos);
        EXPECT_NE(dx12Restriction->find("only supports DX12"), std::string::npos);
    }

    const auto dx11Restriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        NLS::Render::Settings::EGraphicsBackend::DX11,
        "Game runtime");
    ASSERT_TRUE(dx11Restriction.has_value());
    EXPECT_NE(dx11Restriction->find("Game runtime"), std::string::npos);
    EXPECT_NE(dx11Restriction->find("only supports DX12"), std::string::npos);
    EXPECT_NE(dx11Restriction->find("DX11"), std::string::npos);

    const auto noneRestriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        NLS::Render::Settings::EGraphicsBackend::NONE,
        "Launcher");
    ASSERT_TRUE(noneRestriction.has_value());
    EXPECT_NE(noneRestriction->find("Launcher"), std::string::npos);
    EXPECT_NE(noneRestriction->find("only supports DX12"), std::string::npos);
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

TEST(GraphicsBackendUtilsTests, DeviceCapabilitiesExposeFeatureReasonsAndStructuredLimits)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::EditorPickingReadback,
        false,
        "Readback queue is not wired for this backend");
    capabilities.limits.maxTextureDimension2D = 8192u;
    capabilities.limits.maxColorAttachments = 4u;

    const auto feature = capabilities.GetFeature(NLS::Render::RHI::RHIDeviceFeature::EditorPickingReadback);

    EXPECT_FALSE(feature.supported);
    EXPECT_EQ(feature.reason, "Readback queue is not wired for this backend");
    EXPECT_EQ(capabilities.limits.maxTextureDimension2D, 8192u);
    EXPECT_EQ(capabilities.limits.maxColorAttachments, 4u);
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

TEST(GraphicsBackendUtilsTests, EditorRuntimeReadinessDecisionStaysClearWhenCapabilitiesAreSufficient)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsUITextureHandles = true;
    capabilities.supportsDepthBlit = true;
    capabilities.supportsCubemaps = true;

    const auto decision = NLS::Render::Settings::EvaluateEditorMainRuntimeReadiness(
        NLS::Render::Settings::GetPlatformDefaultGraphicsBackend(),
        capabilities);

    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(
            NLS::Render::Settings::GetPlatformDefaultGraphicsBackend()))
    {
        EXPECT_FALSE(decision.primaryWarning.has_value());
        EXPECT_FALSE(decision.detailWarning.has_value());
    }
    else
    {
        ASSERT_TRUE(decision.primaryWarning.has_value());
        EXPECT_NE(decision.primaryWarning->find("only supports DX12"), std::string::npos);
        ASSERT_TRUE(decision.detailWarning.has_value());
    }
}

TEST(GraphicsBackendUtilsTests, EditorRuntimeReadinessDecisionExplainsBackendNotReady)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = false;

    const auto decision = NLS::Render::Settings::EvaluateEditorMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::DX12,
        capabilities);

    ASSERT_TRUE(decision.primaryWarning.has_value());
#if defined(_WIN32)
    EXPECT_NE(decision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
#else
    EXPECT_NE(decision.primaryWarning->find("only supports DX12"), std::string::npos);
#endif
    ASSERT_TRUE(decision.detailWarning.has_value());
    EXPECT_NE(decision.detailWarning->find("only active runtime backend"), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, EditorRuntimeReadinessDecisionExplainsCapabilityGapWhenBackendIsReady)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = false;
    capabilities.supportsUITextureHandles = true;
    capabilities.supportsDepthBlit = true;
    capabilities.supportsCubemaps = true;

    const auto decision = NLS::Render::Settings::EvaluateEditorMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::DX12,
        capabilities);

    ASSERT_TRUE(decision.primaryWarning.has_value());
#if defined(_WIN32)
    EXPECT_NE(decision.primaryWarning->find("before startup can continue on DX12"), std::string::npos);
#else
    EXPECT_NE(decision.primaryWarning->find("only supports DX12"), std::string::npos);
#endif
    ASSERT_TRUE(decision.detailWarning.has_value());
    EXPECT_NE(decision.detailWarning->find("only active runtime backend"), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, EditorRuntimeReadinessDecisionIncludesStructuredCapabilityReason)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady, true);
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::CurrentSceneRenderer, true);
    capabilities.SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::OffscreenFramebuffers,
        false,
        "Offscreen target allocator is disabled");
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::UITextureHandles, true);
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::DepthBlit, true);
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Cubemaps, true);

    const auto decision = NLS::Render::Settings::EvaluateEditorMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::DX12,
        capabilities);

    ASSERT_TRUE(decision.primaryWarning.has_value());
#if defined(_WIN32)
    EXPECT_NE(decision.primaryWarning->find("Offscreen target allocator is disabled"), std::string::npos);
#else
    EXPECT_NE(decision.primaryWarning->find("only supports DX12"), std::string::npos);
#endif
}

TEST(GraphicsBackendUtilsTests, GameRuntimeReadinessDecisionExplainsBackendNotReady)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = false;

    const auto decision = NLS::Render::Settings::EvaluateGameMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::DX12,
        capabilities);

    ASSERT_TRUE(decision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(NLS::Render::Settings::EGraphicsBackend::DX12))
        EXPECT_NE(decision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(decision.primaryWarning->find("only supports DX12"), std::string::npos);
    ASSERT_TRUE(decision.detailWarning.has_value());
    EXPECT_NE(decision.detailWarning->find("only active runtime backend"), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, GameRuntimeReadinessDecisionReportsUnsupportedBackendsExplicitly)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsSwapchain = true;

    const auto decision = NLS::Render::Settings::EvaluateGameMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::DX11,
        capabilities);

    ASSERT_TRUE(decision.primaryWarning.has_value());
    EXPECT_NE(decision.primaryWarning->find("only supports DX12"), std::string::npos);
    ASSERT_TRUE(decision.detailWarning.has_value());
    EXPECT_NE(decision.detailWarning->find("only permits DX12"), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, TierARenderFoundationRequiresCentralizedDescriptorAndPipelineSupport)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsGraphics = true;
    capabilities.supportsCompute = true;
    capabilities.supportsSwapchain = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsMultiRenderTargets = true;
    capabilities.supportsExplicitBarriers = true;

    EXPECT_FALSE(NLS::Render::Settings::SupportsTierARenderFoundation(capabilities));

    capabilities.supportsCentralizedDescriptorManagement = true;
    capabilities.supportsPipelineStateCache = true;

    EXPECT_TRUE(NLS::Render::Settings::SupportsTierARenderFoundation(capabilities));
}

TEST(GraphicsBackendUtilsTests, TransientRenderGraphResourcesRequireFoundationAndTransientAllocator)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsGraphics = true;
    capabilities.supportsCompute = true;
    capabilities.supportsSwapchain = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsMultiRenderTargets = true;
    capabilities.supportsExplicitBarriers = true;
    capabilities.supportsCentralizedDescriptorManagement = true;
    capabilities.supportsPipelineStateCache = true;

    EXPECT_FALSE(NLS::Render::Settings::SupportsRenderGraphTransientResources(capabilities));

    capabilities.supportsTransientResourceAllocator = true;

    EXPECT_TRUE(NLS::Render::Settings::SupportsRenderGraphTransientResources(capabilities));
}

TEST(GraphicsBackendUtilsTests, AsyncComputeRequiresFoundationPlusDedicatedComputeReadiness)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsGraphics = true;
    capabilities.supportsCompute = true;
    capabilities.supportsSwapchain = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsMultiRenderTargets = true;
    capabilities.supportsExplicitBarriers = true;
    capabilities.supportsCentralizedDescriptorManagement = true;
    capabilities.supportsPipelineStateCache = true;

    EXPECT_FALSE(NLS::Render::Settings::SupportsAsyncComputeFoundation(capabilities));

    capabilities.supportsAsyncCompute = true;
    EXPECT_FALSE(NLS::Render::Settings::SupportsAsyncComputeFoundation(capabilities));

    capabilities.supportsDedicatedComputeQueue = true;
    EXPECT_TRUE(NLS::Render::Settings::SupportsAsyncComputeFoundation(capabilities));
}

TEST(GraphicsBackendUtilsTests, ParallelCommandFoundationRequiresRecordingAndTranslation)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsGraphics = true;
    capabilities.supportsCompute = true;
    capabilities.supportsSwapchain = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsMultiRenderTargets = true;
    capabilities.supportsExplicitBarriers = true;
    capabilities.supportsCentralizedDescriptorManagement = true;
    capabilities.supportsPipelineStateCache = true;

    EXPECT_FALSE(NLS::Render::Settings::SupportsParallelCommandFoundation(capabilities));

    capabilities.supportsParallelCommandRecording = true;
    EXPECT_FALSE(NLS::Render::Settings::SupportsParallelCommandFoundation(capabilities));

    capabilities.supportsParallelCommandTranslation = true;
    EXPECT_TRUE(NLS::Render::Settings::SupportsParallelCommandFoundation(capabilities));
}

TEST(GraphicsBackendUtilsTests, ThreadedRenderFoundationPathRequiresTierABackendAndCapabilities)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsGraphics = true;
    capabilities.supportsCompute = true;
    capabilities.supportsSwapchain = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsMultiRenderTargets = true;
    capabilities.supportsExplicitBarriers = true;
    capabilities.supportsCentralizedDescriptorManagement = true;
    capabilities.supportsPipelineStateCache = true;

    EXPECT_TRUE(NLS::Render::Settings::SupportsThreadedRenderFoundationPath(
        NLS::Render::RHI::NativeBackendType::DX12,
        capabilities));
    EXPECT_FALSE(NLS::Render::Settings::SupportsThreadedRenderFoundationPath(
        NLS::Render::RHI::NativeBackendType::Vulkan,
        capabilities));
    EXPECT_FALSE(NLS::Render::Settings::SupportsThreadedRenderFoundationPath(
        NLS::Render::RHI::NativeBackendType::OpenGL,
        capabilities));
    EXPECT_FALSE(NLS::Render::Settings::SupportsThreadedRenderFoundationPath(
        NLS::Render::RHI::NativeBackendType::DX11,
        capabilities));
}

TEST(GraphicsBackendUtilsTests, ThreadedRenderFoundationPathRejectsNonFoundationCapabilitiesEvenOnTierABackends)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = true;
    capabilities.supportsGraphics = true;
    capabilities.supportsCompute = true;
    capabilities.supportsSwapchain = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsMultiRenderTargets = true;
    capabilities.supportsExplicitBarriers = true;
    capabilities.supportsCentralizedDescriptorManagement = true;

    EXPECT_FALSE(NLS::Render::Settings::SupportsThreadedRenderFoundationPath(
        NLS::Render::RHI::NativeBackendType::DX12,
        capabilities));
    EXPECT_FALSE(NLS::Render::Settings::SupportsThreadedRenderFoundationPath(
        NLS::Render::RHI::NativeBackendType::Vulkan,
        capabilities));
}

TEST(GraphicsBackendUtilsTests, Phase1ImGuiRuntimeRoutingRejectsAllNonDx12Backends)
{
    EXPECT_FALSE(NLS::Render::Settings::SupportsImGuiRendererBackend(
        NLS::Render::Settings::EGraphicsBackend::OPENGL));
    EXPECT_FALSE(NLS::Render::Settings::SupportsImGuiRendererBackend(
        NLS::Render::Settings::EGraphicsBackend::VULKAN));
    EXPECT_FALSE(NLS::Render::Settings::SupportsImGuiRendererBackend(
        NLS::Render::Settings::EGraphicsBackend::DX11));
    EXPECT_FALSE(NLS::Render::Settings::SupportsImGuiRendererBackend(
        NLS::Render::Settings::EGraphicsBackend::METAL));
    EXPECT_FALSE(NLS::Render::Settings::SupportsImGuiRendererBackend(
        NLS::Render::Settings::EGraphicsBackend::NONE));
}

TEST(GraphicsBackendUtilsTests, Phase1EditorAndGameConsumersShareTheSameDx12OnlyRestriction)
{
    const auto editorRestriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        NLS::Render::Settings::EGraphicsBackend::VULKAN,
        "Editor runtime");
    const auto gameRestriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        NLS::Render::Settings::EGraphicsBackend::VULKAN,
        "Game runtime");

    ASSERT_TRUE(editorRestriction.has_value());
    ASSERT_TRUE(gameRestriction.has_value());
    EXPECT_NE(editorRestriction->find("only supports DX12 during UE5 alignment phase 1"), std::string::npos);
    EXPECT_NE(gameRestriction->find("only supports DX12 during UE5 alignment phase 1"), std::string::npos);
    EXPECT_NE(editorRestriction->find("Vulkan"), std::string::npos);
    EXPECT_NE(gameRestriction->find("Vulkan"), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, WindowsPhase1DefaultBackendMatchesTheOnlyAcceptedRuntimeBackend)
{
#if defined(_WIN32)
    EXPECT_EQ(
        NLS::Render::Settings::GetPlatformDefaultGraphicsBackend(),
        NLS::Render::Settings::GetPhase1RequiredRuntimeBackend());
#else
    EXPECT_NE(
        NLS::Render::Settings::GetPlatformDefaultGraphicsBackend(),
        NLS::Render::Settings::GetPhase1RequiredRuntimeBackend());
#endif
}

TEST(GraphicsBackendUtilsTests, BackendPhaseGateReportExplainsUnsupportedBackendFallback)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady, true);

    const auto report = NLS::Render::Settings::EvaluateBackendPhaseGate(
        NLS::Render::Settings::EGraphicsBackend::VULKAN,
        NLS::Render::Settings::RuntimeConsumer::Editor,
        capabilities);

    EXPECT_EQ(report.requestedBackend, NLS::Render::Settings::EGraphicsBackend::VULKAN);
    EXPECT_EQ(report.fallbackBackend, NLS::Render::Settings::EGraphicsBackend::NONE);
    ASSERT_FALSE(report.gates.empty());
    EXPECT_EQ(report.gates.front().phase, NLS::Render::Settings::BackendPhaseGate::BackendSelection);
    EXPECT_EQ(report.gates.front().severity, NLS::Render::Settings::BackendPhaseGateSeverity::Error);
    EXPECT_NE(report.gates.front().reason.find("only supports DX12"), std::string::npos);
    EXPECT_NE(report.summary.find("Vulkan"), std::string::npos);
    EXPECT_NE(report.summary.find("fallback=None"), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, BackendPhaseGateReportIncludesMissingCapabilityReason)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady, true);
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::CurrentSceneRenderer, true);
    capabilities.SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::OffscreenFramebuffers,
        false,
        "Offscreen allocator missing for this backend");
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::UITextureHandles, true);
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::DepthBlit, true);
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Cubemaps, true);

    const auto report = NLS::Render::Settings::EvaluateBackendPhaseGate(
        NLS::Render::Settings::EGraphicsBackend::DX12,
        NLS::Render::Settings::RuntimeConsumer::Editor,
        capabilities);

    ASSERT_FALSE(report.gates.empty());
#if defined(_WIN32)
    EXPECT_EQ(report.gates.front().phase, NLS::Render::Settings::BackendPhaseGate::CapabilityValidation);
    EXPECT_EQ(report.fallbackBackend, NLS::Render::Settings::EGraphicsBackend::NONE);
    EXPECT_NE(report.gates.front().reason.find("Offscreen allocator missing"), std::string::npos);
    EXPECT_NE(report.summary.find("CapabilityValidation"), std::string::npos);
#else
    EXPECT_EQ(report.gates.front().phase, NLS::Render::Settings::BackendPhaseGate::BackendSelection);
    EXPECT_EQ(report.fallbackBackend, NLS::Render::Settings::EGraphicsBackend::NONE);
    EXPECT_NE(report.gates.front().reason.find("only supports DX12"), std::string::npos);
    EXPECT_NE(report.summary.find("BackendSelection"), std::string::npos);
#endif
}
