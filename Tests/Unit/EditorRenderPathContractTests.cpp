#include <gtest/gtest.h>

#include <fg/Blackboard.hpp>
#include <fg/FrameGraph.hpp>

#include "Rendering/EditorHelperLifecycle.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilder.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace
{
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
