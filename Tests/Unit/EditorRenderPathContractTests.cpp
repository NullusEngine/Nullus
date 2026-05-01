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
#include "ReflectionTestUtils.h"

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

    std::filesystem::path GetRepositoryRoot()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    }

    std::string ReadRepositorySource(std::string_view relativePath)
    {
        return NLS::Tests::Reflection::ReadAllText(GetRepositoryRoot() / relativePath);
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

TEST(EditorRenderPathContractTests, EditorProductEntryDefaultsToThreadedRenderingMainline)
{
    const auto editorMainSource = ReadRepositorySource("Project/Editor/Main.cpp");

    EXPECT_NE(editorMainSource.find("bool enableThreadedRendering = true;"), std::string::npos);
    EXPECT_EQ(editorMainSource.find("bool enableThreadedRendering = false;"), std::string::npos);
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
        nullptr,
        std::nullopt);
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

TEST(EditorRenderPathContractTests, SceneViewRequiresSelectedActorBeforeStartingGizmoPicking)
{
    const auto sceneViewSource = ReadRepositorySource("Project/Editor/Panels/SceneView.cpp");

    EXPECT_NE(
        sceneViewSource.find("if (m_highlightedGizmoDirection && EDITOR_EXEC(IsAnyActorSelected()))"),
        std::string::npos);
    EXPECT_EQ(
        sceneViewSource.find("*EDITOR_EXEC(GetSelectedActor()),"),
        std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewCameraSuppressesCursorCaptureTransitionDelta)
{
    const auto cameraControllerHeader = ReadRepositorySource("Project/Editor/Core/CameraController.h");
    const auto cameraControllerSource = ReadRepositorySource("Project/Editor/Core/CameraController.cpp");

    EXPECT_NE(
        cameraControllerHeader.find("m_pendingMouseDeltaSuppressionFrames"),
        std::string::npos);
    EXPECT_NE(
        cameraControllerHeader.find("SuppressMouseDeltaAfterCursorCapture()"),
        std::string::npos);
    EXPECT_NE(
        cameraControllerSource.find("ConsumeSuppressedMouseDelta(pos)"),
        std::string::npos);
    EXPECT_NE(
        cameraControllerSource.find("SuppressMouseDeltaAfterCursorCapture();"),
        std::string::npos);
    EXPECT_NE(
        cameraControllerHeader.find("ClampMouseDeltaForCameraControl"),
        std::string::npos);
    EXPECT_NE(
        cameraControllerSource.find("kMaxCameraMouseDeltaPerFrame = 16.0f"),
        std::string::npos);
    EXPECT_NE(
        cameraControllerSource.find("ClampMouseDeltaForCameraControl(mouseOffset)"),
        std::string::npos);
    EXPECT_EQ(
        cameraControllerSource.find("m_firstMouse = false;\n    --m_pendingMouseDeltaSuppressionFrames"),
        std::string::npos);
    const auto fpsMouseOffset =
        cameraControllerSource.find("void Editor::Core::CameraController::HandleCameraFPSMouse");
    ASSERT_NE(fpsMouseOffset, std::string::npos);
    const auto fpsMouseInitializationOffset =
        cameraControllerSource.find("m_ypr = Maths::Quaternion::EulerAngles(m_camera.GetRotation())", fpsMouseOffset);
    const auto fpsMouseFirstReturnOffset = cameraControllerSource.find("return;", fpsMouseInitializationOffset);
    const auto fpsMouseRotationWriteOffset = cameraControllerSource.find("m_camera.SetRotation", fpsMouseInitializationOffset);
    ASSERT_NE(fpsMouseInitializationOffset, std::string::npos);
    ASSERT_NE(fpsMouseFirstReturnOffset, std::string::npos);
    ASSERT_NE(fpsMouseRotationWriteOffset, std::string::npos);
    EXPECT_LT(fpsMouseFirstReturnOffset, fpsMouseRotationWriteOffset);
    const auto activeMouseBlockOffset =
        cameraControllerSource.find("if (m_rightMousePressed || m_middleMousePressed || m_leftMousePressed)");
    ASSERT_NE(activeMouseBlockOffset, std::string::npos);
    const auto consumeSuppressedOffset =
        cameraControllerSource.find("ConsumeSuppressedMouseDelta(pos)", activeMouseBlockOffset);
    const auto firstMouseOffset = cameraControllerSource.find("if (m_firstMouse)", activeMouseBlockOffset);
    ASSERT_NE(consumeSuppressedOffset, std::string::npos);
    ASSERT_NE(firstMouseOffset, std::string::npos);
    EXPECT_LT(consumeSuppressedOffset, firstMouseOffset);
    EXPECT_EQ(cameraControllerSource.find("ECursorMode::DISABLED"), std::string::npos);
    EXPECT_NE(cameraControllerSource.find("ECursorMode::HIDDEN"), std::string::npos);
}

TEST(EditorRenderPathContractTests, LightGridDefaultAmbientFloorStaysBelowOverexposureThreshold)
{
    const auto lightGridSource = ReadRepositorySource("Runtime/Engine/Rendering/LightGridPrepass.cpp");

    EXPECT_NE(lightGridSource.find("constexpr float kDefaultAmbientFloor = 0.05f;"), std::string::npos);
    EXPECT_NE(lightGridSource.find("kDefaultAmbientFloor,"), std::string::npos);
    EXPECT_EQ(lightGridSource.find("0.20f,"), std::string::npos);
}

TEST(EditorRenderPathContractTests, RenderValidationSceneKeepsNeutralAmbientLight)
{
    const auto renderValidationScene = ReadRepositorySource("TestProject/Assets/Scenes/RenderValidation.scene");

    EXPECT_NE(renderValidationScene.find("\"name\": \"Ambient Light\""), std::string::npos);
    EXPECT_NE(renderValidationScene.find("\"intensity\": 0.1"), std::string::npos);
    EXPECT_EQ(renderValidationScene.find("\"intensity\": 0.8"), std::string::npos);
}

TEST(EditorRenderPathContractTests, RenderValidationSceneKeepsDirectionalLightFacingVisibleCubeFace)
{
    const auto renderValidationScene = ReadRepositorySource("TestProject/Assets/Scenes/RenderValidation.scene");
    const auto directionalLightOffset = renderValidationScene.find("\"name\": \"Directional Light\"");
    ASSERT_NE(directionalLightOffset, std::string::npos);

    const auto validationCubeOffset = renderValidationScene.find("\"name\": \"Validation Cube\"");
    ASSERT_NE(validationCubeOffset, std::string::npos);
    const auto directionalLightBlock = renderValidationScene.substr(
        directionalLightOffset,
        validationCubeOffset - directionalLightOffset);

    EXPECT_NE(directionalLightBlock.find("\"x\": 0.0"), std::string::npos);
    EXPECT_NE(directionalLightBlock.find("\"y\": 0.0"), std::string::npos);
    EXPECT_NE(directionalLightBlock.find("\"z\": 0.0"), std::string::npos);
    EXPECT_NE(directionalLightBlock.find("\"w\": 1.0"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DefaultLightedSceneKeepsDirectionalLightFacingVisibleCubeFace)
{
    const auto sceneManagerSource = ReadRepositorySource("Runtime/Engine/SceneSystem/SceneManager.cpp");

    EXPECT_NE(
        sceneManagerSource.find("tr->SetLocalRotation(Maths::Quaternion({60.0f, 40.0f, 0.0f}));"),
        std::string::npos);
    EXPECT_EQ(
        sceneManagerSource.find("tr->SetLocalRotation(Maths::Quaternion({120.0f, -40.0f, 0.0f}));"),
        std::string::npos);
}

TEST(EditorRenderPathContractTests, RuntimeContextsStopRenderingThreadsBeforeResourceUnload)
{
    const auto editorApplicationSource = ReadRepositorySource("Project/Editor/Core/Application.cpp");
    const auto gameApplicationSource = ReadRepositorySource("Project/Game/Core/Application.cpp");
    const auto editorContextSource = ReadRepositorySource("Project/Editor/Core/Context.cpp");
    const auto gameContextSource = ReadRepositorySource("Project/Game/Core/Context.cpp");

    const auto applicationDestructorOffset = editorApplicationSource.find("Editor::Core::Application::~Application()");
    ASSERT_NE(applicationDestructorOffset, std::string::npos);
    const auto applicationShutdownOffset =
        editorApplicationSource.find("m_context.ShutdownThreadedRendering()", applicationDestructorOffset);
    ASSERT_NE(applicationShutdownOffset, std::string::npos);

    const auto gameApplicationDestructorOffset =
        gameApplicationSource.find("Game::Core::Application::~Application()");
    ASSERT_NE(gameApplicationDestructorOffset, std::string::npos);
    const auto gameApplicationShutdownOffset =
        gameApplicationSource.find("m_context.ShutdownThreadedRendering()", gameApplicationDestructorOffset);
    ASSERT_NE(gameApplicationShutdownOffset, std::string::npos);

    const auto assertShutdownBeforeUnload = [](const std::string& source, const std::string& destructorName)
    {
        const auto destructorOffset = source.find(destructorName);
        ASSERT_NE(destructorOffset, std::string::npos);

        const auto shutdownOffset = source.find("ShutdownThreadedRendering()", destructorOffset);
        const auto unloadOffset = source.find("UnloadResources()", destructorOffset);
        ASSERT_NE(shutdownOffset, std::string::npos);
        ASSERT_NE(unloadOffset, std::string::npos);
        EXPECT_LT(shutdownOffset, unloadOffset);

        const auto methodOffset = source.find("::ShutdownThreadedRendering()");
        ASSERT_NE(methodOffset, std::string::npos);
        EXPECT_NE(source.find("driver->ShutdownThreadedRendering()", methodOffset), std::string::npos);
    };

    assertShutdownBeforeUnload(editorContextSource, "Editor::Core::Context::~Context()");
    assertShutdownBeforeUnload(gameContextSource, "Game::Context::~Context()");
}
