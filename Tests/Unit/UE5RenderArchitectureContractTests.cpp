#include <gtest/gtest.h>

#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilder.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

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
    snapshot.sceneGameObjectCount = 4u;
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
                case NLS::Render::Context::RenderPassCommandKind::Decal:
                    passInput.drawCount = package.decalDrawCount;
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
    ASSERT_EQ(graphPasses.size(), 4u);
    EXPECT_EQ(graphPasses[0].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(graphPasses[1].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Decal);
    EXPECT_EQ(graphPasses[2].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(graphPasses[3].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Transparent);

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
