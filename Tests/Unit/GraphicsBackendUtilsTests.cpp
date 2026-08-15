#include <gtest/gtest.h>

#include <string>

#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace
{
using NLS::Render::Settings::EGraphicsBackend;

EGraphicsBackend RequiredBackend()
{
    return NLS::Render::Settings::GetPhase1RequiredRuntimeBackend();
}

EGraphicsBackend UnsupportedBackend()
{
    return RequiredBackend() == EGraphicsBackend::METAL
        ? EGraphicsBackend::DX12
        : EGraphicsBackend::METAL;
}
}

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
    const auto requiredBackend = RequiredBackend();
    const std::string requiredBackendName = NLS::Render::Settings::ToString(requiredBackend);
    const auto requiredDescription =
        NLS::Render::Settings::SceneRendererSupportDescription(requiredBackend);
    EXPECT_NE(
        requiredDescription.find(requiredBackend == EGraphicsBackend::VULKAN
                ? "active runtime backend"
                : "only active runtime backend"),
        std::string::npos);
    if (requiredBackend == EGraphicsBackend::VULKAN)
    {
        EXPECT_NE(requiredDescription.find("Vulkan"), std::string::npos);
    }
    else
    {
        const auto vulkanDescription =
            NLS::Render::Settings::SceneRendererSupportDescription(EGraphicsBackend::VULKAN);
        if (NLS::Render::Settings::IsBackendSelectableForPhase1(EGraphicsBackend::VULKAN))
            EXPECT_NE(vulkanDescription.find("explicitly selectable"), std::string::npos);
        else
            EXPECT_NE(vulkanDescription.find("disabled"), std::string::npos);
    }
    for (const auto backend : {EGraphicsBackend::DX12, EGraphicsBackend::DX11,
                               EGraphicsBackend::OPENGL, EGraphicsBackend::VULKAN,
                               EGraphicsBackend::METAL})
    {
        if (backend == requiredBackend)
            continue;
        const auto description = NLS::Render::Settings::SceneRendererSupportDescription(backend);
        if (NLS::Render::Settings::IsBackendSelectableForPhase1(backend))
            EXPECT_NE(description.find("runtime backend"), std::string::npos);
        else
            EXPECT_NE(description.find(requiredBackendName), std::string::npos);
    }
}

TEST(GraphicsBackendUtilsTests, Phase1BackendSelectionOnlyAcceptsPlatformRuntimeBackend)
{
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    EXPECT_TRUE(NLS::Render::Settings::IsBackendSelectableForPhase1(
        RequiredBackend()));
#else
    EXPECT_FALSE(NLS::Render::Settings::IsBackendSelectableForPhase1(
        RequiredBackend()));
#endif
    for (const auto backend : {EGraphicsBackend::DX12, EGraphicsBackend::DX11,
                               EGraphicsBackend::VULKAN, EGraphicsBackend::OPENGL,
                               EGraphicsBackend::METAL})
    {
        if (backend == RequiredBackend() ||
            NLS::Render::Settings::IsBackendSelectableForPhase1(backend))
            continue;
        EXPECT_FALSE(NLS::Render::Settings::IsBackendSelectableForPhase1(backend));
    }
#if defined(_WIN32)
    EXPECT_EQ(
        NLS::Render::Settings::IsBackendSelectableForPhase1(EGraphicsBackend::VULKAN),
        NLS_HAS_VULKAN != 0);
#endif
}

TEST(GraphicsBackendUtilsTests, Phase1BackendRestrictionMessageExplainsExplicitDX12Requirement)
{
    const auto requiredRestriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        RequiredBackend(),
        "Editor runtime");
    EXPECT_FALSE(requiredRestriction.has_value());

    const auto unsupportedRestriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        UnsupportedBackend(),
        "Game runtime");
    ASSERT_TRUE(unsupportedRestriction.has_value());
    EXPECT_NE(unsupportedRestriction->find("Game runtime"), std::string::npos);
    EXPECT_NE(unsupportedRestriction->find(
        "only supports " + std::string(NLS::Render::Settings::ToString(RequiredBackend()))), std::string::npos);
    EXPECT_NE(unsupportedRestriction->find(NLS::Render::Settings::ToString(UnsupportedBackend())), std::string::npos);

    const auto noneRestriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        NLS::Render::Settings::EGraphicsBackend::NONE,
        "Launcher");
    ASSERT_TRUE(noneRestriction.has_value());
    EXPECT_NE(noneRestriction->find("Launcher"), std::string::npos);
    EXPECT_NE(noneRestriction->find(
        "only supports " + std::string(NLS::Render::Settings::ToString(RequiredBackend()))), std::string::npos);
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
        EXPECT_NE(decision.primaryWarning->find(
            "only supports " + std::string(NLS::Render::Settings::ToString(RequiredBackend()))), std::string::npos);
        ASSERT_TRUE(decision.detailWarning.has_value());
    }
}

TEST(GraphicsBackendUtilsTests, EditorRuntimeReadinessDecisionExplainsBackendNotReady)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = false;

    const auto decision = NLS::Render::Settings::EvaluateEditorMainRuntimeReadiness(
        RequiredBackend(),
        capabilities);

    ASSERT_TRUE(decision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendSelectableForPhase1(RequiredBackend()))
        EXPECT_NE(decision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(decision.primaryWarning->find(
            "only supports " + std::string(NLS::Render::Settings::ToString(RequiredBackend()))), std::string::npos);
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
        RequiredBackend(),
        capabilities);

    ASSERT_TRUE(decision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendSelectableForPhase1(RequiredBackend()))
        EXPECT_NE(decision.primaryWarning->find(
            "before startup can continue on " + std::string(NLS::Render::Settings::ToString(RequiredBackend()))), std::string::npos);
    else
        EXPECT_NE(decision.primaryWarning->find(
            "only supports " + std::string(NLS::Render::Settings::ToString(RequiredBackend()))), std::string::npos);
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
        RequiredBackend(),
        capabilities);

    ASSERT_TRUE(decision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendSelectableForPhase1(RequiredBackend()))
        EXPECT_NE(decision.primaryWarning->find("Offscreen target allocator is disabled"), std::string::npos);
    else
        EXPECT_NE(decision.primaryWarning->find(
            "only supports " + std::string(NLS::Render::Settings::ToString(RequiredBackend()))), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, GameRuntimeReadinessDecisionExplainsBackendNotReady)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.backendReady = false;

    const auto decision = NLS::Render::Settings::EvaluateGameMainRuntimeReadiness(
        RequiredBackend(),
        capabilities);

    ASSERT_TRUE(decision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(RequiredBackend()))
        EXPECT_NE(decision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(decision.primaryWarning->find(
            "only supports " + std::string(NLS::Render::Settings::ToString(RequiredBackend()))), std::string::npos);
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
        UnsupportedBackend(),
        capabilities);

    ASSERT_TRUE(decision.primaryWarning.has_value());
    EXPECT_NE(decision.primaryWarning->find(
        "only supports " + std::string(NLS::Render::Settings::ToString(RequiredBackend()))), std::string::npos);
    ASSERT_TRUE(decision.detailWarning.has_value());
    EXPECT_NE(decision.detailWarning->find(NLS::Render::Settings::ToString(RequiredBackend())), std::string::npos);
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

TEST(GraphicsBackendUtilsTests, ThreadedRenderFoundationPathAcceptsTierABackendsWithRequiredCapabilities)
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
    EXPECT_TRUE(NLS::Render::Settings::SupportsThreadedRenderFoundationPath(
        NLS::Render::RHI::NativeBackendType::Metal,
        capabilities));
    EXPECT_TRUE(NLS::Render::Settings::SupportsThreadedRenderFoundationPath(
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
        NLS::Render::RHI::NativeBackendType::Metal,
        capabilities));
    EXPECT_FALSE(NLS::Render::Settings::SupportsThreadedRenderFoundationPath(
        NLS::Render::RHI::NativeBackendType::Vulkan,
        capabilities));
}

TEST(GraphicsBackendUtilsTests, OrderedParallelSubmissionAcceptsTierABackendsWithRequiredCapabilities)
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

    EXPECT_TRUE(NLS::Render::Settings::SupportsOrderedParallelCommandSubmissionPath(
        NLS::Render::RHI::NativeBackendType::DX12,
        capabilities));
    EXPECT_TRUE(NLS::Render::Settings::SupportsOrderedParallelCommandSubmissionPath(
        NLS::Render::RHI::NativeBackendType::Metal,
        capabilities));
    EXPECT_TRUE(NLS::Render::Settings::SupportsOrderedParallelCommandSubmissionPath(
        NLS::Render::RHI::NativeBackendType::Vulkan,
        capabilities));
    EXPECT_FALSE(NLS::Render::Settings::SupportsOrderedParallelCommandSubmissionPath(
        NLS::Render::RHI::NativeBackendType::OpenGL,
        capabilities));
    EXPECT_FALSE(NLS::Render::Settings::SupportsOrderedParallelCommandSubmissionPath(
        NLS::Render::RHI::NativeBackendType::DX11,
        capabilities));
}

TEST(GraphicsBackendUtilsTests, Phase1ImGuiRuntimeRoutingRejectsAllNonDx12Backends)
{
    EXPECT_FALSE(NLS::Render::Settings::SupportsImGuiRendererBackend(
        NLS::Render::Settings::EGraphicsBackend::OPENGL));
    EXPECT_FALSE(NLS::Render::Settings::SupportsImGuiRendererBackend(
        NLS::Render::Settings::EGraphicsBackend::DX11));

    if (NLS::Render::Settings::IsBackendSelectableForPhase1(
            NLS::Render::Settings::EGraphicsBackend::VULKAN))
        EXPECT_EQ(
            NLS::Render::Settings::SupportsImGuiRendererBackend(
                NLS::Render::Settings::EGraphicsBackend::VULKAN),
            NLS::Render::Settings::HasCompiledOfficialImGuiBackend(
                NLS::Render::Settings::EGraphicsBackend::VULKAN));
    else
        EXPECT_FALSE(NLS::Render::Settings::SupportsImGuiRendererBackend(
            NLS::Render::Settings::EGraphicsBackend::VULKAN));

    EXPECT_EQ(
        NLS::Render::Settings::SupportsImGuiRendererBackend(RequiredBackend()),
        NLS::Render::Settings::HasCompiledOfficialImGuiBackend(RequiredBackend()));
    EXPECT_FALSE(NLS::Render::Settings::SupportsImGuiRendererBackend(
        NLS::Render::Settings::EGraphicsBackend::NONE));
}

TEST(GraphicsBackendUtilsTests, Phase1EditorAndGameConsumersShareTheSamePlatformRestriction)
{
    const auto unsupportedBackend = UnsupportedBackend();
    const auto editorRestriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        unsupportedBackend,
        "Editor runtime");
    const auto gameRestriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        unsupportedBackend,
        "Game runtime");

    ASSERT_TRUE(editorRestriction.has_value());
    ASSERT_TRUE(gameRestriction.has_value());
    const std::string requiredMessage =
        "only supports " + std::string(NLS::Render::Settings::ToString(RequiredBackend())) +
        " during UE5 alignment phase 1";
    EXPECT_NE(editorRestriction->find(requiredMessage), std::string::npos);
    EXPECT_NE(gameRestriction->find(requiredMessage), std::string::npos);
    EXPECT_NE(editorRestriction->find(NLS::Render::Settings::ToString(unsupportedBackend)), std::string::npos);
    EXPECT_NE(gameRestriction->find(NLS::Render::Settings::ToString(unsupportedBackend)), std::string::npos);
}

TEST(GraphicsBackendUtilsTests, WindowsPhase1DefaultBackendMatchesTheOnlyAcceptedRuntimeBackend)
{
    EXPECT_EQ(
        NLS::Render::Settings::GetPlatformDefaultGraphicsBackend(),
        NLS::Render::Settings::GetPhase1RequiredRuntimeBackend());
}

TEST(GraphicsBackendUtilsTests, BackendPhaseGateReportExplainsUnsupportedBackendFallback)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;
    capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady, true);

    const auto unsupportedBackend = UnsupportedBackend();
    const auto report = NLS::Render::Settings::EvaluateBackendPhaseGate(
        unsupportedBackend,
        NLS::Render::Settings::RuntimeConsumer::Editor,
        capabilities);

    EXPECT_EQ(report.requestedBackend, unsupportedBackend);
    EXPECT_EQ(report.fallbackBackend, NLS::Render::Settings::EGraphicsBackend::NONE);
    ASSERT_FALSE(report.gates.empty());
    EXPECT_EQ(report.gates.front().phase, NLS::Render::Settings::BackendPhaseGate::BackendSelection);
    EXPECT_EQ(report.gates.front().severity, NLS::Render::Settings::BackendPhaseGateSeverity::Error);
    EXPECT_NE(report.gates.front().reason.find(
        "only supports " + std::string(NLS::Render::Settings::ToString(RequiredBackend()))), std::string::npos);
    EXPECT_NE(
        report.summary.find(NLS::Render::Settings::ToString(unsupportedBackend)),
        std::string::npos);
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
        RequiredBackend(),
        NLS::Render::Settings::RuntimeConsumer::Editor,
        capabilities);

    ASSERT_FALSE(report.gates.empty());
    EXPECT_EQ(report.gates.front().phase, NLS::Render::Settings::BackendPhaseGate::CapabilityValidation);
    EXPECT_EQ(report.fallbackBackend, NLS::Render::Settings::EGraphicsBackend::NONE);
    EXPECT_NE(report.gates.front().reason.find("Offscreen allocator missing"), std::string::npos);
    EXPECT_NE(report.summary.find("CapabilityValidation"), std::string::npos);
}
