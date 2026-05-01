#include <gtest/gtest.h>

#include <filesystem>

#include "ReflectionTestUtils.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/RHI/Core/RHIRenderSurfaceConvention.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilder.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace
{
    std::filesystem::path GetRepositoryRoot()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    }

    std::string ReadRepositorySource(std::string_view relativePath)
    {
        return NLS::Tests::Reflection::ReadAllText(GetRepositoryRoot() / relativePath);
    }

    size_t CountOccurrences(std::string_view content, std::string_view needle)
    {
        size_t count = 0u;
        size_t offset = 0u;
        while ((offset = content.find(needle, offset)) != std::string_view::npos)
        {
            ++count;
            offset += needle.size();
        }
        return count;
    }

    std::string ExtractSection(
        std::string_view content,
        std::string_view beginNeedle,
        std::string_view endNeedle)
    {
        const size_t begin = content.find(beginNeedle);
        if (begin == std::string_view::npos)
            return std::string(content);

        const size_t end = content.find(endNeedle, begin);
        if (end == std::string_view::npos || end <= begin)
            return std::string(content.substr(begin));

        return std::string(content.substr(begin, end - begin));
    }
}

TEST(UE5RenderArchitectureContractTests, Phase1BackendGateKeepsOnlyDx12Enabled)
{
    EXPECT_EQ(
        NLS::Render::Settings::GetPhase1RequiredRuntimeBackend(),
        NLS::Render::Settings::EGraphicsBackend::DX12);

#if defined(_WIN32)
    EXPECT_TRUE(NLS::Render::Settings::IsBackendEnabledForCurrentBuild(
        NLS::Render::Settings::EGraphicsBackend::DX12));
#else
    EXPECT_FALSE(NLS::Render::Settings::IsBackendEnabledForCurrentBuild(
        NLS::Render::Settings::EGraphicsBackend::DX12));
#endif

    EXPECT_FALSE(NLS::Render::Settings::IsBackendEnabledForCurrentBuild(
        NLS::Render::Settings::EGraphicsBackend::VULKAN));
    EXPECT_FALSE(NLS::Render::Settings::IsBackendEnabledForCurrentBuild(
        NLS::Render::Settings::EGraphicsBackend::OPENGL));
    EXPECT_FALSE(NLS::Render::Settings::IsBackendEnabledForCurrentBuild(
        NLS::Render::Settings::EGraphicsBackend::DX11));
    EXPECT_FALSE(NLS::Render::Settings::IsBackendEnabledForCurrentBuild(
        NLS::Render::Settings::EGraphicsBackend::METAL));
}

TEST(UE5RenderArchitectureContractTests, Phase1RestrictionMessageRejectsNonDx12Backends)
{
    const auto dx12Restriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        NLS::Render::Settings::EGraphicsBackend::DX12,
        "Contract test");
#if defined(_WIN32)
    EXPECT_FALSE(dx12Restriction.has_value());
#else
    ASSERT_TRUE(dx12Restriction.has_value());
#endif

    const auto vkRestriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        NLS::Render::Settings::EGraphicsBackend::VULKAN,
        "Contract test");
    ASSERT_TRUE(vkRestriction.has_value());
    EXPECT_NE(vkRestriction->find("only supports DX12"), std::string::npos);
    EXPECT_NE(vkRestriction->find("Vulkan"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Phase1ThreadedFoundationOnlyAcceptsDx12)
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
}

TEST(UE5RenderArchitectureContractTests, Phase1OrderedParallelSubmissionOnlyAcceptsDx12)
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
    EXPECT_FALSE(NLS::Render::Settings::SupportsOrderedParallelCommandSubmissionPath(
        NLS::Render::RHI::NativeBackendType::Vulkan,
        capabilities));
}

TEST(UE5RenderArchitectureContractTests, OwnershipStagesFlowFromRenderFrameInputToRenderFrameBuild)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 91u;
    snapshot.sceneRevision = 5u;
    snapshot.renderWidth = 1280u;
    snapshot.renderHeight = 720u;
    snapshot.targetsSwapchain = true;
    snapshot.hasSceneInput = true;
    snapshot.sceneActorCount = 4u;
    snapshot.visibleOpaqueDrawCount = 2u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot));

    size_t renderSlotIndex = 0u;
    NLS::Render::Context::RenderFrameInput renderFrameInput;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderFrameBuild(&renderSlotIndex, &renderFrameInput));
    EXPECT_TRUE(renderFrameInput.immutable);
    EXPECT_EQ(renderFrameInput.frameId, snapshot.frameId);
    EXPECT_EQ(renderFrameInput.sceneRevision, snapshot.sceneRevision);
    EXPECT_EQ(renderFrameInput.visibleDrawCount, 2u);

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = renderFrameInput.frameId;
    package.targetsSwapchain = true;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.visibleDrawCount = 2u;
    package.passPlanCount = 1u;
    package.drawCommandCount = 2u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(renderSlotIndex, package));

    size_t rhiSlotIndex = 0u;
    NLS::Render::Context::RenderFrameBuild renderFrameBuild;
    ASSERT_TRUE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));
    EXPECT_EQ(rhiSlotIndex, renderSlotIndex);
    EXPECT_TRUE(renderFrameBuild.renderThreadOwned);
    EXPECT_EQ(renderFrameBuild.frameId, package.frameId);
    EXPECT_TRUE(renderFrameBuild.hasVisibleDraws);
    EXPECT_EQ(renderFrameBuild.drawCommandCount, package.drawCommandCount);
}

TEST(UE5RenderArchitectureContractTests, PreparedBuilderPackagesDoNotCarryRendererLocalPassScheduling)
{
    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 12u;
    snapshot.renderWidth = 1280u;
    snapshot.renderHeight = 720u;
    snapshot.targetsSwapchain = false;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 1u;
    snapshot.visibleSkyboxDrawCount = 1u;
    snapshot.visibleTransparentDrawCount = 1u;
    snapshot.recordedDrawCommands.resize(3u);

    const auto package = NLS::Render::Context::BuildSnapshotOwnedRenderScenePackage(
        snapshot,
        NLS::Render::Context::SnapshotRenderScenePackageBuildMode::SkipDefaultPassInputs);

    EXPECT_EQ(package.opaqueDrawCount, 1u);
    EXPECT_EQ(package.skyboxDrawCount, 1u);
    EXPECT_EQ(package.transparentDrawCount, 1u);
    EXPECT_EQ(package.recordedDrawCommands.size(), 3u);
    EXPECT_EQ(package.passPlanCount, 0u);
    EXPECT_FALSE(package.containsCommandInputs);
    EXPECT_TRUE(package.passCommandInputs.empty());
    EXPECT_FALSE(package.containsParallelCommandWorkUnits);
    EXPECT_TRUE(package.parallelCommandWorkUnits.empty());
}

TEST(UE5RenderArchitectureContractTests, RuntimeCompilationRebuildsPreparedPassPlanFromForwardSceneMetadata)
{
    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 33u;
    snapshot.renderWidth = 1280u;
    snapshot.renderHeight = 720u;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 2u;
    snapshot.visibleSkyboxDrawCount = 1u;
    snapshot.visibleTransparentDrawCount = 1u;
    snapshot.recordedDrawCommands.resize(4u);

    auto package = NLS::Render::Context::BuildSnapshotOwnedRenderScenePackage(
        snapshot,
        NLS::Render::Context::SnapshotRenderScenePackageBuildMode::SkipDefaultPassInputs);
    ASSERT_TRUE(package.passCommandInputs.empty());

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = snapshot.renderWidth;
    frameDescriptor.renderHeight = snapshot.renderHeight;
    frameDescriptor.camera = &camera;

    const auto compiledExecution = NLS::Render::FrameGraph::CompileAndApplyThreadedRenderSceneExecution(
        package,
        frameDescriptor,
        -1,
        -1,
        NLS::Render::FrameGraph::GetForwardScenePassDescriptors(),
        [&package](const auto& compiledPasses)
        {
            std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;
            passInputs.reserve(compiledPasses.size());

            for (const auto& compiledPass : compiledPasses)
            {
                NLS::Render::Context::RenderPassCommandInput passInput;
                passInput.kind = compiledPass.metadata.commandKind;
                passInput.debugName = compiledPass.metadata.graphPassName;

                switch (compiledPass.metadata.commandKind)
                {
                case NLS::Render::Context::RenderPassCommandKind::Opaque:
                    passInput.drawCount = package.opaqueDrawCount;
                    break;
                case NLS::Render::Context::RenderPassCommandKind::Skybox:
                    passInput.drawCount = package.skyboxDrawCount;
                    break;
                case NLS::Render::Context::RenderPassCommandKind::Transparent:
                    passInput.drawCount = package.transparentDrawCount;
                    break;
                default:
                    break;
                }

                if (passInput.drawCount == 0u)
                    continue;

                passInputs.push_back(std::move(passInput));
            }

            return passInputs;
        });

    const auto& graphPasses = compiledExecution.graphPasses;
    ASSERT_EQ(graphPasses.size(), 3u);
    EXPECT_EQ(graphPasses[0].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(graphPasses[1].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(graphPasses[2].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Transparent);

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(package.passCommandInputs[2].kind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_TRUE(package.containsParallelCommandWorkUnits);
    EXPECT_EQ(package.parallelCommandWorkUnitCount, 3u);
}

TEST(UE5RenderArchitectureContractTests, PreparedBuilderResolutionIgnoresSnapshotHarnessPackageBuilders)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 44u;
    snapshot.renderWidth = 640u;
    snapshot.renderHeight = 360u;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 1u;

    ASSERT_TRUE(lifecycle.TryPublishPreparedFrameBuilder(
        snapshot,
        []()
        {
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = 44u;
            package.hasVisibleDraws = true;
            package.visibleDrawCount = 1u;
            package.frameDataReady = true;
            package.objectDataReady = true;
            return package;
        }));

    size_t slotIndex = 99u;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderScene(&slotIndex, nullptr));

    NLS::Render::Context::RenderScenePreparingResolutionDesc desc;
    desc.buildSnapshotHarnessRenderScenePackage = [](const NLS::Render::Context::FrameSnapshot&)
    {
        NLS::Render::Context::RenderScenePackage package;
        package.frameId = 900u;
        package.hasVisibleDraws = true;
        package.visibleDrawCount = 99u;
        package.frameDataReady = true;
        package.objectDataReady = true;
        return package;
    };

    ASSERT_TRUE(lifecycle.ResolveRenderScenePreparing(slotIndex, desc));

    const auto* slot = lifecycle.PeekSlot(slotIndex);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(slot->renderScenePackage.has_value());
    EXPECT_EQ(slot->publishOrigin, NLS::Render::Context::ThreadedFramePublishOrigin::PreparedBuilder);
    EXPECT_EQ(slot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::RendererPrepared);
    EXPECT_EQ(slot->renderScenePackage->frameId, 44u);
    EXPECT_EQ(slot->renderScenePackage->visibleDrawCount, 1u);
}

TEST(UE5RenderArchitectureContractTests, EditorViewportSourcesDoNotBypassPresentOrUiSubmission)
{
    const auto sceneViewSource = ReadRepositorySource("Project/Editor/Panels/SceneView.cpp");
    const auto gameViewSource = ReadRepositorySource("Project/Editor/Panels/GameView.cpp");
    const auto assetViewSource = ReadRepositorySource("Project/Editor/Panels/AssetView.cpp");
    const auto aViewSource = ReadRepositorySource("Project/Editor/Panels/AView.cpp");
    const auto debugSceneRendererSource = ReadRepositorySource("Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const auto gridRenderPassSource = ReadRepositorySource("Project/Editor/Rendering/GridRenderPass.cpp");
    const auto pickingRenderPassSource = ReadRepositorySource("Project/Editor/Rendering/PickingRenderPass.cpp");
    const auto sceneRenderGraphBuilderSource = ReadRepositorySource("Runtime/Rendering/FrameGraph/SceneRenderGraphBuilder.cpp");
    const auto externalResourceBridgeSource = ReadRepositorySource("Runtime/Rendering/FrameGraph/ExternalResourceBridge.cpp");

    for (const auto* source : {
             &sceneViewSource,
             &gameViewSource,
             &assetViewSource,
             &debugSceneRendererSource,
             &pickingRenderPassSource })
    {
        EXPECT_EQ(source->find("PresentSwapchain("), std::string::npos);
        EXPECT_EQ(source->find("SubmitUIRendering("), std::string::npos);
        EXPECT_EQ(source->find("MakeCurrentContext("), std::string::npos);
        EXPECT_EQ(source->find("AcquireNextImage("), std::string::npos);
    }

    EXPECT_NE(aViewSource.find("SetExternalSceneOutputFramebuffer"), std::string::npos);
    EXPECT_NE(debugSceneRendererSource.find("FrameTargetsSwapchain"), std::string::npos);
    EXPECT_NE(gridRenderPassSource.find("FrameTargetsSwapchain"), std::string::npos);
    EXPECT_NE(sceneRenderGraphBuilderSource.find("ResolveExternalSceneOutputFramebuffer"), std::string::npos);
    EXPECT_NE(externalResourceBridgeSource.find("ResolveExternalSceneOutputFramebuffer"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, BackendNamedEditorBranchesRemainQuarantinedToViewOrientationAndPickingCoordinates)
{
    const auto aViewSource = ReadRepositorySource("Project/Editor/Panels/AView.cpp");
    const auto sceneViewSource = ReadRepositorySource("Project/Editor/Panels/SceneView.cpp");
    const auto gameViewSource = ReadRepositorySource("Project/Editor/Panels/GameView.cpp");
    const auto assetViewSource = ReadRepositorySource("Project/Editor/Panels/AssetView.cpp");
    const auto uiManagerHeaderSource = ReadRepositorySource("Runtime/UI/UIManager.h");
    const auto uiManagerSource = ReadRepositorySource("Runtime/UI/UIManager.cpp");

    EXPECT_EQ(CountOccurrences(aViewSource, "EGraphicsBackend::OPENGL"), 0u);
    EXPECT_NE(aViewSource.find("ShouldFlipPresentedRenderTargetVertically"), std::string::npos);

    EXPECT_EQ(CountOccurrences(sceneViewSource, "EGraphicsBackend::OPENGL"), 0u);
    EXPECT_NE(sceneViewSource.find("UsesBottomLeftRenderTargetOrigin"), std::string::npos);

    EXPECT_EQ(CountOccurrences(gameViewSource, "EGraphicsBackend::OPENGL"), 0u);
    EXPECT_EQ(CountOccurrences(assetViewSource, "EGraphicsBackend::OPENGL"), 0u);
    EXPECT_NE(uiManagerHeaderSource.find("ShouldFlipPresentedRenderTargetVertically"), std::string::npos);
    EXPECT_NE(uiManagerHeaderSource.find("UsesBottomLeftRenderTargetOrigin"), std::string::npos);
    EXPECT_NE(uiManagerSource.find("GetRenderSurfaceConvention"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, RenderSurfaceConventionEncodesViewportOrientationWithoutEditorBackendBranches)
{
    const auto dx12Convention = NLS::Render::RHI::GetRenderSurfaceConvention(
        NLS::Render::RHI::NativeBackendType::DX12);
    EXPECT_FALSE(dx12Convention.RequiresPresentedTextureVerticalFlip());
    EXPECT_FALSE(dx12Convention.UsesBottomLeftRenderTargetOrigin());

    const auto openGlConvention = NLS::Render::RHI::GetRenderSurfaceConvention(
        NLS::Render::RHI::NativeBackendType::OpenGL);
    EXPECT_TRUE(openGlConvention.RequiresPresentedTextureVerticalFlip());
    EXPECT_TRUE(openGlConvention.UsesBottomLeftRenderTargetOrigin());
}

TEST(UE5RenderArchitectureContractTests, DebugSceneRendererThreadedBuilderCarriesExplicitHelperPassMetadata)
{
    const auto debugSceneRendererSource = ReadRepositorySource("Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const auto gridRenderPassSource = ReadRepositorySource("Project/Editor/Rendering/GridRenderPass.cpp");
    const auto outlineRendererSource = ReadRepositorySource("Project/Editor/Rendering/OutlineRenderer.cpp");
    const auto gizmoRendererSource = ReadRepositorySource("Project/Editor/Rendering/GizmoRenderer.cpp");

    EXPECT_NE(debugSceneRendererSource.find("BuildPreparedRenderSceneBuilder"), std::string::npos);
    EXPECT_NE(debugSceneRendererSource.find("RenderPassCommandKind::Helper"), std::string::npos);
    EXPECT_NE(debugSceneRendererSource.find("BuildPreparedComputeAndScenePassInputs"), std::string::npos);
    EXPECT_NE(debugSceneRendererSource.find("visibleHelperDrawCount"), std::string::npos);
    EXPECT_NE(debugSceneRendererSource.find("EditorGridPass"), std::string::npos);
    EXPECT_NE(debugSceneRendererSource.find("EditorSelectionPass"), std::string::npos);
    EXPECT_NE(debugSceneRendererSource.find("EditorPickingPass"), std::string::npos);
    EXPECT_NE(gridRenderPassSource.find("BuildThreadedPassInput"), std::string::npos);
    EXPECT_NE(gridRenderPassSource.find("GetPreparedThreadedPassInput"), std::string::npos);
    EXPECT_NE(outlineRendererSource.find("CaptureOutlineDrawCommands"), std::string::npos);
    EXPECT_NE(gizmoRendererSource.find("CaptureGizmoDrawCommands"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, EditorHelperSourcesExcludeRetiredFramebufferAndLocalReadbackPaths)
{
    const auto gridRenderPassSource = ReadRepositorySource("Project/Editor/Rendering/GridRenderPass.cpp");
    const auto outlineRendererSource = ReadRepositorySource("Project/Editor/Rendering/OutlineRenderer.cpp");
    const auto gizmoRendererSource = ReadRepositorySource("Project/Editor/Rendering/GizmoRenderer.cpp");
    const auto pickingRenderPassSource = ReadRepositorySource("Project/Editor/Rendering/PickingRenderPass.cpp");
    const auto debugModelRendererSource = ReadRepositorySource("Project/Editor/Rendering/DebugModelRenderer.cpp");

    for (const auto* source : {
             &gridRenderPassSource,
             &outlineRendererSource,
             &gizmoRendererSource,
             &pickingRenderPassSource })
    {
        EXPECT_EQ(source->find("ExecuteLegacyFramebufferPass"), std::string::npos);
        EXPECT_EQ(source->find("BeginLegacyFramebufferPass"), std::string::npos);
        EXPECT_EQ(source->find("ReadPixelsFromFramebufferTexture"), std::string::npos);
    }

    EXPECT_NE(debugModelRendererSource.find("CaptureModelDrawCommandsWithSingleMaterial"), std::string::npos);
    EXPECT_NE(debugModelRendererSource.find("CaptureRecordedDrawCommand"), std::string::npos);
    EXPECT_NE(outlineRendererSource.find("CaptureOutlineDrawCommands"), std::string::npos);
    EXPECT_NE(gizmoRendererSource.find("CaptureGizmoDrawCommands"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, BaseRendererSourcesExcludeRetiredFramebufferSurface)
{
    const auto baseRendererHeaderSource = ReadRepositorySource("Runtime/Rendering/Core/ABaseRenderer.h");
    const auto baseRendererSource = ReadRepositorySource("Runtime/Rendering/Core/ABaseRenderer.cpp");

    for (const auto* source : {
             &baseRendererHeaderSource,
             &baseRendererSource })
    {
        EXPECT_EQ(source->find("ReadPixelsFromFramebufferTexture"), std::string::npos);
        EXPECT_EQ(source->find("ExecuteLegacyFramebufferPass"), std::string::npos);
        EXPECT_EQ(source->find("ExecuteLegacyFramebufferReadbackPass"), std::string::npos);
        EXPECT_EQ(source->find("BeginLegacyFramebufferPass"), std::string::npos);
        EXPECT_EQ(source->find("EndLegacyFramebufferPass"), std::string::npos);
        EXPECT_EQ(source->find("ReadPixelsFromLegacyFramebuffer"), std::string::npos);
        EXPECT_EQ(source->find("BindLegacyOutputTarget"), std::string::npos);
        EXPECT_EQ(source->find("UnbindLegacyOutputTarget"), std::string::npos);
        EXPECT_EQ(source->find("BeginLegacyDrawSection"), std::string::npos);
        EXPECT_EQ(source->find("EndLegacyDrawSection"), std::string::npos);
        EXPECT_EQ(source->find("SupportsFramebufferReadback"), std::string::npos);
    }
}

TEST(UE5RenderArchitectureContractTests, FrameGraphRecordedRenderPassExecutionUsesRecordedOnlySurface)
{
    const auto executionPlanSource = ReadRepositorySource("Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h");

    EXPECT_NE(executionPlanSource.find("ExecuteRecordedRenderPass"), std::string::npos);
    EXPECT_EQ(executionPlanSource.find("TFallbackFn"), std::string::npos);
    EXPECT_EQ(executionPlanSource.find("drawFallback"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, RhiStandaloneSubmissionSurfaceUsesStandaloneExplicitFrameNaming)
{
    const auto driverAccessHeaderSource = ReadRepositorySource("Runtime/Rendering/Context/DriverAccess.h");
    const auto driverSource = ReadRepositorySource("Runtime/Rendering/Context/Driver.cpp");
    const auto rhiCoordinatorHeaderSource = ReadRepositorySource("Runtime/Rendering/Context/RhiThreadCoordinator.h");
    const auto rhiCoordinatorSource = ReadRepositorySource("Runtime/Rendering/Context/RhiThreadCoordinator.cpp");

    for (const auto* source : {
             &driverAccessHeaderSource,
             &driverSource,
             &rhiCoordinatorHeaderSource,
             &rhiCoordinatorSource })
    {
        EXPECT_EQ(source->find("DirectExplicitFrame"), std::string::npos);
        EXPECT_NE(source->find("StandaloneExplicitFrame"), std::string::npos);
    }
}

TEST(UE5RenderArchitectureContractTests, ThreadedSubmissionTelemetryUsesSerialPathNaming)
{
    const auto threadedLifecycleHeaderSource = ReadRepositorySource("Runtime/Rendering/Context/ThreadedRenderingLifecycle.h");
    const auto rhiCoordinatorSource = ReadRepositorySource("Runtime/Rendering/Context/RhiThreadCoordinator.cpp");

    EXPECT_EQ(threadedLifecycleHeaderSource.find("usedSerialCommandFallback"), std::string::npos);
    EXPECT_EQ(rhiCoordinatorSource.find("usedSerialCommandFallback"), std::string::npos);
    EXPECT_EQ(rhiCoordinatorSource.find("usedSerialFallback"), std::string::npos);
    EXPECT_NE(threadedLifecycleHeaderSource.find("usedSerialCommandPath"), std::string::npos);
    EXPECT_NE(rhiCoordinatorSource.find("usedSerialPath"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, RenderScenePreparingResolutionSurfaceUsesExplicitPackageResolutionNaming)
{
    const auto threadedLifecycleHeaderSource = ReadRepositorySource("Runtime/Rendering/Context/ThreadedRenderingLifecycle.h");
    const auto threadedLifecycleSource = ReadRepositorySource("Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp");
    const auto renderThreadCoordinatorSource = ReadRepositorySource("Runtime/Rendering/Context/RenderThreadCoordinator.cpp");

    for (const auto* source : {
             &threadedLifecycleHeaderSource,
             &threadedLifecycleSource,
             &renderThreadCoordinatorSource })
    {
        EXPECT_EQ(source->find("rejectedHarnessRenderSceneBuilder"), std::string::npos);
        EXPECT_EQ(source->find("missingPreparedRenderSceneBuilder"), std::string::npos);
        EXPECT_NE(source->find("buildSnapshotHarnessRenderScenePackage"), std::string::npos);
        EXPECT_NE(source->find("buildPreparedBuilderMissingRenderScenePackage"), std::string::npos);
    }
}

TEST(UE5RenderArchitectureContractTests, RenderThreadCoordinatorUsesEmptyRenderScenePackageNaming)
{
    const auto renderThreadCoordinatorSource = ReadRepositorySource("Runtime/Rendering/Context/RenderThreadCoordinator.cpp");

    EXPECT_EQ(renderThreadCoordinatorSource.find("BuildEmptyPreparedRenderScenePackage"), std::string::npos);
    EXPECT_NE(renderThreadCoordinatorSource.find("BuildEmptyRenderScenePackage"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, CoreRenderResourceWrappersUseBackendNeutralSurface)
{
    const auto vertexBufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/VertexBuffer.h");
    const auto vertexBufferInlSource = ReadRepositorySource("Runtime/Rendering/Buffers/VertexBuffer.inl");
    const auto indexBufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/IndexBuffer.h");
    const auto indexBufferCppSource = ReadRepositorySource("Runtime/Rendering/Buffers/IndexBuffer.cpp");
    const auto uniformBufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/UniformBuffer.h");
    const auto uniformBufferCppSource = ReadRepositorySource("Runtime/Rendering/Buffers/UniformBuffer.cpp");
    const auto shaderStorageBufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/ShaderStorageBuffer.h");
    const auto shaderStorageBufferCppSource = ReadRepositorySource("Runtime/Rendering/Buffers/ShaderStorageBuffer.cpp");
    const auto framebufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/Framebuffer.h");
    const auto multiFramebufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/MultiFramebuffer.h");
    const auto vertexArraySource = ReadRepositorySource("Runtime/Rendering/Buffers/VertexArray.h");
    const auto textureSource = ReadRepositorySource("Runtime/Rendering/Resources/Texture.h");

    for (const auto* source : {
             &vertexBufferSource,
             &indexBufferSource,
             &uniformBufferSource,
             &shaderStorageBufferSource,
             &framebufferSource,
             &multiFramebufferSource,
             &vertexArraySource,
             &textureSource })
    {
        EXPECT_EQ(source->find("Wraps OpenGL"), std::string::npos);
        EXPECT_EQ(source->find("OpenGL texture wrapper"), std::string::npos);
        EXPECT_EQ(source->find("always returns nullptr now"), std::string::npos);
        EXPECT_EQ(source->find("GetRHIBuffer("), std::string::npos);
        EXPECT_EQ(source->find("GetRHIBufferHandle("), std::string::npos);
        EXPECT_EQ(source->find("GetRHITexture("), std::string::npos);
        EXPECT_EQ(source->find("GetRHITextureHandle("), std::string::npos);
        EXPECT_EQ(source->find("GetID("), std::string::npos);
        EXPECT_EQ(source->find("IRHITexture"), std::string::npos);
        EXPECT_EQ(source->find("IRHIResource.h"), std::string::npos);
    }

    EXPECT_NE(vertexBufferSource.find("formal RHI surface"), std::string::npos);
    EXPECT_NE(indexBufferSource.find("formal RHI surface"), std::string::npos);
    EXPECT_NE(textureSource.find("formal RHI surface"), std::string::npos);
    EXPECT_NE(multiFramebufferSource.find("IsInitialized() const"), std::string::npos);
    EXPECT_EQ(uniformBufferSource.find("legacy call sites"), std::string::npos);
    EXPECT_EQ(vertexArraySource.find("m_bufferID"), std::string::npos);
    EXPECT_EQ(vertexBufferSource.find("void Bind();"), std::string::npos);
    EXPECT_EQ(vertexBufferSource.find("void Unbind();"), std::string::npos);
    EXPECT_EQ(vertexBufferInlSource.find("VertexBuffer<T>::Bind()"), std::string::npos);
    EXPECT_EQ(vertexBufferInlSource.find("VertexBuffer<T>::Unbind()"), std::string::npos);
    EXPECT_EQ(indexBufferSource.find("void Bind();"), std::string::npos);
    EXPECT_EQ(indexBufferSource.find("void Unbind();"), std::string::npos);
    EXPECT_EQ(indexBufferCppSource.find("IndexBuffer::Bind()"), std::string::npos);
    EXPECT_EQ(indexBufferCppSource.find("IndexBuffer::Unbind()"), std::string::npos);
    EXPECT_EQ(uniformBufferSource.find("void Bind(uint32_t p_bindingPoint);"), std::string::npos);
    EXPECT_EQ(uniformBufferSource.find("void Unbind();"), std::string::npos);
    EXPECT_EQ(uniformBufferCppSource.find("UniformBuffer::Bind("), std::string::npos);
    EXPECT_EQ(uniformBufferCppSource.find("UniformBuffer::Unbind()"), std::string::npos);
    EXPECT_EQ(shaderStorageBufferSource.find("void Bind(uint32_t p_bindingPoint);"), std::string::npos);
    EXPECT_EQ(shaderStorageBufferSource.find("void Unbind();"), std::string::npos);
    EXPECT_EQ(shaderStorageBufferCppSource.find("ShaderStorageBuffer::Bind("), std::string::npos);
    EXPECT_EQ(shaderStorageBufferCppSource.find("ShaderStorageBuffer::Unbind()"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, CoreResourceWrappersDoNotSwallowDriverAcquisitionFailures)
{
    const auto vertexBufferInlSource = ReadRepositorySource("Runtime/Rendering/Buffers/VertexBuffer.inl");
    const auto indexBufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/IndexBuffer.cpp");
    const auto uniformBufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/UniformBuffer.cpp");
    const auto shaderStorageBufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/ShaderStorageBuffer.inl");
    const auto textureSource = ReadRepositorySource("Runtime/Rendering/Resources/Texture.cpp");

    for (const auto* source : {
             &vertexBufferInlSource,
             &indexBufferSource,
             &uniformBufferSource,
             &shaderStorageBufferSource,
             &textureSource })
    {
        EXPECT_EQ(source->find("catch (...)"), std::string::npos);
        EXPECT_EQ(source->find("return nullptr;\n\t\t}"), std::string::npos);
    }

    EXPECT_NE(vertexBufferInlSource.find("RequireLocatedDriver(\"VertexBuffer\")"), std::string::npos);
    EXPECT_NE(indexBufferSource.find("RequireLocatedDriver(\"IndexBuffer\")"), std::string::npos);
    EXPECT_NE(uniformBufferSource.find("RequireLocatedDriver(\"UniformBuffer\")"), std::string::npos);
    EXPECT_NE(shaderStorageBufferSource.find("RequireLocatedDriver(\"ShaderStorageBuffer\")"), std::string::npos);
    EXPECT_NE(textureSource.find("RequireLocatedDriver(\"Texture::CreateRHITexture\")"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, ShaderStorageBufferCreatesRhiStorageOnlyFromPayloadUploads)
{
    const auto shaderStorageBufferHeaderSource = ReadRepositorySource("Runtime/Rendering/Buffers/ShaderStorageBuffer.h");
    const auto shaderStorageBufferCppSource = ReadRepositorySource("Runtime/Rendering/Buffers/ShaderStorageBuffer.cpp");
    const auto shaderStorageBufferInlSource = ReadRepositorySource("Runtime/Rendering/Buffers/ShaderStorageBuffer.inl");

    EXPECT_EQ(shaderStorageBufferCppSource.find("ToRHIBufferUsage"), std::string::npos);
    EXPECT_EQ(shaderStorageBufferCppSource.find("desc.size = 0"), std::string::npos);
    EXPECT_EQ(shaderStorageBufferCppSource.find("CreateBuffer(desc, nullptr)"), std::string::npos);
    EXPECT_EQ(shaderStorageBufferInlSource.find("if (m_explicitBuffer == nullptr)\n\t\t\treturn;"), std::string::npos);

    EXPECT_EQ(shaderStorageBufferHeaderSource.find("EAccessSpecifier"), std::string::npos);
    EXPECT_EQ(shaderStorageBufferHeaderSource.find("access hint"), std::string::npos);
    EXPECT_EQ(shaderStorageBufferHeaderSource.find("m_accessSpecifier"), std::string::npos);
    EXPECT_EQ(shaderStorageBufferCppSource.find("p_accessSpecifier"), std::string::npos);
    EXPECT_NE(shaderStorageBufferHeaderSource.find("ShaderStorageBuffer()"), std::string::npos);
    EXPECT_NE(shaderStorageBufferInlSource.find("if (p_data == nullptr || p_size == 0)"), std::string::npos);
    EXPECT_NE(shaderStorageBufferInlSource.find("desc.size = p_size"), std::string::npos);
    EXPECT_NE(shaderStorageBufferInlSource.find("desc.usage = NLS::Render::RHI::BufferUsageFlags::Storage"), std::string::npos);
    EXPECT_NE(shaderStorageBufferInlSource.find("m_explicitBuffer = device->CreateBuffer(desc, p_data)"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, MeshSurfaceUsesViewsAndRhiMeshInsteadOfLegacyBindUnbind)
{
    const auto iMeshSource = ReadRepositorySource("Runtime/Rendering/Resources/IMesh.h");
    const auto meshHeaderSource = ReadRepositorySource("Runtime/Rendering/Resources/Mesh.h");
    const auto meshSource = ReadRepositorySource("Runtime/Rendering/Resources/Mesh.cpp");
    const auto vertexArrayHeaderSource = ReadRepositorySource("Runtime/Rendering/Buffers/VertexArray.h");
    const auto vertexArraySource = ReadRepositorySource("Runtime/Rendering/Buffers/VertexArray.cpp");

    EXPECT_EQ(iMeshSource.find("virtual void Bind() const = 0;"), std::string::npos);
    EXPECT_EQ(iMeshSource.find("virtual void Unbind() const = 0;"), std::string::npos);
    EXPECT_EQ(meshHeaderSource.find("virtual void Bind() const override;"), std::string::npos);
    EXPECT_EQ(meshHeaderSource.find("virtual void Unbind() const override;"), std::string::npos);
    EXPECT_EQ(meshSource.find("void Mesh::Bind() const"), std::string::npos);
    EXPECT_EQ(meshSource.find("void Mesh::Unbind() const"), std::string::npos);
    EXPECT_EQ(meshSource.find("m_vertexArray.Bind()"), std::string::npos);
    EXPECT_EQ(meshSource.find("m_vertexArray.Unbind()"), std::string::npos);
    EXPECT_EQ(vertexArrayHeaderSource.find("void Bind() const;"), std::string::npos);
    EXPECT_EQ(vertexArrayHeaderSource.find("void Unbind() const;"), std::string::npos);
    EXPECT_EQ(vertexArraySource.find("VertexArray::Bind()"), std::string::npos);
    EXPECT_EQ(vertexArraySource.find("VertexArray::Unbind()"), std::string::npos);

    EXPECT_NE(iMeshSource.find("GetVertexBufferView() const"), std::string::npos);
    EXPECT_NE(iMeshSource.find("GetRHIMesh() const"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, TextureSurfaceUsesExplicitHandlesAndUploadWithoutLegacyBindUnbind)
{
    const auto textureSource = ReadRepositorySource("Runtime/Rendering/Resources/Texture.h");
    const auto texture2DHeaderSource = ReadRepositorySource("Runtime/Rendering/Resources/Texture2D.h");
    const auto texture2DSource = ReadRepositorySource("Runtime/Rendering/Resources/Texture2D.cpp");
    const auto textureCubeHeaderSource = ReadRepositorySource("Runtime/Rendering/Resources/TextureCube.h");
    const auto textureCubeSource = ReadRepositorySource("Runtime/Rendering/Resources/TextureCube.cpp");
    const auto textureLoaderSource = ReadRepositorySource("Runtime/Rendering/Resources/Loaders/TextureLoader.cpp");

    EXPECT_EQ(textureSource.find("virtual void Bind("), std::string::npos);
    EXPECT_EQ(textureSource.find("virtual void Unbind() const"), std::string::npos);
    EXPECT_EQ(texture2DHeaderSource.find("virtual void Bind("), std::string::npos);
    EXPECT_EQ(texture2DHeaderSource.find("virtual void Unbind() const"), std::string::npos);
    EXPECT_EQ(texture2DSource.find("void Texture2D::Bind("), std::string::npos);
    EXPECT_EQ(texture2DSource.find("void Texture2D::Unbind() const"), std::string::npos);
    EXPECT_EQ(textureCubeHeaderSource.find("virtual void Bind("), std::string::npos);
    EXPECT_EQ(textureCubeHeaderSource.find("virtual void Unbind() const"), std::string::npos);
    EXPECT_EQ(textureCubeSource.find("void TextureCube::Bind("), std::string::npos);
    EXPECT_EQ(textureCubeSource.find("void TextureCube::Unbind() const"), std::string::npos);
    EXPECT_EQ(textureLoaderSource.find("cubeMap->Bind()"), std::string::npos);
    EXPECT_EQ(textureLoaderSource.find("cubeMap->Unbind()"), std::string::npos);
    EXPECT_EQ(textureLoaderSource.find("texture->Bind()"), std::string::npos);
    EXPECT_EQ(textureLoaderSource.find("texture->Unbind()"), std::string::npos);

    EXPECT_NE(textureSource.find("GetTextureHandle() const"), std::string::npos);
    EXPECT_NE(textureSource.find("GetOrCreateExplicitTextureView"), std::string::npos);
    EXPECT_NE(texture2DSource.find("RecreateRHITextureIfNeeded"), std::string::npos);
    EXPECT_NE(textureCubeSource.find("RecreateRHITextureIfNeeded"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, FramebufferSurfaceUsesExplicitViewsWithoutLegacyBindUnbind)
{
    const auto framebufferHeaderSource = ReadRepositorySource("Runtime/Rendering/Buffers/Framebuffer.h");
    const auto framebufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/Framebuffer.cpp");
    const auto multiFramebufferHeaderSource = ReadRepositorySource("Runtime/Rendering/Buffers/MultiFramebuffer.h");
    const auto multiFramebufferSource = ReadRepositorySource("Runtime/Rendering/Buffers/MultiFramebuffer.cpp");
    const auto baseRendererSource = ReadRepositorySource("Runtime/Rendering/Core/ABaseRenderer.cpp");

    EXPECT_EQ(framebufferHeaderSource.find("void Bind() const;"), std::string::npos);
    EXPECT_EQ(framebufferHeaderSource.find("void Unbind() const;"), std::string::npos);
    EXPECT_EQ(framebufferSource.find("Framebuffer::Bind()"), std::string::npos);
    EXPECT_EQ(framebufferSource.find("Framebuffer::Unbind()"), std::string::npos);
    EXPECT_EQ(multiFramebufferHeaderSource.find("void Bind() const;"), std::string::npos);
    EXPECT_EQ(multiFramebufferHeaderSource.find("void Unbind() const;"), std::string::npos);
    EXPECT_EQ(multiFramebufferSource.find("MultiFramebuffer::Bind()"), std::string::npos);
    EXPECT_EQ(multiFramebufferSource.find("MultiFramebuffer::Unbind()"), std::string::npos);
    EXPECT_EQ(baseRendererSource.find("externalOutputBuffer->Bind()"), std::string::npos);
    EXPECT_EQ(baseRendererSource.find("externalOutputBuffer->Unbind()"), std::string::npos);

    EXPECT_NE(framebufferHeaderSource.find("GetOrCreateExplicitColorView"), std::string::npos);
    EXPECT_NE(framebufferHeaderSource.find("GetOrCreateExplicitDepthStencilView"), std::string::npos);
    EXPECT_NE(multiFramebufferHeaderSource.find("GetOrCreateExplicitColorView"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("BeginRecordedRenderPass("), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, CentralDescriptorBindingCreationRemainsOnDriverMainline)
{
    const auto materialSource = ReadRepositorySource("Runtime/Rendering/Resources/Material.cpp");
    const auto lightGridPrepassSource = ReadRepositorySource("Runtime/Engine/Rendering/LightGridPrepass.cpp");
    const auto baseRendererSource = ReadRepositorySource("Runtime/Rendering/Core/ABaseRenderer.cpp");

    EXPECT_NE(materialSource.find("DriverRendererAccess::CreateExplicitBindingSet"), std::string::npos);
    EXPECT_EQ(materialSource.find("device->CreateBindingSet("), std::string::npos);

    EXPECT_NE(lightGridPrepassSource.find("DriverRendererAccess::CreateExplicitBindingSet"), std::string::npos);
    EXPECT_EQ(lightGridPrepassSource.find("device->CreateBindingSet("), std::string::npos);

    EXPECT_NE(baseRendererSource.find("CreateExplicitBindingSet(m_driver"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, MaterialMainlineUsesBindingSetPopulationWithoutUniformBufferDirectBinding)
{
    const auto source = ReadRepositorySource("Runtime/Rendering/Resources/Material.cpp");

    EXPECT_EQ(source.find("bufferState.buffer->Bind("), std::string::npos);
    EXPECT_EQ(source.find("bindingPoint = 0;"), std::string::npos);
    EXPECT_NE(source.find("state.bindingSet.SetBuffer("), std::string::npos);
    EXPECT_NE(source.find("CreateExplicitBindingSet("), std::string::npos);
    EXPECT_EQ(source.find("static_cast<int32_t>(constantBuffer.bindingIndex)"), std::string::npos);
    EXPECT_EQ(source.find("static_cast<int32_t>(property.bindingIndex)"), std::string::npos);
    EXPECT_EQ(source.find("state.bindingSet.SetTexture(binding.name, texture != nullptr ? texture : GetDefaultWhiteTexture2D())"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, EngineFrameObjectBindingProviderUsesExplicitBindingSetsWithoutUniformBufferDirectBinding)
{
    const auto source = ReadRepositorySource("Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp");

    EXPECT_EQ(source.find("m_engineBuffer->Bind("), std::string::npos);
    EXPECT_EQ(source.find("m_hlslFrameBuffer->Bind("), std::string::npos);
    EXPECT_EQ(source.find("writeBuffer.Bind("), std::string::npos);
    EXPECT_EQ(source.find("m_engineBuffer->Unbind("), std::string::npos);
    EXPECT_EQ(source.find("m_hlslFrameBuffer->Unbind("), std::string::npos);
    EXPECT_EQ(source.find("m_hlslObjectBuffer->Unbind("), std::string::npos);
    EXPECT_EQ(source.find("m_hlslObjectBufferAlt->Unbind("), std::string::npos);

    EXPECT_NE(source.find("CreateExplicitUniformBufferBindingSet"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, BindingSetSurfaceKeepsSingleExplicitHandleTruth)
{
    const auto bindingSetHeaderSource = ReadRepositorySource("Runtime/Rendering/Resources/BindingSet.h");
    const auto bindingSetSource = ReadRepositorySource("Runtime/Rendering/Resources/BindingSet.cpp");
    const auto resourceBindingSource = ReadRepositorySource("Runtime/Rendering/Resources/ResourceBinding.h");

    EXPECT_EQ(bindingSetHeaderSource.find("void SetTexture(const std::string& name, const Texture* texture);"), std::string::npos);
    EXPECT_EQ(bindingSetHeaderSource.find("const Texture* GetTexture(const std::string& name) const;"), std::string::npos);
    EXPECT_EQ(bindingSetHeaderSource.find("std::shared_ptr<RHI::RHITexture> GetTextureHandle(const std::string& name) const;"), std::string::npos);
    EXPECT_EQ(bindingSetHeaderSource.find("std::shared_ptr<RHI::RHIBuffer> GetBufferHandle(const std::string& name) const;"), std::string::npos);
    EXPECT_EQ(bindingSetHeaderSource.find("const ResourceBindingEntry* Find(const std::string& name) const;"), std::string::npos);
    EXPECT_EQ(bindingSetHeaderSource.find("const std::vector<ResourceBindingEntry>& Entries() const;"), std::string::npos);
    EXPECT_EQ(bindingSetSource.find("void BindingSet::SetTexture(const std::string& name, const Texture* texture)"), std::string::npos);
    EXPECT_EQ(bindingSetSource.find("const Texture* BindingSet::GetTexture("), std::string::npos);
    EXPECT_EQ(bindingSetSource.find("std::shared_ptr<RHI::RHITexture> BindingSet::GetTextureHandle("), std::string::npos);
    EXPECT_EQ(bindingSetSource.find("std::shared_ptr<RHI::RHIBuffer> BindingSet::GetBufferHandle("), std::string::npos);
    EXPECT_EQ(bindingSetSource.find("const ResourceBindingEntry* BindingSet::Find("), std::string::npos);
    EXPECT_EQ(bindingSetSource.find("const std::vector<ResourceBindingEntry>& BindingSet::Entries() const"), std::string::npos);

    EXPECT_EQ(resourceBindingSource.find("int32_t slot = -1;"), std::string::npos);
    EXPECT_EQ(resourceBindingSource.find("const Texture* texture = nullptr;"), std::string::npos);
    EXPECT_NE(resourceBindingSource.find("std::shared_ptr<RHI::RHITexture> textureHandle;"), std::string::npos);
    EXPECT_NE(resourceBindingSource.find("std::shared_ptr<RHI::RHIBuffer> bufferHandle;"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, ShaderLoaderDefaultsUnresolvedRuntimeCompilationToDx12Mainline)
{
    const auto shaderLoaderSource = ReadRepositorySource("Runtime/Rendering/Resources/Loaders/ShaderLoader.cpp");

    EXPECT_NE(shaderLoaderSource.find("return NLS::Render::Settings::GetPhase1RequiredRuntimeBackend();"), std::string::npos);
    EXPECT_EQ(shaderLoaderSource.find("return NLS::Render::Settings::GetPlatformDefaultGraphicsBackend();"), std::string::npos);
    EXPECT_EQ(shaderLoaderSource.find("activeBackend == NLS::Render::Settings::EGraphicsBackend::NONE"), std::string::npos);
    EXPECT_NE(shaderLoaderSource.find("const bool compileDxil = activeBackend == NLS::Render::Settings::EGraphicsBackend::DX12;"), std::string::npos);
    EXPECT_NE(shaderLoaderSource.find("const bool compileSpirv = activeBackend == NLS::Render::Settings::EGraphicsBackend::VULKAN ||"), std::string::npos);
    EXPECT_NE(shaderLoaderSource.find("activeBackend == NLS::Render::Settings::EGraphicsBackend::OPENGL;"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, LightGridPrepassResolvesComputeShadersThroughEngineShaderManager)
{
    const auto lightGridSource = ReadRepositorySource("Runtime/Engine/Rendering/LightGridPrepass.cpp");

    EXPECT_NE(lightGridSource.find("ShaderManager"), std::string::npos);
    EXPECT_NE(lightGridSource.find("\":Shaders/LightGridInjection.hlsl\""), std::string::npos);
    EXPECT_NE(lightGridSource.find("\":Shaders/LightGridCompact.hlsl\""), std::string::npos);
    EXPECT_EQ(lightGridSource.find("App/Assets/Engine/Shaders/LightGridInjection.hlsl"), std::string::npos);
    EXPECT_EQ(lightGridSource.find("App/Assets/Engine/Shaders/LightGridCompact.hlsl"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, LightGridPrepassDoesNotDestroyShaderManagerOwnedShaders)
{
    const auto lightGridSource = ReadRepositorySource("Runtime/Engine/Rendering/LightGridPrepass.cpp");
    const auto destructorSection = ExtractSection(
        lightGridSource,
        "LightGridPrepass::~LightGridPrepass()",
        "LightGridPrepass::PreparedFrameInputs");

    EXPECT_EQ(destructorSection.find("ShaderLoader::Destroy"), std::string::npos);
    EXPECT_EQ(destructorSection.find("delete m_injectionShader"), std::string::npos);
    EXPECT_EQ(destructorSection.find("delete m_compactShader"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, UiBridgeSelectorUsesPhase1BackendGateBeforeBackendSpecificBridgeCreation)
{
    const auto uiBridgeSource = ReadRepositorySource("Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp");

    EXPECT_NE(uiBridgeSource.find("Render::Settings::SupportsImGuiRendererBackend(resolvedGraphicsBackend)"), std::string::npos);
    EXPECT_NE(uiBridgeSource.find("Render::Settings::ToGraphicsBackend(resolvedNativeInfo.backend)"), std::string::npos);
    EXPECT_NE(uiBridgeSource.find("return std::make_unique<NullUIBridge>();"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, UiBridgeSelectorDoesNotCarryInactiveBackendCreationBranches)
{
    const auto uiBridgeSource = ReadRepositorySource("Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp");
    const auto uiBridgeInternalSource = ReadRepositorySource("Runtime/Rendering/RHI/Utils/RHIUIBridgeInternal.h");

    EXPECT_EQ(uiBridgeSource.find("CreateOpenGLRHIUIBridge"), std::string::npos);
    EXPECT_EQ(uiBridgeSource.find("CreateVulkanRHIUIBridge"), std::string::npos);
    EXPECT_EQ(uiBridgeSource.find("case NativeBackendType::OpenGL"), std::string::npos);
    EXPECT_EQ(uiBridgeSource.find("case NativeBackendType::Vulkan"), std::string::npos);
    EXPECT_EQ(uiBridgeInternalSource.find("CreateOpenGLRHIUIBridge"), std::string::npos);
    EXPECT_EQ(uiBridgeInternalSource.find("CreateVulkanRHIUIBridge"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, RhiDeviceFactoryTopLevelSelectorKeepsDx12AsItsOnlyExecutableRuntimePath)
{
    const auto source = ReadRepositorySource("Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp");
    const auto selectorSource = ExtractSection(
        source,
        "std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateRhiDevice(",
        "std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX12Device(");

    EXPECT_NE(selectorSource.find("settings.graphicsBackend != NLS::Render::Settings::EGraphicsBackend::DX12"), std::string::npos);
    EXPECT_NE(selectorSource.find("phase-1 runtime only allows DX12 through the top-level selector"), std::string::npos);
    EXPECT_NE(selectorSource.find("CreateDX12Device(settings)"), std::string::npos);

    EXPECT_EQ(selectorSource.find("CreateDX11RhiDevice"), std::string::npos);
    EXPECT_EQ(selectorSource.find("CreateOpenGLRhiDevice"), std::string::npos);
    EXPECT_EQ(selectorSource.find("CreateVulkanDevice(settings)"), std::string::npos);
    EXPECT_EQ(selectorSource.find("case NLS::Render::Settings::EGraphicsBackend::VULKAN"), std::string::npos);
    EXPECT_EQ(selectorSource.find("case NLS::Render::Settings::EGraphicsBackend::METAL"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, RhiDeviceFactoryDoesNotCarryInactiveBackendCreationCode)
{
    const auto source = ReadRepositorySource("Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp");

    EXPECT_EQ(source.find("VulkanExplicitDeviceFactory"), std::string::npos);
    EXPECT_EQ(source.find("MetalExplicitDeviceFactory"), std::string::npos);
    EXPECT_EQ(source.find("CreateVulkanDevice"), std::string::npos);
    EXPECT_EQ(source.find("VK_USE_PLATFORM"), std::string::npos);
    EXPECT_EQ(source.find("VkApplicationInfo"), std::string::npos);
    EXPECT_EQ(source.find("vkCreateInstance"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, AcceptedDx12RhiSourcesAvoidFallbackTerminology)
{
    const auto rhiDeviceFactorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp");
    const auto dx12DeviceFactorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");

    EXPECT_EQ(rhiDeviceFactorySource.find("fallback"), std::string::npos);
    EXPECT_EQ(rhiDeviceFactorySource.find("Fallback"), std::string::npos);
    EXPECT_EQ(dx12DeviceFactorySource.find("fallback"), std::string::npos);
    EXPECT_EQ(dx12DeviceFactorySource.find("Fallback"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12SemaphoreUsesMonotonicFenceValues)
{
    const auto factorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");
    const auto syncHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Synchronization.h");
    const auto syncSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Synchronization.cpp");

    EXPECT_EQ(factorySource.find("UINT64 GetSignalValue() const { return 1u; }"), std::string::npos);
    EXPECT_NE(syncHeaderSource.find("UINT64 GetWaitValue() const"), std::string::npos);
    EXPECT_NE(syncHeaderSource.find("bool SignalOnCpu()"), std::string::npos);
    EXPECT_NE(syncHeaderSource.find("bool SignalOnQueue(ID3D12CommandQueue* queue)"), std::string::npos);
    EXPECT_NE(syncSource.find("++m_signalValue"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12SynchronizationObjectsLiveInDedicatedModule)
{
    const auto repositoryRoot = GetRepositoryRoot();
    const auto factorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");
    const auto syncHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Synchronization.h");
    const auto syncSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Synchronization.cpp");

    EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Synchronization.h"));
    EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Synchronization.cpp"));
    EXPECT_NE(factorySource.find("#include \"Rendering/RHI/Backends/DX12/DX12Synchronization.h\""), std::string::npos);
    EXPECT_EQ(factorySource.find("class NativeDX12Fence final"), std::string::npos);
    EXPECT_EQ(factorySource.find("class NativeDX12Semaphore final"), std::string::npos);
    EXPECT_NE(syncHeaderSource.find("class NativeDX12Fence final"), std::string::npos);
    EXPECT_NE(syncHeaderSource.find("class NativeDX12Semaphore final"), std::string::npos);
    EXPECT_NE(syncSource.find("NativeDX12Fence::Reset()"), std::string::npos);
	EXPECT_NE(syncSource.find("NativeDX12Semaphore::SignalOnQueue"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12QueueObjectsLiveInDedicatedModule)
{
	const auto repositoryRoot = GetRepositoryRoot();
	const auto factorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");
	const auto queueHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Queue.h");
	const auto queueSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp");

	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.h"));
	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp"));
	EXPECT_NE(factorySource.find("#include \"Rendering/RHI/Backends/DX12/DX12Queue.h\""), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12Queue final"), std::string::npos);
	EXPECT_EQ(factorySource.find("void NativeDX12Queue::Submit"), std::string::npos);
	EXPECT_NE(queueHeaderSource.find("class NativeDX12Queue final"), std::string::npos);
	EXPECT_NE(queueSource.find("NativeDX12Queue::Submit"), std::string::npos);
	EXPECT_NE(queueSource.find("NativeDX12Queue::Present"), std::string::npos);
	EXPECT_NE(queueSource.find("GetNativeCommandBuffer()"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12CommandObjectsLiveInDedicatedModule)
{
	const auto repositoryRoot = GetRepositoryRoot();
	const auto factorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");
	const auto commandHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Command.h");
	const auto commandSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");

	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Command.h"));
	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp"));
	EXPECT_NE(factorySource.find("#include \"Rendering/RHI/Backends/DX12/DX12Command.h\""), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12CommandBuffer final"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12CommandPool final"), std::string::npos);
	EXPECT_EQ(factorySource.find("void NativeDX12CommandBuffer::BindGraphicsPipeline"), std::string::npos);
	EXPECT_EQ(factorySource.find("void NativeDX12CommandBuffer::BindBindingSet"), std::string::npos);
	EXPECT_NE(commandHeaderSource.find("class NativeDX12CommandBuffer final"), std::string::npos);
	EXPECT_NE(commandHeaderSource.find("class NativeDX12CommandPool final"), std::string::npos);
	EXPECT_NE(commandSource.find("NativeDX12CommandBuffer::BindGraphicsPipeline"), std::string::npos);
	EXPECT_NE(commandSource.find("NativeDX12CommandBuffer::BindComputePipeline"), std::string::npos);
	EXPECT_NE(commandSource.find("NativeDX12CommandBuffer::BindBindingSet"), std::string::npos);
	EXPECT_NE(commandSource.find("IDX12GraphicsPipelineAccess"), std::string::npos);
	EXPECT_NE(commandSource.find("IDX12ComputePipelineAccess"), std::string::npos);
	EXPECT_NE(commandSource.find("IDX12BindingSetAccess"), std::string::npos);
	EXPECT_NE(commandSource.find("SetComputeRootSignature"), std::string::npos);
	EXPECT_NE(commandSource.find("SetComputeRootDescriptorTable"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12BindingSetResolutionUsesNativeHandleInsteadOfRtti)
{
	const auto bindingHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Core/RHIBinding.h");
	const auto driverSource = ReadRepositorySource("Runtime/Rendering/Context/Driver.cpp");
	const auto commandSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp");
	const auto descriptorHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.h");
	const auto descriptorSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp");

	EXPECT_NE(bindingHeaderSource.find("GetNativeBindingSetHandle"), std::string::npos);
	EXPECT_NE(driverSource.find("GetNativeBindingSetHandle"), std::string::npos);
	EXPECT_NE(descriptorHeaderSource.find("GetNativeBindingSetHandle"), std::string::npos);
	EXPECT_NE(descriptorSource.find("NativeDX12BindingSet::GetNativeBindingSetHandle"), std::string::npos);
	EXPECT_NE(commandSource.find("GetNativeBindingSetHandle()"), std::string::npos);
	EXPECT_EQ(commandSource.find("dynamic_cast<IDX12BindingSetAccess*>"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12SwapchainObjectsLiveInDedicatedModule)
{
	const auto repositoryRoot = GetRepositoryRoot();
	const auto factorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");
	const auto swapchainHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Swapchain.h");
	const auto swapchainSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Swapchain.cpp");

	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Swapchain.h"));
	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Swapchain.cpp"));
	EXPECT_NE(factorySource.find("#include \"Rendering/RHI/Backends/DX12/DX12Swapchain.h\""), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12BackbufferTexture final"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12BackbufferView final"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12Swapchain final"), std::string::npos);
	EXPECT_EQ(factorySource.find("CreateSwapChainForHwnd"), std::string::npos);
	EXPECT_NE(factorySource.find("CreateNativeDX12Swapchain"), std::string::npos);
	EXPECT_NE(swapchainHeaderSource.find("class NativeDX12Swapchain final"), std::string::npos);
	EXPECT_NE(swapchainHeaderSource.find("CreateNativeDX12Swapchain"), std::string::npos);
	EXPECT_NE(swapchainSource.find("CreateNativeDX12Swapchain"), std::string::npos);
	EXPECT_NE(swapchainSource.find("CreateSwapChainForHwnd"), std::string::npos);
	EXPECT_NE(swapchainSource.find("NativeDX12Swapchain::AcquireNextImage"), std::string::npos);
	EXPECT_NE(swapchainSource.find("NativeDX12Swapchain::RecreateBackbufferViews"), std::string::npos);
	EXPECT_NE(swapchainSource.find("NativeDX12BackbufferView"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12DescriptorObjectsLiveInDedicatedModule)
{
	const auto repositoryRoot = GetRepositoryRoot();
	const auto factorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");
	const auto descriptorHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.h");
	const auto descriptorSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp");

	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.h"));
	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp"));
	EXPECT_NE(factorySource.find("#include \"Rendering/RHI/Backends/DX12/DX12Descriptor.h\""), std::string::npos);
	EXPECT_EQ(factorySource.find("class DX12ShaderVisibleDescriptorHeapAllocator"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12BindingSet final"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12BindingLayout final"), std::string::npos);
	EXPECT_EQ(factorySource.find("WriteNullStructuredBufferDescriptor"), std::string::npos);
	EXPECT_EQ(factorySource.find("WriteResourceDescriptor"), std::string::npos);
	EXPECT_NE(descriptorHeaderSource.find("class DX12ShaderVisibleDescriptorHeapAllocator"), std::string::npos);
	EXPECT_NE(descriptorHeaderSource.find("class NativeDX12BindingSet final"), std::string::npos);
	EXPECT_NE(descriptorHeaderSource.find("class NativeDX12BindingLayout final"), std::string::npos);
	EXPECT_NE(descriptorSource.find("WriteNullStructuredBufferDescriptor"), std::string::npos);
	EXPECT_NE(descriptorSource.find("WriteNullStorageBufferDescriptor"), std::string::npos);
	EXPECT_NE(descriptorSource.find("WriteResourceDescriptor"), std::string::npos);
	EXPECT_NE(descriptorSource.find("CreateShaderResourceView(nullptr"), std::string::npos);
	EXPECT_NE(descriptorSource.find("CreateUnorderedAccessView(nullptr"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12DeviceConstructionLivesInDedicatedModule)
{
	const auto repositoryRoot = GetRepositoryRoot();
	const auto factorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");
	const auto deviceHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Device.h");
	const auto deviceSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp");

	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Device.h"));
	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp"));
	EXPECT_NE(factorySource.find("#include \"Rendering/RHI/Backends/DX12/DX12Device.h\""), std::string::npos);
	EXPECT_NE(factorySource.find("CreateDX12DeviceResources(debugMode)"), std::string::npos);
	EXPECT_NE(factorySource.find("CreateNativeDX12ExplicitDevice("), std::string::npos);
	EXPECT_EQ(factorySource.find("CreateDXGIFactory2("), std::string::npos);
	EXPECT_EQ(factorySource.find("EnumAdapters1("), std::string::npos);
	EXPECT_EQ(factorySource.find("D3D12CreateDevice("), std::string::npos);
	EXPECT_EQ(factorySource.find("RHIDeviceCapabilities capabilities"), std::string::npos);
	EXPECT_EQ(factorySource.find("supportsDedicatedComputeQueue ="), std::string::npos);
	EXPECT_NE(deviceHeaderSource.find("DX12DeviceResources"), std::string::npos);
	EXPECT_NE(deviceHeaderSource.find("CreateDX12DeviceResources"), std::string::npos);
	EXPECT_NE(deviceSource.find("CreateDXGIFactory2("), std::string::npos);
	EXPECT_NE(deviceSource.find("EnumAdapters1("), std::string::npos);
	EXPECT_NE(deviceSource.find("D3D12CreateDevice("), std::string::npos);
	EXPECT_NE(deviceSource.find("RHIDeviceCapabilities capabilities"), std::string::npos);
	EXPECT_NE(deviceSource.find("supportsDedicatedComputeQueue ="), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12ResourceObjectsLiveInDedicatedModule)
{
	const auto repositoryRoot = GetRepositoryRoot();
	const auto factorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");
	const auto resourceHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Resource.h");
	const auto resourceSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp");

	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Resource.h"));
	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp"));
	EXPECT_NE(factorySource.find("#include \"Rendering/RHI/Backends/DX12/DX12Resource.h\""), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12Buffer final"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12Texture final"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12TextureView final"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12Sampler final"), std::string::npos);
	EXPECT_EQ(factorySource.find("UploadInitialTextureData("), std::string::npos);
	EXPECT_NE(resourceHeaderSource.find("class NativeDX12Buffer final"), std::string::npos);
	EXPECT_NE(resourceHeaderSource.find("class NativeDX12Texture final"), std::string::npos);
	EXPECT_NE(resourceHeaderSource.find("class NativeDX12TextureView final"), std::string::npos);
	EXPECT_NE(resourceHeaderSource.find("class NativeDX12Sampler final"), std::string::npos);
	EXPECT_NE(resourceSource.find("UploadInitialTextureData("), std::string::npos);
	EXPECT_NE(resourceSource.find("BuildDX12TextureUploadPlan"), std::string::npos);
	EXPECT_NE(resourceSource.find("BuildDX12TextureViewDescriptorSet"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12PipelineObjectsLiveInDedicatedModule)
{
	const auto repositoryRoot = GetRepositoryRoot();
	const auto factorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");
	const auto pipelineHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Pipeline.h");
	const auto pipelineSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Pipeline.cpp");

	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Pipeline.h"));
	EXPECT_TRUE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX12/DX12Pipeline.cpp"));
	EXPECT_NE(factorySource.find("#include \"Rendering/RHI/Backends/DX12/DX12Pipeline.h\""), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12PipelineLayout final"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12ShaderModule final"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12GraphicsPipeline final"), std::string::npos);
	EXPECT_EQ(factorySource.find("class NativeDX12ComputePipeline final"), std::string::npos);
	EXPECT_EQ(factorySource.find("CreateGraphicsPipelineState("), std::string::npos);
	EXPECT_EQ(factorySource.find("D3D12SerializeRootSignature("), std::string::npos);
	EXPECT_NE(pipelineHeaderSource.find("class NativeDX12PipelineLayout final"), std::string::npos);
	EXPECT_NE(pipelineHeaderSource.find("class NativeDX12ShaderModule final"), std::string::npos);
	EXPECT_NE(pipelineHeaderSource.find("class NativeDX12GraphicsPipeline final"), std::string::npos);
	EXPECT_NE(pipelineHeaderSource.find("class NativeDX12ComputePipeline final"), std::string::npos);
	EXPECT_NE(pipelineHeaderSource.find("public IDX12ComputePipelineAccess"), std::string::npos);
	EXPECT_NE(pipelineSource.find("CreateGraphicsPipelineState("), std::string::npos);
	EXPECT_NE(pipelineSource.find("CreateComputePipelineState("), std::string::npos);
	EXPECT_NE(pipelineSource.find("D3D12SerializeRootSignature("), std::string::npos);
	EXPECT_NE(pipelineSource.find("BuildDX12OwnedRootParameters"), std::string::npos);
	EXPECT_NE(pipelineSource.find("BuildDX12OwnedInputLayout"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12ReadbackExecutionLivesInReadbackUtils)
{
	const auto factorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp");
	const auto readbackHeaderSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h");
	const auto readbackSource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12ReadbackUtils.cpp");

	EXPECT_NE(factorySource.find("ExecuteDX12ReadPixels("), std::string::npos);
	EXPECT_EQ(factorySource.find("CopyTextureRegion("), std::string::npos);
	EXPECT_EQ(factorySource.find("readbackResource->Map("), std::string::npos);
	EXPECT_EQ(factorySource.find("BuildDX12ReadbackLayout("), std::string::npos);
	EXPECT_NE(readbackHeaderSource.find("ExecuteDX12ReadPixels"), std::string::npos);
	EXPECT_NE(readbackSource.find("ExecuteDX12ReadPixels("), std::string::npos);
	EXPECT_NE(readbackSource.find("CopyTextureRegion("), std::string::npos);
	EXPECT_NE(readbackSource.find("readbackResource->Map("), std::string::npos);
	EXPECT_NE(readbackSource.find("BuildDX12ReadbackLayout("), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, Dx12StructuredAndStorageBindingsWriteNullDescriptorsWhenInvalid)
{
    const auto source = ReadRepositorySource("Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp");

    EXPECT_NE(source.find("WriteNullStructuredBufferDescriptor("), std::string::npos);
    EXPECT_NE(source.find("WriteNullStorageBufferDescriptor("), std::string::npos);
    EXPECT_NE(source.find("WriteNullStructuredBufferDescriptor(destination);"), std::string::npos);
    EXPECT_NE(source.find("WriteNullStorageBufferDescriptor(destination);"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, DriverMainlineDoesNotConstructLegacyCommandListExecutors)
{
    const auto driverInternalHeaderSource = ReadRepositorySource("Runtime/Rendering/Context/DriverInternal.h");
    const auto driverSource = ReadRepositorySource("Runtime/Rendering/Context/Driver.cpp");
    const auto rhiDeviceFactorySource = ReadRepositorySource("Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp");

    EXPECT_EQ(driverInternalHeaderSource.find("IRHICommandListExecutor"), std::string::npos);
    EXPECT_EQ(driverInternalHeaderSource.find("commandExecutor"), std::string::npos);
    EXPECT_EQ(driverInternalHeaderSource.find("unique_ptr<Render::RHI::RHICommandList> commandList;"), std::string::npos);
    EXPECT_EQ(driverSource.find("CreateCommandListExecutor("), std::string::npos);
    EXPECT_EQ(driverSource.find("commandExecutor"), std::string::npos);
    EXPECT_EQ(driverSource.find("DefaultRHICommandList"), std::string::npos);
    EXPECT_EQ(driverSource.find("RHICommandListExecutor.h"), std::string::npos);
    EXPECT_EQ(driverSource.find("RHICommandList.h"), std::string::npos);
    EXPECT_EQ(rhiDeviceFactorySource.find("CreateCommandListExecutor("), std::string::npos);
    EXPECT_EQ(rhiDeviceFactorySource.find("CommandListExecutor"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, LegacyRecordedRhiCommandListSurfaceIsRemoved)
{
    const auto repositoryRoot = GetRepositoryRoot();

    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Core/RHICommandList.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Core/RHICommandList.cpp"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Core/RHICommandTypes.h"));
}

TEST(UE5RenderArchitectureContractTests, LegacyDx11AndOpenGlRhiFactoriesAreRemoved)
{
    const auto repositoryRoot = GetRepositoryRoot();

    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX11/DX11ExplicitDeviceFactory.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/DX11/DX11ExplicitDeviceFactory.cpp"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/OpenGL/OpenGLExplicitDeviceFactory.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/OpenGL/OpenGLExplicitDeviceFactory.cpp"));
}

TEST(UE5RenderArchitectureContractTests, InactiveBackendImplementationsAreRemovedFromRuntimeSource)
{
    const auto repositoryRoot = GetRepositoryRoot();
    const auto cmakeSource = ReadRepositorySource("Runtime/Rendering/CMakeLists.txt");

    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/OpenGL/OpenGLUIBridge.cpp"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/OpenGL/OpenGLAPI.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/OpenGL/OpenGLAPI.cpp"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/OpenGL/OpenGLFormalTypes.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/OpenGL/OpenGLFormalTypes.cpp"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/OpenGL/OpenGLShaderProgramAPI.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.cpp"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/Vulkan/VulkanUIBridge.cpp"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.cpp"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/Backend/OpenGL/OpenGLVertexInputAPI.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/Backend/OpenGL/OpenGLTypeMappings.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/glad/glad.h"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/glad/glad.c"));
    EXPECT_FALSE(std::filesystem::exists(repositoryRoot / "Runtime/Rendering/KHR/khrplatform.h"));

    EXPECT_NE(cmakeSource.find(R"(RHI[/\\\\]Backends[/\\\\]OpenGL)"), std::string::npos);
    EXPECT_NE(cmakeSource.find(R"(RHI[/\\\\]Backends[/\\\\]Vulkan)"), std::string::npos);
    EXPECT_NE(cmakeSource.find(R"(RHI[/\\\\]Backends[/\\\\]Metal)"), std::string::npos);
    EXPECT_NE(cmakeSource.find(R"(Backend[/\\\\]OpenGL)"), std::string::npos);
    EXPECT_NE(cmakeSource.find(R"(glad[/\\\\])"), std::string::npos);
    EXPECT_NE(cmakeSource.find(R"(KHR[/\\\\])"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, CentralPipelineStateCreationRemainsOnPipelineCacheMainline)
{
    const auto materialSource = ReadRepositorySource("Runtime/Rendering/Resources/Material.cpp");
    const auto lightGridPrepassSource = ReadRepositorySource("Runtime/Engine/Rendering/LightGridPrepass.cpp");
    const auto forwardSceneRendererSource = ReadRepositorySource("Runtime/Engine/Rendering/ForwardSceneRenderer.cpp");
    const auto deferredSceneRendererSource = ReadRepositorySource("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    EXPECT_NE(materialSource.find("GetOrCreateGraphicsPipeline"), std::string::npos);
    EXPECT_EQ(CountOccurrences(materialSource, "CreateGraphicsPipeline("), 2u);

    EXPECT_NE(lightGridPrepassSource.find("GetOrCreateComputePipeline"), std::string::npos);
    EXPECT_EQ(CountOccurrences(lightGridPrepassSource, "CreateComputePipeline("), 2u);

    EXPECT_EQ(forwardSceneRendererSource.find("CreateGraphicsPipeline("), std::string::npos);
    EXPECT_EQ(forwardSceneRendererSource.find("CreateComputePipeline("), std::string::npos);

    EXPECT_EQ(deferredSceneRendererSource.find("CreateGraphicsPipeline("), std::string::npos);
    EXPECT_EQ(deferredSceneRendererSource.find("CreateComputePipeline("), std::string::npos);
    EXPECT_NE(deferredSceneRendererSource.find("BuildRecordedGraphicsPipeline"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, BindingPipelineAndTransientBypassesRemainRejectedInAcceptedSources)
{
    const auto driverSource = ReadRepositorySource("Runtime/Rendering/Context/Driver.cpp");
    const auto rhiThreadSource = ReadRepositorySource("Runtime/Rendering/Context/RhiThreadCoordinator.cpp");
    const auto materialSource = ReadRepositorySource("Runtime/Rendering/Resources/Material.cpp");
    const auto lightGridPrepassSource = ReadRepositorySource("Runtime/Engine/Rendering/LightGridPrepass.cpp");

    EXPECT_EQ(driverSource.find("return innerBindingSet;"), std::string::npos);
    EXPECT_NE(driverSource.find("descriptor allocator missing"), std::string::npos);

    EXPECT_NE(materialSource.find("if (pipelineCache == nullptr)"), std::string::npos);
    EXPECT_NE(lightGridPrepassSource.find("if (pipelineCache == nullptr)"), std::string::npos);
    EXPECT_EQ(CountOccurrences(materialSource, "CreateGraphicsPipeline("), 2u);
    EXPECT_EQ(CountOccurrences(lightGridPrepassSource, "CreateComputePipeline("), 2u);

    EXPECT_NE(rhiThreadSource.find("submissionFrame->usedResourceStateTracker = true;"), std::string::npos);
    EXPECT_NE(rhiThreadSource.find("frameContext.resourceStateTracker->RetireTransientResources"), std::string::npos);
    EXPECT_NE(rhiThreadSource.find("frameContext.resourceStateTracker->Reset();"), std::string::npos);
    EXPECT_EQ(rhiThreadSource.find("submissionFrame->retirementFenceWaited = true;"), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, ReadbackSelectionRemainsOnFrameGraphAndFramebufferMainline)
{
    const auto pickingRenderPassSource = ReadRepositorySource("Project/Editor/Rendering/PickingRenderPass.cpp");
    const auto debugSceneRendererSource = ReadRepositorySource("Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const auto baseRendererSource = ReadRepositorySource("Runtime/Rendering/Core/ABaseRenderer.cpp");
    const auto driverAccessHeaderSource = ReadRepositorySource("Runtime/Rendering/Context/DriverAccess.h");
    const auto driverSource = ReadRepositorySource("Runtime/Rendering/Context/Driver.cpp");
    const auto externalResourceBridgeSource = ReadRepositorySource("Runtime/Rendering/FrameGraph/ExternalResourceBridge.cpp");

    EXPECT_EQ(pickingRenderPassSource.find("SetActiveReadbackTexture("), std::string::npos);
    EXPECT_EQ(pickingRenderPassSource.find("ReadPixelsFromFramebufferTexture("), std::string::npos);
    EXPECT_NE(pickingRenderPassSource.find("ReadPixels("), std::string::npos);

    EXPECT_NE(debugSceneRendererSource.find("RegisterPreferredReadbackTexture"), std::string::npos);
    EXPECT_EQ(debugSceneRendererSource.find("extractedTextures.insert("), std::string::npos);
    EXPECT_NE(externalResourceBridgeSource.find("ResolveFrameReadbackTexture"), std::string::npos);
    EXPECT_NE(externalResourceBridgeSource.find("BuildExtractionVisibilityPassInput"), std::string::npos);
    EXPECT_NE(externalResourceBridgeSource.find("TransitionExternalSceneOutputToShaderRead"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("TransitionExternalSceneOutputToShaderRead"), std::string::npos);
    EXPECT_EQ(baseRendererSource.find("TransitionTextureToShaderRead("), std::string::npos);
    EXPECT_EQ(driverAccessHeaderSource.find("class IRHIBuffer;"), std::string::npos);
    EXPECT_EQ(driverAccessHeaderSource.find("class IRHITexture;"), std::string::npos);
    EXPECT_EQ(driverSource.find("IRHIResource.h"), std::string::npos);

    EXPECT_EQ(driverAccessHeaderSource.find("SetActiveReadbackTexture("), std::string::npos);
    EXPECT_EQ(driverSource.find("void DriverRendererAccess::SetActiveReadbackTexture("), std::string::npos);
    EXPECT_EQ(driverAccessHeaderSource.find("TransitionTextureToShaderRead("), std::string::npos);
    EXPECT_EQ(driverSource.find("void DriverRendererAccess::TransitionTextureToShaderRead("), std::string::npos);
}
