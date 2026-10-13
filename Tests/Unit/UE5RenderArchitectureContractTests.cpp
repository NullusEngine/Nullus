#include <gtest/gtest.h>

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilder.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Tests/RenderingThreadTestHelpers.h"

TEST(UE5RenderArchitectureContractTests, PlatformBackendGateKeepsOnlyPlatformRuntimeEnabled)
{
    const auto requiredBackend = NLS::Render::Settings::GetPhase1RequiredRuntimeBackend();
    EXPECT_EQ(requiredBackend, NLS::Render::Settings::GetPlatformDefaultGraphicsBackend());
    EXPECT_TRUE(NLS::Render::Settings::IsBackendEnabledForCurrentBuild(
        requiredBackend));

    for (const auto backend : {NLS::Render::Settings::EGraphicsBackend::DX12,
                               NLS::Render::Settings::EGraphicsBackend::DX11,
                               NLS::Render::Settings::EGraphicsBackend::VULKAN,
                               NLS::Render::Settings::EGraphicsBackend::OPENGL,
                               NLS::Render::Settings::EGraphicsBackend::METAL})
    {
        if (backend == requiredBackend ||
            NLS::Render::Settings::IsBackendSelectableForPhase1(backend))
            continue;
        EXPECT_FALSE(NLS::Render::Settings::IsBackendEnabledForCurrentBuild(backend));
    }
}

TEST(UE5RenderArchitectureContractTests, PlatformRestrictionMessageRejectsNonPlatformBackends)
{
    const auto requiredBackend = NLS::Render::Settings::GetPhase1RequiredRuntimeBackend();
    const auto requiredRestriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        requiredBackend,
        "Contract test");
    EXPECT_FALSE(requiredRestriction.has_value());

    const auto unsupportedBackend = []()
    {
        using NLS::Render::Settings::EGraphicsBackend;
        for (const auto backend : { EGraphicsBackend::DX12, EGraphicsBackend::DX11,
                                    EGraphicsBackend::VULKAN, EGraphicsBackend::OPENGL,
                                    EGraphicsBackend::METAL })
        {
            if (!NLS::Render::Settings::IsBackendSelectableForPhase1(backend))
                return backend;
        }
        return EGraphicsBackend::NONE;
    }();
    const auto restriction = NLS::Render::Settings::GetPhase1BackendRestrictionMessage(
        unsupportedBackend,
        "Contract test");
    ASSERT_TRUE(restriction.has_value());
    EXPECT_NE(restriction->find(
        "only supports " + std::string(NLS::Render::Settings::ToString(requiredBackend))), std::string::npos);
    EXPECT_NE(restriction->find(NLS::Render::Settings::ToString(unsupportedBackend)), std::string::npos);
}

TEST(UE5RenderArchitectureContractTests, ThreadedFoundationAcceptsDX12MetalAndVulkan)
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
}

TEST(UE5RenderArchitectureContractTests, OrderedParallelSubmissionAcceptsDX12MetalAndVulkan)
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
}

TEST(UE5RenderArchitectureContractTests, OwnershipFlowsFromFramePacketToRenderScenePackage)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto preparedScene = NLS::Tests::ObserveNextRenderSceneForTest(driver);

    RenderFramePacket packet;
    packet.frameId = 91u;
    packet.renderWidth = 1280u;
    packet.renderHeight = 720u;
    packet.targetsSwapchain = false;
    RenderSceneBuildData scene;
    scene.sceneGameObjectCount = 4u;
    scene.visibleOpaqueDrawCount = 2u;
    scene.recordedDrawCommands.resize(2u);
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, packet,
        [packet, scene = std::move(scene)](size_t) mutable
        {
            EXPECT_TRUE(RenderingThreadSystem::IsInRenderingThread());
            return BuildRenderScenePackage(packet, std::move(scene));
        }));
    EXPECT_EQ(preparedScene.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    ASSERT_TRUE(DriverTestAccess::TryDrainThreadedRendering(driver));
    ASSERT_EQ(preparedScene.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    const auto renderPackage = preparedScene.get();
    EXPECT_EQ(renderPackage.frameId, packet.frameId);
    EXPECT_EQ(renderPackage.renderWidth, 1280u);
    EXPECT_EQ(renderPackage.renderHeight, 720u);
    EXPECT_EQ(renderPackage.sceneGameObjectCount, 4u);
    EXPECT_TRUE(renderPackage.hasVisibleDraws);
    EXPECT_EQ(renderPackage.drawCommandCount, 2u);
    EXPECT_EQ(renderPackage.recordedDrawCommands.size(), 2u);
}

TEST(UE5RenderArchitectureContractTests, PreparedBuilderPackagesDoNotCarryRendererLocalPassScheduling)
{
    NLS::Render::Context::RenderFramePacket packet;
    packet.frameId = 12u;
    packet.renderWidth = 1280u;
    packet.renderHeight = 720u;
    packet.targetsSwapchain = false;
    NLS::Render::Context::RenderSceneBuildData scene;
    scene.visibleOpaqueDrawCount = 1u;
    scene.visibleSkyboxDrawCount = 1u;
    scene.visibleTransparentDrawCount = 1u;
    scene.recordedDrawCommands.resize(3u);

    const auto package = NLS::Render::Context::BuildRenderScenePackage(
        packet, std::move(scene),
        NLS::Render::Context::RenderScenePackageBuildMode::SkipDefaultPassInputs);

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

TEST(UE5RenderArchitectureContractTests, RenderSceneBuildDataTransfersDrawCommandsWithoutChangingFramePacket)
{
    NLS::Render::Context::RenderFramePacket packet;
    packet.frameId = 13u;
    packet.renderWidth = 1280u;
    packet.renderHeight = 720u;
    NLS::Render::Context::RenderSceneBuildData scene;
    scene.visibleOpaqueDrawCount = 2u;
    scene.recordedDrawCommands.resize(2u);
    scene.recordedDrawCommands[0].instanceCount = 7u;
    scene.recordedDrawCommands[1].vertexStart = 11u;
    const auto* drawStorage = scene.recordedDrawCommands.data();

    auto package = NLS::Render::Context::BuildRenderScenePackage(
        packet, std::move(scene),
        NLS::Render::Context::RenderScenePackageBuildMode::SkipDefaultPassInputs);

    ASSERT_EQ(package.recordedDrawCommands.size(), 2u);
    EXPECT_EQ(package.recordedDrawCommands[0].instanceCount, 7u);
    EXPECT_EQ(package.recordedDrawCommands[1].vertexStart, 11u);
    EXPECT_TRUE(scene.recordedDrawCommands.empty());
    EXPECT_EQ(package.recordedDrawCommands.data(), drawStorage);
    EXPECT_EQ(packet.frameId, 13u);
    EXPECT_EQ(packet.renderWidth, 1280u);
    EXPECT_EQ(packet.renderHeight, 720u);
}

TEST(UE5RenderArchitectureContractTests, RuntimeCompilationRebuildsPreparedPassPlanFromForwardSceneMetadata)
{
    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 33u;
    snapshot.renderWidth = 1280u;
    snapshot.renderHeight = 720u;
    NLS::Render::Context::RenderSceneBuildData scene;
    scene.visibleOpaqueDrawCount = 2u;
    scene.visibleSkyboxDrawCount = 1u;
    scene.visibleTransparentDrawCount = 1u;
    scene.recordedDrawCommands.resize(4u);

    auto package = NLS::Render::Context::BuildRenderScenePackage(
        snapshot, std::move(scene),
        NLS::Render::Context::RenderScenePackageBuildMode::SkipDefaultPassInputs);
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

TEST(UE5RenderArchitectureContractTests, PreparedBuilderProducesItsOwnRenderScenePackage)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto preparedScene = NLS::Tests::ObserveNextRenderSceneForTest(driver);

    RenderFramePacket packet;
    packet.frameId = 44u;
    packet.renderWidth = 640u;
    packet.renderHeight = 360u;
    packet.targetsSwapchain = false;
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(
        driver, packet,
        [](const size_t)
        {
            EXPECT_TRUE(RenderingThreadSystem::IsInRenderingThread());
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = 999u;
            package.targetsSwapchain = false;
            package.hasVisibleDraws = true;
            package.visibleDrawCount = 1u;
            package.frameDataReady = true;
            package.objectDataReady = true;
            return package;
        }));

    EXPECT_EQ(preparedScene.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    ASSERT_TRUE(DriverTestAccess::TryDrainThreadedRendering(driver));
    ASSERT_EQ(preparedScene.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    const auto package = preparedScene.get();
    EXPECT_EQ(package.frameId, 44u);
    EXPECT_EQ(package.visibleDrawCount, 1u);
    const auto telemetry = DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.publishOrigin, ThreadedFramePublishOrigin::PreparedBuilder);
    EXPECT_EQ(telemetry.renderSceneAttribution, RenderSceneAttribution::RendererPrepared);
}
