#include <gtest/gtest.h>

#include <fg/Blackboard.hpp>
#include <fg/FrameGraph.hpp>

#include <filesystem>
#include <fstream>
#include <type_traits>

#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/EditorHelperLifecycle.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilder.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/ForwardSceneRenderer.h"

namespace
{
    struct DeferredSnapshotHarness : NLS::Engine::Rendering::DeferredSceneRenderer
    {
        using NLS::Engine::Rendering::DeferredSceneRenderer::SynchronizeThreadedDeferredSnapshot;
    };

    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        explicit TestTexture(NLS::Render::RHI::RHITextureDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    class TestTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        TestTextureView(
            std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
            NLS::Render::RHI::RHITextureViewDesc desc)
            : m_texture(std::move(texture))
            , m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
        NLS::Render::RHI::RHITextureViewDesc m_desc {};
    };

    NLS::Render::RHI::RHIDeviceCapabilities MakeEditorReadyCapabilities()
    {
        NLS::Render::RHI::RHIDeviceCapabilities capabilities;
        capabilities.backendReady = true;
        capabilities.supportsCurrentSceneRenderer = true;
        capabilities.supportsOffscreenFramebuffers = true;
        capabilities.supportsUITextureHandles = true;
        capabilities.supportsDepthBlit = true;
        capabilities.supportsCubemaps = true;
        return capabilities;
    }

}

TEST(EditorRenderPathContractTests, EditorRuntimeRejectsNonDx12BackendsBeforeStartup)
{
    const auto decision = NLS::Render::Settings::EvaluateEditorMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::VULKAN,
        MakeEditorReadyCapabilities());

    ASSERT_TRUE(decision.primaryWarning.has_value());
    EXPECT_NE(decision.primaryWarning->find("only supports DX12"), std::string::npos);
    ASSERT_TRUE(decision.detailWarning.has_value());
    EXPECT_NE(decision.detailWarning->find("DX12-only"), std::string::npos);
}

TEST(EditorRenderPathContractTests, EditorRuntimeAcceptsDx12WhenCapabilitiesAreSufficient)
{
    const auto decision = NLS::Render::Settings::EvaluateEditorMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::DX12,
        MakeEditorReadyCapabilities());

#if defined(_WIN32)
    EXPECT_FALSE(decision.primaryWarning.has_value());
    EXPECT_FALSE(decision.detailWarning.has_value());
#else
    ASSERT_TRUE(decision.primaryWarning.has_value());
#endif
}

TEST(EditorRenderPathContractTests, EditorDeferredPathKeepsGraphOwnedGBufferBeforeLightingOrder)
{
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;

    FrameGraph frameGraph;
    FrameGraphBlackboard blackboard;

    const auto resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
        frameGraph,
        blackboard,
        frameDescriptor,
        {});
    const auto lightGridContext = NLS::Render::FrameGraph::BuildLightGridCompileContext(
        frameDescriptor,
        NLS::Render::FrameGraph::PreparedComputeDispatchSource{},
        nullptr);
    const auto preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
        resourceRequest,
        lightGridContext);

    const auto& graphPasses = preparedGraph.execution.compiledExecution.graphPasses;
    ASSERT_EQ(graphPasses.size(), 2u);
    EXPECT_EQ(graphPasses[0].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(graphPasses[1].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_STREQ(graphPasses[0].metadata.graphPassName, "DeferredGBuffer");
    EXPECT_STREQ(graphPasses[1].metadata.graphPassName, "DeferredLighting");
    EXPECT_TRUE(preparedGraph.execution.compiledExecution.threadedPlan.passes.empty());
}

TEST(EditorRenderPathContractTests, DebugSceneRendererUsesDeferredMainScenePath)
{
    EXPECT_TRUE((std::is_base_of_v<
        NLS::Engine::Rendering::DeferredSceneRenderer,
        NLS::Editor::Rendering::DebugSceneRenderer>));
    EXPECT_FALSE((std::is_base_of_v<
        NLS::Engine::Rendering::ForwardSceneRenderer,
        NLS::Editor::Rendering::DebugSceneRenderer>));
}

TEST(EditorRenderPathContractTests, DeferredThreadedGBufferCountComesFromBeginFrameCapture)
{
    const std::filesystem::path rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp";

    std::ifstream stream(rendererSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("m_threadedQueuedGBufferDrawCount = queuedGBufferDrawCount"), std::string::npos);
    EXPECT_NE(source.find("SynchronizeThreadedDeferredSnapshot(pendingFrameSnapshot.value(), m_threadedQueuedGBufferDrawCount)"), std::string::npos);
    EXPECT_EQ(source.find("recordedDrawCount - 1u"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DeferredPreparedScenePackageAppendsEditorOverlayPassInputsAfterLighting)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.helperDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(2u);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;

    NLS::Render::Context::RenderPassCommandInput gridPass;
    gridPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridPass.debugName = "EditorGridPass";
    gridPass.drawCount = 1u;

    NLS::Render::Context::RenderPassCommandInput pickingPass;
    pickingPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    pickingPass.debugName = "EditorPickingPass";
    pickingPass.drawCount = 0u;

    std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> appendedMetadata;
    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata gridMetadata;
    gridMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    gridMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    gridMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(gridMetadata, "EditorGridPass");
    appendedMetadata.push_back(gridMetadata);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata pickingMetadata;
    pickingMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    pickingMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary;
    pickingMetadata.executionMode = NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded;
    pickingMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    pickingMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    pickingMetadata.propagatesColorOutput = false;
    pickingMetadata.propagatesDepthOutput = false;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(pickingMetadata, "EditorPickingPass");
    appendedMetadata.push_back(pickingMetadata);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        { gridPass, pickingPass },
        appendedMetadata);

    ASSERT_EQ(package.passCommandInputs.size(), 4u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_EQ(package.passCommandInputs[2].debugName, "EditorGridPass");
    EXPECT_EQ(package.passCommandInputs[3].debugName, "EditorPickingPass");
}

TEST(EditorRenderPathContractTests, DeferredPreparedScenePackageIgnoresDuplicatedDeferredAppendedMetadata)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(2u);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    NLS::Render::Context::RenderPassCommandInput gridPass;
    gridPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridPass.debugName = "EditorGridPass";
    gridPass.drawCount = 1u;

    std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> appendedMetadata;
    for (const auto& descriptor : NLS::Render::FrameGraph::GetDeferredScenePassDescriptors())
        appendedMetadata.push_back(descriptor.metadata);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata gridMetadata;
    gridMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    gridMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    gridMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(gridMetadata, "EditorGridPass");
    appendedMetadata.push_back(gridMetadata);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        {},
        { gridPass },
        appendedMetadata);

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_EQ(package.passCommandInputs[2].debugName, "EditorGridPass");
}

TEST(EditorRenderPathContractTests, DeferredEditorOverlayPassesKeepMetadataMatchedByDebugName)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(2u);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;

    NLS::Render::Context::RenderPassCommandInput gridPass;
    gridPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridPass.debugName = "EditorGridPass";
    gridPass.drawCount = 1u;

    NLS::Render::Context::RenderPassCommandInput pickingPass;
    pickingPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    pickingPass.debugName = "EditorPickingPass";
    pickingPass.drawCount = 0u;

    std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> appendedMetadata;

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata gridMetadata;
    gridMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    gridMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    gridMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(gridMetadata, "EditorGridPass");
    appendedMetadata.push_back(gridMetadata);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata aggregateHelperMetadata;
    aggregateHelperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    aggregateHelperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    aggregateHelperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    aggregateHelperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(aggregateHelperMetadata, "EditorHelperPass");
    appendedMetadata.push_back(aggregateHelperMetadata);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata pickingMetadata;
    pickingMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    pickingMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary;
    pickingMetadata.executionMode = NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded;
    pickingMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    pickingMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    pickingMetadata.propagatesColorOutput = false;
    pickingMetadata.propagatesDepthOutput = false;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(pickingMetadata, "EditorPickingPass");
    appendedMetadata.push_back(pickingMetadata);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        { gridPass, pickingPass },
        appendedMetadata);

    ASSERT_EQ(package.passCommandInputs.size(), 4u);
    EXPECT_EQ(package.passCommandInputs[2].debugName, "EditorGridPass");
    EXPECT_EQ(package.passCommandInputs[2].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::Previous);
    EXPECT_EQ(package.passCommandInputs[3].debugName, "EditorPickingPass");
    EXPECT_EQ(package.passCommandInputs[3].queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::Previous);
}

TEST(EditorRenderPathContractTests, DeferredEditorAggregateHelperDrawsRemainInEditorHelperPass)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 3u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(3u);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata helperMetadata;
    helperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    helperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    helperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(helperMetadata, "EditorHelperPass");

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        {},
        { helperMetadata });

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_EQ(package.passCommandInputs[2].kind, NLS::Render::Context::RenderPassCommandKind::Helper);
    EXPECT_EQ(package.passCommandInputs[2].debugName, "EditorHelperPass");
    EXPECT_EQ(package.passCommandInputs[2].drawCount, 1u);
    EXPECT_EQ(package.passCommandInputs[2].recordedDrawCommands.size(), 1u);
}

TEST(EditorRenderPathContractTests, DeferredEditorAggregateHelperStartsAfterOpaqueWhenLightingDrawIsMissing)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.helperDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(2u);
    package.recordedDrawCommands[1].instanceCount = 17u;

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata helperMetadata;
    helperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    helperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    helperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(helperMetadata, "EditorHelperPass");

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        {},
        { helperMetadata });

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_EQ(package.passCommandInputs[1].drawCount, 0u);
    EXPECT_TRUE(package.passCommandInputs[1].recordedDrawCommands.empty());
    EXPECT_EQ(package.passCommandInputs[2].debugName, "EditorHelperPass");
    ASSERT_EQ(package.passCommandInputs[2].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[2].recordedDrawCommands[0].instanceCount, 17u);
}

TEST(EditorRenderPathContractTests, DeferredLightingDrawIsPreservedWhenGridIsAnExplicitAppendedPass)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.helperDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(2u);
    package.recordedDrawCommands[1].instanceCount = 31u;

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    NLS::Render::Context::RenderPassCommandInput gridPass;
    gridPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridPass.debugName = "EditorGridPass";
    gridPass.drawCount = 1u;
    gridPass.recordedDrawCommands.resize(1u);
    gridPass.recordedDrawCommands[0].instanceCount = 23u;

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata gridMetadata;
    gridMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    gridMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    gridMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(gridMetadata, "EditorGridPass");

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        {},
        { gridPass },
        { gridMetadata });

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    EXPECT_EQ(package.passCommandInputs[1].debugName, "DeferredLighting");
    EXPECT_EQ(package.passCommandInputs[1].drawCount, 1u);
    ASSERT_EQ(package.passCommandInputs[1].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[1].recordedDrawCommands[0].instanceCount, 31u);
    EXPECT_EQ(package.passCommandInputs[2].debugName, "EditorGridPass");
    ASSERT_EQ(package.passCommandInputs[2].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[2].recordedDrawCommands[0].instanceCount, 23u);
}

TEST(EditorRenderPathContractTests, DeferredEditorOverlayPassInputsFollowMetadataDebugNameOrder)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 3u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(3u);
    package.recordedDrawCommands[2].instanceCount = 17u;

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    NLS::Render::Context::RenderPassCommandInput gridPass;
    gridPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridPass.debugName = "EditorGridPass";
    gridPass.drawCount = 1u;
    gridPass.recordedDrawCommands.resize(1u);
    gridPass.recordedDrawCommands[0].instanceCount = 23u;

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata gridMetadata;
    gridMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    gridMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    gridMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(gridMetadata, "EditorGridPass");

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata helperMetadata;
    helperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    helperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    helperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(helperMetadata, "EditorHelperPass");

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        {},
        { gridPass },
        { gridMetadata, helperMetadata });

    ASSERT_EQ(package.passCommandInputs.size(), 4u);
    EXPECT_EQ(package.passCommandInputs[2].debugName, "EditorGridPass");
    EXPECT_EQ(package.passCommandInputs[2].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[2].recordedDrawCommands[0].instanceCount, 23u);
    EXPECT_EQ(package.passCommandInputs[3].debugName, "EditorHelperPass");
    EXPECT_EQ(package.passCommandInputs[3].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[3].recordedDrawCommands[0].instanceCount, 17u);
}

TEST(EditorRenderPathContractTests, DebugDeferredSkyboxUsesActualTextureAvailabilityNotComponentCount)
{
    const std::filesystem::path rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp";

    std::ifstream stream(rendererSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    EXPECT_EQ(source.find("snapshot.sceneSkyboxCount > 0u"), std::string::npos);
    EXPECT_NE(source.find("DeferredSceneDescriptor"), std::string::npos);
    EXPECT_NE(source.find("hasSkyboxTexture"), std::string::npos);
}

TEST(EditorRenderPathContractTests, ThreadedGridPassSubmitsAxisLinesBeforeCapturingPlane)
{
    const std::filesystem::path gridPassSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/GridRenderPass.cpp";

    std::ifstream stream(gridPassSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto drawFunction = source.find("void Editor::Rendering::GridRenderPass::Draw");
    const auto threadedBranch = source.find("IsThreadedRenderingEnabled", drawFunction);
    const auto firstAxisSubmit = source.find("debugDrawService.SubmitLine", drawFunction);
    ASSERT_NE(drawFunction, std::string::npos);
    ASSERT_NE(threadedBranch, std::string::npos);
    ASSERT_NE(firstAxisSubmit, std::string::npos);
    EXPECT_LT(firstAxisSubmit, threadedBranch);
}

TEST(EditorRenderPathContractTests, GridShaderOutputsStraightAlphaColorForBlending)
{
    const std::filesystem::path gridShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Editor/Shaders/Grid.hlsl";

    std::ifstream stream(gridShaderPath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("const float3 blendColor"), std::string::npos);
    EXPECT_NE(source.find("return float4(blendColor, alpha);"), std::string::npos);
    EXPECT_EQ(source.find("return float4(color, alpha);"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GridPlaneModelKeepsEditorPlaneOnWorldXZGround)
{
    const std::filesystem::path gridPassSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/GridRenderPass.cpp";

    std::ifstream stream(gridPassSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("BuildGridPlaneModelMatrix"), std::string::npos);
    EXPECT_EQ(source.find("Maths::Matrix4::RotationOnAxisX(-kHalfPi)"), std::string::npos);
    EXPECT_NE(source.find("Maths::Matrix4::Scaling({ gridSize * 2.0f, 1.f, gridSize * 2.0f })"), std::string::npos);
    EXPECT_EQ(source.find("Maths::Matrix4::Scaling({ gridSize * 2.0f, gridSize * 2.0f, 1.f })"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DeferredEditorOverlayInputsReceiveExternalSceneOutputDepthBeforePlanning)
{
    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "SceneViewColor";
    colorDesc.extent = { 320u, 180u, 1u };
    auto colorTexture = std::make_shared<TestTexture>(colorDesc);

    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.debugName = "SceneViewColorView";
    auto colorView = std::make_shared<TestTextureView>(colorTexture, colorViewDesc);

    NLS::Render::RHI::RHITextureDesc depthDesc;
    depthDesc.debugName = "SceneViewDepth";
    depthDesc.extent = { 320u, 180u, 1u };
    auto depthTexture = std::make_shared<TestTexture>(depthDesc);

    NLS::Render::RHI::RHITextureViewDesc depthViewDesc;
    depthViewDesc.debugName = "SceneViewDepthView";
    auto depthView = std::make_shared<TestTextureView>(depthTexture, depthViewDesc);

    std::vector<NLS::Render::Context::RenderPassCommandInput> appendedPassInputs;
    NLS::Render::Context::RenderPassCommandInput gridPass;
    gridPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridPass.debugName = "EditorGridPass";
    gridPass.usesColorAttachment = true;
    gridPass.usesDepthStencilAttachment = true;
    appendedPassInputs.push_back(gridPass);

    NLS::Render::FrameGraph::ExternalSceneOutputAttachments attachments;
    attachments.colorView = colorView;
    attachments.depthStencilView = depthView;

    EXPECT_TRUE(NLS::Render::FrameGraph::ApplyExternalSceneOutputAttachments(
        appendedPassInputs,
        attachments,
        {
            NLS::Render::Context::RenderPassCommandKind::Helper
        }));

    ASSERT_EQ(appendedPassInputs.size(), 1u);
    ASSERT_EQ(appendedPassInputs[0].colorAttachmentViews.size(), 1u);
    EXPECT_EQ(appendedPassInputs[0].colorAttachmentViews[0], colorView);
    EXPECT_EQ(appendedPassInputs[0].depthStencilAttachmentView, depthView);
}

TEST(EditorRenderPathContractTests, DeferredSceneViewLightingTargetsExternalColorDuringPlanBuild)
{
    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "SceneViewColor";
    colorDesc.extent = { 320u, 180u, 1u };
    auto colorTexture = std::make_shared<TestTexture>(colorDesc);

    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.debugName = "SceneViewColorView";
    auto colorView = std::make_shared<TestTextureView>(colorTexture, colorViewDesc);

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.outputColorTexture = colorTexture;
    frameDescriptor.outputColorView = colorView;

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    package.renderWidth = frameDescriptor.renderWidth;
    package.renderHeight = frameDescriptor.renderHeight;
    package.clearColorBuffer = true;
    package.clearDepthBuffer = true;
    package.clearStencilBuffer = true;

    const auto lightGridContext = NLS::Render::FrameGraph::BuildLightGridCompileContext(
        frameDescriptor,
        NLS::Render::FrameGraph::PreparedComputeDispatchSource{},
        nullptr);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        {});

    ASSERT_EQ(package.passCommandInputs.size(), 2u);
    const auto& lightingPass = package.passCommandInputs[1];
    EXPECT_EQ(lightingPass.kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_FALSE(lightingPass.targetsSwapchain);
    ASSERT_EQ(lightingPass.colorAttachmentViews.size(), 1u);
    EXPECT_EQ(lightingPass.colorAttachmentViews[0], colorView);
}

TEST(EditorRenderPathContractTests, DeferredSceneViewEditorOverlayWritesExternalColorAndTestsGBufferDepthDuringPlanBuild)
{
    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "SceneViewColor";
    colorDesc.extent = { 320u, 180u, 1u };
    auto colorTexture = std::make_shared<TestTexture>(colorDesc);

    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.debugName = "SceneViewColorView";
    auto colorView = std::make_shared<TestTextureView>(colorTexture, colorViewDesc);

    NLS::Render::RHI::RHITextureDesc depthDesc;
    depthDesc.debugName = "SceneViewDepth";
    depthDesc.extent = { 320u, 180u, 1u };
    auto depthTexture = std::make_shared<TestTexture>(depthDesc);

    NLS::Render::RHI::RHITextureViewDesc depthViewDesc;
    depthViewDesc.debugName = "SceneViewDepthView";
    auto depthView = std::make_shared<TestTextureView>(depthTexture, depthViewDesc);

    NLS::Render::RHI::RHITextureDesc gbufferDepthDesc;
    gbufferDepthDesc.debugName = "DeferredGBufferDepth";
    gbufferDepthDesc.extent = { 320u, 180u, 1u };
    auto gbufferDepthTexture = std::make_shared<TestTexture>(gbufferDepthDesc);

    NLS::Render::RHI::RHITextureViewDesc gbufferDepthViewDesc;
    gbufferDepthViewDesc.debugName = "DeferredGBufferDepthView";
    auto gbufferDepthView = std::make_shared<TestTextureView>(gbufferDepthTexture, gbufferDepthViewDesc);

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.outputColorTexture = colorTexture;
    frameDescriptor.outputDepthStencilTexture = depthTexture;
    frameDescriptor.outputColorView = colorView;
    frameDescriptor.outputDepthStencilView = depthView;

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = frameDescriptor.renderWidth;
    package.renderHeight = frameDescriptor.renderHeight;
    package.recordedDrawCommands.resize(2u);

    NLS::Render::Context::RenderPassCommandInput gridPass;
    gridPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridPass.debugName = "EditorGridPass";
    gridPass.drawCount = 1u;
    gridPass.usesColorAttachment = true;
    gridPass.usesDepthStencilAttachment = true;

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata gridMetadata;
    gridMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    gridMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    gridMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(gridMetadata, "EditorGridPass");

    const auto lightGridContext = NLS::Render::FrameGraph::BuildLightGridCompileContext(
        frameDescriptor,
        NLS::Render::FrameGraph::PreparedComputeDispatchSource{},
        nullptr);

    NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;
    resources.gbufferDepthView = gbufferDepthView;

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        { gridPass },
        { gridMetadata });

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    const auto& gbufferPass = package.passCommandInputs[0];
    EXPECT_EQ(gbufferPass.depthStencilAttachmentView, gbufferDepthView);

    const auto& overlayPass = package.passCommandInputs[2];
    EXPECT_EQ(overlayPass.debugName, "EditorGridPass");
    ASSERT_EQ(overlayPass.colorAttachmentViews.size(), 1u);
    EXPECT_EQ(overlayPass.colorAttachmentViews[0], colorView);
    EXPECT_EQ(overlayPass.depthStencilAttachmentView, gbufferDepthView);
    EXPECT_NE(overlayPass.depthStencilAttachmentView, depthView);
    ASSERT_FALSE(overlayPass.textureResourceAccesses.empty());
    const auto depthAccess = std::find_if(
        overlayPass.textureResourceAccesses.begin(),
        overlayPass.textureResourceAccesses.end(),
        [&gbufferDepthTexture](const NLS::Render::Context::TextureResourceAccess& access)
        {
            return access.texture == gbufferDepthTexture;
        });
    ASSERT_NE(depthAccess, overlayPass.textureResourceAccesses.end());
    EXPECT_EQ(depthAccess->mode, NLS::Render::Context::ResourceAccessMode::Read);
    EXPECT_EQ(depthAccess->state, NLS::Render::RHI::ResourceState::DepthRead);
    EXPECT_EQ(depthAccess->stages, NLS::Render::RHI::PipelineStageMask::DepthStencil);
    EXPECT_EQ(depthAccess->access, NLS::Render::RHI::AccessMask::DepthStencilRead);
}

TEST(EditorRenderPathContractTests, DeferredSceneViewEditorOverlayKeepsGBufferDepthWhenExternalAttachmentsWerePreapplied)
{
    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "SceneViewColor";
    colorDesc.extent = { 320u, 180u, 1u };
    auto colorTexture = std::make_shared<TestTexture>(colorDesc);

    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.debugName = "SceneViewColorView";
    auto colorView = std::make_shared<TestTextureView>(colorTexture, colorViewDesc);

    NLS::Render::RHI::RHITextureDesc sceneDepthDesc;
    sceneDepthDesc.debugName = "SceneViewDepth";
    sceneDepthDesc.extent = { 320u, 180u, 1u };
    auto sceneDepthTexture = std::make_shared<TestTexture>(sceneDepthDesc);

    NLS::Render::RHI::RHITextureViewDesc sceneDepthViewDesc;
    sceneDepthViewDesc.debugName = "SceneViewDepthView";
    auto sceneDepthView = std::make_shared<TestTextureView>(sceneDepthTexture, sceneDepthViewDesc);

    NLS::Render::RHI::RHITextureDesc gbufferDepthDesc;
    gbufferDepthDesc.debugName = "DeferredGBufferDepth";
    gbufferDepthDesc.extent = { 320u, 180u, 1u };
    auto gbufferDepthTexture = std::make_shared<TestTexture>(gbufferDepthDesc);

    NLS::Render::RHI::RHITextureViewDesc gbufferDepthViewDesc;
    gbufferDepthViewDesc.debugName = "DeferredGBufferDepthView";
    auto gbufferDepthView = std::make_shared<TestTextureView>(gbufferDepthTexture, gbufferDepthViewDesc);

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.outputColorTexture = colorTexture;
    frameDescriptor.outputDepthStencilTexture = sceneDepthTexture;
    frameDescriptor.outputColorView = colorView;
    frameDescriptor.outputDepthStencilView = sceneDepthView;

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 2u;
    package.renderWidth = frameDescriptor.renderWidth;
    package.renderHeight = frameDescriptor.renderHeight;
    package.recordedDrawCommands.resize(2u);

    NLS::Render::Context::RenderPassCommandInput gridPass;
    gridPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridPass.debugName = "EditorGridPass";
    gridPass.drawCount = 1u;
    gridPass.usesColorAttachment = true;
    gridPass.usesDepthStencilAttachment = true;

    std::vector<NLS::Render::Context::RenderPassCommandInput> appendedPassInputs { gridPass };
    NLS::Render::FrameGraph::ApplyExternalSceneOutputAttachments(
        appendedPassInputs,
        NLS::Render::FrameGraph::ResolveExternalSceneOutputAttachments(
            frameDescriptor,
            "DeferredEditorOverlayColorView",
            "DeferredEditorOverlayDepthView"),
        {
            NLS::Render::Context::RenderPassCommandKind::Helper
        });

    ASSERT_EQ(appendedPassInputs[0].depthStencilAttachmentView, sceneDepthView);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata gridMetadata;
    gridMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    gridMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    gridMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    gridMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(gridMetadata, "EditorGridPass");

    const auto lightGridContext = NLS::Render::FrameGraph::BuildLightGridCompileContext(
        frameDescriptor,
        NLS::Render::FrameGraph::PreparedComputeDispatchSource{},
        nullptr);

    NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;
    resources.gbufferDepthView = gbufferDepthView;

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        appendedPassInputs,
        { gridMetadata });

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    const auto& overlayPass = package.passCommandInputs[2];
    EXPECT_EQ(overlayPass.debugName, "EditorGridPass");
    EXPECT_EQ(overlayPass.depthStencilAttachmentView, gbufferDepthView);
    EXPECT_NE(overlayPass.depthStencilAttachmentView, sceneDepthView);
}

TEST(EditorRenderPathContractTests, DeferredThreadedGBufferUsesQueuedOpaqueDrawCountForPassSplit)
{
    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.visibleOpaqueDrawCount = 3u;
    snapshot.recordedDrawCommands.resize(2u);

    DeferredSnapshotHarness::SynchronizeThreadedDeferredSnapshot(snapshot, 1u);

    EXPECT_EQ(snapshot.visibleOpaqueDrawCount, 1u);
    EXPECT_EQ(snapshot.recordedDrawCommands.size(), 2u);
}

TEST(EditorRenderPathContractTests, DeferredPreparedBuilderFreezesFrameDescriptorCameraForRenderThread)
{
    const std::filesystem::path rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp";

    std::ifstream stream(rendererSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("FreezeDeferredPreparedFrameDescriptor"), std::string::npos);
    EXPECT_NE(source.find("frameDescriptorForBuilder = frozenFrameDescriptor.descriptor"), std::string::npos);
    EXPECT_NE(source.find("lightGridContext.frameDescriptor = frozenFrameDescriptor.descriptor"), std::string::npos);
    EXPECT_NE(source.find("frozenFrameDescriptor = std::move(frozenFrameDescriptor)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, EditorReadbackPrefersGraphExtractionBeforeSwapchainReadbackSurface)
{
    NLS::Render::RHI::RHITextureDesc extractedDesc;
    extractedDesc.debugName = "EditorGraphExtractedColor";
    extractedDesc.extent = { 320u, 180u, 1u };
    auto extractedTexture = std::make_shared<TestTexture>(extractedDesc);

    NLS::Render::RHI::RHITextureDesc backbufferDesc;
    backbufferDesc.debugName = "EditorSwapchainBackbuffer";
    backbufferDesc.extent = { 320u, 180u, 1u };
    auto backbufferTexture = std::make_shared<TestTexture>(backbufferDesc);

    NLS::Render::RHI::RHITextureViewDesc backbufferViewDesc;
    backbufferViewDesc.debugName = "EditorSwapchainBackbufferView";
    auto backbufferView = std::make_shared<TestTextureView>(backbufferTexture, backbufferViewDesc);

    NLS::Render::Context::RenderScenePackage package;
    package.extractedTextures.push_back(extractedTexture);

    NLS::Render::RHI::RHIFrameContext frameContext;
    frameContext.swapchainBackbufferView = backbufferView;

    EXPECT_EQ(
        NLS::Render::FrameGraph::ResolveFrameReadbackTexture(&package, &frameContext),
        extractedTexture);

    const auto visibilityPassInput =
        NLS::Render::FrameGraph::BuildExtractionVisibilityPassInput(package);
    EXPECT_TRUE(visibilityPassInput.requiresDependencyVisibility);
    ASSERT_EQ(visibilityPassInput.textureVisibilityTransitions.size(), 1u);
    EXPECT_EQ(visibilityPassInput.textureVisibilityTransitions[0].texture, extractedTexture);
}

TEST(EditorRenderPathContractTests, EditorReadbackHonorsPreferredReadbackTextureBeforeGenericExtractions)
{
    NLS::Render::RHI::RHITextureDesc preferredDesc;
    preferredDesc.debugName = "EditorPickingReadback";
    preferredDesc.extent = { 320u, 180u, 1u };
    auto preferredTexture = std::make_shared<TestTexture>(preferredDesc);

    NLS::Render::RHI::RHITextureDesc extractedDesc;
    extractedDesc.debugName = "EditorSceneColor";
    extractedDesc.extent = { 320u, 180u, 1u };
    auto extractedTexture = std::make_shared<TestTexture>(extractedDesc);

    NLS::Render::Context::RenderScenePackage package;
    package.preferredReadbackTexture = preferredTexture;
    package.extractedTextures.push_back(extractedTexture);

    EXPECT_EQ(
        NLS::Render::FrameGraph::ResolveFrameReadbackTexture(&package, nullptr),
        preferredTexture);
}

TEST(EditorRenderPathContractTests, EditorHelpersRemainThreadedFrameVisibleWork)
{
    NLS::Editor::Rendering::ThreadedEditorHelperState helperState;
    helperState.gridPassEnabled = true;
    helperState.cameraPassEnabled = true;
    helperState.lightPassEnabled = true;
    helperState.actorPassEnabled = true;
    helperState.debugDrawPassEnabled = true;
    helperState.gridEnabled = true;
    helperState.sceneCameraCount = 1u;
    helperState.sceneLightCount = 2u;
    helperState.hasSelectedActor = true;
    helperState.hasVisibleDebugDrawPrimitives = true;

    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedGridHelperPass(helperState));
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedCameraHelperPass(helperState));
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedLightHelperPass(helperState));
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedOutlineHelperPass(helperState));
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedGizmoHelperPass(helperState));
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedDebugDrawHelperPass(helperState));
    EXPECT_EQ(NLS::Editor::Rendering::CountThreadedEditorHelperPasses(helperState), 6u);

    helperState.actorPassEnabled = false;
    EXPECT_FALSE(NLS::Editor::Rendering::HasThreadedOutlineHelperPass(helperState));
    EXPECT_FALSE(NLS::Editor::Rendering::HasThreadedGizmoHelperPass(helperState));
    EXPECT_EQ(NLS::Editor::Rendering::CountThreadedEditorHelperPasses(helperState), 4u);
}

TEST(EditorRenderPathContractTests, EditorCameraHelpersRespectDebugDrawCameraToggle)
{
    NLS::Editor::Rendering::ThreadedEditorHelperState helperState;
    helperState.cameraPassEnabled = true;
    helperState.debugDrawEnabled = true;
    helperState.debugDrawCamera = false;
    helperState.sceneCameraCount = 1u;

    EXPECT_FALSE(NLS::Editor::Rendering::HasThreadedCameraHelperPass(helperState));

    helperState.debugDrawCamera = true;
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedCameraHelperPass(helperState));
}

TEST(EditorRenderPathContractTests, EditorLightHelpersRespectDebugDrawLightingToggle)
{
    NLS::Editor::Rendering::ThreadedEditorHelperState helperState;
    helperState.lightPassEnabled = true;
    helperState.debugDrawEnabled = true;
    helperState.debugDrawLighting = false;
    helperState.sceneLightCount = 1u;

    EXPECT_FALSE(NLS::Editor::Rendering::HasThreadedLightHelperPass(helperState));

    helperState.debugDrawLighting = true;
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedLightHelperPass(helperState));
}

TEST(EditorRenderPathContractTests, PickingAuxiliaryPassDoesNotSatisfySceneOutputClear)
{
    NLS::Render::Context::RenderPassCommandInput sceneHelperPass;
    sceneHelperPass.usesColorAttachment = true;
    sceneHelperPass.clearColor = false;

    NLS::Render::Context::RenderPassCommandInput pickingPass;
    pickingPass.usesColorAttachment = true;
    pickingPass.clearColor = true;

    NLS::Render::RHI::RHITextureDesc pickingTextureDesc;
    pickingTextureDesc.debugName = "PickingColor";
    auto pickingTexture = std::make_shared<TestTexture>(pickingTextureDesc);

    NLS::Render::RHI::RHITextureViewDesc pickingViewDesc;
    pickingViewDesc.debugName = "PickingColorView";
    pickingPass.colorAttachmentViews.push_back(
        std::make_shared<TestTextureView>(pickingTexture, pickingViewDesc));

    EXPECT_TRUE(NLS::Editor::Rendering::WritesThreadedEditorSceneOutput(sceneHelperPass));
    EXPECT_FALSE(NLS::Editor::Rendering::WritesThreadedEditorSceneOutput(pickingPass));
}
