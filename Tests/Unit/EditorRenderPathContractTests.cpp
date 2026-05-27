#include <gtest/gtest.h>

#include <fg/Blackboard.hpp>
#include <fg/FrameGraph.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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
    std::string NormalizeSourceLineEndings(std::string text)
    {
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        return text;
    }

    std::string ReadSourceText(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return NormalizeSourceLineEndings(buffer.str());
    }

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

TEST(EditorRenderPathContractTests, EditorPickingAndOutlineRetryDeferredMeshAndMaterialReferencesAtSubmitTime)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto pickingSourcePath = root / "Project/Editor/Rendering/PickingRenderPass.cpp";
    const auto outlineSourcePath = root / "Project/Editor/Rendering/OutlineRenderer.cpp";

    std::ifstream pickingStream(pickingSourcePath, std::ios::binary);
    const std::string pickingSource{
        std::istreambuf_iterator<char>(pickingStream),
        std::istreambuf_iterator<char>()};
    std::ifstream outlineStream(outlineSourcePath, std::ios::binary);
    const std::string outlineSource{
        std::istreambuf_iterator<char>(outlineStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(pickingSource.empty());
    ASSERT_FALSE(outlineSource.empty());
    EXPECT_NE(pickingSource.find("meshFilter->ResolveMesh()"), std::string::npos);
    EXPECT_NE(outlineSource.find("meshFilter->ResolveMesh()"), std::string::npos);
    EXPECT_NE(pickingSource.find("modelRenderer->ResolveMaterials()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, EditorMeshFilterConsumersResolveDeferredMeshReferences)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto meshRendererPath = root / "Runtime/Engine/Components/MeshRenderer.cpp";
    const std::vector<std::filesystem::path> deferredConsumerPaths = {
        root / "Project/Editor/Core/CameraController.cpp",
        root / "Project/Editor/Core/SceneViewImGuizmo.cpp",
        root / "Project/Editor/Rendering/DebugSceneRenderer.cpp"
    };

    std::ifstream meshRendererStream(meshRendererPath, std::ios::binary);
    const std::string meshRendererSource{
        std::istreambuf_iterator<char>(meshRendererStream),
        std::istreambuf_iterator<char>()};
    ASSERT_FALSE(meshRendererSource.empty());
    EXPECT_NE(meshRendererSource.find("ResolveMesh()"), std::string::npos);
    EXPECT_EQ(meshRendererSource.find("GetModel()"), std::string::npos);

    for (const auto& sourcePath : deferredConsumerPaths)
    {
        std::ifstream stream(sourcePath, std::ios::binary);
        const std::string source{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};

        ASSERT_FALSE(source.empty()) << sourcePath.string();
        EXPECT_NE(source.find("ResolveMesh()"), std::string::npos) << sourcePath.string();
        EXPECT_EQ(source.find("GetModel()"), std::string::npos) << sourcePath.string();
    }

    const auto editorRoot = root / "Project/Editor";
    const auto resourceLifecyclePath = root / "Project/Editor/Core/EditorActions.cpp";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(editorRoot))
    {
        if (!entry.is_regular_file())
            continue;

        const auto extension = entry.path().extension();
        if (extension != ".cpp" && extension != ".h")
            continue;

        std::ifstream stream(entry.path(), std::ios::binary);
        const std::string source{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
        ASSERT_FALSE(source.empty()) << entry.path().string();
        if (source.find("MeshFilter") == std::string::npos)
            continue;

        const auto relativePath = std::filesystem::relative(entry.path(), root).generic_string();
        if (std::filesystem::equivalent(entry.path(), resourceLifecyclePath))
            continue;

        EXPECT_EQ(source.find("GetModel()"), std::string::npos) << relativePath;
    }
}

TEST(EditorRenderPathContractTests, SceneRenderFrameDoesNotLoadRendererAssetsInsideMainPass)
{
    const auto rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.cpp";
    const auto renderSceneSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/RenderScene.cpp";

    std::ifstream stream(rendererSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    std::ifstream renderSceneStream(renderSceneSourcePath, std::ios::binary);
    const std::string renderSceneSource{
        std::istreambuf_iterator<char>(renderSceneStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(renderSceneSource.empty());
    const auto parseScene = source.find("BaseSceneRenderer::AllDrawables BaseSceneRenderer::ParseScene()");
    ASSERT_NE(parseScene, std::string::npos);

    const auto parseSceneBody = source.substr(parseScene);
    EXPECT_EQ(source.find("ResolveModel()"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("meshFilter->ResolveMesh()"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("primitive.meshRenderer->ResolveMaterialAtIndex("), std::string::npos);
    EXPECT_EQ(source.find("MaterialManager)[\":Materials\\\\Default.mat\"]"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("primitive.meshRenderer->ResolveMaterialAtIndex("), std::string::npos);
    EXPECT_NE(source.find("GetResource(\":Materials\\\\Default.mat\", false)"), std::string::npos);
    EXPECT_NE(parseSceneBody.find("m_renderScene.Synchronize(scene"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DeferredMaterialTextureBindingDoesNotColdLoadTextures)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    std::ifstream stream(sourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto bindFunction = source.find("bool BindDeferredMaterialTextures(");
    ASSERT_NE(bindFunction, std::string::npos);
    const auto nextFunction = source.find("template<typename FrameBudgetExpired>", bindFunction + 1u);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto body = source.substr(bindFunction, nextFunction - bindFunction);

    EXPECT_NE(body.find("textureManager.GetResource(texturePath, false)"), std::string::npos);
    EXPECT_EQ(body.find("textureManager.GetResource(texturePath, true)"), std::string::npos);
    EXPECT_EQ(body.find("LoadResource(texturePath)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, InspectorDelayedComponentCloseResolvesOwnerByInstanceId)
{
    const auto inspectorSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/Inspector.cpp";

    std::ifstream stream(inspectorSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto closeEvent = source.find("header.CloseEvent +=");
    ASSERT_NE(closeEvent, std::string::npos);
    const auto closeBody = source.substr(closeEvent);

    EXPECT_NE(closeBody.find("ownerInstanceID"), std::string::npos);
    EXPECT_NE(closeBody.find("NLS::Object::IDToPointer<NLS::Engine::GameObject>(ownerInstanceID)"), std::string::npos);
    EXPECT_EQ(closeBody.find("ownerGameObject"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneRenderFrameKeepsMeshesVisibleWhenMaterialComponentIsMissing)
{
    const auto rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.cpp";
    const auto renderSceneSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/RenderScene.cpp";

    std::ifstream stream(rendererSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    std::ifstream renderSceneStream(renderSceneSourcePath, std::ios::binary);
    const std::string renderSceneSource{
        std::istreambuf_iterator<char>(renderSceneStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(renderSceneSource.empty());
    const auto parseScene = source.find("BaseSceneRenderer::AllDrawables BaseSceneRenderer::ParseScene()");
    ASSERT_NE(parseScene, std::string::npos);

    const auto parseSceneBody = source.substr(parseScene);
    (void)parseSceneBody;
    EXPECT_EQ(parseSceneBody.find("if (!mesh || !meshRenderer)"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("if (primitive.mesh == nullptr)"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("primitive.meshRenderer != nullptr && mesh.GetMaterialIndex() < Components::MeshRenderer::kMaxMaterialCount"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("return options.defaultMaterial != nullptr && options.defaultMaterial->IsValid()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneFallbackMaterialIsRendererOwnedAndStartupPreloaded)
{
    const auto rendererHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.h";
    const auto rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.cpp";
    const auto editorContextPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Context.cpp";
    const auto gameContextPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Game/Core/Context.cpp";

    const auto rendererHeader = ReadSourceText(rendererHeaderPath);
    const auto rendererSource = ReadSourceText(rendererSourcePath);
    const auto editorContext = ReadSourceText(editorContextPath);
    const auto gameContext = ReadSourceText(gameContextPath);

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(editorContext.empty());
    ASSERT_FALSE(gameContext.empty());

    EXPECT_NE(rendererHeader.find("std::unique_ptr<Material> m_sceneFallbackMaterial"), std::string::npos);
    EXPECT_NE(rendererSource.find("void BaseSceneRenderer::PreloadSceneFallbackShader("), std::string::npos);
    EXPECT_NE(rendererSource.find("shaderManager.GetResource(resourcePath, false)"), std::string::npos);
    EXPECT_NE(rendererSource.find("shaderManager.GetResource(resourcePath, true)"), std::string::npos);
    EXPECT_NE(rendererSource.find("BaseSceneRenderer failed to preload a scene fallback shader"), std::string::npos);
    EXPECT_EQ(rendererSource.find("static NLS::Render::Resources::Material fallbackMaterial"), std::string::npos);
    EXPECT_EQ(rendererSource.find("static NLS::Render::Resources::Shader* boundShader"), std::string::npos);
    EXPECT_NE(editorContext.find("BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager)"), std::string::npos);
    EXPECT_NE(gameContext.find("BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneDrawableCollectionAvoidsNodeAllocatedMultimapSort)
{
    const auto rendererHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.h";
    const auto rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.cpp";
    const auto renderSceneSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/RenderScene.cpp";

    std::ifstream headerStream(rendererHeaderPath, std::ios::binary);
    const std::string headerSource{
        std::istreambuf_iterator<char>(headerStream),
        std::istreambuf_iterator<char>()};
    std::ifstream sourceStream(rendererSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(sourceStream),
        std::istreambuf_iterator<char>()};
    std::ifstream renderSceneStream(renderSceneSourcePath, std::ios::binary);
    const std::string renderSceneSource{
        std::istreambuf_iterator<char>(renderSceneStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(renderSceneSource.empty());
    const auto parseScene = source.find("BaseSceneRenderer::AllDrawables BaseSceneRenderer::ParseScene()");
    ASSERT_NE(parseScene, std::string::npos);
    const auto parseSceneBody = source.substr(parseScene);

    EXPECT_EQ(headerSource.find("std::multimap<float, Drawable"), std::string::npos);
    EXPECT_NE(headerSource.find("std::vector<std::pair<float, Drawable>>"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("std::stable_sort("), std::string::npos);
    EXPECT_EQ(renderSceneSource.find("SortVisibleQueue(output.opaques, std::less<float>{})"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("FinalizeOpaqueQueue(output.opaques)"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("SortVisibleQueue(output.transparents, std::greater<float>{})"), std::string::npos);
    EXPECT_NE(parseSceneBody.find("SortSceneDrawables(skyboxes, std::less<float>{})"), std::string::npos);
    EXPECT_EQ(parseSceneBody.find(".emplace(distanceToActor, drawable)"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("std::move(drawable)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneDrawableCollectionUsesPersistentRenderSceneCache)
{
    const auto rendererHeaderPath = std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Engine/Rendering/BaseSceneRenderer.h";
    const auto rendererSourcePath = std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime/Engine/Rendering/BaseSceneRenderer.cpp";

    std::ifstream headerStream(rendererHeaderPath, std::ios::binary);
    const std::string headerSource{
        std::istreambuf_iterator<char>(headerStream),
        std::istreambuf_iterator<char>()};
    std::ifstream sourceStream(rendererSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(sourceStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_NE(headerSource.find("RenderScene m_renderScene"), std::string::npos);
    EXPECT_NE(source.find("m_renderScene.Synchronize(scene"), std::string::npos);
    EXPECT_NE(source.find("m_renderScene.GatherVisibleCommands"), std::string::npos);
    EXPECT_EQ(source.find("for (auto* modelRenderer : fastAccess.modelRenderers)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, RegisteredObjectDataShadersUseInstanceIdOffset)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::vector<std::filesystem::path> shaderPaths = {
        root / "App/Assets/Engine/Shaders/Standard.hlsl",
        root / "App/Assets/Engine/Shaders/StandardPBR.hlsl",
        root / "App/Assets/Engine/Shaders/Lambert.hlsl",
        root / "App/Assets/Engine/Shaders/DeferredGBuffer.hlsl"
    };

    for (const auto& shaderPath : shaderPaths)
    {
        std::ifstream shaderStream(shaderPath, std::ios::binary);
        const std::string shaderSource{
            std::istreambuf_iterator<char>(shaderStream),
            std::istreambuf_iterator<char>()};

        ASSERT_FALSE(shaderSource.empty()) << shaderPath.string();
        EXPECT_NE(shaderSource.find("StructuredBuffer<float4x4> ObjectData : register(t0, space3)"), std::string::npos)
            << shaderPath.string();
        EXPECT_NE(shaderSource.find("uint instanceId : SV_InstanceID"), std::string::npos)
            << shaderPath.string();
        EXPECT_NE(shaderSource.find("ObjectData[u_ObjectIndex + instanceId]"), std::string::npos)
            << shaderPath.string();
        EXPECT_EQ(shaderSource.find("ObjectData[u_ObjectIndex]"), std::string::npos)
            << shaderPath.string();
    }
}

TEST(EditorRenderPathContractTests, UiPanelAndHierarchyDrawPathsExposeActionablePerformanceScopes)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto panelSourcePath = root / "Runtime/UI/Panels/APanel.cpp";
    const auto panelHeaderPath = root / "Runtime/UI/Panels/APanel.h";
    const auto hierarchyHeaderPath = root / "Project/Editor/Panels/Hierarchy.h";
    const auto treeNodeSourcePath = root / "Runtime/UI/Widgets/Layout/TreeNode.cpp";
    const auto treeNodeHeaderPath = root / "Runtime/UI/Widgets/Layout/TreeNode.h";

    std::ifstream panelStream(panelSourcePath, std::ios::binary);
    const std::string panelSource{
        std::istreambuf_iterator<char>(panelStream),
        std::istreambuf_iterator<char>()};
    std::ifstream panelHeaderStream(panelHeaderPath, std::ios::binary);
    const std::string panelHeader{
        std::istreambuf_iterator<char>(panelHeaderStream),
        std::istreambuf_iterator<char>()};
    std::ifstream hierarchyHeaderStream(hierarchyHeaderPath, std::ios::binary);
    const std::string hierarchyHeader{
        std::istreambuf_iterator<char>(hierarchyHeaderStream),
        std::istreambuf_iterator<char>()};
    std::ifstream treeNodeStream(treeNodeSourcePath, std::ios::binary);
    const std::string treeNodeSource{
        std::istreambuf_iterator<char>(treeNodeStream),
        std::istreambuf_iterator<char>()};
    std::ifstream treeNodeHeaderStream(treeNodeHeaderPath, std::ios::binary);
    const std::string treeNodeHeader{
        std::istreambuf_iterator<char>(treeNodeHeaderStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(panelSource.empty());
    ASSERT_FALSE(panelHeader.empty());
    ASSERT_FALSE(hierarchyHeader.empty());
    ASSERT_FALSE(treeNodeSource.empty());
    ASSERT_FALSE(treeNodeHeader.empty());

    EXPECT_NE(panelSource.find("const char* APanel::GetProfilerScopeName()"), std::string::npos);
    EXPECT_NE(panelSource.find("m_cachedProfilerScopeName = \"Panel::Draw:\""), std::string::npos);
    EXPECT_NE(panelSource.find("GetProfilerName()"), std::string::npos);
    EXPECT_NE(panelHeader.find("uint64_t GetLastDrawDurationUs() const"), std::string::npos);
    EXPECT_NE(panelSource.find("m_lastDrawDurationUs"), std::string::npos);
    EXPECT_NE(hierarchyHeader.find("size_t GetHierarchyNodeCount() const"), std::string::npos);
    EXPECT_NE(hierarchyHeader.find("GetHierarchyNodeCount() const { return m_widgetGameObjectLink.size(); }"), std::string::npos);
    EXPECT_NE(treeNodeHeader.find("std::string m_imguiLabel"), std::string::npos);
    EXPECT_NE(treeNodeHeader.find("std::string m_cachedName"), std::string::npos);
    EXPECT_NE(treeNodeHeader.find("std::string m_cachedWidgetID"), std::string::npos);
    EXPECT_NE(treeNodeSource.find("const char* TreeNode::GetImGuiLabel()"), std::string::npos);
    EXPECT_NE(treeNodeSource.find("m_cachedWidgetID != m_widgetID"), std::string::npos);
    EXPECT_NE(treeNodeSource.find("m_imguiLabel = name + \"###\" + m_widgetID"), std::string::npos);
    EXPECT_NE(treeNodeSource.find("ImGui::TreeNodeEx(GetImGuiLabel(), flags)"), std::string::npos);
    EXPECT_EQ(treeNodeSource.find("const std::string label = name + \"###\" + m_widgetID"), std::string::npos);
    EXPECT_EQ(treeNodeHeader.find("textColorProvider"), std::string::npos);
    EXPECT_EQ(treeNodeSource.find("textColorProvider"), std::string::npos);
}

TEST(EditorRenderPathContractTests, WidgetContainersSkipPerFrameGarbageScansWhenClean)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto containerHeaderPath = root / "Runtime/UI/Internal/WidgetContainer.h";
    const auto containerSourcePath = root / "Runtime/UI/Internal/WidgetContainer.cpp";
    const auto widgetHeaderPath = root / "Runtime/UI/Widgets/AWidget.h";
    const auto widgetSourcePath = root / "Runtime/UI/Widgets/AWidget.cpp";
    const auto columnsSourcePath = root / "Runtime/UI/Widgets/Layout/Columns.cpp";

    std::ifstream headerStream(containerHeaderPath, std::ios::binary);
    const std::string headerSource{
        std::istreambuf_iterator<char>(headerStream),
        std::istreambuf_iterator<char>()};
    std::ifstream sourceStream(containerSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(sourceStream),
        std::istreambuf_iterator<char>()};
    std::ifstream widgetHeaderStream(widgetHeaderPath, std::ios::binary);
    const std::string widgetHeaderSource{
        std::istreambuf_iterator<char>(widgetHeaderStream),
        std::istreambuf_iterator<char>()};
    std::ifstream widgetStream(widgetSourcePath, std::ios::binary);
    const std::string widgetSource{
        std::istreambuf_iterator<char>(widgetStream),
        std::istreambuf_iterator<char>()};
    std::ifstream columnsStream(columnsSourcePath, std::ios::binary);
    const std::string columnsSource{
        std::istreambuf_iterator<char>(columnsStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(widgetHeaderSource.empty());
    ASSERT_FALSE(widgetSource.empty());
    ASSERT_FALSE(columnsSource.empty());

    EXPECT_NE(headerSource.find("bool m_garbageCollectionDirty = false"), std::string::npos);
    EXPECT_NE(headerSource.find("void MarkGarbageCollectionDirty()"), std::string::npos);
    EXPECT_NE(headerSource.find("const std::vector<std::pair<UI::Widgets::AWidget*, Internal::EMemoryMode>>& GetWidgets() const"), std::string::npos);
    EXPECT_EQ(headerSource.find("Internal::EMemoryMode>>& GetWidgets();"), std::string::npos);
    EXPECT_NE(source.find("if (!m_garbageCollectionDirty)"), std::string::npos);
    EXPECT_NE(source.find("m_garbageCollectionDirty = false"), std::string::npos);
    EXPECT_NE(source.find("if (widget.IsDestroyed())"), std::string::npos);
    EXPECT_NE(headerSource.find("if (instance.IsDestroyed())"), std::string::npos);
    EXPECT_NE(source.find("MarkGarbageCollectionDirty();"), std::string::npos);
    EXPECT_NE(source.find("p_widget.GetParent() == this"), std::string::npos);
    EXPECT_NE(headerSource.find("std::optional<Internal::EMemoryMode> ExtractWidget(Widgets::AWidget& p_widget)"), std::string::npos);
    EXPECT_NE(source.find("parent->ExtractWidget(p_widget)"), std::string::npos);
    EXPECT_NE(source.find("memoryMode = *previousMemoryMode"), std::string::npos);
    EXPECT_LT(source.find("parent->ExtractWidget(p_widget)"), source.find("m_widgets.emplace_back"));
    EXPECT_NE(source.find("SetParent(nullptr)"), std::string::npos);
    EXPECT_NE(widgetHeaderSource.find("Internal::WidgetContainer* m_parent = nullptr"), std::string::npos);
    EXPECT_NE(widgetSource.find("m_parent->MarkGarbageCollectionDirty()"), std::string::npos);
    EXPECT_NE(source.find("void WidgetContainer::DrawWidgets()"), std::string::npos);
    EXPECT_NE(columnsSource.find("CollectGarbages();"), std::string::npos);
}

TEST(EditorRenderPathContractTests, PickingKeepsLoadedMeshesPickableWhenMaterialComponentIsMissing)
{
    const auto pickingSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/PickingRenderPass.cpp";

    std::ifstream stream(pickingSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    EXPECT_EQ(source.find("mesh == nullptr || meshRenderer == nullptr"), std::string::npos);
    EXPECT_NE(source.find("if (mesh == nullptr)"), std::string::npos);
    EXPECT_NE(source.find("const auto* materials = &modelRenderer->ResolveMaterials()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelDropLoadsMaterialsOnlyThroughResolutionQueue)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto collector = source.find("void CollectPrefabAssetResolutionTasks(");
    ASSERT_NE(collector, std::string::npos);

    const auto queueCode = source.substr(collector);
    EXPECT_EQ(queueCode.find("meshRenderer->SetMaterialPaths(resolvedPaths)"), std::string::npos);
    EXPECT_NE(queueCode.find("meshRenderer.SetMaterialPathHints(task.materialPaths)"), std::string::npos);
    EXPECT_NE(queueCode.find("if (!task.materialHintsApplied)"), std::string::npos);
    EXPECT_NE(queueCode.find("task.materialHintsApplied = true"), std::string::npos);
    EXPECT_NE(queueCode.find("FillEmptySlotsWithMaterial"), std::string::npos);
    EXPECT_NE(queueCode.find("ApplyVisibleFallbackMaterial"), std::string::npos);
    EXPECT_EQ(source.find("if (!meshRenderer->GetMaterialPaths().empty())"), std::string::npos);
    EXPECT_NE(source.find("HasResolvedMaterialBindings(*meshRenderer)"), std::string::npos);
    const auto resolvedBegin = source.find("bool HasResolvedMaterialBindings(");
    ASSERT_NE(resolvedBegin, std::string::npos);
    const auto resolvedEnd = source.find("void CountResolvedRendererResources(", resolvedBegin);
    ASSERT_NE(resolvedEnd, std::string::npos);
    const auto resolvedCode = source.substr(resolvedBegin, resolvedEnd - resolvedBegin);
    EXPECT_NE(resolvedCode.find("if (!material || !material->IsValid() || material->path != paths[index])"), std::string::npos);
    EXPECT_NE(queueCode.find("resolvedPaths.push_back(materialPath.value_or(std::string {}))"), std::string::npos);
    EXPECT_EQ(queueCode.find("if (auto materialPath = ResolvePrefabAssetPath(prefab, value))\n                            resolvedPaths.push_back(*materialPath)"), std::string::npos);
    EXPECT_EQ(queueCode.find("materialManager[resolvedPaths[index]]"), std::string::npos);
    EXPECT_EQ(queueCode.find("materialManager.GetResource(resolvedPaths[index], false)"), std::string::npos);
    EXPECT_EQ(queueCode.find("materialManager.GetResource(task.materialPaths[index], true)"), std::string::npos);
    EXPECT_NE(queueCode.find("materialManager.GetResource(task.materialPaths[index], false)"), std::string::npos);
    EXPECT_NE(queueCode.find("meshRenderer.SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *material)"), std::string::npos);
    EXPECT_EQ(queueCode.find("meshRenderer.SetMaterialAtIndex(static_cast<uint8_t>(index), *material)"), std::string::npos);
    EXPECT_NE(queueCode.find("++task.nextMaterialSlot"), std::string::npos);
    EXPECT_NE(queueCode.find("unresolvedMaterialSlots"), std::string::npos);
    EXPECT_NE(queueCode.find("failedMaterialSlots"), std::string::npos);
    EXPECT_NE(queueCode.find("ImportJobTerminalStatus::Failed"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelDropDoesNotQueueExplicitModelPackage)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto collector = source.find("void CollectPrefabAssetResolutionTasks(");
    ASSERT_NE(collector, std::string::npos);
    const auto queueCode = source.substr(collector);

    EXPECT_EQ(source.find("RendererResourceResolutionTaskKind::ModelPackage"), std::string::npos);
    EXPECT_EQ(source.find("std::string modelPackagePath"), std::string::npos);
    EXPECT_EQ(source.find("FindPrefabMainModelPackagePath"), std::string::npos);
    EXPECT_EQ(queueCode.find("modelPackageTask.kind = RendererResourceResolutionTaskKind::ModelPackage"), std::string::npos);
    EXPECT_EQ(queueCode.find("tasks.push_back(std::move(modelPackageTask))"), std::string::npos);
    EXPECT_EQ(source.find("modelManager.GetResource(task.modelPackagePath"), std::string::npos);
    EXPECT_EQ(source.find("state->mainModelPackageReady"), std::string::npos);
    EXPECT_EQ(source.find("bool ProbeMainModelPackageCache("), std::string::npos);
    EXPECT_EQ(source.find("if (task.kind == RendererResourceResolutionTaskKind::ModelPackage)"), std::string::npos);
    EXPECT_NE(source.find("state->remainingTasks.push_front(std::move(task));"), std::string::npos);
    EXPECT_NE(source.find("std::deque<RendererResourceResolutionTask> remainingTasks"), std::string::npos);
    EXPECT_NE(source.find("PopNextRemainingTask("), std::string::npos);
    EXPECT_EQ(source.find("state->remainingTasks.erase("), std::string::npos);
    EXPECT_EQ(source.find("state->inFlightTasks.erase("), std::string::npos);
    EXPECT_EQ(source.find("Core/RendererResourceResolutionFailure.h"), std::string::npos);
    EXPECT_EQ(source.find("auto failMainModelPackage ="), std::string::npos);
    EXPECT_EQ(source.find("FailRendererMainModelPackageResolution("), std::string::npos);
    const auto stepBegin = source.find("void RunRendererResourceResolutionStep(");
    ASSERT_NE(stepBegin, std::string::npos);
    const auto stepEnd = source.find("NLS_LOG_INFO(\n        \"Renderer resources ready for prefab instance", stepBegin);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepCode = source.substr(stepBegin, stepEnd - stepBegin);
    EXPECT_EQ(stepCode.find("if (task.kind == RendererResourceResolutionTaskKind::ModelPackage)\n                    state->failed = true"), std::string::npos);
    EXPECT_NE(source.find("ImportJobTerminalStatus::Failed"), std::string::npos);
    const auto inFlightLoop = source.find("for (size_t index = 0u;");
    ASSERT_NE(inFlightLoop, std::string::npos);
    const auto inFlightRetryInsertion = source.find("state->inFlightTasks.push_back(", inFlightLoop);
    ASSERT_NE(inFlightRetryInsertion, std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelDropChecksCachedMeshAliasesBeforeSchedulingMeshArtifactLoads)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto resolutionLoop = source.find("void RunRendererResourceResolutionStep(");
    ASSERT_NE(resolutionLoop, std::string::npos);
    const auto loopCode = source.substr(resolutionLoop);

    const auto cachedAliasCheck = loopCode.find("IsMeshTaskAlreadyCached(task)");
    const auto meshArtifactLoad = loopCode.find("StartMeshArtifactLoad(actions, task, state->meshLoadsByPath)");
    ASSERT_NE(cachedAliasCheck, std::string::npos);
    ASSERT_NE(meshArtifactLoad, std::string::npos);
    EXPECT_LT(cachedAliasCheck, meshArtifactLoad);
    EXPECT_NE(source.find("bool IsMeshTaskAlreadyCached("), std::string::npos);
    EXPECT_NE(source.find("std::unordered_map<std::string, std::shared_ptr<MeshArtifactLoadState>> meshLoadsByPath"), std::string::npos);
    EXPECT_NE(source.find("task.meshLoad = existingLoad->second"), std::string::npos);
    EXPECT_NE(source.find("meshLoadsByPath.emplace(task.modelPath, task.meshLoad)"), std::string::npos);
    EXPECT_NE(source.find("state->failed = true"), std::string::npos);
    EXPECT_NE(source.find("Editor background task queue rejected mesh artifact load"), std::string::npos);
}

TEST(EditorRenderPathContractTests, StartupPreimportDoesNotSynchronouslyPrewarmRuntimeModelOrMaterialResources)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/EditorStartupAssetPreimport.cpp";

    std::ifstream stream(sourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    EXPECT_EQ(source.find("PrewarmStartupModelPackages"), std::string::npos);
    EXPECT_EQ(source.find("PrewarmStartupMaterialArtifacts"), std::string::npos);
    EXPECT_EQ(source.find("GetResource(resourcePath, true)"), std::string::npos);
    EXPECT_EQ(source.find("PrewarmArtifact(resourcePath)"), std::string::npos);
    EXPECT_EQ(source.find("#include \"Core/ResourceManagement/ModelManager.h\""), std::string::npos);
    EXPECT_EQ(source.find("#include \"Core/ResourceManagement/MaterialManager.h\""), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelMaterialResolutionKeepsColdMissesCacheOnly)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("BindDeferredMaterialTextures("), std::string::npos);

    const auto bindBegin = source.find("bool BindDeferredMaterialPaths(");
    ASSERT_NE(bindBegin, std::string::npos);
    const auto bindEnd = source.find("bool BindDeferredMeshPath(", bindBegin);
    ASSERT_NE(bindEnd, std::string::npos);
    const auto bindCode = source.substr(bindBegin, bindEnd - bindBegin);
    EXPECT_EQ(bindCode.find("NLS::Render::Resources::Loaders::MaterialLoader::Reload("), std::string::npos);
    EXPECT_EQ(bindCode.find("loadMissingTextures = true"), std::string::npos);
    EXPECT_EQ(bindCode.find("loadMissingShaders = true"), std::string::npos);
    EXPECT_EQ(bindCode.find("materialManager.PrewarmArtifact("), std::string::npos);
    EXPECT_EQ(bindCode.find("materialManager.GetResource(task.materialPaths[index], true)"), std::string::npos);
    EXPECT_NE(bindCode.find("materialManager.LoadArtifactWithoutTextures(task.materialPaths[index])"), std::string::npos);
    EXPECT_NE(bindCode.find("materialManager.GetResource(task.materialPaths[index], false)"), std::string::npos);
    EXPECT_NE(bindCode.find("if (!material || !material->IsValid())"), std::string::npos);
    EXPECT_LT(
        bindCode.find("if (!material || !material->IsValid())"),
        bindCode.find("meshRenderer.SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *material)"));
    EXPECT_NE(bindCode.find("meshRenderer.SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *material)"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetMaterialAtIndex(static_cast<uint8_t>(index), *material)"), std::string::npos);
    EXPECT_EQ(bindCode.find("++stats->failedMaterialSlots"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelMaterialResolutionBindsOnlyCachedTextures)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const auto meshRendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Components/MeshRenderer.cpp";
    const auto meshFilterSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Components/MeshFilter.cpp";

    std::ifstream actionsStream(actionsSourcePath, std::ios::binary);
    const std::string actionsSource{
        std::istreambuf_iterator<char>(actionsStream),
        std::istreambuf_iterator<char>()};
    std::ifstream meshRendererStream(meshRendererSourcePath, std::ios::binary);
    const std::string meshRendererSource{
        std::istreambuf_iterator<char>(meshRendererStream),
        std::istreambuf_iterator<char>()};
    std::ifstream meshFilterStream(meshFilterSourcePath, std::ios::binary);
    const std::string meshFilterSource{
        std::istreambuf_iterator<char>(meshFilterStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(actionsSource.empty());
    ASSERT_FALSE(meshRendererSource.empty());
    ASSERT_FALSE(meshFilterSource.empty());

    const auto textureBindBegin = actionsSource.find("bool BindDeferredMaterialTextures(");
    ASSERT_NE(textureBindBegin, std::string::npos);
    const auto textureBindEnd = actionsSource.find("template<typename FrameBudgetExpired>", textureBindBegin);
    ASSERT_NE(textureBindEnd, std::string::npos);
    const auto textureBindCode = actionsSource.substr(textureBindBegin, textureBindEnd - textureBindBegin);

    EXPECT_NE(textureBindCode.find("material.GetTextureResourcePaths()"), std::string::npos);
    EXPECT_NE(textureBindCode.find("textureManager.GetResource(texturePath, false)"), std::string::npos);
    EXPECT_EQ(textureBindCode.find("textureManager.GetResource(texturePath, true)"), std::string::npos);
    EXPECT_EQ(textureBindCode.find("LoadResource(texturePath)"), std::string::npos);
    EXPECT_NE(textureBindCode.find("material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture)"), std::string::npos);
    EXPECT_NE(textureBindCode.find("frameBudgetExpired()"), std::string::npos);

    const auto resolveMaterialsBegin = meshRendererSource.find("const MeshRenderer::MaterialList& MeshRenderer::ResolveMaterials()");
    ASSERT_NE(resolveMaterialsBegin, std::string::npos);
    const auto resolveMaterialsEnd = meshRendererSource.find("NLS::Array<std::string> MeshRenderer::GetMaterialPaths() const", resolveMaterialsBegin);
    ASSERT_NE(resolveMaterialsEnd, std::string::npos);
    const auto resolveMaterialsCode = meshRendererSource.substr(resolveMaterialsBegin, resolveMaterialsEnd - resolveMaterialsBegin);
    EXPECT_EQ(resolveMaterialsCode.find("GetResource(path, true)"), std::string::npos);

    const auto resolveMeshBegin = meshFilterSource.find("Render::Resources::Mesh* MeshFilter::ResolveMesh()");
    ASSERT_NE(resolveMeshBegin, std::string::npos);
    const auto resolveMeshEnd = meshFilterSource.find("std::string MeshFilter::GetModelPath() const", resolveMeshBegin);
    ASSERT_NE(resolveMeshEnd, std::string::npos);
    const auto resolveMeshCode = meshFilterSource.substr(resolveMeshBegin, resolveMeshEnd - resolveMeshBegin);
    const auto primitiveAliasGuard = resolveMeshCode.find("TryGetPrimitiveTypeFromMeshResourcePath(path)");
    ASSERT_NE(primitiveAliasGuard, std::string::npos);
    const auto canonicalAliasGuard = resolveMeshCode.find("path == NLS::Engine::GetPrimitiveMeshResourcePath(*primitiveType)");
    ASSERT_NE(canonicalAliasGuard, std::string::npos);
    const auto primitiveAliasLoad = resolveMeshCode.find("GetResource(path, true)", primitiveAliasGuard);
    ASSERT_NE(primitiveAliasLoad, std::string::npos);
    EXPECT_LT(canonicalAliasGuard, primitiveAliasLoad);
    EXPECT_EQ(resolveMeshCode.find("GetResource(path, true)"), primitiveAliasLoad);
    EXPECT_EQ(resolveMeshCode.find("GetResource(path, true)", primitiveAliasLoad + 1), std::string::npos);
    EXPECT_EQ(resolveMeshCode.find("SetModelPath(m_modelPath)"), std::string::npos);

    const auto setModelPathBegin = meshFilterSource.find("void MeshFilter::SetModelPath(");
    ASSERT_NE(setModelPathBegin, std::string::npos);
    const auto setModelPathEnd = meshFilterSource.find("void MeshFilter::SetModelPathHint(", setModelPathBegin);
    ASSERT_NE(setModelPathEnd, std::string::npos);
    const auto setModelPathCode = meshFilterSource.substr(setModelPathBegin, setModelPathEnd - setModelPathBegin);
    EXPECT_EQ(setModelPathCode.find("GetResource(p_path, true)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, MaterialArtifactPrewarmDoesNotSynchronouslyLoadShaderDependencies)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Core/ResourceManagement/MaterialManager.cpp";

    std::ifstream stream(sourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto prewarmBegin = source.find("Material* MaterialManager::PrewarmArtifact(");
    ASSERT_NE(prewarmBegin, std::string::npos);
    const auto prewarmEnd = source.find("void MaterialManager::DestroyResource(", prewarmBegin);
    ASSERT_NE(prewarmEnd, std::string::npos);
    const auto prewarmCode = source.substr(prewarmBegin, prewarmEnd - prewarmBegin);

    EXPECT_EQ(prewarmCode.find("{false}"), std::string::npos);
    EXPECT_NE(prewarmCode.find("{false, false}"), std::string::npos);
    EXPECT_NE(prewarmCode.find("auto* prewarmed = CreateResource(path, {false, false})"), std::string::npos);
    EXPECT_NE(prewarmCode.find("if (prewarmed && prewarmed->IsValid())"), std::string::npos);
    EXPECT_LT(
        prewarmCode.find("if (prewarmed && prewarmed->IsValid())"),
        prewarmCode.find("RegisterResource(path, prewarmed)"));
    EXPECT_NE(prewarmCode.find("DestroyResource(prewarmed)"), std::string::npos);
    EXPECT_LT(
        prewarmCode.find("DestroyResource(prewarmed)"),
        prewarmCode.find("return nullptr"));
}

TEST(EditorRenderPathContractTests, MaterialArtifactLoadWithoutTexturesLoadsShaderButDefersTextureDependencies)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Core/ResourceManagement/MaterialManager.cpp";

    std::ifstream stream(sourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto begin = source.find("Material* MaterialManager::LoadArtifactWithoutTextures(");
    ASSERT_NE(begin, std::string::npos);
    const auto end = source.find("void MaterialManager::DestroyResource(", begin);
    ASSERT_NE(end, std::string::npos);
    const auto code = source.substr(begin, end - begin);

    EXPECT_NE(code.find("CreateResource(path, {false, true})"), std::string::npos);
    EXPECT_NE(code.find("RegisterResource(path, loaded)"), std::string::npos);
    EXPECT_EQ(code.find("{true, true}"), std::string::npos);
    EXPECT_EQ(code.find("GetResource(path, true)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelMaterialResolutionDoesNotDelayMissingSlotsFrameByFrame)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto bindBegin = source.find("bool BindDeferredMaterialPaths(");
    ASSERT_NE(bindBegin, std::string::npos);
    const auto bindEnd = source.find("bool BindDeferredMeshPath(", bindBegin);
    ASSERT_NE(bindEnd, std::string::npos);
    const auto bindCode = source.substr(bindBegin, bindEnd - bindBegin);
    const auto cacheMissCounter = bindCode.find("++stats->unresolvedMaterialSlots");
    ASSERT_NE(cacheMissCounter, std::string::npos);
    EXPECT_NE(source.find("kRendererResourceResolutionMaterialSlotsPerTask"), std::string::npos);
    EXPECT_NE(bindCode.find("visitedSlots < kRendererResourceResolutionMaterialSlotsPerTask"), std::string::npos);
    EXPECT_EQ(bindCode.find("materialManager.PrewarmArtifact("), std::string::npos);
    EXPECT_NE(bindCode.find("frameBudgetExpired()"), std::string::npos);
    EXPECT_NE(bindCode.find("return false;"), std::string::npos);
    EXPECT_NE(bindCode.find("return task.nextMaterialSlot >= task.materialPaths.size()"), std::string::npos);

    EXPECT_EQ(bindCode.find("break;", cacheMissCounter), std::string::npos);
}

TEST(EditorRenderPathContractTests, EditorGeneratedModelFallbackMaterialDoesNotSynchronouslyLoadDefaultMaterial)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto fallbackBegin = source.find("NLS::Render::Resources::Material* GetOrCreateEditorDefaultMaterial(");
    ASSERT_NE(fallbackBegin, std::string::npos);
    const auto fallbackEnd = source.find("BuildPrefabObjectRecordIndex(", fallbackBegin);
    ASSERT_NE(fallbackEnd, std::string::npos);
    const auto fallbackCode = source.substr(fallbackBegin, fallbackEnd - fallbackBegin);

    EXPECT_EQ(fallbackCode.find("context.materialManager["), std::string::npos);
    EXPECT_EQ(fallbackCode.find("GetResource(\":Materials\\\\Default.mat\", true)"), std::string::npos);
    EXPECT_EQ(fallbackCode.find("MaterialManager)[\":Materials\\\\Default.mat\"]"), std::string::npos);
    EXPECT_NE(fallbackCode.find("context.editorResources->GetLoadedShader(\"DebugLitColor\")"), std::string::npos);
    EXPECT_EQ(fallbackCode.find("context.editorResources->GetShader(\"DebugLitColor\")"), std::string::npos);
    EXPECT_NE(fallbackCode.find("static NLS::Render::Resources::Material fallback"), std::string::npos);
    EXPECT_NE(fallbackCode.find("fallback.Set<Maths::Vector4>(\"u_Diffuse\""), std::string::npos);
}

TEST(EditorRenderPathContractTests, MaterialEditorUniformWidgetsUpdateMaterialThroughSetters)
{
    const auto editorSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/MaterialEditor.cpp";
    const auto drawerHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/GUIDrawer.h";
    const auto materialHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Resources/Material.h";
    const auto editorActionsPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    std::ifstream editorStream(editorSourcePath, std::ios::binary);
    const std::string editorSource{
        std::istreambuf_iterator<char>(editorStream),
        std::istreambuf_iterator<char>()};
    ASSERT_FALSE(editorSource.empty());

    const auto generateBegin = editorSource.find("void Editor::Panels::MaterialEditor::GenerateShaderSettingsContent()");
    ASSERT_NE(generateBegin, std::string::npos);
    const auto generateEnd = editorSource.find("void Editor::Panels::MaterialEditor::GenerateMaterialSettingsContent()", generateBegin);
    ASSERT_NE(generateEnd, std::string::npos);
    const auto generateCode = editorSource.substr(generateBegin, generateEnd - generateBegin);

    EXPECT_EQ(generateCode.find("reinterpret_cast<Maths::Vector3&>(*info.second)"), std::string::npos);
    EXPECT_EQ(generateCode.find("reinterpret_cast<Maths::Vector4&>(*info.second)"), std::string::npos);
    EXPECT_EQ(generateCode.find("reinterpret_cast<Texture2D * &>(*info.second)"), std::string::npos);
    EXPECT_EQ(generateCode.find("std::any*"), std::string::npos);
    EXPECT_NE(generateCode.find("m_target->Set<Maths::Vector3>(name, value)"), std::string::npos);
    EXPECT_NE(generateCode.find("m_target->Set<Maths::Vector4>(name, value)"), std::string::npos);
    EXPECT_NE(generateCode.find("m_target->Set<Texture2D*>(name, value)"), std::string::npos);

    std::ifstream drawerStream(drawerHeaderPath, std::ios::binary);
    const std::string drawerHeader{
        std::istreambuf_iterator<char>(drawerStream),
        std::istreambuf_iterator<char>()};
    ASSERT_FALSE(drawerHeader.empty());
    EXPECT_NE(drawerHeader.find("std::function<Texture2D*(void)>"), std::string::npos);
    EXPECT_NE(drawerHeader.find("std::function<void(Texture2D*)>"), std::string::npos);

    std::ifstream materialHeaderStream(materialHeaderPath, std::ios::binary);
    const std::string materialHeader{
        std::istreambuf_iterator<char>(materialHeaderStream),
        std::istreambuf_iterator<char>()};
    ASSERT_FALSE(materialHeader.empty());
    EXPECT_EQ(materialHeader.find("\n    MaterialParameterBlock& GetParameterBlock()"), std::string::npos);
    EXPECT_NE(materialHeader.find("const MaterialParameterBlock& GetParameterBlock() const"), std::string::npos);
    EXPECT_EQ(materialHeader.find("\n    std::map<std::string, std::any>& GetUniformsData()"), std::string::npos);
    EXPECT_NE(materialHeader.find("const std::map<std::string, std::any>& GetUniformsData() const"), std::string::npos);

    std::ifstream editorActionsStream(editorActionsPath, std::ios::binary);
    const std::string editorActionsSource{
        std::istreambuf_iterator<char>(editorActionsStream),
        std::istreambuf_iterator<char>()};
    ASSERT_FALSE(editorActionsSource.empty());
    EXPECT_EQ(editorActionsSource.find("for (auto& [name, value] : instance->GetUniformsData())"), std::string::npos);
    EXPECT_EQ(editorActionsSource.find("value = static_cast<Render::Resources::Texture2D*>(nullptr)"), std::string::npos);
    EXPECT_NE(editorActionsSource.find("instance->Set<Render::Resources::Texture2D*>(uniformName, nullptr)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelDeferredMeshBindUsesNonBlockingUploadHeap)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto bindBegin = source.find("bool BindDeferredMeshPath(");
    ASSERT_NE(bindBegin, std::string::npos);
    const auto bindEnd = source.find("bool IsMeshTaskAlreadyCached(", bindBegin);
    ASSERT_NE(bindEnd, std::string::npos);
    const auto bindCode = source.substr(bindBegin, bindEnd - bindBegin);
    const auto loadBegin = source.find("bool StartMeshArtifactLoad(");
    ASSERT_NE(loadBegin, std::string::npos);
    const auto loadEnd = source.find("void RunRendererResourceResolutionStep(", loadBegin);
    ASSERT_NE(loadEnd, std::string::npos);
    const auto loadCode = source.substr(loadBegin, loadEnd - loadBegin);

    EXPECT_NE(bindCode.find("new Render::Resources::Mesh("), std::string::npos);
    EXPECT_NE(bindCode.find("Render::Resources::MeshBufferUploadMode::CpuToGpu"), std::string::npos);
    EXPECT_EQ(loadCode.find("new Render::Resources::Mesh("), std::string::npos);
    EXPECT_EQ(loadCode.find("Render::Resources::MeshBufferUploadMode::CpuToGpu"), std::string::npos);
    EXPECT_EQ(loadCode.find("Render::Resources::Loaders::ModelLoader::Create(path, meshes)"), std::string::npos);
    EXPECT_NE(loadCode.find("NLS::Render::Assets::LoadMeshArtifact(artifactPath)"), std::string::npos);
    EXPECT_NE(loadCode.find("std::unordered_map<std::string, std::shared_ptr<MeshArtifactLoadState>>& meshLoadsByPath"), std::string::npos);
    EXPECT_NE(loadCode.find("task.meshLoad = existingLoad->second"), std::string::npos);
    EXPECT_NE(loadCode.find("catch (const std::exception& exception)"), std::string::npos);
    EXPECT_NE(loadCode.find("state->completed = true"), std::string::npos);
    EXPECT_EQ(bindCode.find("Render::Resources::MeshBufferUploadMode::GpuOnly"), std::string::npos);
    EXPECT_EQ(bindCode.find("modelManager[task.modelPath]"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetModel(gpuModel)"), std::string::npos);
    EXPECT_NE(bindCode.find("meshManager.GetResource(task.modelPath, false)"), std::string::npos);
    EXPECT_NE(bindCode.find("meshFilter.SetResolvedTransientMeshFromReference(std::move(owner))"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetResolvedTransientModelFromReference(std::move(owner))"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetTransientModel(std::move(owner))"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetModel(cached)"), std::string::npos);
    EXPECT_EQ(bindCode.find("modelManager.RegisterResource(task.modelPath, model)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelDeferredMeshLoadStaysCpuOnlyUntilUiThreadBind)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto stateBegin = source.find("struct MeshArtifactLoadState");
    ASSERT_NE(stateBegin, std::string::npos);
    const auto stateEnd = source.find("struct RendererResourceResolutionState", stateBegin);
    ASSERT_NE(stateEnd, std::string::npos);
    const auto stateCode = source.substr(stateBegin, stateEnd - stateBegin);
    const auto bindBegin = source.find("bool BindDeferredMeshPath(");
    ASSERT_NE(bindBegin, std::string::npos);
    const auto bindEnd = source.find("bool IsMeshTaskAlreadyCached(", bindBegin);
    ASSERT_NE(bindEnd, std::string::npos);
    const auto bindCode = source.substr(bindBegin, bindEnd - bindBegin);

    EXPECT_EQ(stateCode.find("Render::Resources::Model* model"), std::string::npos);
    EXPECT_EQ(stateCode.find("~MeshArtifactLoadState()"), std::string::npos);
    EXPECT_EQ(stateCode.find("delete model"), std::string::npos);
    EXPECT_NE(stateCode.find("std::shared_ptr<const NLS::Render::Assets::MeshArtifactData>"), std::string::npos);
    EXPECT_EQ(stateCode.find("std::shared_ptr<NLS::Render::Resources::Model> transientModel"), std::string::npos);
    EXPECT_NE(stateCode.find("std::shared_ptr<NLS::Render::Resources::Mesh> transientMesh"), std::string::npos);
    EXPECT_EQ(bindCode.find("std::move(task.meshLoad->data)"), std::string::npos);
    EXPECT_NE(bindCode.find("transientMesh = task.meshLoad->transientMesh"), std::string::npos);
    EXPECT_NE(bindCode.find("data = task.meshLoad->data"), std::string::npos);
    EXPECT_NE(bindCode.find("auto* cached = meshManager.GetResource(task.modelPath, false)"), std::string::npos);
    EXPECT_EQ(bindCode.find("else if (auto* gpuModel = modelManager[task.modelPath])"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetModel(gpuModel)"), std::string::npos);
    EXPECT_NE(bindCode.find("data->boundingSphere"), std::string::npos);
    EXPECT_EQ(bindCode.find("Render::Resources::Loaders::ModelLoader::Create(task.modelPath, meshes)"), std::string::npos);
    EXPECT_NE(bindCode.find("task.meshLoad->transientMesh = owner"), std::string::npos);
    EXPECT_NE(bindCode.find("meshFilter.SetResolvedTransientMeshFromReference(std::move(owner))"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetResolvedTransientModelFromReference(std::move(owner))"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetTransientModel(std::move(owner))"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetModel(cached)"), std::string::npos);
    EXPECT_EQ(bindCode.find("modelManager.RegisterResource(task.modelPath, model)"), std::string::npos);
    EXPECT_EQ(bindCode.find("LoadMeshArtifact"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelDeferredMeshArtifactFailureFailsResolutionJob)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto taskBegin = source.find("struct RendererResourceResolutionTask");
    ASSERT_NE(taskBegin, std::string::npos);
    const auto loadStateBegin = source.find("struct MeshArtifactLoadState", taskBegin);
    ASSERT_NE(loadStateBegin, std::string::npos);
    const auto rendererStateBegin = source.find("struct RendererResourceResolutionState", loadStateBegin);
    ASSERT_NE(rendererStateBegin, std::string::npos);
    const auto taskCode = source.substr(taskBegin, loadStateBegin - taskBegin);
    const auto loadStateCode = source.substr(loadStateBegin, rendererStateBegin - loadStateBegin);

    EXPECT_NE(taskCode.find("bool failed = false"), std::string::npos);
    EXPECT_NE(loadStateCode.find("bool failed = false"), std::string::npos);

    const auto bindBegin = source.find("bool BindDeferredMeshPath(");
    ASSERT_NE(bindBegin, std::string::npos);
    const auto bindEnd = source.find("bool IsMeshTaskAlreadyCached(", bindBegin);
    ASSERT_NE(bindEnd, std::string::npos);
    const auto bindCode = source.substr(bindBegin, bindEnd - bindBegin);
    EXPECT_NE(bindCode.find("loadFailed = task.meshLoad->failed"), std::string::npos);
    EXPECT_NE(bindCode.find("if (!loadAccepted || loadFailed)"), std::string::npos);
    const auto loadFailureBranch = bindCode.find("if (!loadAccepted || loadFailed)");
    ASSERT_NE(loadFailureBranch, std::string::npos);
    const auto loadFailureTaskFailed = bindCode.find("task.failed = true", loadFailureBranch);
    const auto loadFailureReturn = bindCode.find("return true;", loadFailureBranch);
    ASSERT_NE(loadFailureTaskFailed, std::string::npos);
    ASSERT_NE(loadFailureReturn, std::string::npos);
    EXPECT_LT(loadFailureTaskFailed, loadFailureReturn);

    const auto loadBegin = source.find("bool StartMeshArtifactLoad(");
    ASSERT_NE(loadBegin, std::string::npos);
    const auto loadEnd = source.find("std::optional<RendererResourceResolutionTask> PopNextRemainingTask(", loadBegin);
    ASSERT_NE(loadEnd, std::string::npos);
    const auto loadCode = source.substr(loadBegin, loadEnd - loadBegin);
    EXPECT_NE(loadCode.find("state->failed = true"), std::string::npos);
    EXPECT_NE(loadCode.find("state->failed = false"), std::string::npos);

    const auto stepBegin = source.find("void RunRendererResourceResolutionStep(");
    ASSERT_NE(stepBegin, std::string::npos);
    const auto stepEnd = source.find("NLS_LOG_INFO(\n        \"Renderer resources ready for prefab instance", stepBegin);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepCode = source.substr(stepBegin, stepEnd - stepBegin);
    EXPECT_NE(stepCode.find("if (task.failed || (task.meshLoad && !task.meshLoad->accepted))"), std::string::npos);
    EXPECT_NE(stepCode.find("state->failed = true"), std::string::npos);
    EXPECT_NE(stepCode.find("finishFailed();"), std::string::npos);

    const auto finalBegin = stepEnd;
    const auto finalCode = source.substr(finalBegin);
    EXPECT_NE(finalCode.find("if (state->failed || (state->stats && state->stats->failedMaterialSlots > 0u))"), std::string::npos);
    EXPECT_NE(source.find("++stats->failedMeshTasks"), std::string::npos);
}

TEST(EditorRenderPathContractTests, EditorBackgroundTasksUseSharedJobSystemQueue)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto actionsHeaderPath = root / "Project/Editor/Core/EditorActions.h";
    const auto actionsSourcePath = root / "Project/Editor/Core/EditorActions.cpp";
    const auto trackerSourcePath = root / "Project/Editor/Core/EditorBackgroundTaskTracker.cpp";
    const auto editorSourcePath = root / "Project/Editor/Core/Editor.cpp";

    std::ifstream headerStream(actionsHeaderPath, std::ios::binary);
    const std::string header{
        std::istreambuf_iterator<char>(headerStream),
        std::istreambuf_iterator<char>()};
    std::ifstream sourceStream(actionsSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(sourceStream),
        std::istreambuf_iterator<char>()};
    std::ifstream trackerSourceStream(trackerSourcePath, std::ios::binary);
    const std::string trackerSource{
        std::istreambuf_iterator<char>(trackerSourceStream),
        std::istreambuf_iterator<char>()};
    std::ifstream editorSourceStream(editorSourcePath, std::ios::binary);
    const std::string editorSource{
        std::istreambuf_iterator<char>(editorSourceStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(trackerSource.empty());
    ASSERT_FALSE(editorSource.empty());

    EXPECT_NE(header.find("bool TrackBackgroundTask(std::function<void()> task)"), std::string::npos);
    EXPECT_NE(header.find("#include \"Core/EditorBackgroundTaskTracker.h\""), std::string::npos);
    EXPECT_NE(header.find("EditorBackgroundTaskTracker m_backgroundTasks"), std::string::npos);
    EXPECT_EQ(header.find("bool m_ownsBackgroundJobSystem"), std::string::npos);
    EXPECT_EQ(header.find("struct BackgroundWorker"), std::string::npos);
    EXPECT_EQ(header.find("m_backgroundTaskQueue"), std::string::npos);
    EXPECT_EQ(header.find("m_backgroundTaskCondition"), std::string::npos);
    EXPECT_EQ(header.find("m_runningBackgroundTaskCount"), std::string::npos);

    EXPECT_NE(trackerSource.find("#include <Jobs/BackgroundJobQueue.h>"), std::string::npos);
    EXPECT_NE(trackerSource.find("#include <Jobs/JobSystem.h>"), std::string::npos);
    EXPECT_NE(trackerSource.find("NLS::Base::Jobs::ScheduleBackgroundJob"), std::string::npos);
    EXPECT_NE(source.find("return m_backgroundTasks.Track(std::move(task));"), std::string::npos);
    EXPECT_EQ(source.find("NLS::Base::Jobs::ShutdownJobSystem"), std::string::npos);
    EXPECT_NE(trackerSource.find("catch (const std::exception& exception)"), std::string::npos);
    EXPECT_NE(source.find("kEditorBackgroundTaskQueueCapacity"), std::string::npos);
    EXPECT_EQ(source.find("void Editor::Core::EditorActions::CollectFinishedBackgroundTasks()"), std::string::npos);
    EXPECT_EQ(source.find("void Editor::Core::EditorActions::EnsureBackgroundWorkersStarted()"), std::string::npos);
    EXPECT_EQ(source.find("void Editor::Core::EditorActions::RunBackgroundWorker()"), std::string::npos);
    EXPECT_EQ(source.find("m_backgroundTaskQueue.push(std::move(task))"), std::string::npos);
    EXPECT_EQ(source.find("m_backgroundTaskCondition.wait("), std::string::npos);
    EXPECT_NE(editorSource.find("Editor::Core::Editor::JobSystemLifetime::JobSystemLifetime()"), std::string::npos);
    EXPECT_NE(editorSource.find("NLS::Base::Jobs::ShutdownJobSystem"), std::string::npos);
}

TEST(EditorRenderPathContractTests, AsyncModelDropsScheduleImportAndCompletionCallbacks)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const auto bridgeHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/EditorAssetDragDropBridge.h";
    const auto bridgeSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/EditorAssetDragDropBridge.cpp";

    std::ifstream actionsStream(actionsSourcePath, std::ios::binary);
    const std::string actionsSource{
        std::istreambuf_iterator<char>(actionsStream),
        std::istreambuf_iterator<char>()};
    std::ifstream headerStream(bridgeHeaderPath, std::ios::binary);
    const std::string header{
        std::istreambuf_iterator<char>(headerStream),
        std::istreambuf_iterator<char>()};
    std::ifstream sourceStream(bridgeSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(sourceStream),
            std::istreambuf_iterator<char>()};

    ASSERT_FALSE(actionsSource.empty());
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    const auto asyncBegin = source.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropModelAssetIntoHierarchyAsync(");
    ASSERT_NE(asyncBegin, std::string::npos);
    const auto asyncEnd = source.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropImportedAssetHandleIntoHierarchy(", asyncBegin);
    ASSERT_NE(asyncEnd, std::string::npos);
    const auto asyncCode = source.substr(asyncBegin, asyncEnd - asyncBegin);

    EXPECT_NE(header.find("std::function<bool(std::function<void()>)> scheduleBackgroundTask"), std::string::npos);
    EXPECT_NE(asyncCode.find("request.scheduleBackgroundTask"), std::string::npos);
    EXPECT_NE(asyncCode.find("const bool scheduled = request.scheduleBackgroundTask"), std::string::npos);
    EXPECT_NE(asyncCode.find("dragdrop-background-task-rejected"), std::string::npos);
    EXPECT_NE(asyncCode.find("AssetImporterFacade importer"), std::string::npos);
    EXPECT_NE(asyncCode.find("importer.SaveAndReimport(assetPath"), std::string::npos);
    EXPECT_NE(asyncCode.find("completionResult.pendingImport = false"), std::string::npos);
    EXPECT_EQ(asyncCode.find("auto* progressTracker = request.progressTracker"), std::string::npos);
    EXPECT_EQ(asyncCode.find("progressTracker]"), std::string::npos);
    EXPECT_NE(asyncCode.find("request.completion"), std::string::npos);
    const auto importCall = asyncCode.find("importer.SaveAndReimport(assetPath");
    ASSERT_NE(importCall, std::string::npos);
    const auto exceptionCatch = asyncCode.find("catch (const std::exception& exception)", importCall);
    ASSERT_NE(exceptionCatch, std::string::npos);
    EXPECT_NE(asyncCode.find("catch (...)", exceptionCatch), std::string::npos);
    EXPECT_NE(asyncCode.find("dragdrop-background-import-failed", exceptionCatch), std::string::npos);
    EXPECT_NE(asyncCode.find("completionResult.importSucceeded = false"), std::string::npos);
    EXPECT_NE(asyncCode.find("(*completion)(std::move(completionResult))", exceptionCatch), std::string::npos);
    EXPECT_EQ(asyncCode.find("(void)request"), std::string::npos);

    const auto createBegin = actionsSource.find("Engine::GameObject* NLS::Editor::Core::EditorActions::CreateGameObjectFromAsset(");
    ASSERT_NE(createBegin, std::string::npos);
    const auto createEnd = actionsSource.find(
        "const NLS::Editor::Assets::EditorAssetDragPayload& payload",
        createBegin);
    ASSERT_NE(createEnd, std::string::npos);
    const auto createCode = actionsSource.substr(createBegin, createEnd - createBegin);
    const auto completionBegin = actionsSource.find("void NLS::Editor::Core::EditorActions::CompletePendingAssetDrop(");
    ASSERT_NE(completionBegin, std::string::npos);
    const auto completionEnd = actionsSource.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(", completionBegin);
    ASSERT_NE(completionEnd, std::string::npos);
    const auto completionCode = actionsSource.substr(completionBegin, completionEnd - completionBegin);

    EXPECT_NE(actionsSource.find("NLS::Engine::GameObject* FindLiveGameObjectByAddress("), std::string::npos);
    EXPECT_NE(actionsSource.find("SceneMutationToken"), std::string::npos);
    EXPECT_NE(actionsSource.find("CaptureSceneMutationToken()"), std::string::npos);
    EXPECT_NE(actionsSource.find("PendingAssetDropParentGuard"), std::string::npos);
    EXPECT_NE(actionsSource.find("m_sceneSourcePathChangedListener"), std::string::npos);
    EXPECT_NE(actionsSource.find("m_sceneDirtyStateChangedListener"), std::string::npos);
    EXPECT_NE(actionsSource.find("CurrentSceneSourcePathChangedEvent -= m_sceneSourcePathChangedListener"), std::string::npos);
    EXPECT_NE(actionsSource.find("CurrentSceneDirtyStateChangedEvent -= m_sceneDirtyStateChangedListener"), std::string::npos);
    EXPECT_NE(createCode.find("parentAddress = static_cast<const Engine::GameObject*>(p_parent)"), std::string::npos);
    EXPECT_NE(createCode.find("PendingAssetDropParentGuard parentGuard"), std::string::npos);
    EXPECT_NE(createCode.find("parentGuard.parentDestroyed = std::make_shared<bool>(false)"), std::string::npos);
    EXPECT_NE(createCode.find("parentGuard.destroyedListener = TrackGameObjectDestroyedListener("), std::string::npos);
    EXPECT_NE(createCode.find("const auto sceneToken = CaptureSceneMutationToken()"), std::string::npos);
    EXPECT_EQ(createCode.find("currentScene, p_parent, importResult"), std::string::npos);
    EXPECT_EQ(createCode.find("CompletePendingAssetDrop(path, focusOnCreation, currentScene, p_parent"), std::string::npos);
    EXPECT_NE(completionCode.find("PendingAssetDropParentGuard parentGuard"), std::string::npos);
    EXPECT_NE(completionCode.find("SceneMutationToken sceneToken"), std::string::npos);
    EXPECT_NE(completionCode.find("IsGameObjectCreationSceneLive(m_context, scene, sceneToken, *this)"), std::string::npos);
    EXPECT_NE(completionCode.find("if (parentGuard.parentDestroyed && *parentGuard.parentDestroyed)"), std::string::npos);
    EXPECT_NE(completionCode.find("FindLiveGameObjectByAddress(*scene, parentGuard.parentAddress)"), std::string::npos);
    EXPECT_NE(completionCode.find("if (parentGuard.parentAddress != nullptr && liveParent == nullptr)"), std::string::npos);
    EXPECT_NE(completionCode.find("ReleaseGameObjectDestroyedListener(parentGuard.destroyedListener)"), std::string::npos);
    EXPECT_NE(completionCode.find("Cancelled imported asset drop because the target parent was destroyed"), std::string::npos);
    EXPECT_EQ(completionCode.find("parent && parent->IsAlive()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, AsyncDestroyedListenersAreTrackedForEditorTeardownCleanup)
{
    const auto actionsHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.h";
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    std::ifstream headerStream(actionsHeaderPath, std::ios::binary);
    const std::string header{
        std::istreambuf_iterator<char>(headerStream),
        std::istreambuf_iterator<char>()};
    std::ifstream sourceStream(actionsSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(sourceStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_NE(header.find("TrackGameObjectDestroyedListener("), std::string::npos);
    EXPECT_NE(header.find("ReleaseGameObjectDestroyedListener("), std::string::npos);
    EXPECT_NE(header.find("ReleaseTrackedGameObjectDestroyedListeners()"), std::string::npos);
    EXPECT_NE(header.find("m_gameObjectDestroyedListeners"), std::string::npos);

    const auto destructorBegin = source.find("Editor::Core::EditorActions::~EditorActions()");
    ASSERT_NE(destructorBegin, std::string::npos);
    const auto loadSceneBegin = source.find("void Editor::Core::EditorActions::LoadEmptyScene()", destructorBegin);
    ASSERT_NE(loadSceneBegin, std::string::npos);
    const auto destructorCode = source.substr(destructorBegin, loadSceneBegin - destructorBegin);
    EXPECT_NE(destructorCode.find("ReleaseTrackedGameObjectDestroyedListeners()"), std::string::npos);

    const auto createBegin = source.find("Engine::GameObject* NLS::Editor::Core::EditorActions::CreateGameObjectFromAsset(");
    ASSERT_NE(createBegin, std::string::npos);
    const auto payloadBegin =
        source.find("const NLS::Editor::Assets::EditorAssetDragPayload& payload", createBegin);
    ASSERT_NE(payloadBegin, std::string::npos);
    const auto createCode = source.substr(createBegin, payloadBegin - createBegin);
    EXPECT_NE(createCode.find("parentGuard.destroyedListener = TrackGameObjectDestroyedListener("), std::string::npos);
    EXPECT_EQ(createCode.find("parentGuard.destroyedListener = Engine::GameObject::DestroyedEvent +="), std::string::npos);
    EXPECT_NE(createCode.find("ReleaseGameObjectDestroyedListener(parentGuard.destroyedListener)"), std::string::npos);
    EXPECT_EQ(createCode.find("Engine::GameObject::DestroyedEvent -= parentGuard.destroyedListener"), std::string::npos);

    const auto completionBegin = source.find("void NLS::Editor::Core::EditorActions::CompletePendingAssetDrop(");
    ASSERT_NE(completionBegin, std::string::npos);
    const auto resolutionBegin =
        source.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(", completionBegin);
    ASSERT_NE(resolutionBegin, std::string::npos);
    const auto completionCode = source.substr(completionBegin, resolutionBegin - completionBegin);
    EXPECT_NE(completionCode.find("ReleaseGameObjectDestroyedListener(parentGuard.destroyedListener)"), std::string::npos);
    EXPECT_EQ(completionCode.find("Engine::GameObject::DestroyedEvent -= parentGuard.destroyedListener"), std::string::npos);

    const auto stepBegin = source.find("void RunRendererResourceResolutionStep(");
    ASSERT_NE(stepBegin, std::string::npos);
    const auto constructorBegin =
        source.find("Editor::Core::EditorActions::EditorActions(Context& p_context, PanelsManager& p_panelsManager)", stepBegin);
    ASSERT_NE(constructorBegin, std::string::npos);
    const auto stepCode = source.substr(stepBegin, constructorBegin - stepBegin);
    EXPECT_NE(stepCode.find("actions.ReleaseGameObjectDestroyedListener(state->destroyedListener)"), std::string::npos);
    EXPECT_EQ(stepCode.find("NLS::Engine::GameObject::DestroyedEvent -= state->destroyedListener"), std::string::npos);

    const auto queueCode = source.substr(resolutionBegin);
    EXPECT_NE(queueCode.find("state->destroyedListener = TrackGameObjectDestroyedListener("), std::string::npos);
    EXPECT_EQ(queueCode.find("state->destroyedListener = NLS::Engine::GameObject::DestroyedEvent +="), std::string::npos);
}

TEST(EditorRenderPathContractTests, PendingImportedAssetPayloadDropsUseAsyncCompletionPath)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("BuildPrefabResourcePathFromPayload("), std::string::npos);

    const auto payloadBegin =
        source.find("Engine::GameObject* NLS::Editor::Core::EditorActions::CreateGameObjectFromAsset(\n    const NLS::Editor::Assets::EditorAssetDragPayload& payload");
    ASSERT_NE(payloadBegin, std::string::npos);
    const auto completionBegin =
        source.find("void NLS::Editor::Core::EditorActions::CompletePendingAssetDrop(", payloadBegin);
    ASSERT_NE(completionBegin, std::string::npos);
    const auto payloadCode = source.substr(payloadBegin, completionBegin - payloadBegin);

    EXPECT_NE(payloadCode.find("if (result.pendingImport)"), std::string::npos);
    EXPECT_NE(payloadCode.find("if (payload.imported != 0u)"), std::string::npos);
    EXPECT_NE(payloadCode.find("const auto resourcePath = BuildPrefabResourcePathFromPayload(payload)"), std::string::npos);
    EXPECT_NE(payloadCode.find("return CreateGameObjectFromAsset(resourcePath, focusOnCreation, p_parent)"), std::string::npos);
    EXPECT_EQ(payloadCode.find("Asset drag handle is not imported yet; waiting for background preimport"), std::string::npos);
    EXPECT_EQ(payloadCode.find("return nullptr;\n    }\n\n    if (!result.handled"), std::string::npos);
}

TEST(EditorRenderPathContractTests, AssetDropsTargetActivePrefabStageSceneWhenOpen)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("ResolveGameObjectCreationScene("), std::string::npos);
    EXPECT_NE(source.find("MarkGameObjectCreationSceneDirty("), std::string::npos);
    EXPECT_NE(source.find("m_context.activePrefabStage->editable"), std::string::npos);
    EXPECT_NE(source.find("if (!m_context.activePrefabStage->editable)\n            return nullptr"), std::string::npos);
    EXPECT_NE(source.find("NotifyPrefabStageOpened()"), std::string::npos);
    EXPECT_NE(source.find("++m_prefabStageGeneration"), std::string::npos);
    EXPECT_NE(source.find("m_context.activePrefabStage->stageScene.get() == &scene"), std::string::npos);
    EXPECT_NE(source.find("m_context.activePrefabStage->dirty = true"), std::string::npos);

    const auto emptyBegin = source.find("Engine::GameObject* Editor::Core::EditorActions::CreateEmptyGameObject(");
    ASSERT_NE(emptyBegin, std::string::npos);
    const auto primitiveBegin = source.find("Engine::GameObject* Editor::Core::EditorActions::CreatePrimitive(", emptyBegin);
    ASSERT_NE(primitiveBegin, std::string::npos);
    const auto emptyCode = source.substr(emptyBegin, primitiveBegin - emptyBegin);
    EXPECT_NE(emptyCode.find("auto* creationScene = ResolveGameObjectCreationScene(m_context, p_parent)"), std::string::npos);
    EXPECT_NE(emptyCode.find("return nullptr"), std::string::npos);
    EXPECT_EQ(emptyCode.find("throw std::runtime_error"), std::string::npos);
    EXPECT_EQ(emptyCode.find("m_context.sceneManager.GetCurrentScene()"), std::string::npos);
    EXPECT_NE(emptyCode.find("MarkGameObjectCreationSceneDirty(m_context, *creationScene)"), std::string::npos);
    EXPECT_EQ(emptyCode.find("m_context.sceneManager.MarkCurrentSceneDirty()"), std::string::npos);

    const auto pathCreateBegin =
        source.find("Engine::GameObject* NLS::Editor::Core::EditorActions::CreateGameObjectFromAsset(");
    ASSERT_NE(pathCreateBegin, std::string::npos);
    const auto payloadCreateBegin =
        source.find("const NLS::Editor::Assets::EditorAssetDragPayload& payload", pathCreateBegin);
    ASSERT_NE(payloadCreateBegin, std::string::npos);
    const auto pathCreateCode = source.substr(pathCreateBegin, payloadCreateBegin - pathCreateBegin);
    EXPECT_NE(pathCreateCode.find("auto* creationScene = ResolveGameObjectCreationScene(m_context, p_parent)"), std::string::npos);
    EXPECT_EQ(pathCreateCode.find("const auto currentScene = m_context.sceneManager.GetCurrentScene()"), std::string::npos);
    EXPECT_NE(pathCreateCode.find("*creationScene"), std::string::npos);
    EXPECT_NE(pathCreateCode.find("CompletePendingAssetDrop(path, focusOnCreation, creationScene, sceneToken"), std::string::npos);
    EXPECT_NE(pathCreateCode.find("MarkGameObjectCreationSceneDirty(m_context, *creationScene)"), std::string::npos);

    const auto payloadCreateEnd = source.find("void NLS::Editor::Core::EditorActions::CompletePendingAssetDrop(", payloadCreateBegin);
    ASSERT_NE(payloadCreateEnd, std::string::npos);
    const auto payloadCreateCode = source.substr(payloadCreateBegin, payloadCreateEnd - payloadCreateBegin);
    EXPECT_NE(payloadCreateCode.find("auto* creationScene = ResolveGameObjectCreationScene(m_context, p_parent)"), std::string::npos);
    EXPECT_EQ(payloadCreateCode.find("const auto currentScene = m_context.sceneManager.GetCurrentScene()"), std::string::npos);
    EXPECT_NE(payloadCreateCode.find("*creationScene"), std::string::npos);
    EXPECT_NE(payloadCreateCode.find("MarkGameObjectCreationSceneDirty(m_context, *creationScene)"), std::string::npos);

    const auto completionBegin = payloadCreateEnd;
    const auto completionEnd =
        source.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(", completionBegin);
    ASSERT_NE(completionEnd, std::string::npos);
    const auto completionCode = source.substr(completionBegin, completionEnd - completionBegin);
    EXPECT_NE(completionCode.find("IsGameObjectCreationSceneLive(m_context, scene, sceneToken, *this)"), std::string::npos);
    EXPECT_EQ(completionCode.find("scene != m_context.sceneManager.GetCurrentScene()"), std::string::npos);
    EXPECT_NE(completionCode.find("MarkGameObjectCreationSceneDirty(m_context, *scene)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, BuiltInPrimitiveCreationUsesCreatePrimitiveNotModelPaths)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto actionsSourcePath = root / "Project/Editor/Core/EditorActions.cpp";
    const auto menuSourcePath = root / "Project/Editor/Utils/GameObjectCreationMenu.cpp";
    const auto sceneManagerSourcePath = root / "Runtime/Engine/SceneSystem/SceneManager.cpp";
    const auto primitiveFactorySourcePath = root / "Runtime/Engine/PrimitiveFactory.cpp";
    const auto editorResourcesSourcePath = root / "Project/Editor/Core/EditorResources.cpp";

    std::ifstream actionsStream(actionsSourcePath, std::ios::binary);
    std::ifstream menuStream(menuSourcePath, std::ios::binary);
    std::ifstream sceneManagerStream(sceneManagerSourcePath, std::ios::binary);
    std::ifstream primitiveFactoryStream(primitiveFactorySourcePath, std::ios::binary);
    std::ifstream editorResourcesStream(editorResourcesSourcePath, std::ios::binary);
    const std::string actionsSource{
        std::istreambuf_iterator<char>(actionsStream),
        std::istreambuf_iterator<char>()};
    const std::string menuSource{
        std::istreambuf_iterator<char>(menuStream),
        std::istreambuf_iterator<char>()};
    const std::string sceneManagerSource{
        std::istreambuf_iterator<char>(sceneManagerStream),
        std::istreambuf_iterator<char>()};
    const std::string primitiveFactorySource{
        std::istreambuf_iterator<char>(primitiveFactoryStream),
        std::istreambuf_iterator<char>()};
    const std::string editorResourcesSource{
        std::istreambuf_iterator<char>(editorResourcesStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(actionsSource.empty());
    ASSERT_FALSE(menuSource.empty());
    ASSERT_FALSE(sceneManagerSource.empty());
    ASSERT_FALSE(primitiveFactorySource.empty());
    ASSERT_FALSE(editorResourcesSource.empty());

    EXPECT_NE(actionsSource.find("CreatePrimitive("), std::string::npos);
    EXPECT_NE(menuSource.find("PrimitiveType::Cube"), std::string::npos);
    EXPECT_NE(sceneManagerSource.find("PrimitiveType::Cube"), std::string::npos);
    EXPECT_NE(primitiveFactorySource.find("builtin:Primitive/"), std::string::npos);
    EXPECT_EQ(actionsSource.find("\":Models\\\\Cube.fbx\""), std::string::npos);
    EXPECT_EQ(menuSource.find("\":Models\\\\"), std::string::npos);
    EXPECT_EQ(sceneManagerSource.find("ModelManager"), std::string::npos);
    EXPECT_EQ(sceneManagerSource.find("SetModel("), std::string::npos);
    EXPECT_EQ(primitiveFactorySource.find(".fbx"), std::string::npos);
    EXPECT_EQ(primitiveFactorySource.find(":Models"), std::string::npos);
    EXPECT_EQ(primitiveFactorySource.find("models/"), std::string::npos);
    EXPECT_EQ(editorResourcesSource.find("m_modelPaths[\"Cube\"]"), std::string::npos);
    EXPECT_EQ(editorResourcesSource.find("\"Cube.fbx\""), std::string::npos);
}

TEST(EditorRenderPathContractTests, RendererResourceResolutionUsesExactCachedPrefabInstance)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto resolveBegin = source.find("const NLS::Editor::Assets::PrefabInstanceRecord* ResolveLivePrefabInstance(");
    ASSERT_NE(resolveBegin, std::string::npos);
    const auto resolveEnd = source.find("void RendererResourceResolutionTargetDestroyed(", resolveBegin);
    ASSERT_NE(resolveEnd, std::string::npos);
    const auto resolveCode = source.substr(resolveBegin, resolveEnd - resolveBegin);

    EXPECT_NE(resolveCode.find("const auto* cached = state.cachedLiveInstance"), std::string::npos);
    EXPECT_NE(
        resolveCode.find("IsGameObjectCreationSceneLive(actions.GetContext(), state.scene, state.sceneToken, actions)"),
        std::string::npos);
    EXPECT_NE(resolveCode.find("auto* root = cached->instanceRoot"), std::string::npos);
    EXPECT_NE(resolveCode.find("actions.GetContext().prefabInstanceRegistry.FindInstance(*root)"), std::string::npos);
    EXPECT_NE(resolveCode.find("registered != cached"), std::string::npos);
    EXPECT_NE(resolveCode.find("return cached"), std::string::npos);
    EXPECT_EQ(resolveCode.find("for (auto* root : state.scene->GetGameObjects())"), std::string::npos);
    EXPECT_EQ(resolveCode.find("instance->prefabAssetId != state.prefabAssetId"), std::string::npos);

    const auto queueBegin =
        source.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(");
    ASSERT_NE(queueBegin, std::string::npos);
    const auto queueCode = source.substr(queueBegin);
    EXPECT_NE(queueCode.find("state->cachedLiveInstance = instance"), std::string::npos);
    EXPECT_NE(queueCode.find("state->scene = ResolveSceneForLiveObject("), std::string::npos);
    EXPECT_NE(queueCode.find("state->sceneToken = CaptureSceneMutationToken()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionIndexesAssetsAndLiveObjectsOncePerStep)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto collector = source.find("void CollectPrefabAssetResolutionTasks(");
    ASSERT_NE(collector, std::string::npos);
    const auto queue = source.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(");
    ASSERT_NE(queue, std::string::npos);
    const auto step = source.find("void RunRendererResourceResolutionStep(");
    ASSERT_NE(step, std::string::npos);
    const auto task = source.find("bool RunRendererResourceResolutionTask(");
    ASSERT_NE(task, std::string::npos);

    const auto collectorCode = source.substr(collector, queue - collector);
    const auto stepCode = source.substr(step, queue - step);
    const auto taskCode = source.substr(task, step - task);
    const auto stateBegin = source.find("struct RendererResourceResolutionState");
    ASSERT_NE(stateBegin, std::string::npos);
    const auto stateEnd = source.find("struct RendererResourceResolutionStats", stateBegin);
    ASSERT_NE(stateEnd, std::string::npos);
    const auto stateCode = source.substr(stateBegin, stateEnd - stateBegin);
    const auto queueCode = source.substr(queue);

    EXPECT_NE(source.find("struct PrefabResolvedAssetIndex"), std::string::npos);
    EXPECT_NE(source.find("PrefabResolvedAssetIndex BuildPrefabResolvedAssetIndex("), std::string::npos);
    EXPECT_NE(source.find("ResolvePrefabAssetPath(\n    const PrefabResolvedAssetIndex&"), std::string::npos);
    EXPECT_NE(collectorCode.find("const auto resolvedAssetIndex = BuildPrefabResolvedAssetIndex(prefab)"), std::string::npos);
    EXPECT_EQ(collectorCode.find("ResolvePrefabAssetPath(prefab,"), std::string::npos);

    EXPECT_NE(source.find("struct RendererResourceLiveObjectIndex"), std::string::npos);
    EXPECT_NE(source.find("EnsureRendererResourceLiveObjectIndex("), std::string::npos);
    EXPECT_NE(source.find("RendererResourceResolutionTargetDestroyed("), std::string::npos);
    EXPECT_NE(stateCode.find("bool cancelled = false"), std::string::npos);
    EXPECT_NE(stateCode.find("ListenerID destroyedListener ="), std::string::npos);
    EXPECT_NE(stateCode.find("NLS::Editor::Core::EditorActions::SceneMutationToken sceneToken"), std::string::npos);
    EXPECT_NE(stateCode.find("const NLS::Editor::Assets::PrefabInstanceRecord* cachedLiveInstance"), std::string::npos);
    EXPECT_NE(stateCode.find("RendererResourceLiveObjectIndex liveObjects"), std::string::npos);
    EXPECT_EQ(stateCode.find("NLS::Engine::GameObject* instanceRoot"), std::string::npos);
    EXPECT_EQ(stepCode.find("const auto liveObjectsBySourceId = BuildPrefabInstanceObjectIndex(*liveInstance)"), std::string::npos);
    EXPECT_NE(stepCode.find("auto* liveInstance = ResolveLivePrefabInstance(actions, *state)"), std::string::npos);
    EXPECT_NE(stepCode.find("const auto* liveObjectsBySourceId = EnsureRendererResourceLiveObjectIndex(*state, *liveInstance)"), std::string::npos);
    EXPECT_NE(stepCode.find("RunRendererResourceResolutionTask(\n            actions,\n            *liveInstance,\n            task,\n            *liveObjectsBySourceId"), std::string::npos);
    EXPECT_EQ(queueCode.find("state->liveObjectsBySourceId"), std::string::npos);
    EXPECT_NE(queueCode.find("state->destroyedListener = TrackGameObjectDestroyedListener("), std::string::npos);
    EXPECT_EQ(source.find("FindPrefabInstanceObjectBySourceId("), std::string::npos);
    EXPECT_EQ(taskCode.find("FindPrefabInstanceObjectBySourceId"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneAndHierarchyProjectModelDropsUseImportedAssetHandlesButBuiltInsUseFileDrop)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto sceneViewSourcePath = root / "Project/Editor/Panels/SceneView.cpp";
    const auto hierarchySourcePath = root / "Project/Editor/Panels/Hierarchy.cpp";
    const auto assetViewSourcePath = root / "Project/Editor/Panels/AssetView.cpp";

    const std::string sceneViewSource = ReadSourceText(sceneViewSourcePath);
    const std::string hierarchySource = ReadSourceText(hierarchySourcePath);
    const std::string assetViewSource = ReadSourceText(assetViewSourcePath);

    ASSERT_FALSE(sceneViewSource.empty());
    ASSERT_FALSE(hierarchySource.empty());
    ASSERT_FALSE(assetViewSource.empty());

    const auto sceneFileTarget = sceneViewSource.find("DDTarget<std::pair<std::string, UI::Widgets::Group*>>>(\"File\")");
    const auto sceneAssetTarget =
        sceneViewSource.find("DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>");
    ASSERT_NE(sceneFileTarget, std::string::npos);
    ASSERT_NE(sceneAssetTarget, std::string::npos);
    const auto sceneLegacyDropCode = sceneViewSource.substr(sceneFileTarget, sceneAssetTarget - sceneFileTarget);
    EXPECT_NE(sceneLegacyDropCode.find("EFileType::SCENE"), std::string::npos);
    EXPECT_NE(sceneLegacyDropCode.find("EFileType::PREFAB"), std::string::npos);
    EXPECT_NE(sceneLegacyDropCode.find("EFileType::MODEL"), std::string::npos);
    EXPECT_EQ(sceneLegacyDropCode.find("IsBuiltInResourcePath(path)"), std::string::npos);
    EXPECT_NE(sceneLegacyDropCode.find("CreateGameObjectFromAsset(path"), std::string::npos);
    EXPECT_EQ(sceneLegacyDropCode.find("modelManager["), std::string::npos);
    EXPECT_EQ(sceneLegacyDropCode.find("ModelManager>().GetResource(path"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("CreateGameObjectFromAsset(payload"), std::string::npos);

    EXPECT_EQ(hierarchySource.find("void DropAssetIntoHierarchy(const std::string& path"), std::string::npos);
    const auto hierarchyFileHelper = hierarchySource.find("void DropModelFileIntoHierarchy(");
    const auto hierarchyContextMenu = hierarchySource.find("class GameObjectContextualMenu", hierarchyFileHelper);
    ASSERT_NE(hierarchyFileHelper, std::string::npos);
    ASSERT_NE(hierarchyContextMenu, std::string::npos);
    const auto hierarchyFileHelperCode =
        hierarchySource.substr(hierarchyFileHelper, hierarchyContextMenu - hierarchyFileHelper);
    EXPECT_NE(hierarchyFileHelperCode.find("EFileType::MODEL"), std::string::npos);
    EXPECT_NE(hierarchyFileHelperCode.find("EFileType::PREFAB"), std::string::npos);
    const auto hierarchyFileCreate = hierarchyFileHelperCode.find("CreateGameObjectFromAsset(path");
    ASSERT_NE(hierarchyFileCreate, std::string::npos);
    EXPECT_EQ(hierarchyFileHelperCode.find("IsBuiltInResourcePath(path)"), std::string::npos);
    EXPECT_EQ(hierarchyFileHelperCode.find("modelManager["), std::string::npos);
    EXPECT_EQ(hierarchyFileHelperCode.find("ModelManager>().GetResource(path"), std::string::npos);
    EXPECT_NE(hierarchySource.find("DDTarget<std::pair<std::string, UI::Widgets::Group*>>>(\"File\")"), std::string::npos);
    EXPECT_NE(
        hierarchySource.find("void DropAssetIntoHierarchy(\n\tconst NLS::Editor::Assets::EditorAssetDragPayload& payload"),
        std::string::npos);
    EXPECT_NE(hierarchySource.find("CreateGameObjectFromAsset(payload"), std::string::npos);

    const auto assetBrowserSourcePath = root / "Project/Editor/Panels/AssetBrowser.cpp";
    const std::string assetBrowserSource = ReadSourceText(assetBrowserSourcePath);
    ASSERT_FALSE(assetBrowserSource.empty());
    EXPECT_NE(assetBrowserSource.find("BuildEditorAssetDragPayloadForFile("), std::string::npos);
    EXPECT_NE(
        assetBrowserSource.find("AddPlugin<UI::DDSource<NLS::Editor::Assets::EditorAssetDragPayload>>"),
        std::string::npos);
    EXPECT_NE(assetBrowserSource.find("EFileType::MATERIAL"), std::string::npos);
    EXPECT_NE(assetBrowserSource.find("EFileType::TEXTURE"), std::string::npos);
    EXPECT_NE(assetBrowserSource.find("EFileType::SHADER"), std::string::npos);
    EXPECT_NE(assetBrowserSource.find("artifactType = NLS::Core::Assets::ArtifactType::Material"), std::string::npos);
    EXPECT_NE(assetBrowserSource.find("artifactType = NLS::Core::Assets::ArtifactType::Texture"), std::string::npos);
    EXPECT_NE(assetBrowserSource.find("artifactType = NLS::Core::Assets::ArtifactType::Shader"), std::string::npos);

    const auto assetViewFileTarget =
        assetViewSource.find("DDTarget<std::pair<std::string, UI::Widgets::Group*>>>(\"File\")");
    ASSERT_NE(assetViewFileTarget, std::string::npos);
    const auto assetViewLegacyDropCode = assetViewSource.substr(assetViewFileTarget);
    EXPECT_EQ(assetViewLegacyDropCode.find("EFileType::MODEL"), std::string::npos);
    EXPECT_EQ(assetViewLegacyDropCode.find("ModelManager>().GetResource(path"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionUsesFrameTimeBudget)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    std::ifstream stream(actionsSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto queue = source.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(");
    ASSERT_NE(queue, std::string::npos);

    const auto queueCode = source.substr(queue);
    EXPECT_NE(source.find("kRendererResourceResolutionFrameBudget"), std::string::npos);
    EXPECT_NE(source.find("kRendererResourceResolutionBindTasksPerFrame"), std::string::npos);
    EXPECT_NE(source.find("kRendererResourceResolutionScheduleTasksPerFrame"), std::string::npos);
    EXPECT_EQ(source.find("kRendererResourceResolutionTasksPerFrame"), std::string::npos);
    EXPECT_NE(source.find("std::chrono::steady_clock::now()"), std::string::npos);
    EXPECT_NE(queueCode.find("remainingTasks"), std::string::npos);
    EXPECT_NE(queueCode.find("RunRendererResourceResolutionStep"), std::string::npos);
    EXPECT_NE(queueCode.find("DelayAction("), std::string::npos);
    EXPECT_NE(source.find("BuildPrefabObjectRecordIndex"), std::string::npos);
    EXPECT_NE(source.find("BuildPrefabInstanceObjectIndex"), std::string::npos);
    EXPECT_EQ(queueCode.find("std::make_shared<std::function<void()>>()"), std::string::npos);
    EXPECT_EQ(queueCode.find("static_cast<uint32_t>(index + 1u)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, RendererResourceResolutionProgressDoesNotShowNativeBlockingTaskDialog)
{
    const auto contextSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Context.cpp";
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    std::ifstream contextStream(contextSourcePath, std::ios::binary);
    const std::string contextSource{
        std::istreambuf_iterator<char>(contextStream),
        std::istreambuf_iterator<char>()};
    std::ifstream actionsStream(actionsSourcePath, std::ios::binary);
    const std::string actionsSource{
        std::istreambuf_iterator<char>(actionsStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(contextSource.empty());
    ASSERT_FALSE(actionsSource.empty());

    const auto helper = contextSource.find("bool ShouldShowNativeTaskProgress(");
    const auto subscribe = contextSource.find("importProgressTracker.Subscribe");
    ASSERT_NE(helper, std::string::npos);
    ASSERT_NE(subscribe, std::string::npos);
    const auto subscribeSource = contextSource.substr(subscribe);
    const auto hiddenEventCheck = subscribeSource.find("!ShouldShowNativeTaskProgress(event)");
    const auto presentTaskProgress = subscribeSource.find("PresentTaskProgress(");
    ASSERT_NE(hiddenEventCheck, std::string::npos);
    ASSERT_NE(presentTaskProgress, std::string::npos);

    EXPECT_NE(contextSource.find("kRendererResourceResolutionTargetPlatform"), std::string::npos);
    EXPECT_NE(contextSource.find("\"asset-resolution\""), std::string::npos);
    EXPECT_NE(contextSource.find("event.targetPlatform != kRendererResourceResolutionTargetPlatform"), std::string::npos);
    EXPECT_LT(hiddenEventCheck, presentTaskProgress);
    EXPECT_NE(actionsSource.find("\"asset-resolution\""), std::string::npos);
    EXPECT_NE(actionsSource.find("Resolving renderer resource "), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneValidationReadbackWaitsForDeferredAssetResolution)
{
    const auto sceneViewSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/SceneView.cpp";

    std::ifstream stream(sceneViewSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto readback = source.find("void Editor::Panels::SceneView::TryWriteValidationReadback()");
    ASSERT_NE(readback, std::string::npos);

    const auto readbackCode = source.substr(readback);
    EXPECT_NE(readbackCode.find("importProgressTracker.HasRunningJobs()"), std::string::npos);
    EXPECT_NE(readbackCode.find("m_validationReadbackReadyFrames = 0u"), std::string::npos);
}

TEST(EditorRenderPathContractTests, StartupPreimportRunsBeforeEditorUiAndAssetBrowserDoesNotScheduleStartupImport)
{
    const auto applicationHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Application.h";
    const auto applicationSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Application.cpp";
    const auto contextSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Context.cpp";
    const auto editorSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Editor.cpp";
    const auto assetBrowserSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/AssetBrowser.cpp";

    std::ifstream applicationHeaderStream(applicationHeaderPath, std::ios::binary);
    const std::string applicationHeader{
        std::istreambuf_iterator<char>(applicationHeaderStream),
        std::istreambuf_iterator<char>()};
    std::ifstream applicationStream(applicationSourcePath, std::ios::binary);
    const std::string applicationSource{
        std::istreambuf_iterator<char>(applicationStream),
        std::istreambuf_iterator<char>()};
    std::ifstream contextStream(contextSourcePath, std::ios::binary);
    const std::string contextSource{
        std::istreambuf_iterator<char>(contextStream),
        std::istreambuf_iterator<char>()};
    std::ifstream editorStream(editorSourcePath, std::ios::binary);
    const std::string editorSource{
        std::istreambuf_iterator<char>(editorStream),
        std::istreambuf_iterator<char>()};
    std::ifstream assetBrowserStream(assetBrowserSourcePath, std::ios::binary);
    const std::string assetBrowserSource{
        std::istreambuf_iterator<char>(assetBrowserStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(applicationHeader.empty());
    ASSERT_FALSE(applicationSource.empty());
    ASSERT_FALSE(contextSource.empty());
    ASSERT_FALSE(editorSource.empty());
    ASSERT_FALSE(assetBrowserSource.empty());

    EXPECT_NE(applicationHeader.find("#include <memory>"), std::string::npos);
    EXPECT_NE(applicationHeader.find("std::unique_ptr<Editor> m_editor"), std::string::npos);
    EXPECT_EQ(applicationHeader.find("Editor m_editor"), std::string::npos);

    const auto startProjectWatcher = applicationSource.find("startupProjectAssetsWatcher.Start");
    const auto startupPreimport = applicationSource.find("RunBlockingStartupAssetPreimport(");
    const auto constructEditor = applicationSource.find("std::make_unique<Editor>(m_context)");
    const auto adoptWatchers = applicationSource.find("AdoptStartupAssetWatchers(");
    const auto startupWatcherPreimport = applicationSource.find("RunStartupWatcherPreimport(");
    const auto firstStartupWatcherPreimportCall =
        applicationSource.find("Importing startup asset changes");
    const auto firstFrame = applicationSource.find("RunEditorFrame(0.0f)");
    const auto finalStartupWatcherPreimportCall =
        applicationSource.find("Importing final startup asset changes");
    const auto completeWatcherGate = applicationSource.find("CompleteStartupWatcherPreimportGate(");
    const auto openEditor = applicationSource.find("CompleteStartupProgress()");
    ASSERT_NE(startProjectWatcher, std::string::npos);
    ASSERT_NE(startupPreimport, std::string::npos);
    ASSERT_NE(constructEditor, std::string::npos);
    ASSERT_NE(adoptWatchers, std::string::npos);
    ASSERT_NE(startupWatcherPreimport, std::string::npos);
    ASSERT_NE(firstStartupWatcherPreimportCall, std::string::npos);
    ASSERT_NE(firstFrame, std::string::npos);
    ASSERT_NE(finalStartupWatcherPreimportCall, std::string::npos);
    ASSERT_NE(completeWatcherGate, std::string::npos);
    ASSERT_NE(openEditor, std::string::npos);
    EXPECT_LT(startProjectWatcher, startupPreimport);
    EXPECT_LT(startupPreimport, constructEditor);
    EXPECT_LT(constructEditor, adoptWatchers);
    EXPECT_LT(adoptWatchers, firstStartupWatcherPreimportCall);
    EXPECT_LT(firstStartupWatcherPreimportCall, firstFrame);
    EXPECT_LT(firstFrame, finalStartupWatcherPreimportCall);
    EXPECT_LT(finalStartupWatcherPreimportCall, completeWatcherGate);
    EXPECT_LT(completeWatcherGate, openEditor);
    EXPECT_LT(constructEditor, firstFrame);
    EXPECT_LT(constructEditor, openEditor);
    EXPECT_LT(startupPreimport, firstFrame);
    EXPECT_LT(startupPreimport, openEditor);
    const auto refreshAssetBrowser = applicationSource.find("m_editor->RefreshProjectAssetBrowser()");
    ASSERT_NE(refreshAssetBrowser, std::string::npos);
    EXPECT_LT(constructEditor, refreshAssetBrowser);
    EXPECT_LT(refreshAssetBrowser, firstFrame);
    const auto startupFailureLog = applicationSource.find("Startup asset preimport failed:");
    const auto startupFailureThrow = applicationSource.find("throw std::runtime_error", startupFailureLog);
    ASSERT_NE(startupFailureLog, std::string::npos);
    ASSERT_NE(startupFailureThrow, std::string::npos);
    EXPECT_LT(startupFailureThrow, constructEditor);
    EXPECT_NE(applicationSource.find("!startupPreimport.hadRunningJobsAfterCompletion"), std::string::npos);

    const auto beforeDraw = assetBrowserSource.find("void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()");
    const auto requestRefresh = assetBrowserSource.find("void Editor::Panels::AssetBrowser::RequestRefresh()");
    ASSERT_NE(beforeDraw, std::string::npos);
    ASSERT_NE(requestRefresh, std::string::npos);
    const auto beforeDrawSource = assetBrowserSource.substr(beforeDraw, requestRefresh - beforeDraw);
    EXPECT_EQ(beforeDrawSource.find("AssetPreimportReason::EditorStartup"), std::string::npos);
    EXPECT_NE(beforeDrawSource.find("AssetPreimportReason::FileWatcherChanged"), std::string::npos);
    EXPECT_NE(contextSource.find("windowSettings.visible = false"), std::string::npos);
    EXPECT_EQ(contextSource.find("windowSettings.visible = true"), std::string::npos);
    const auto completeStartupProgress = contextSource.find("void Editor::Core::Context::CompleteStartupProgress()");
    const auto nativeAvailability = contextSource.find("bool Editor::Core::Context::IsNativeStartupProgressAvailable()", completeStartupProgress);
    ASSERT_NE(completeStartupProgress, std::string::npos);
    ASSERT_NE(nativeAvailability, std::string::npos);
    const auto completeStartupProgressSource =
        contextSource.substr(completeStartupProgress, nativeAvailability - completeStartupProgress);
    EXPECT_EQ(contextSource.substr(0, completeStartupProgress).find("window->Show()"), std::string::npos);
    EXPECT_NE(completeStartupProgressSource.find("window->Show()"), std::string::npos);
    EXPECT_EQ(completeStartupProgressSource.find(".release()"), std::string::npos);
    EXPECT_EQ(completeStartupProgressSource.find("DetachThread()"), std::string::npos);
    EXPECT_NE(completeStartupProgressSource.find("std::move(m_nativeProgressDialog)"), std::string::npos);
    EXPECT_NE(completeStartupProgressSource.find("dialog->Close()"), std::string::npos);

    const auto editorConstructor = editorSource.find("Editor::Core::Editor::Editor(Context& p_context)");
    const auto editorDestructor = editorSource.find("Editor::Core::Editor::~Editor()", editorConstructor);
    ASSERT_NE(editorConstructor, std::string::npos);
    ASSERT_NE(editorDestructor, std::string::npos);
    const auto editorConstructorSource = editorSource.substr(editorConstructor, editorDestructor - editorConstructor);
    EXPECT_EQ(editorConstructorSource.find("CompleteStartupProgress()"), std::string::npos);
    EXPECT_NE(editorSource.find("void Editor::Core::Editor::AdoptStartupAssetWatchers("), std::string::npos);
    EXPECT_NE(editorSource.find("bool Editor::Core::Editor::RunStartupWatcherPreimport("), std::string::npos);
    EXPECT_NE(editorSource.find("bool Editor::Core::Editor::CompleteStartupWatcherPreimportGate("), std::string::npos);
    EXPECT_NE(editorSource.find("AdoptStartupWatchers("), std::string::npos);
    EXPECT_NE(editorSource.find("RunStartupWatcherPreimport("), std::string::npos);
    EXPECT_NE(editorSource.find("CompleteStartupWatcherPreimportGate("), std::string::npos);

    const auto adoptStartupWatchers =
        assetBrowserSource.find("void Editor::Panels::AssetBrowser::AdoptStartupWatchers(");
    const auto requestRefreshFunction =
        assetBrowserSource.find("void Editor::Panels::AssetBrowser::RequestRefresh()", adoptStartupWatchers);
    ASSERT_NE(adoptStartupWatchers, std::string::npos);
    ASSERT_NE(requestRefreshFunction, std::string::npos);
    const auto adoptStartupWatchersSource =
        assetBrowserSource.substr(adoptStartupWatchers, requestRefreshFunction - adoptStartupWatchers);
    EXPECT_NE(adoptStartupWatchersSource.find("m_startupWatcherPreimportGateOpen = false"), std::string::npos);

    const auto runStartupWatcherPreimport =
        assetBrowserSource.find("bool Editor::Panels::AssetBrowser::RunStartupWatcherPreimport(");
    ASSERT_NE(runStartupWatcherPreimport, std::string::npos);
    const auto runStartupWatcherPreimportSource =
        assetBrowserSource.substr(runStartupWatcherPreimport, requestRefreshFunction - runStartupWatcherPreimport);
    EXPECT_NE(runStartupWatcherPreimportSource.find("m_projectAssetsWatcher.ConsumeChangedPaths()"), std::string::npos);
    EXPECT_NE(runStartupWatcherPreimportSource.find("AssetPreimportReason::FileWatcherChanged"), std::string::npos);
    EXPECT_NE(runStartupWatcherPreimportSource.find("preimportScheduler.Run("), std::string::npos);

    const auto completeStartupGate =
        assetBrowserSource.find("bool Editor::Panels::AssetBrowser::CompleteStartupWatcherPreimportGate(");
    const auto completeWatcherStartupIfReady =
        assetBrowserSource.find("void Editor::Panels::AssetBrowser::CompleteWatcherStartupIfReady()", completeStartupGate);
    ASSERT_NE(completeStartupGate, std::string::npos);
    ASSERT_NE(completeWatcherStartupIfReady, std::string::npos);
    const auto completeStartupGateSource =
        assetBrowserSource.substr(completeStartupGate, completeWatcherStartupIfReady - completeStartupGate);
    const auto gateDrain = completeStartupGateSource.find("RunStartupWatcherPreimport(progressSink)");
    const auto gateOpen = completeStartupGateSource.find("m_startupWatcherPreimportGateOpen = true");
    ASSERT_NE(gateDrain, std::string::npos);
    ASSERT_NE(gateOpen, std::string::npos);
    EXPECT_LT(gateDrain, gateOpen);
    EXPECT_NE(completeStartupGateSource.find("m_startupWatcherPreimportGateOpen = true"), std::string::npos);
    EXPECT_NE(completeStartupGateSource.find("return true"), std::string::npos);

    const auto buildDragPayload =
        assetBrowserSource.find("BuildEditorAssetDragPayloadForFile(");
    ASSERT_NE(buildDragPayload, std::string::npos);
    const auto manifestFreshness =
        assetBrowserSource.find("bool ManifestDependencyStampsAreCurrent(");
    ASSERT_NE(manifestFreshness, std::string::npos);
    const auto manifestFreshnessSource =
        assetBrowserSource.substr(manifestFreshness, buildDragPayload - manifestFreshness);
    EXPECT_NE(
        manifestFreshnessSource.find("JsonUInt(manifest, \"importerVersion\")"),
        std::string::npos);
    EXPECT_NE(
        manifestFreshnessSource.find("JsonString(manifest, \"targetPlatform\")"),
        std::string::npos);

    const auto renameAsset =
        assetBrowserSource.find("void RenameAsset(", buildDragPayload);
    ASSERT_NE(renameAsset, std::string::npos);
    const auto dragPayloadSource = assetBrowserSource.substr(buildDragPayload, renameAsset - buildDragPayload);
    EXPECT_NE(
        dragPayloadSource.find("fileType == Utils::PathParser::EFileType::PREFAB"),
        std::string::npos);
    EXPECT_NE(
        dragPayloadSource.find("JsonString(manifest, \"primarySubAssetKey\")"),
        std::string::npos);
    EXPECT_NE(
        dragPayloadSource.find("ResolveArtifactPathForManifest(ProjectRootFromAssetsFolder(projectAssetsFolder), subAsset)"),
        std::string::npos);
    EXPECT_NE(
        dragPayloadSource.find("std::filesystem::is_regular_file(resolvedArtifactPath)"),
        std::string::npos);
    EXPECT_NE(
        dragPayloadSource.find("resolvedArtifactPath.empty()"),
        std::string::npos);
    EXPECT_NE(
        dragPayloadSource.find("subAssetKey = \"prefab:\" + std::filesystem::path(resourceFormatPath).stem().generic_string()"),
        std::string::npos);
}

TEST(EditorRenderPathContractTests, EditorManifestJsonParsingHasSingleSchemaReader)
{
    const auto manifestJsonPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/EditorAssetManifestJson.h";
    const auto facadeSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/AssetDatabaseFacade.cpp";
    const auto bridgeSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/EditorAssetDragDropBridge.cpp";

    std::ifstream manifestStream(manifestJsonPath, std::ios::binary);
    const std::string manifestSource{
        std::istreambuf_iterator<char>(manifestStream),
        std::istreambuf_iterator<char>()};
    std::ifstream facadeStream(facadeSourcePath, std::ios::binary);
    const std::string facadeSource{
        std::istreambuf_iterator<char>(facadeStream),
        std::istreambuf_iterator<char>()};
    std::ifstream bridgeStream(bridgeSourcePath, std::ios::binary);
    const std::string bridgeSource{
        std::istreambuf_iterator<char>(bridgeStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(manifestSource.empty());
    ASSERT_FALSE(facadeSource.empty());
    ASSERT_FALSE(bridgeSource.empty());
    EXPECT_NE(manifestSource.find("ParseArtifactManifestJson("), std::string::npos);
    EXPECT_NE(manifestSource.find("JsonUIntOrDefault(root, \"importerVersion\", 1u)"), std::string::npos);
    EXPECT_NE(facadeSource.find("return ParseArtifactManifestJson(root, false);"), std::string::npos);
    EXPECT_NE(bridgeSource.find("return ParseArtifactManifestJson(root, true);"), std::string::npos);
    EXPECT_EQ(facadeSource.find("JsonUIntOrDefault(root, \"importerVersion\""), std::string::npos);
    EXPECT_EQ(bridgeSource.find("JsonUIntOrDefault(root, \"importerVersion\""), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionUsesPrefabInstanceMappings)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    std::ifstream stream(actionsSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto collector = source.find("void CollectPrefabAssetResolutionTasks(");
    ASSERT_NE(collector, std::string::npos);
    const auto queue = source.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(");
    ASSERT_NE(queue, std::string::npos);

    const auto collectorCode = source.substr(collector, queue - collector);
    EXPECT_NE(collectorCode.find("BuildPrefabInstanceObjectIndex"), std::string::npos);
    EXPECT_NE(collectorCode.find("instanceObjectsBySourceId.find(sourceObject.id)"), std::string::npos);
    EXPECT_NE(source.find("instance.sourceByInstanceObject"), std::string::npos);
    EXPECT_NE(collectorCode.find("GetComponent<NLS::Engine::Components::MeshFilter>()"), std::string::npos);
    EXPECT_NE(collectorCode.find("GetComponent<NLS::Engine::Components::MeshRenderer>()"), std::string::npos);
    EXPECT_NE(collectorCode.find("PrefabComponentRecordMatches<NLS::Engine::Components::MeshFilter>(*componentRecord)"), std::string::npos);
    EXPECT_NE(collectorCode.find("PrefabComponentRecordMatches<NLS::Engine::Components::MeshRenderer>(*componentRecord) && meshRenderer"), std::string::npos);
    EXPECT_NE(source.find("BindDeferredMeshPath"), std::string::npos);
    EXPECT_NE(source.find("GetResource(task.modelPath, false)"), std::string::npos);
    EXPECT_NE(source.find("meshFilter.SetModelPathHint(task.modelPath)"), std::string::npos);
    EXPECT_EQ(source.find("meshRenderer.SetModelPathHint(task.modelPath)"), std::string::npos);
    EXPECT_EQ(collectorCode.find("meshRenderer->SetModelPathHint(modelPath)"), std::string::npos);
    EXPECT_NE(source.find("meshRenderer.SetFrustumBehaviour("), std::string::npos);
    EXPECT_NE(source.find("EFrustumBehaviour::CULL_MODEL"), std::string::npos);
    EXPECT_EQ(source.find("EFrustumBehaviour::DISABLED"), std::string::npos);
    EXPECT_EQ(collectorCode.find("std::min(sourceComponents.size(), liveComponents.size())"), std::string::npos);
}

TEST(EditorRenderPathContractTests, EditorViewportCamerasEnableGeometryFrustumCullingForLargeImportedModels)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto controllableViewSourcePath = root / "Project/Editor/Panels/AViewControllable.cpp";
    const auto sceneManagerSourcePath = root / "Runtime/Engine/SceneSystem/SceneManager.cpp";

    std::ifstream controllableViewStream(controllableViewSourcePath, std::ios::binary);
    const std::string controllableViewSource{
        std::istreambuf_iterator<char>(controllableViewStream),
        std::istreambuf_iterator<char>()};
    std::ifstream sceneManagerStream(sceneManagerSourcePath, std::ios::binary);
    const std::string sceneManagerSource{
        std::istreambuf_iterator<char>(sceneManagerStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(controllableViewSource.empty());
    ASSERT_FALSE(sceneManagerSource.empty());
    EXPECT_NE(controllableViewSource.find("m_camera.SetFrustumGeometryCulling(true)"), std::string::npos);
    EXPECT_NE(sceneManagerSource.find("cameraComponent->SetFrustumGeometryCulling(true)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, EditorCameraHelperUsesOverlayPipelineState)
{
    const std::filesystem::path rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp";

    std::ifstream stream(rendererSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto cameraPass = source.find("class DebugCamerasRenderPass");
    const auto lightPass = source.find("class DebugLightsRenderPass");
    ASSERT_NE(cameraPass, std::string::npos);
    ASSERT_NE(lightPass, std::string::npos);

    const auto cameraSource = source.substr(cameraPass, lightPass - cameraPass);
    EXPECT_NE(
        cameraSource.find("CreateEditorOverlayPipelineState"),
        std::string::npos);
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

TEST(EditorRenderPathContractTests, DeferredEditorCameraAndLightOverlayPassesAreSubmittedByDebugName)
{
    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.drawCommandCount = 4u;
    package.helperDrawCount = 2u;
    package.renderWidth = 320u;
    package.renderHeight = 180u;
    package.recordedDrawCommands.resize(4u);

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor.renderWidth = 320u;
    lightGridContext.frameDescriptor.renderHeight = 180u;

    NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;

    NLS::Render::Context::RenderPassCommandInput cameraPass;
    cameraPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    cameraPass.debugName = "EditorDebugCamerasPass";
    cameraPass.drawCount = 3u;

    NLS::Render::Context::RenderPassCommandInput lightPass;
    lightPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    lightPass.debugName = "EditorDebugLightsPass";
    lightPass.drawCount = 2u;

    std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> appendedMetadata;

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata cameraMetadata;
    cameraMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    cameraMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    cameraMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    cameraMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    cameraMetadata.visibleDrawCountContribution = 1u;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(cameraMetadata, "EditorDebugCamerasPass");
    appendedMetadata.push_back(cameraMetadata);

    NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata lightMetadata;
    lightMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
    lightMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
    lightMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
    lightMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    lightMetadata.visibleDrawCountContribution = 1u;
    NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(lightMetadata, "EditorDebugLightsPass");
    appendedMetadata.push_back(lightMetadata);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        { cameraPass, lightPass },
        appendedMetadata);

    ASSERT_EQ(package.passCommandInputs.size(), 4u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_EQ(package.passCommandInputs[2].debugName, "EditorDebugCamerasPass");
    EXPECT_EQ(package.passCommandInputs[3].debugName, "EditorDebugLightsPass");
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

TEST(EditorRenderPathContractTests, EditorHelperMeshesLoadThroughGeneratedMeshArtifacts)
{
    const std::filesystem::path editorResourcesSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorResources.cpp";

    const std::string source = ReadSourceText(editorResourcesSourcePath);

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("EnsureEditorHelperMeshArtifact"), std::string::npos);
    EXPECT_NE(source.find("projectRoot / \"Library\""), std::string::npos);
    EXPECT_NE(source.find("\"EditorHelperArtifacts\""), std::string::npos);
    EXPECT_NE(source.find("\"Models\""), std::string::npos);
    EXPECT_NE(source.find("GenerateEditorHelperMeshArtifact"), std::string::npos);
    EXPECT_NE(source.find("meshManager.GetResource(resourcePath->string(), true)"), std::string::npos);
    EXPECT_EQ(source.find("meshManager.GetResource(found->second, true)"), std::string::npos);
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

TEST(EditorRenderPathContractTests, DeferredSceneViewEditorVisualHelpersDeclareExternalColorWrites)
{
    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "SceneViewColor";
    colorDesc.extent = { 320u, 180u, 1u };
    colorDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    colorDesc.mipLevels = 3u;
    colorDesc.arrayLayers = 4u;
    auto colorTexture = std::make_shared<TestTexture>(colorDesc);

    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.debugName = "SceneViewColorView";
    colorViewDesc.subresourceRange.baseMipLevel = 1u;
    colorViewDesc.subresourceRange.mipLevelCount = 1u;
    colorViewDesc.subresourceRange.baseArrayLayer = 2u;
    colorViewDesc.subresourceRange.arrayLayerCount = 1u;
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
    package.drawCommandCount = 4u;
    package.helperDrawCount = 3u;
    package.renderWidth = frameDescriptor.renderWidth;
    package.renderHeight = frameDescriptor.renderHeight;
    package.recordedDrawCommands.resize(4u);

    const auto makeHelperPass =
        [](const char* debugName)
        {
            NLS::Render::Context::RenderPassCommandInput passInput;
            passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
            passInput.debugName = debugName;
            passInput.drawCount = 1u;
            passInput.usesColorAttachment = true;
            passInput.usesDepthStencilAttachment = true;
            return passInput;
        };

    const auto makeHelperMetadata =
        [](const char* debugName)
        {
            NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata metadata;
            metadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
            metadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
            metadata.queueType = NLS::Render::RHI::QueueType::Graphics;
            metadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(metadata, debugName);
            return metadata;
        };

    const std::vector<NLS::Render::Context::RenderPassCommandInput> appendedPassInputs {
        makeHelperPass("EditorGridPass"),
        makeHelperPass("EditorDebugCamerasPass"),
        makeHelperPass("EditorDebugLightsPass")
    };
    const std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> appendedMetadata {
        makeHelperMetadata("EditorGridPass"),
        makeHelperMetadata("EditorDebugCamerasPass"),
        makeHelperMetadata("EditorDebugLightsPass")
    };

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
        appendedMetadata);

    ASSERT_EQ(package.passCommandInputs.size(), 5u);
    for (size_t passIndex = 2u; passIndex < package.passCommandInputs.size(); ++passIndex)
    {
        const auto& overlayPass = package.passCommandInputs[passIndex];
        SCOPED_TRACE(overlayPass.debugName);
        ASSERT_EQ(overlayPass.colorAttachmentViews.size(), 1u);
        EXPECT_EQ(overlayPass.colorAttachmentViews[0], colorView);
        EXPECT_EQ(overlayPass.depthStencilAttachmentView, gbufferDepthView);

        const auto colorAccess = std::find_if(
            overlayPass.textureResourceAccesses.begin(),
            overlayPass.textureResourceAccesses.end(),
            [&colorTexture](const NLS::Render::Context::TextureResourceAccess& access)
            {
                return access.texture == colorTexture;
            });
        ASSERT_NE(colorAccess, overlayPass.textureResourceAccesses.end());
        EXPECT_EQ(colorAccess->subresourceRange.baseMipLevel, colorViewDesc.subresourceRange.baseMipLevel);
        EXPECT_EQ(colorAccess->subresourceRange.mipLevelCount, colorViewDesc.subresourceRange.mipLevelCount);
        EXPECT_EQ(colorAccess->subresourceRange.baseArrayLayer, colorViewDesc.subresourceRange.baseArrayLayer);
        EXPECT_EQ(colorAccess->subresourceRange.arrayLayerCount, colorViewDesc.subresourceRange.arrayLayerCount);
        EXPECT_EQ(colorAccess->mode, NLS::Render::Context::ResourceAccessMode::Write);
        EXPECT_EQ(colorAccess->state, NLS::Render::RHI::ResourceState::RenderTarget);
        EXPECT_EQ(colorAccess->stages, NLS::Render::RHI::PipelineStageMask::RenderTarget);
        EXPECT_EQ(
            colorAccess->access,
            NLS::Render::RHI::AccessMask::ColorAttachmentRead |
                NLS::Render::RHI::AccessMask::ColorAttachmentWrite);
    }
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
    helperState.gameObjectPassEnabled = true;
    helperState.debugDrawPassEnabled = true;
    helperState.gridEnabled = true;
    helperState.sceneCameraCount = 1u;
    helperState.sceneLightCount = 2u;
    helperState.hasSelectedGameObject = true;
    helperState.hasVisibleDebugDrawPrimitives = true;

    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedGridHelperPass(helperState));
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedCameraHelperPass(helperState));
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedLightHelperPass(helperState));
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedOutlineHelperPass(helperState));
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedGizmoHelperPass(helperState));
    EXPECT_TRUE(NLS::Editor::Rendering::HasThreadedDebugDrawHelperPass(helperState));
    EXPECT_EQ(NLS::Editor::Rendering::CountThreadedEditorHelperPasses(helperState), 6u);

    helperState.gameObjectPassEnabled = false;
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
