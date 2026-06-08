#include <gtest/gtest.h>

#include <fg/Blackboard.hpp>
#include <fg/FrameGraph.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <type_traits>

#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/DebugGameObjectSelectionCollector.h"
#include "Rendering/EditorHelperLifecycle.h"
#include "Components/CameraComponent.h"
#include "Engine/LayerMask.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilder.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/SelectionOutlineMaskRenderer.h"
#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/ForwardSceneRenderer.h"
#include "Rendering/SceneHLOD.h"
#include "Rendering/SceneStreamingResidency.h"

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

    std::size_t CountOccurrences(const std::string& text, const std::string& pattern)
    {
        std::size_t count = 0u;
        std::size_t position = 0u;
        while ((position = text.find(pattern, position)) != std::string::npos)
        {
            ++count;
            position += pattern.size();
        }
        return count;
    }

    std::map<std::string, int> ParseHlslStaticIntConstants(const std::string& source)
    {
        std::map<std::string, int> constants;
        const std::regex pattern(R"(static\s+const\s+int\s+(\w+)\s*=\s*(-?\d+)\s*;)");
        for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it)
            constants[it->str(1)] = std::stoi(it->str(2));
        return constants;
    }

    struct SelectionMaskChannelContract
    {
        std::string swizzle;
        int index = -1;
    };

    std::map<std::string, SelectionMaskChannelContract> ParseSelectionMaskChannels(
        const std::string& channelTable)
    {
        std::map<std::string, SelectionMaskChannelContract> channels;
        std::istringstream stream(channelTable);
        std::string line;
        while (std::getline(stream, line))
        {
            const auto macro = line.find("NLS_SELECTION_OUTLINE_MASK_CHANNEL(");
            if (macro == std::string::npos)
                continue;

            const auto argsStart = line.find('(', macro);
            const auto argsEnd = line.find(')', argsStart);
            if (argsStart == std::string::npos || argsEnd == std::string::npos)
                continue;

            const auto args = line.substr(argsStart + 1u, argsEnd - argsStart - 1u);
            std::array<std::string, 3> parts {};
            std::istringstream argsStream(args);
            for (auto& part : parts)
            {
                std::getline(argsStream, part, ',');
                part.erase(std::remove_if(part.begin(), part.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }), part.end());
            }

            if (!parts[0].empty() && !parts[1].empty() && !parts[2].empty())
                channels[parts[0]] = { parts[1], std::stoi(parts[2]) };
        }
        return channels;
    }

    std::string ExpandSelectionOutlinePassModeConstantsForTest(const std::string& passModeTable)
    {
        std::ostringstream expanded;
        std::istringstream stream(passModeTable);
        std::string line;
        const std::regex pattern(R"(NLS_SELECTION_OUTLINE_MASK_PASS_MODE\((\w+),\s*(-?\d+)\))");
        while (std::getline(stream, line))
        {
            std::smatch match;
            if (std::regex_search(line, match, pattern))
                expanded << "static const int SelectionOutlinePassMode" << match[1].str() << " = " << match[2].str() << ";\n";
        }
        return expanded.str();
    }

    std::string ExpandSelectionOutlineChannelIndexConstantsForTest(const std::string& channelTable)
    {
        std::ostringstream expanded;
        const auto channels = ParseSelectionMaskChannels(channelTable);
        for (const auto& [name, channel] : channels)
            expanded << "static const int SelectionOutlineMask" << name << "Index = " << channel.index << ";\n";
        return expanded.str();
    }

    NLS::Render::Resources::Mesh CreateSelectionTestMesh()
    {
        return NLS::Render::Resources::Mesh(
            {},
            {},
            0u,
            NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
            {{0.0f, 0.0f, 0.0f}, 1.0f});
    }

    class ToggleableMeshRenderer final : public NLS::Engine::Components::MeshRenderer
    {
    public:
        void SetSelfEnabledForTest(const bool enabled)
        {
            m_enabled = enabled;
        }
    };

    NLS::Render::Data::Frustum CreateForwardSelectionFrustum()
    {
        NLS::Render::Data::Frustum frustum;
        const auto view = NLS::Maths::Matrix4::CreateView(
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, -1.0f,
            0.0f, 1.0f, 0.0f);
        const auto projection = NLS::Maths::Matrix4::CreatePerspective(90.0f, 1.0f, 0.1f, 100.0f);
        frustum.CalculateFrustum(projection * view);
        return frustum;
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

    std::shared_ptr<TestTexture> MakeDeferredTestTexture(
        const char* debugName,
        const NLS::Render::RHI::TextureFormat format = NLS::Render::RHI::TextureFormat::RGBA8,
        const NLS::Render::RHI::TextureUsageFlags usage = NLS::Render::RHI::TextureUsageFlags::Sampled)
    {
        NLS::Render::RHI::RHITextureDesc desc;
        desc.debugName = debugName;
        desc.extent = { 320u, 180u, 1u };
        desc.format = format;
        desc.usage = usage;
        desc.mipLevels = 1u;
        desc.arrayLayers = 1u;
        return std::make_shared<TestTexture>(desc);
    }

    std::shared_ptr<TestTextureView> MakeDeferredTestTextureView(
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
        const char* debugName)
    {
        NLS::Render::RHI::RHITextureViewDesc desc;
        desc.debugName = debugName;
        desc.format = texture->GetDesc().format;
        return std::make_shared<TestTextureView>(texture, desc);
    }

    NLS::Render::FrameGraph::DeferredPreparedSceneResources MakeCompleteDeferredPreparedSceneResources(
        std::shared_ptr<NLS::Render::RHI::RHITextureView> depthView = nullptr)
    {
        NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;
        const auto albedoTexture = MakeDeferredTestTexture(
            "DeferredGBufferAlbedo",
            NLS::Render::FrameGraph::kDeferredGBufferColorFormats[0],
            NLS::Render::FrameGraph::kDeferredGBufferColorUsage);
        const auto normalTexture = MakeDeferredTestTexture(
            "DeferredGBufferNormal",
            NLS::Render::FrameGraph::kDeferredGBufferColorFormats[1],
            NLS::Render::FrameGraph::kDeferredGBufferColorUsage);
        const auto materialTexture = MakeDeferredTestTexture(
            "DeferredGBufferMaterial",
            NLS::Render::FrameGraph::kDeferredGBufferColorFormats[2],
            NLS::Render::FrameGraph::kDeferredGBufferColorUsage);
        auto depthTexture = depthView != nullptr
            ? depthView->GetTexture()
            : std::static_pointer_cast<NLS::Render::RHI::RHITexture>(
                MakeDeferredTestTexture(
                    "DeferredGBufferDepth",
                    NLS::Render::FrameGraph::kDeferredGBufferDepthFormat,
                    NLS::Render::FrameGraph::kDeferredGBufferDepthUsage));

        resources.gbufferColorViews.push_back(MakeDeferredTestTextureView(albedoTexture, "DeferredGBufferAlbedoView"));
        resources.gbufferColorViews.push_back(MakeDeferredTestTextureView(normalTexture, "DeferredGBufferNormalView"));
        resources.gbufferColorViews.push_back(MakeDeferredTestTextureView(materialTexture, "DeferredGBufferMaterialView"));
        resources.gbufferDepthView = depthView != nullptr
            ? std::move(depthView)
            : MakeDeferredTestTextureView(depthTexture, "DeferredGBufferDepthView");
        resources.gbufferTextures = {
            resources.gbufferColorViews[0]->GetTexture(),
            resources.gbufferColorViews[1]->GetTexture(),
            resources.gbufferColorViews[2]->GetTexture(),
            resources.gbufferDepthView->GetTexture()
        };
        return resources;
    }

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

TEST(EditorRenderPathContractTests, EditorDeferredPathKeepsGBufferBeforeLightingDescriptorOrder)
{
    const auto passDescriptors = NLS::Render::FrameGraph::GetDeferredScenePassDescriptors();

    ASSERT_EQ(passDescriptors.size(), 2u);
    EXPECT_EQ(passDescriptors[0].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(passDescriptors[1].metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_STREQ(passDescriptors[0].metadata.graphPassName, "DeferredGBuffer");
    EXPECT_STREQ(passDescriptors[1].metadata.graphPassName, "DeferredLighting");
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

TEST(EditorRenderPathContractTests, CamerasDefaultToAllLayersAndExposeViewLayerMasks)
{
    NLS::Render::Entities::Camera camera;

    EXPECT_EQ(
        camera.GetVisibleLayerMask(),
        NLS::Render::Entities::Camera::kAllVisibleLayersMask);
    EXPECT_TRUE(camera.IsLayerVisible(0));
    EXPECT_TRUE(camera.IsLayerVisible(31));
    EXPECT_FALSE(camera.IsLayerVisible(32));

    camera.SetVisibleLayerMask(1u << 7u);

    EXPECT_EQ(camera.GetVisibleLayerMask(), 1u << 7u);
    EXPECT_FALSE(camera.IsLayerVisible(0));
    EXPECT_TRUE(camera.IsLayerVisible(7));
    EXPECT_FALSE(camera.IsLayerVisible(31));
}

TEST(EditorRenderPathContractTests, CameraComponentForwardsLayerMaskToRenderCamera)
{
    NLS::Engine::GameObject actor("LayerMaskedCamera");
    auto* cameraComponent = actor.AddComponent<NLS::Engine::Components::CameraComponent>();
    ASSERT_NE(cameraComponent, nullptr);
    ASSERT_NE(cameraComponent->GetCamera(), nullptr);

    EXPECT_EQ(
        cameraComponent->GetVisibleLayers().GetMask(),
        NLS::Render::Entities::Camera::kAllVisibleLayersMask);

    cameraComponent->SetVisibleLayers(NLS::Engine::LayerMask(1u << 5u));

    EXPECT_EQ(cameraComponent->GetVisibleLayers().GetMask(), 1u << 5u);
    EXPECT_EQ(cameraComponent->GetCamera()->GetVisibleLayerMask(), 1u << 5u);
    EXPECT_TRUE(cameraComponent->GetCamera()->IsLayerVisible(5));
    EXPECT_FALSE(cameraComponent->GetCamera()->IsLayerVisible(0));
}

TEST(EditorRenderPathContractTests, SceneHLODEditorSelectionOverrideKeepsSelectedChildInspectable)
{
    using NLS::Engine::Rendering::HLODClusterRecord;
    using NLS::Engine::Rendering::HLODCompatibilityFlags;
    using NLS::Engine::Rendering::RepresentationResidencySnapshot;
    using NLS::Engine::Rendering::SceneHLODSystem;
    using NLS::Engine::Rendering::SceneHLODViewInput;
    using NLS::Engine::Rendering::ScenePrimitiveHandle;

    const ScenePrimitiveHandle firstChild { 0x61u, 0u, 1u };
    const ScenePrimitiveHandle selectedChild { 0x61u, 1u, 1u };
    const ScenePrimitiveHandle proxy { 0x61u, 10u, 1u };

    HLODClusterRecord cluster;
    cluster.clusterHandle = { 1u };
    cluster.childPrimitives = { firstChild, selectedChild };
    cluster.proxyPrimitive = proxy;
    cluster.worldReferencePoint = { 0.0f, 0.0f, -250.0f };
    cluster.worldSize = 100.0f;
    cluster.activationScreenRelativeSize = 0.5f;
    cluster.compatibilityFlags = HLODCompatibilityFlags::OpaqueOnly | HLODCompatibilityFlags::ProxySafe;

    RepresentationResidencySnapshot residency;
    residency.MarkReady(firstChild);
    residency.MarkReady(selectedChild);
    residency.MarkHLODProxyReady(proxy);

    SceneHLODViewInput input;
    input.cameraPosition = { 0.0f, 0.0f, 0.0f };
    input.allowHLOD = true;
    input.editorInspectionView = true;
    input.selectedPrimitiveHandles = { selectedChild };

    const auto result = SceneHLODSystem::SelectCluster(input, cluster, residency);

    EXPECT_TRUE(result.usesProxy);
    ASSERT_EQ(result.suppressedChildPrimitives.size(), 1u);
    EXPECT_EQ(result.suppressedChildPrimitives.front(), firstChild);
    ASSERT_EQ(result.inspectableChildPrimitives.size(), 1u);
    EXPECT_EQ(result.inspectableChildPrimitives.front(), selectedChild);
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
    const auto debugSelectionCollectorPath = root / "Project/Editor/Rendering/DebugGameObjectSelectionCollector.h";
    const std::vector<std::filesystem::path> deferredConsumerPaths = {
        root / "Project/Editor/Core/CameraController.cpp",
        root / "Project/Editor/Core/SceneViewImGuizmo.cpp"
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

    const auto debugSelectionCollectorSource = ReadSourceText(debugSelectionCollectorPath);
    ASSERT_FALSE(debugSelectionCollectorSource.empty());
    EXPECT_NE(debugSelectionCollectorSource.find("meshFilter->ResolveMesh()"), std::string::npos);
    EXPECT_EQ(debugSelectionCollectorSource.find("GetModel()"), std::string::npos);

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
    EXPECT_NE(parseSceneBody.find("renderScene.Synchronize(scene"), std::string::npos);
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

    EXPECT_NE(body.find("FindCachedTextureByEquivalentPath(textureManager, texturePath)"), std::string::npos);
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
    EXPECT_NE(renderSceneSource.find("GetMaterialPaths()"), std::string::npos)
        << "Meshes with explicit material path hints must wait for those materials instead of rendering with the white fallback.";
    EXPECT_NE(renderSceneSource.find("materialPaths[mesh.GetMaterialIndex()].empty()"), std::string::npos);
    EXPECT_NE(renderSceneSource.find("return options.defaultMaterial != nullptr && options.defaultMaterial->IsValid()"), std::string::npos)
        << "Meshes without any material path hints still use the renderer-owned fallback material.";
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
    EXPECT_NE(source.find("renderScene.Synchronize(scene"), std::string::npos);
    EXPECT_NE(source.find("renderScene.GatherVisibleCommands"), std::string::npos);
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

TEST(EditorRenderPathContractTests, SelectionOutlineMaskUsesIndexedObjectDataForLargeSelections)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto shaderSource =
        ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl");

    ASSERT_FALSE(shaderSource.empty());

    EXPECT_NE(
        shaderSource.find("StructuredBuffer<float4x4> ObjectData : register(t0, space3)"),
        std::string::npos);
    EXPECT_NE(shaderSource.find("uint instanceId : SV_InstanceID"), std::string::npos);
    EXPECT_NE(shaderSource.find("ObjectData[u_ObjectIndex + instanceId]"), std::string::npos);
    EXPECT_EQ(shaderSource.find("cbuffer ObjectConstants : register(b0, space3)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineCompositeShaderDoesNotUseIndexedObjectData)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto compositeShaderPath = root / "App/Assets/Editor/Shaders/SelectionOutlineComposite.hlsl";
    const auto compositeCorePath = root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli";
    ASSERT_TRUE(std::filesystem::is_regular_file(compositeShaderPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(compositeCorePath));
    const auto compositeShader = ReadSourceText(compositeShaderPath);
    const auto compositeCore = ReadSourceText(compositeCorePath);
    const auto rendererSource =
        ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto editorResourcesSource =
        ReadSourceText(root / "Project/Editor/Core/EditorResources.cpp");

    ASSERT_FALSE(compositeShader.empty());
    ASSERT_FALSE(compositeCore.empty());
    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(editorResourcesSource.empty());

    EXPECT_EQ(compositeShader.find("StructuredBuffer<float4x4> ObjectData"), std::string::npos)
        << "The fullscreen composite draw must not force EngineFrameObjectBindingProvider indexed object-data preparation.";
    EXPECT_EQ(compositeShader.find("ObjectData["), std::string::npos);
    EXPECT_EQ(compositeShader.find("u_ObjectIndex"), std::string::npos);
    EXPECT_NE(compositeShader.find("BuildFullscreenVertex"), std::string::npos);
    EXPECT_NE(compositeShader.find("#include \"SelectionOutlineCompositeCore.hlsli\""), std::string::npos);
    EXPECT_NE(compositeCore.find("float4 Composite(VSOutput input) : SV_Target0"), std::string::npos);
    EXPECT_NE(editorResourcesSource.find("m_shaderPaths[\"SelectionOutlineComposite\"]"), std::string::npos);
    EXPECT_NE(editorResourcesSource.find("GetShader(\"SelectionOutlineComposite\")"), std::string::npos);
    EXPECT_NE(rendererSource.find("GetShader(\"SelectionOutlineComposite\")"), std::string::npos);
    EXPECT_NE(rendererSource.find("EnsureSelectionOutlineCompositeMaterial"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineCompositeLogicHasSingleShaderSourceOfTruth)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto maskShader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl");
    const auto compositeShader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineComposite.hlsl");
    const auto compositeCore = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli");

    ASSERT_FALSE(maskShader.empty());
    ASSERT_FALSE(compositeShader.empty());
    ASSERT_FALSE(compositeCore.empty());

    EXPECT_NE(maskShader.find("#include \"SelectionOutlineCompositeCore.hlsli\""), std::string::npos)
        << "The legacy mask-shader composite path must consume the same implementation as the runtime split composite shader.";
    EXPECT_NE(compositeShader.find("#include \"SelectionOutlineCompositeCore.hlsli\""), std::string::npos);
    EXPECT_EQ(maskShader.find("struct SelectionOutlineMaskNeighborhood"), std::string::npos);
    EXPECT_EQ(compositeShader.find("struct SelectionOutlineMaskNeighborhood"), std::string::npos);
    EXPECT_NE(compositeCore.find("struct SelectionOutlineMaskNeighborhood"), std::string::npos);
    EXPECT_NE(compositeCore.find("SelectionOutlineSoftOutline ComputeSoftOutline"), std::string::npos);
    EXPECT_NE(compositeCore.find("float4 Composite(VSOutput input) : SV_Target0"), std::string::npos);
    EXPECT_NE(compositeCore.find("NLS_SELECTION_OUTLINE_COMPOSITE_CORE_HLSLI"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DebugGameObjectPathExposesNestedPerformanceScopes)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSourcePath = root / "Project/Editor/Rendering/DebugSceneRenderer.cpp";
    const auto outlineSourcePath = root / "Project/Editor/Rendering/OutlineRenderer.cpp";

    std::ifstream debugSceneStream(debugSceneSourcePath, std::ios::binary);
    const std::string debugSceneSource{
        std::istreambuf_iterator<char>(debugSceneStream),
        std::istreambuf_iterator<char>()};
    std::ifstream outlineStream(outlineSourcePath, std::ios::binary);
    const std::string outlineSource{
        std::istreambuf_iterator<char>(outlineStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(debugSceneSource.empty());
    ASSERT_FALSE(outlineSource.empty());
    EXPECT_NE(debugSceneSource.find("NLS_PROFILE_NAMED_SCOPE(\"DebugGameObject::DrawDebugElements\")"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("NLS_PROFILE_NAMED_SCOPE(\"DebugGameObject::BuildThreadedPassInput\")"), std::string::npos);
    EXPECT_NE(outlineSource.find("NLS_PROFILE_NAMED_SCOPE(\"DebugGameObject::CaptureOutlineDrawCommands\")"), std::string::npos);
    EXPECT_NE(outlineSource.find("NLS_PROFILE_NAMED_SCOPE(\"DebugGameObject::CaptureOutlineStencil\")"), std::string::npos);
    EXPECT_NE(outlineSource.find("NLS_PROFILE_NAMED_SCOPE(\"DebugGameObject::CaptureOutlineShell\")"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DebugGameObjectUsesProjectProfilerScopesForSelectedOutlineHotPath)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSource =
        ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const auto outlineSource =
        ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(debugSceneSource.empty());
    ASSERT_FALSE(outlineSource.empty());

    EXPECT_EQ(debugSceneSource.find("#include <Profiler.h>"), std::string::npos);
    EXPECT_EQ(outlineSource.find("#include <Profiler.h>"), std::string::npos);
    EXPECT_EQ(debugSceneSource.find("PROFILE_CPU_SCOPE"), std::string::npos);
    EXPECT_EQ(outlineSource.find("PROFILE_CPU_SCOPE"), std::string::npos);

    EXPECT_NE(debugSceneSource.find("NLS_PROFILE_NAMED_SCOPE(\"DebugGameObject::CollectSelectedItems\")"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("NLS_PROFILE_NAMED_SCOPE(\"DebugGameObject::BuildThreadedPassInput\")"), std::string::npos);
    EXPECT_NE(outlineSource.find("NLS_PROFILE_NAMED_SCOPE(\"SelectionOutlineMask::BuildPreparedOutput\")"), std::string::npos);
    EXPECT_NE(outlineSource.find("NLS_PROFILE_NAMED_SCOPE(\"SelectionOutlineMask::ResolveSelectionCaptureGroups\")"), std::string::npos);
    EXPECT_NE(outlineSource.find("NLS_PROFILE_NAMED_SCOPE(\"SelectionOutlineMask::CaptureMask\")"), std::string::npos);
    EXPECT_NE(outlineSource.find("NLS_PROFILE_NAMED_SCOPE(\"SelectionOutlineMask::RecordComposite\")"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DebugGameObjectDebugDrawSelectionCollectorIgnoresUnrelatedSceneObjects)
{
    NLS::Render::Resources::Mesh selectedMesh({}, {}, 0u);
    NLS::Engine::GameObject selected("Selected");
    NLS::Engine::GameObject selectedChild("SelectedChild");
    NLS::Engine::GameObject unrelated("Unrelated");
    selectedChild.SetParent(selected);

    auto* selectedChildMeshFilter = selectedChild.AddComponent<NLS::Engine::Components::MeshFilter>();
    selectedChildMeshFilter->SetMesh(&selectedMesh);
    auto* selectedChildMeshRenderer = selectedChild.AddComponent<NLS::Engine::Components::MeshRenderer>();
    auto* selectedChildCamera = selectedChild.AddComponent<NLS::Engine::Components::CameraComponent>();
    auto* selectedChildLight = selectedChild.AddComponent<NLS::Engine::Components::LightComponent>();

    unrelated.AddComponent<NLS::Engine::Components::MeshFilter>();
    unrelated.AddComponent<NLS::Engine::Components::MeshRenderer>();
    unrelated.AddComponent<NLS::Engine::Components::CameraComponent>();
    unrelated.AddComponent<NLS::Engine::Components::LightComponent>();

    NLS::Editor::Rendering::DebugGameObjectDebugDrawItems items;
    NLS::Editor::Rendering::CollectSelectedDebugGameObjectDebugDrawItems(selected, items);

    EXPECT_EQ(items.visitedGameObjects, 2u);
    ASSERT_EQ(items.selectionMeshItems.size(), 1u);
    ASSERT_EQ(items.cameras.size(), 1u);
    ASSERT_EQ(items.lights.size(), 1u);
    EXPECT_EQ(items.selectionMeshItems[0].meshRenderer, selectedChildMeshRenderer);
    EXPECT_EQ(items.selectionMeshItems[0].mesh, &selectedMesh);
    EXPECT_EQ(items.cameras[0].cameraComponent, selectedChildCamera);
    EXPECT_EQ(
        items.cameras[0].selectionClassification,
        NLS::Editor::Rendering::kSelectionOutlineChildClassification);
    EXPECT_EQ(items.lights[0], selectedChildLight);
    EXPECT_NE(selectedChildMeshFilter, nullptr);

    selectedChild.DetachFromParent();
}

TEST(EditorRenderPathContractTests, DebugGameObjectSelectionCollectorClassifiesRootAndChildCameras)
{
    NLS::Engine::GameObject selected("Selected");
    NLS::Engine::GameObject selectedChild("SelectedChild");
    selectedChild.SetParent(selected);

    auto* selectedCamera = selected.AddComponent<NLS::Engine::Components::CameraComponent>();
    auto* selectedChildCamera = selectedChild.AddComponent<NLS::Engine::Components::CameraComponent>();

    NLS::Editor::Rendering::DebugGameObjectDebugDrawItems items;
    NLS::Editor::Rendering::CollectSelectedDebugGameObjectDebugDrawItems(selected, items);

    ASSERT_EQ(items.cameras.size(), 2u);
    EXPECT_EQ(items.cameras[0].cameraComponent, selectedCamera);
    EXPECT_EQ(
        items.cameras[0].selectionClassification,
        NLS::Editor::Rendering::kSelectionOutlineParentClassification);
    EXPECT_EQ(items.cameras[1].cameraComponent, selectedChildCamera);
    EXPECT_EQ(
        items.cameras[1].selectionClassification,
        NLS::Editor::Rendering::kSelectionOutlineChildClassification);

    selectedChild.DetachFromParent();
}

TEST(EditorRenderPathContractTests, DebugGameObjectSelectionCollectorKeepsMeshItemsWhileSkippingDisabledComponents)
{
    NLS::Render::Resources::Mesh selectedMesh({}, {}, 0u);
    NLS::Engine::GameObject selected("Selected");
    NLS::Engine::GameObject selectedChild("SelectedChild");
    selectedChild.SetParent(selected);

    selectedChild.AddComponent<NLS::Engine::Components::MeshFilter>()->SetMesh(&selectedMesh);
    selectedChild.AddComponent<NLS::Engine::Components::MeshRenderer>();
    selectedChild.AddComponent<NLS::Engine::Components::CameraComponent>();
    selectedChild.AddComponent<NLS::Engine::Components::LightComponent>();

    NLS::Editor::Rendering::DebugGameObjectDebugDrawItems items;
    NLS::Editor::Rendering::CollectSelectedDebugGameObjectDebugDrawItems(
        selected,
        items,
        false,
        false);

    EXPECT_EQ(items.visitedGameObjects, 2u);
    ASSERT_EQ(items.selectionMeshItems.size(), 1u);
    EXPECT_EQ(items.selectionMeshItems[0].mesh, &selectedMesh);
    EXPECT_TRUE(items.cameras.empty());
    EXPECT_TRUE(items.lights.empty());

    selectedChild.DetachFromParent();
}

TEST(EditorRenderPathContractTests, DebugGameObjectSelectionCollectorFiltersMaskRenderEligibility)
{
    auto mesh = CreateSelectionTestMesh();
    NLS::Engine::GameObject selected("Selected");
    NLS::Engine::GameObject visible("Visible");
    NLS::Engine::GameObject disabledRenderer("DisabledRenderer");
    NLS::Engine::GameObject editorOnly("EditorOnly");
    NLS::Engine::GameObject layerHidden("LayerHidden");
    NLS::Engine::GameObject layerHiddenVisibleChild("LayerHiddenVisibleChild");
    NLS::Engine::GameObject missingMesh("MissingMesh");
    NLS::Engine::GameObject outsideFrustum("OutsideFrustum");
    NLS::Engine::GameObject cullingDisabledVisible("CullingDisabledVisible");

    visible.SetParent(selected);
    disabledRenderer.SetParent(selected);
    editorOnly.SetParent(selected);
    layerHidden.SetParent(selected);
    layerHiddenVisibleChild.SetParent(layerHidden);
    missingMesh.SetParent(selected);
    outsideFrustum.SetParent(selected);
    cullingDisabledVisible.SetParent(selected);

    auto addRenderable = [&mesh](NLS::Engine::GameObject& actor)
    {
        actor.AddComponent<NLS::Engine::Components::MeshFilter>()->SetMesh(&mesh);
        auto* renderer = actor.AddComponent<ToggleableMeshRenderer>();
        renderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
        return renderer;
    };

    auto* visibleRenderer = addRenderable(visible);
    visible.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});
    auto* disabledRendererComponent = addRenderable(disabledRenderer);
    disabledRenderer.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});
    disabledRendererComponent->SetSelfEnabledForTest(false);
    addRenderable(editorOnly);
    editorOnly.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});
    editorOnly.SetTag("EditorOnly");
    addRenderable(layerHidden);
    layerHidden.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});
    layerHidden.SetLayer(7);
    auto* layerHiddenVisibleChildRenderer = addRenderable(layerHiddenVisibleChild);
    layerHiddenVisibleChild.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});
    missingMesh.AddComponent<NLS::Engine::Components::MeshFilter>();
    missingMesh.AddComponent<ToggleableMeshRenderer>();
    addRenderable(outsideFrustum);
    outsideFrustum.GetTransform()->SetWorldPosition({250.0f, 0.0f, -6.0f});
    auto* cullingDisabledRenderer = addRenderable(cullingDisabledVisible);
    cullingDisabledRenderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
    cullingDisabledVisible.GetTransform()->SetWorldPosition({250.0f, 0.0f, -6.0f});

    NLS::Editor::Rendering::DebugGameObjectDebugDrawItems items;
    auto frustum = CreateForwardSelectionFrustum();
    NLS::Editor::Rendering::DebugGameObjectSelectionFilter filter;
    filter.visibleLayers = NLS::Engine::LayerMask(1u << 0u);
    filter.frustum = &frustum;

    NLS::Editor::Rendering::CollectSelectedDebugGameObjectDebugDrawItems(
        selected,
        items,
        true,
        true,
        filter);

    ASSERT_EQ(items.selectionMeshItems.size(), 3u);
    EXPECT_EQ(items.selectionMeshItems[0].meshRenderer, visibleRenderer);
    EXPECT_EQ(items.selectionMeshItems[1].meshRenderer, layerHiddenVisibleChildRenderer);
    EXPECT_EQ(items.selectionMeshItems[2].meshRenderer, cullingDisabledRenderer);
    EXPECT_EQ(items.visitedGameObjects, 9u);

    cullingDisabledVisible.DetachFromParent();
    outsideFrustum.DetachFromParent();
    missingMesh.DetachFromParent();
    layerHiddenVisibleChild.DetachFromParent();
    layerHidden.DetachFromParent();
    editorOnly.DetachFromParent();
    disabledRenderer.DetachFromParent();
    visible.DetachFromParent();
}

TEST(EditorRenderPathContractTests, DebugGameObjectSelectionCollectorUsesSelectionMeshItemsAsMeshSsot)
{
    const auto collectorHeader = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugGameObjectSelectionCollector.h");

    ASSERT_FALSE(collectorHeader.empty());
    EXPECT_NE(collectorHeader.find("std::vector<SelectionMeshItem> selectionMeshItems"), std::string::npos);
    EXPECT_EQ(collectorHeader.find("modelRenderers"), std::string::npos);
}

TEST(EditorRenderPathContractTests, FrameInfoLargeSceneCountersConsumeFrameInfoSnapshotOnly)
{
    const auto source = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/FrameInfoRendererStats.cpp");

    ASSERT_FALSE(source.empty());
    const auto updateBegin = source.find("void Editor::Panels::FrameInfo::UpdateForFrameInfo(");
    ASSERT_NE(updateBegin, std::string::npos);
    const auto updateBody = source.substr(updateBegin);

    EXPECT_NE(updateBody.find("frameInfo.largeScene.registeredPrimitiveCount"), std::string::npos);
    EXPECT_NE(updateBody.find("frameInfo.largeScene.visibilityTestedPrimitiveCount"), std::string::npos);
    EXPECT_NE(updateBody.find("frameInfo.largeScene.finalizationTouchedPrimitiveCount"), std::string::npos);
    EXPECT_NE(updateBody.find("frameInfo.largeScene.residentCpuBytes"), std::string::npos);
    EXPECT_EQ(updateBody.find("GetScene("), std::string::npos);
    EXPECT_EQ(updateBody.find("FastAccessComponents"), std::string::npos);
    EXPECT_EQ(updateBody.find("GetGameObjects("), std::string::npos);
    EXPECT_EQ(updateBody.find("GetMutableRendererStats"), std::string::npos);
    EXPECT_EQ(updateBody.find("GetFrameInfo()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, FrameInfoShowsCullReasonAndStreamingBudgetCountersFromSnapshot)
{
    const auto source = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/FrameInfoRendererStats.cpp");

    ASSERT_FALSE(source.empty());
    const auto updateBegin = source.find("void Editor::Panels::FrameInfo::UpdateForFrameInfo(");
    ASSERT_NE(updateBegin, std::string::npos);
    const auto updateBody = source.substr(updateBegin);

    EXPECT_NE(source.find("BuildLargeSceneCullReasonText("), std::string::npos);
    EXPECT_NE(source.find("GetCullReasonDisplayBuckets()"), std::string::npos);
    EXPECT_NE(updateBody.find("BuildLargeSceneCullReasonText(frameInfo.largeScene.culledByReason)"), std::string::npos);
    EXPECT_NE(updateBody.find("frameInfo.largeScene.occlusionCulledCount"), std::string::npos);
    EXPECT_NE(updateBody.find("frameInfo.largeScene.streamingRequestCount"), std::string::npos);
    EXPECT_NE(updateBody.find("frameInfo.largeScene.streamingCommitCount"), std::string::npos);
    EXPECT_NE(updateBody.find("frameInfo.largeScene.streamingEvictCount"), std::string::npos);
    EXPECT_EQ(updateBody.find("GetScene("), std::string::npos);
    EXPECT_EQ(updateBody.find("FastAccessComponents"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DebugSceneCullingOverlayConsumesFrameSnapshotOnly)
{
    const auto header = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.h");
    const auto source = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    EXPECT_NE(header.find("CullingOverlayOptions"), std::string::npos);
    EXPECT_NE(header.find("SetCullingOverlayOptions"), std::string::npos);
    EXPECT_NE(header.find("BuildCullingOverlayItems"), std::string::npos);
    EXPECT_NE(header.find("BuildDebugSceneCullingOverlayItems"), std::string::npos);
    EXPECT_NE(header.find("ShouldPublishCullReasonDebugSnapshots"), std::string::npos);
    EXPECT_NE(header.find("const NLS::Render::Context::FrameSnapshot& snapshot"), std::string::npos);

    const auto buildBegin = source.find("BuildDebugSceneCullingOverlayItems");
    ASSERT_NE(buildBegin, std::string::npos);
    const auto buildEnd = source.find("DebugSceneRenderer::SetCullingOverlayOptions", buildBegin);
    ASSERT_NE(buildEnd, std::string::npos);
    const auto buildBody = source.substr(buildBegin, buildEnd - buildBegin);

    EXPECT_NE(buildBody.find("snapshot.largeSceneCullReasonSnapshot"), std::string::npos);
    EXPECT_EQ(buildBody.find("FastAccessComponents"), std::string::npos);
    EXPECT_EQ(buildBody.find("GetGameObjects("), std::string::npos);
    EXPECT_EQ(buildBody.find("Synchronize("), std::string::npos);
    EXPECT_EQ(buildBody.find("Drain"), std::string::npos);

    EXPECT_NE(source.find("bool Editor::Rendering::DebugSceneRenderer::ShouldPublishCullReasonDebugSnapshots() const"), std::string::npos);
    EXPECT_NE(source.find("ShouldPublishDebugSceneCullReasonSnapshots(m_cullingOverlayOptions)"), std::string::npos);
    EXPECT_NE(source.find("return options.enabled && options.maxItems > 0u"), std::string::npos);
}

TEST(EditorRenderPathContractTests, BaseSceneRendererPublishesCullReasonSnapshotAfterVisibility)
{
    const auto source = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.cpp");

    ASSERT_FALSE(source.empty());
    const auto beginFrameStart = source.find("void BaseSceneRenderer::BeginFrame(");
    ASSERT_NE(beginFrameStart, std::string::npos);
    const auto beginFrameEnd = source.find("std::optional<Render::Context::FrameSnapshot> BaseSceneRenderer::BuildFrameSnapshot", beginFrameStart);
    ASSERT_NE(beginFrameEnd, std::string::npos);
    const auto beginFrameBody = source.substr(beginFrameStart, beginFrameEnd - beginFrameStart);
    const auto clearCullSnapshot = beginFrameBody.find("m_lastCullReasonDebugSnapshot = {}");
    const auto buildInitialSnapshot = beginFrameBody.find("BuildFrameSnapshot(p_frameDescriptor)");
    ASSERT_NE(clearCullSnapshot, std::string::npos);
    ASSERT_NE(buildInitialSnapshot, std::string::npos);
    EXPECT_LT(clearCullSnapshot, buildInitialSnapshot)
        << "The initial BeginFrame snapshot must not publish stale cull reasons from the previous parsed scene.";

    EXPECT_NE(source.find("snapshot->largeSceneCullReasonSnapshot = m_lastCullReasonDebugSnapshot"), std::string::npos);
    EXPECT_NE(source.find("SetLastCullReasonDebugSnapshot(renderScene.GetLastCullReasonDebugSnapshot())"), std::string::npos);

    const auto appendSceneBegin = source.find("auto appendSceneDrawables = [&]");
    ASSERT_NE(appendSceneBegin, std::string::npos);
    const auto gatherVisible = source.find("renderScene.GatherVisibleCommands", appendSceneBegin);
    const auto publishCullSnapshot = source.find("SetLastCullReasonDebugSnapshot(renderScene.GetLastCullReasonDebugSnapshot())", appendSceneBegin);
    ASSERT_NE(gatherVisible, std::string::npos);
    ASSERT_NE(publishCullSnapshot, std::string::npos);
    EXPECT_LT(gatherVisible, publishCullSnapshot)
        << "Cull reasons must be published after visibility has produced a renderer snapshot, not by traversing Scene View state.";
}

TEST(EditorRenderPathContractTests, DebugGameObjectDebugDrawCollectsSelectedSubtreeAfterGate)
{
    const auto source = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    const auto debugPassStart = source.find("class DebugGameObjectRenderPass");
    ASSERT_NE(debugPassStart, std::string::npos);
    const auto drawStart = source.find("virtual void Draw(Render::Data::PipelineState p_pso) override", debugPassStart);
    ASSERT_NE(drawStart, std::string::npos);
    const auto debugDrawStart = source.find("void DrawGameObjectDebugElements", drawStart);
    ASSERT_NE(debugDrawStart, std::string::npos);
    const auto frustumStart = source.find("void DrawFrustumLines", debugDrawStart);
    ASSERT_NE(frustumStart, std::string::npos);

    const auto drawBody = source.substr(drawStart, debugDrawStart - drawStart);
    const auto debugDrawBody = source.substr(debugDrawStart, frustumStart - debugDrawStart);

    const auto settingsLookup = drawBody.find("const auto& debugSettings = Editor::Settings::EditorSettings::GetDebugDrawSettingsObject()");
    const auto applySettings = drawBody.find("ApplyDebugDrawSettings(debugSettings)");
    const auto gateCheck = drawBody.find("ShouldSubmitDebugGameObjectElements(debugSettings)");
    const auto prepareItems = drawBody.find("const auto& debugDrawItems = PrepareDebugGameObjectDebugDrawItems");
    const auto debugDrawCall = drawBody.find("DrawGameObjectDebugElements(debugDrawItems, debugSettings)");

    ASSERT_NE(settingsLookup, std::string::npos);
    ASSERT_NE(prepareItems, std::string::npos);
    ASSERT_NE(applySettings, std::string::npos);
    ASSERT_NE(gateCheck, std::string::npos);
    ASSERT_NE(debugDrawCall, std::string::npos);
    EXPECT_LT(settingsLookup, prepareItems);
    EXPECT_LT(prepareItems, applySettings);
    EXPECT_LT(applySettings, gateCheck);
    EXPECT_LT(gateCheck, debugDrawCall);

    EXPECT_EQ(debugDrawBody.find("ApplyDebugDrawSettings"), std::string::npos);
    EXPECT_EQ(debugDrawBody.find("PrepareDebugGameObjectDebugDrawItems"), std::string::npos);
    EXPECT_EQ(debugDrawBody.find("scene.GetFastAccessComponents()"), std::string::npos);
    EXPECT_EQ(debugDrawBody.find("IsSelectedOrDescendant"), std::string::npos);
    EXPECT_NE(debugDrawBody.find("debugDrawItems.selectionMeshItems"), std::string::npos);
    EXPECT_NE(debugDrawBody.find("debugDrawItems.cameras"), std::string::npos);
    EXPECT_NE(debugDrawBody.find("debugDrawItems.lights"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DebugGameObjectThreadedPathDoesNotOpenEmptyOutputRenderPass)
{
    const auto source = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    const auto debugPassStart = source.find("class DebugGameObjectRenderPass");
    ASSERT_NE(debugPassStart, std::string::npos);
    const auto drawStart = source.find("virtual void Draw(Render::Data::PipelineState p_pso) override", debugPassStart);
    ASSERT_NE(drawStart, std::string::npos);
    const auto drawEnd = source.find("static bool ShouldSubmitDebugGameObjectElements", drawStart);
    ASSERT_NE(drawEnd, std::string::npos);
    const auto drawBody = source.substr(drawStart, drawEnd - drawStart);

    EXPECT_NE(
        source.find("bool ManagesOwnRenderPass() const override", debugPassStart),
        std::string::npos);
    EXPECT_NE(drawBody.find("BuildThreadedPassInput("), std::string::npos);
    EXPECT_NE(drawBody.find("BeginOutputRenderPass("), std::string::npos);
    EXPECT_NE(drawBody.find("EndOutputRenderPass(startedRenderPass)"), std::string::npos);
    EXPECT_NE(drawBody.find("m_outlineRenderer.PrepareOutlineDrawItems(debugDrawItems)"), std::string::npos);
    EXPECT_NE(drawBody.find("m_outlineRenderer.DrawPreparedOutline(kSelectedOutlineColor, kSelectedOutlineWidth)"), std::string::npos);
    EXPECT_LT(
        drawBody.find("BuildThreadedPassInput("),
        drawBody.find("BeginOutputRenderPass("));
    EXPECT_LT(
        drawBody.find("m_outlineRenderer.PrepareOutlineDrawItems(debugDrawItems)"),
        drawBody.find("BeginOutputRenderPass("));
}

TEST(EditorRenderPathContractTests, DebugGameObjectReusesSelectedTreeItemsForDebugAndOutline)
{
    const auto debugSceneSource = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const auto outlineHeader = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/OutlineRenderer.h");
    const auto collectorHeader = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugGameObjectSelectionCollector.h");

    const auto debugPassStart = debugSceneSource.find("class DebugGameObjectRenderPass");
    ASSERT_NE(debugPassStart, std::string::npos);
    const auto drawStart = debugSceneSource.find("virtual void Draw(Render::Data::PipelineState p_pso) override", debugPassStart);
    ASSERT_NE(drawStart, std::string::npos);
    const auto drawEnd = debugSceneSource.find("static bool ShouldSubmitDebugGameObjectElements", drawStart);
    ASSERT_NE(drawEnd, std::string::npos);
    const auto drawBody = debugSceneSource.substr(drawStart, drawEnd - drawStart);

    EXPECT_NE(collectorHeader.find("selectionMeshItems"), std::string::npos);
    EXPECT_NE(outlineHeader.find("const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems"), std::string::npos);
    EXPECT_NE(outlineHeader.find("bool PrepareOutlineDrawItems(const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems)"), std::string::npos);
    EXPECT_NE(outlineHeader.find("void DrawPreparedOutline(const Maths::Vector4& color, float thickness)"), std::string::npos);

    const auto prepareItems = drawBody.find("const auto& debugDrawItems = PrepareDebugGameObjectDebugDrawItems");
    const auto drawElements = drawBody.find("DrawGameObjectDebugElements(debugDrawItems, debugSettings)");
    const auto buildThreaded = drawBody.find("BuildThreadedPassInput(");
    const auto prepareOutline = drawBody.find("m_outlineRenderer.PrepareOutlineDrawItems(debugDrawItems)");
    const auto drawOutline = drawBody.find("m_outlineRenderer.DrawPreparedOutline(kSelectedOutlineColor, kSelectedOutlineWidth)");
    ASSERT_NE(prepareItems, std::string::npos);
    ASSERT_NE(drawElements, std::string::npos);
    ASSERT_NE(buildThreaded, std::string::npos);
    ASSERT_NE(prepareOutline, std::string::npos);
    ASSERT_NE(drawOutline, std::string::npos);
    EXPECT_NE(drawBody.find("debugDrawItems", buildThreaded), std::string::npos);
    EXPECT_LT(prepareItems, drawElements);
    EXPECT_LT(drawElements, buildThreaded);
    EXPECT_LT(buildThreaded, prepareOutline);
    EXPECT_LT(prepareOutline, drawOutline);

    const auto buildInputStart = debugSceneSource.find(
        "Editor::Rendering::SelectionOutlinePreparedOutput BuildThreadedPassInput",
        debugPassStart);
    ASSERT_NE(buildInputStart, std::string::npos);
    const auto buildInputEnd = debugSceneSource.find("private:", buildInputStart);
    ASSERT_NE(buildInputEnd, std::string::npos);
    const auto buildInputBody = debugSceneSource.substr(buildInputStart, buildInputEnd - buildInputStart);
    const auto captureOutline = buildInputBody.find("m_outlineRenderer.CaptureOutlineDrawCommands(");
    ASSERT_NE(captureOutline, std::string::npos);
    EXPECT_NE(buildInputBody.find("debugDrawItems", captureOutline), std::string::npos);
    EXPECT_EQ(
        buildInputBody.find("m_outlineRenderer.CaptureOutlineDrawCommands(selectedGameObject"),
        std::string::npos);
}

TEST(EditorRenderPathContractTests, DebugGameObjectSelectionCollectorKeepsCameraItemsForOutline)
{
    const auto source = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    const auto prepareItems = source.find(
        "const Editor::Rendering::DebugGameObjectDebugDrawItems& PrepareDebugGameObjectDebugDrawItems");
    ASSERT_NE(prepareItems, std::string::npos);
    const auto prepareEnd = source.find("void ApplyDebugDrawSettings", prepareItems);
    ASSERT_NE(prepareEnd, std::string::npos);
    const auto prepareBody = source.substr(prepareItems, prepareEnd - prepareItems);

    EXPECT_NE(prepareBody.find("debugSettings.debugDrawLighting"), std::string::npos);
    EXPECT_NE(prepareBody.find("true"), std::string::npos);
    EXPECT_EQ(prepareBody.find("debugSettings.debugDrawBounds"), std::string::npos);
    EXPECT_EQ(prepareBody.find("debugSettings.debugDrawCamera"), std::string::npos);
}

TEST(EditorRenderPathContractTests, OutlineCaptureReusesSelectedTreeScratchBeforePassEmission)
{
    const auto source = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/OutlineRenderer.cpp");

    const auto selectedCaptureStart = source.find(
        "void Editor::Rendering::OutlineRenderer::CaptureOutlineDrawCommands(\n    const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems");
    ASSERT_NE(selectedCaptureStart, std::string::npos);
    const auto selectedCaptureEnd = source.find("bool Editor::Rendering::OutlineRenderer::PrepareOutlineScratchItems", selectedCaptureStart);
    ASSERT_NE(selectedCaptureEnd, std::string::npos);
    const auto selectedCaptureBody = source.substr(selectedCaptureStart, selectedCaptureEnd - selectedCaptureStart);

    const auto collection = selectedCaptureBody.find("PrepareOutlineScratchItems(debugDrawItems)");
    const auto reserve = selectedCaptureBody.find("outDrawCommands.reserve(outDrawCommands.size() + m_outlineScratchItems.size() * 2u)");
    const auto stencilPass = selectedCaptureBody.find("CaptureStencilPass(m_outlineScratchItems, outDrawCommands)");
    const auto shellPass = selectedCaptureBody.find("CaptureOutlinePass(m_outlineScratchItems, color, thickness, outDrawCommands)");

    ASSERT_NE(collection, std::string::npos);
    ASSERT_NE(reserve, std::string::npos);
    ASSERT_NE(stencilPass, std::string::npos);
    ASSERT_NE(shellPass, std::string::npos);
    EXPECT_LT(collection, stencilPass);
    EXPECT_LT(reserve, stencilPass);
    EXPECT_LT(stencilPass, shellPass);

    const auto selectedPrepareStart = source.find(
        "bool Editor::Rendering::OutlineRenderer::PrepareOutlineScratchItems(\n    const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems");
    ASSERT_NE(selectedPrepareStart, std::string::npos);
    const auto selectedPrepareEnd = source.find("void Editor::Rendering::OutlineRenderer::CollectOutlineDrawItems", selectedPrepareStart);
    ASSERT_NE(selectedPrepareEnd, std::string::npos);
    const auto selectedPrepareBody = source.substr(selectedPrepareStart, selectedPrepareEnd - selectedPrepareStart);

    EXPECT_EQ(selectedPrepareBody.find("GetChildren()"), std::string::npos);
    EXPECT_EQ(selectedPrepareBody.find("ResolveMesh()"), std::string::npos);
    EXPECT_NE(selectedPrepareBody.find("debugDrawItems.selectionMeshItems"), std::string::npos);
    EXPECT_NE(selectedPrepareBody.find("debugDrawItems.cameras"), std::string::npos);

    const auto header = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/OutlineRenderer.h");
    EXPECT_NE(header.find("std::vector<OutlineDrawItem> m_outlineScratchItems"), std::string::npos);
    EXPECT_NE(header.find("std::optional<Maths::Vector4> m_lastAppliedOutlineColor"), std::string::npos);
    EXPECT_NE(header.find("void ApplyOutlineMaterialColor(const Maths::Vector4& color)"), std::string::npos);
    EXPECT_NE(header.find("bool PrepareOutlineScratchItems(Engine::GameObject& actor)"), std::string::npos);
    EXPECT_EQ(header.find("std::vector<OutlineDrawItem>& PrepareOutlineScratchItems"), std::string::npos);

    EXPECT_NE(source.find("void Editor::Rendering::OutlineRenderer::ApplyOutlineMaterialColor"), std::string::npos);
    EXPECT_NE(source.find("m_lastAppliedOutlineColor.has_value() && *m_lastAppliedOutlineColor == color"), std::string::npos);
    EXPECT_NE(source.find("m_outlineMaterial.GetParameterBlock().TryGet(\"u_Diffuse\")"), std::string::npos);
    EXPECT_NE(source.find("std::any_cast<const Maths::Vector4&>(*appliedColor) == color"), std::string::npos);
    EXPECT_NE(source.find("m_lastAppliedOutlineColor.reset()"), std::string::npos);
    EXPECT_NE(source.find("if (!m_outlineMaterial.HasShader())"), std::string::npos);
    EXPECT_EQ(source.find("m_outlineMaterial.Set(\"u_Diffuse\", color)"), source.rfind("m_outlineMaterial.Set(\"u_Diffuse\", color)"));
    EXPECT_NE(source.find("ApplyOutlineMaterialColor(color)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DebugGameObjectSelectionHelperDeclaresDepthStencilWrites)
{
    const auto source = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    const auto debugGameObjectPass = source.find("class DebugGameObjectRenderPass");
    ASSERT_NE(debugGameObjectPass, std::string::npos);
    const auto buildInputStart = source.find(
        "Editor::Rendering::SelectionOutlinePreparedOutput BuildThreadedPassInput",
        debugGameObjectPass);
    ASSERT_NE(buildInputStart, std::string::npos);
    const auto returnPassInput = source.find("maskOutput.passInputs.push_back(std::move(passInput))", buildInputStart);
    ASSERT_NE(returnPassInput, std::string::npos);
    const auto buildInputBody = source.substr(buildInputStart, returnPassInput - buildInputStart);

    const auto usesDepth = buildInputBody.find("passInput.usesDepthStencilAttachment = true");
    const auto writesDepth = buildInputBody.find("passInput.writesDepthStencilAttachment = true");
    const auto captureOutline = buildInputBody.find("m_outlineRenderer.CaptureOutlineDrawCommands");

    ASSERT_NE(usesDepth, std::string::npos);
    ASSERT_NE(writesDepth, std::string::npos);
    ASSERT_NE(captureOutline, std::string::npos);
    EXPECT_LT(usesDepth, writesDepth);
    EXPECT_LT(writesDepth, captureOutline);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskRendererFilesAndShaderAreIntegrated)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeaderPath = root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h";
    const auto rendererSourcePath = root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp";
    const auto channelTablePath = root / "Project/Editor/Rendering/SelectionOutlineMaskChannels.def";
    const auto shaderPath = root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl";
    const auto shaderChannelIncludePath = root / "App/Assets/Editor/Shaders/SelectionOutlineMaskChannels.hlsli";

    ASSERT_TRUE(std::filesystem::is_regular_file(rendererHeaderPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(rendererSourcePath));
    ASSERT_TRUE(std::filesystem::is_regular_file(channelTablePath));
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderChannelIncludePath));

    const auto rendererHeader = ReadSourceText(rendererHeaderPath);
    const auto rendererSource = ReadSourceText(rendererSourcePath);
    const auto editorResourcesSource = ReadSourceText(root / "Project/Editor/Core/EditorResources.cpp");

    EXPECT_NE(rendererHeader.find("class SelectionOutlineMaskRenderer"), std::string::npos);
    EXPECT_NE(rendererHeader.find("SelectionOutlinePreparedOutput"), std::string::npos);
    EXPECT_NE(rendererHeader.find("SelectionOutlineFallbackReason"), std::string::npos);
    EXPECT_NE(rendererSource.find("SelectionOutlineMaskChannels.def"), std::string::npos);
    EXPECT_NE(editorResourcesSource.find("m_shaderPaths[\"SelectionOutlineMask\"]"), std::string::npos);
    EXPECT_NE(editorResourcesSource.find("GetShader(\"SelectionOutlineMask\")"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskShaderTablesAreAssetLocalDependencies)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto shaderChannelTablePath = root / "App/Assets/Editor/Shaders/SelectionOutlineMaskChannels.def";
    const auto shaderPassModeTablePath = root / "App/Assets/Editor/Shaders/SelectionOutlineMaskPassModes.def";
    const auto shaderChannelIncludePath = root / "App/Assets/Editor/Shaders/SelectionOutlineMaskChannels.hlsli";
    const auto shaderPath = root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl";
    const auto projectChannelWrapperPath = root / "Project/Editor/Rendering/SelectionOutlineMaskChannels.def";
    const auto projectPassModeWrapperPath = root / "Project/Editor/Rendering/SelectionOutlineMaskPassModes.def";

    ASSERT_TRUE(std::filesystem::is_regular_file(shaderChannelTablePath))
        << "Shader include tables must live under App/Assets so shader import dependency hashing sees table edits.";
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderPassModeTablePath))
        << "Shader pass-mode tables must live under App/Assets so shader import dependency hashing sees table edits.";
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderChannelIncludePath));
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(projectChannelWrapperPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(projectPassModeWrapperPath));

    const auto shaderChannelInclude = ReadSourceText(shaderChannelIncludePath);
    const auto shader = ReadSourceText(shaderPath);
    const auto projectChannelWrapper = ReadSourceText(projectChannelWrapperPath);
    const auto projectPassModeWrapper = ReadSourceText(projectPassModeWrapperPath);

    EXPECT_NE(shaderChannelInclude.find("#include \"SelectionOutlineMaskChannels.def\""), std::string::npos);
    EXPECT_EQ(shaderChannelInclude.find("../../../../Project/Editor/Rendering/SelectionOutlineMaskChannels.def"), std::string::npos);
    EXPECT_NE(shader.find("#include \"SelectionOutlineMaskPassModes.def\""), std::string::npos);
    EXPECT_EQ(shader.find("../../../../Project/Editor/Rendering/SelectionOutlineMaskPassModes.def"), std::string::npos);
    EXPECT_NE(projectChannelWrapper.find("App/Assets/Editor/Shaders/SelectionOutlineMaskChannels.def"), std::string::npos);
    EXPECT_NE(projectPassModeWrapper.find("App/Assets/Editor/Shaders/SelectionOutlineMaskPassModes.def"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskPassNamesAndMetadataAreOrdered)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeaderPath = root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h";
    const auto rendererSourcePath = root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp";
    const auto debugSceneSourcePath = root / "Project/Editor/Rendering/DebugSceneRenderer.cpp";

    ASSERT_TRUE(std::filesystem::is_regular_file(rendererHeaderPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(rendererSourcePath));
    const auto rendererHeader = ReadSourceText(rendererHeaderPath);
    const auto rendererSource = ReadSourceText(rendererSourcePath);
    const auto debugSceneSource = ReadSourceText(debugSceneSourcePath);

    EXPECT_NE(rendererHeader.find("std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs"), std::string::npos);
    EXPECT_NE(rendererHeader.find("std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> metadata"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("std::vector<NLS::Render::Context::RenderPassCommandInput> m_preparedThreadedPassInputs"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("ConsumePreparedThreadedPassInputs()"), std::string::npos);

    std::size_t previous = 0u;
    const std::array<const char*, 2> expectedNames = {
        "SelectionOutlineMask::CaptureMask",
        "SelectionOutlineMask::Composite"
    };
    for (const char* name : expectedNames)
    {
        const auto position = rendererHeader.find(name);
        ASSERT_NE(position, std::string::npos) << name;
        EXPECT_GE(position, previous) << name;
        previous = position;
    }

    EXPECT_NE(debugSceneSource.find("selectionOutlineMetadata"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("selectionOutlinePassInputs"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("GetPreparedThreadedPassMetadata()"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("BuildDebugDeferredThreadedPassMetadata"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("BuildDebugDeferredAppendedPassInputs"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskPassKindIsClosedBeforeIndexingPassNames)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(rendererHeader.find("Count"), std::string::npos)
        << "SelectionOutlineMaskPassKind must be closed so kPassNames cannot silently drift from the enum.";
    EXPECT_NE(rendererHeader.find("SelectionOutlineMaskPassKindCount"), std::string::npos);
    EXPECT_NE(rendererHeader.find("std::array<const char*, 2> kPassNames"), std::string::npos)
        << "The pass-name table must keep an explicit initializer count so enum additions fail static_asserts until names are filled in.";
    EXPECT_NE(rendererHeader.find("static_assert(kPassNames.size() == SelectionOutlineMaskPassKindCount"), std::string::npos);
    EXPECT_NE(rendererHeader.find("SelectionOutlineMaskPassNamesAreComplete"), std::string::npos);
    EXPECT_NE(rendererHeader.find("SelectionOutlineMaskPassKindIsValid"), std::string::npos);
    EXPECT_NE(rendererHeader.find("SelectionOutlineMaskPassName("), std::string::npos);
    EXPECT_NE(rendererHeader.find("passName == nullptr || passName[0] == '\\0'"), std::string::npos);
    EXPECT_NE(rendererHeader.find("\"SelectionOutlineMask::Invalid\""), std::string::npos);
    EXPECT_STREQ(
        NLS::Editor::Rendering::SelectionOutlineMaskPassName(
            NLS::Editor::Rendering::SelectionOutlineMaskPassKind::CaptureMask),
        "SelectionOutlineMask::CaptureMask");
    EXPECT_STREQ(
        NLS::Editor::Rendering::SelectionOutlineMaskPassName(
            NLS::Editor::Rendering::SelectionOutlineMaskPassKind::Composite),
        "SelectionOutlineMask::Composite");
    EXPECT_STREQ(
        NLS::Editor::Rendering::SelectionOutlineMaskPassName(
            NLS::Editor::Rendering::SelectionOutlineMaskPassKind::Count),
        "SelectionOutlineMask::Invalid");
    EXPECT_STREQ(
        NLS::Editor::Rendering::SelectionOutlineMaskPassName(
            static_cast<NLS::Editor::Rendering::SelectionOutlineMaskPassKind>(255u)),
        "SelectionOutlineMask::Invalid");

    const auto buildPassStart = rendererSource.find("RenderPassCommandInput SelectionOutlineMaskRenderer::BuildPassInput");
    ASSERT_NE(buildPassStart, std::string::npos);
    const auto setFallbackStart = rendererSource.find("SelectionOutlineMaskRenderer::SetFallbackReason", buildPassStart);
    ASSERT_NE(setFallbackStart, std::string::npos);
    const auto buildPassBody = rendererSource.substr(buildPassStart, setFallbackStart - buildPassStart);

    EXPECT_NE(buildPassBody.find("SelectionOutlineMaskPassName(kind)"), std::string::npos);
    const auto passKindGuard = buildPassBody.find("SelectionOutlineMaskPassKindIsValid(kind)");
    const auto configureCommon = buildPassBody.find("ConfigureCommonPassInput");
    ASSERT_NE(passKindGuard, std::string::npos);
    ASSERT_NE(configureCommon, std::string::npos);
    EXPECT_LT(passKindGuard, configureCommon)
        << "Invalid pass kinds must be rejected before any debug name is written into RenderPassCommandInput.";
    EXPECT_NE(buildPassBody.find("return passInput;", passKindGuard), std::string::npos);
    EXPECT_EQ(buildPassBody.find("kPassNames[static_cast<size_t>(kind)]"), std::string::npos)
        << "BuildPassInput must not index the pass-name table before validating the enum value.";
    EXPECT_NE(buildPassBody.find("default:"), std::string::npos);
    EXPECT_NE(buildPassBody.find("NLS_ASSERT(false"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskMetadataOnlyCompositePropagatesSceneColor)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto buildMetadataStart = rendererSource.find(
        "std::vector<ThreadedRenderScenePassMetadata> SelectionOutlineMaskRenderer::BuildMetadata");
    ASSERT_NE(buildMetadataStart, std::string::npos);
    const auto buildPreparedStart = rendererSource.find(
        "SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput",
        buildMetadataStart);
    ASSERT_NE(buildPreparedStart, std::string::npos);
    const auto metadataBody = rendererSource.substr(buildMetadataStart, buildPreparedStart - buildMetadataStart);

    EXPECT_NE(metadataBody.find("entry.propagatesDepthOutput = false"), std::string::npos)
        << "Selection outline helper passes only write color targets; they must not keep the scene-depth output chain alive.";
    EXPECT_NE(metadataBody.find("SelectionOutlineMaskPassPropagatesColorOutput"), std::string::npos)
        << "Only the final composite pass writes back to Scene View color; mask capture writes private intermediates.";
    EXPECT_EQ(metadataBody.find("passInput.debugName == kPassNames"), std::string::npos)
        << "Color propagation must not depend on mutable profiler/debug names.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineLegacyFallbackMetadataPropagatesSceneOutputs)
{
    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.debugName = "EditorSelectionPass";
    passInput.drawCount = 2u;

    const auto metadata = NLS::Editor::Rendering::BuildSelectionOutlineLegacyShellMetadata(passInput);

    EXPECT_EQ(metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::Helper);
    EXPECT_EQ(metadata.role, NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper);
    EXPECT_EQ(metadata.queueType, NLS::Render::RHI::QueueType::Graphics);
    EXPECT_EQ(metadata.queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::Previous);
    EXPECT_EQ(metadata.visibleDrawCountContribution, passInput.drawCount);
    EXPECT_TRUE(metadata.propagatesColorOutput)
        << "The legacy shell writes directly into the Scene View color target; otherwise the output chain can go black.";
    EXPECT_TRUE(metadata.propagatesDepthOutput)
        << "The legacy shell also writes the depth/stencil attachment and must keep that chain alive.";
    ASSERT_NE(metadata.graphPassName, nullptr);
    EXPECT_STREQ(metadata.graphPassName, passInput.debugName.c_str());
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskValidPathDoesNotEmitLegacyShellPass)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSourcePath = root / "Project/Editor/Rendering/DebugSceneRenderer.cpp";
    const auto rendererHeaderPath = root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h";
    const auto fallbackReasonTablePath = root / "Project/Editor/Rendering/SelectionOutlineFallbackReasons.def";

    ASSERT_TRUE(std::filesystem::is_regular_file(rendererHeaderPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(fallbackReasonTablePath));
    const auto debugSceneSource = ReadSourceText(debugSceneSourcePath);
    const auto rendererHeader = ReadSourceText(rendererHeaderPath);
    const auto fallbackReasonTable = ReadSourceText(fallbackReasonTablePath);

    const auto buildInputStart = debugSceneSource.find("SelectionOutlinePreparedOutput BuildThreadedPassInput");
    ASSERT_NE(buildInputStart, std::string::npos);
    const auto buildInputEnd = debugSceneSource.find("private:", buildInputStart);
    ASSERT_NE(buildInputEnd, std::string::npos);
    const auto buildInputBody = debugSceneSource.substr(buildInputStart, buildInputEnd - buildInputStart);

    const auto maskPrepare = buildInputBody.find("m_selectionOutlineMaskRenderer.BuildPreparedOutput");
    ASSERT_NE(maskPrepare, std::string::npos);
    const auto legacyFallback = buildInputBody.find("m_outlineRenderer.CaptureOutlineDrawCommands");
    ASSERT_NE(legacyFallback, std::string::npos);
    EXPECT_LT(maskPrepare, legacyFallback);
    EXPECT_NE(buildInputBody.find("fallbackDecision.has_value()"), std::string::npos);
    EXPECT_NE(buildInputBody.find("return maskOutput"), std::string::npos);
    EXPECT_EQ(buildInputBody.find("passInput.debugName = \"EditorSelectionPass\""), std::string::npos);

    EXPECT_NE(rendererHeader.find("SelectionOutlineFallbackReasons.def"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("None"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("MissingSceneDepth"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("ZeroSizeTarget"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("AllocationFailure"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskRuntimeFailuresDoNotFallBackToLegacyShellHotPath)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto fallbackReasonTable = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineFallbackReasons.def");

    ASSERT_FALSE(debugSceneSource.empty());
    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(fallbackReasonTable.empty());

    const auto buildInputStart = debugSceneSource.find("SelectionOutlinePreparedOutput BuildThreadedPassInput");
    ASSERT_NE(buildInputStart, std::string::npos);
    const auto buildInputEnd = debugSceneSource.find("private:", buildInputStart);
    ASSERT_NE(buildInputEnd, std::string::npos);
    const auto buildInputBody = debugSceneSource.substr(buildInputStart, buildInputEnd - buildInputStart);

    EXPECT_NE(buildInputBody.find("ResolveSelectionOutlineFallbackAction"), std::string::npos);
    EXPECT_NE(buildInputBody.find("maskOutput.fallbackDecision->reason"), std::string::npos);
    EXPECT_EQ(debugSceneSource.find("bool ShouldUseLegacySelectionOutlineFallback"), std::string::npos);
    EXPECT_EQ(debugSceneSource.find("const char* SelectionOutlineFallbackReasonToString"), std::string::npos);

    EXPECT_NE(rendererHeader.find("enum class SelectionOutlineFallbackAction"), std::string::npos);
    EXPECT_NE(rendererHeader.find("ResolveSelectionOutlineFallbackAction"), std::string::npos);
    EXPECT_NE(rendererHeader.find("SelectionOutlineFallbackReasonToString"), std::string::npos);
    EXPECT_NE(rendererHeader.find("SelectionOutlineFallbackReasons.def"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("NLS_SELECTION_OUTLINE_FALLBACK_REASON(ZeroSizeTarget, SkipFrame)"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("NLS_SELECTION_OUTLINE_FALLBACK_REASON(MissingSceneDepth, SkipFrame)"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("NLS_SELECTION_OUTLINE_FALLBACK_REASON(MissingOutputColor, SkipFrame)"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("NLS_SELECTION_OUTLINE_FALLBACK_REASON(MissingShader, LegacyShell)"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("NLS_SELECTION_OUTLINE_FALLBACK_REASON(AllocationFailure, SkipFrame)"), std::string::npos)
        << "Transient resource pressure during create/select must skip the current outline frame instead of falling back to the expensive shell path.";
    EXPECT_NE(fallbackReasonTable.find("NLS_SELECTION_OUTLINE_FALLBACK_REASON(UnsupportedMaterialMask, SkipFrame)"), std::string::npos)
        << "Unsupported material semantics should not re-enable the old per-mesh inflated shell hot path in threaded Scene View.";
    EXPECT_NE(fallbackReasonTable.find("NLS_SELECTION_OUTLINE_FALLBACK_REASON(UnsupportedSampleCount, SkipFrame)"), std::string::npos)
        << "MSAA-incompatible outline paths must not fall back to legacy 1x shell rendering on MSAA attachments.";
    EXPECT_NE(fallbackReasonTable.find("NLS_SELECTION_OUTLINE_FALLBACK_REASON(StaleFrameAttachment, SkipFrame)"), std::string::npos)
        << "Stale or resized Scene View attachments must skip the outline frame instead of submitting mismatched color/depth targets.";

    EXPECT_NE(fallbackReasonTable.find("MissingShader"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("ScreenSpaceCommandCaptureFailed"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("UnsupportedBackend"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCompositeDoesNotReadAndWriteSceneOutputInOnePass)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli");

    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(shader.empty());

    const auto bindStart = rendererSource.find("bool SelectionOutlineMaskRenderer::BindScreenSpaceMaterialTextures");
    ASSERT_NE(bindStart, std::string::npos);
    const auto resetStart = rendererSource.find("void SelectionOutlineMaskRenderer::ResetResources", bindStart);
    ASSERT_NE(resetStart, std::string::npos);
    const auto bindBody = rendererSource.substr(bindStart, resetStart - bindStart);
    EXPECT_EQ(bindBody.find("frameDescriptor.outputColorView"), std::string::npos);
    EXPECT_EQ(bindBody.find("u_MainTexture"), std::string::npos)
        << "Sampling the Scene View output while writing it as the composite render target is a read/write hazard.";

    const auto compositeStart = shader.find("float4 Composite(VSOutput input)");
    ASSERT_NE(compositeStart, std::string::npos);
    const auto compositeBody = shader.substr(compositeStart);
    EXPECT_EQ(compositeBody.find("u_MainTexture.Sample"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskMaterializesExternalOutputBufferBeforeResourceValidation)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto buildOutputStart = rendererSource.find(
        "SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput");
    ASSERT_NE(buildOutputStart, std::string::npos);
    const auto captureStart = rendererSource.find(
        "void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands",
        buildOutputStart);
    ASSERT_NE(captureStart, std::string::npos);
    const auto buildOutputBody = rendererSource.substr(buildOutputStart, captureStart - buildOutputStart);

    const auto snapshot = buildOutputBody.find("CaptureExternalSceneOutputSnapshot");
    const auto texelSize = buildOutputBody.find("const auto texelSize");
    const auto prepareResources = buildOutputBody.find("PrepareResources(selectionFrameDescriptor, resources)");
    const auto buildPassInput = buildOutputBody.find("BuildPassInput(");

    ASSERT_NE(snapshot, std::string::npos);
    ASSERT_NE(texelSize, std::string::npos);
    ASSERT_NE(prepareResources, std::string::npos);
    ASSERT_NE(buildPassInput, std::string::npos);
    EXPECT_LT(snapshot, texelSize);
    EXPECT_LT(snapshot, prepareResources);
    EXPECT_LT(snapshot, buildPassInput);
    EXPECT_EQ(buildOutputBody.find("const auto& frameDescriptor = m_renderer.GetFrameDescriptor()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskPreparedBuilderCountsEveryScreenSpacePass)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    ASSERT_FALSE(debugSceneSource.empty());

    const auto builderStart = debugSceneSource.find(
        "PreparedRenderSceneBuilder Editor::Rendering::DebugSceneRenderer::BuildPreparedRenderSceneBuilder");
    ASSERT_NE(builderStart, std::string::npos);
    const auto consumedStart = debugSceneSource.find(
        "auto consumedGridPassInput",
        builderStart);
    ASSERT_NE(consumedStart, std::string::npos);
    const auto builderPrefix = debugSceneSource.substr(builderStart, consumedStart - builderStart);

    EXPECT_NE(builderPrefix.find("selectionOutlineMetadata.size()"), std::string::npos)
        << "Screen-space selection outline emits a dynamic helper pass vector, so aggregate helper accounting must not keep the legacy two-pass constant.";
    EXPECT_EQ(builderPrefix.find("selectionOutlinePassInputs.empty() ? 0u : 2u"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskLegacyFallbackDiagnosticsExposeReason)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    ASSERT_FALSE(debugSceneSource.empty());

    const auto buildInputStart = debugSceneSource.find("SelectionOutlinePreparedOutput BuildThreadedPassInput");
    ASSERT_NE(buildInputStart, std::string::npos);
    const auto buildInputEnd = debugSceneSource.find("private:", buildInputStart);
    ASSERT_NE(buildInputEnd, std::string::npos);
    const auto buildInputBody = debugSceneSource.substr(buildInputStart, buildInputEnd - buildInputStart);

    EXPECT_NE(buildInputBody.find("ShouldLogEditorHelperDiagnostics(m_renderer.GetDriver())"), std::string::npos);
    EXPECT_NE(buildInputBody.find("Selection outline screen-space fallback reason="), std::string::npos);
    EXPECT_NE(buildInputBody.find("SelectionOutlineFallbackReasonToString"), std::string::npos);
    EXPECT_NE(buildInputBody.find("selectedItems="), std::string::npos);
    EXPECT_NE(buildInputBody.find("extent/sample-count compatible"), std::string::npos);
    EXPECT_EQ(buildInputBody.find("not sample-count compatible"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskChannelsUseSharedSourceOfTruth)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto channelTablePath = root / "App/Assets/Editor/Shaders/SelectionOutlineMaskChannels.def";
    const auto projectChannelWrapperPath = root / "Project/Editor/Rendering/SelectionOutlineMaskChannels.def";
    const auto rendererHeaderPath = root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h";
    const auto shaderIncludePath = root / "App/Assets/Editor/Shaders/SelectionOutlineMaskChannels.hlsli";
    const auto shaderPath = root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl";

    ASSERT_TRUE(std::filesystem::is_regular_file(channelTablePath));
    ASSERT_TRUE(std::filesystem::is_regular_file(projectChannelWrapperPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(rendererHeaderPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderIncludePath));
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderPath));

    const auto channelTable = ReadSourceText(channelTablePath);
    const auto rendererHeader = ReadSourceText(rendererHeaderPath);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto shaderInclude = ReadSourceText(shaderIncludePath);
    const auto shader = ReadSourceText(shaderPath);
    const auto compositeCore = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli");
    const auto projectChannelWrapper = ReadSourceText(projectChannelWrapperPath);

    const std::array<const char*, 4> channels = {
        "GroupId",
        "Visible",
        "Selected",
        "Classification"
    };
    for (const char* channel : channels)
    {
        EXPECT_NE(channelTable.find(channel), std::string::npos) << channel;
    }

    EXPECT_NE(rendererHeader.find("#include \"Rendering/SelectionOutlineMaskChannels.def\""), std::string::npos);
    EXPECT_NE(rendererHeader.find("SelectionOutlineMaskChannelDesc"), std::string::npos);
    EXPECT_NE(rendererSource.find("SelectionOutlineMaskChannels.def"), std::string::npos);
    EXPECT_NE(projectChannelWrapper.find("App/Assets/Editor/Shaders/SelectionOutlineMaskChannels.def"), std::string::npos);
    EXPECT_NE(shaderInclude.find("#define NLS_SELECTION_OUTLINE_MASK_CHANNEL"), std::string::npos);
    EXPECT_NE(shaderInclude.find("SelectionOutlineMask##name##Index"), std::string::npos);
    EXPECT_NE(shaderInclude.find("SelectionOutlineMaskGet##name"), std::string::npos);
    EXPECT_NE(shaderInclude.find("SelectionOutlineMaskSet##name"), std::string::npos);
    EXPECT_NE(shaderInclude.find("#include \"SelectionOutlineMaskChannels.def\""), std::string::npos);
    EXPECT_EQ(shaderInclude.find("../../../../Project/Editor/Rendering/SelectionOutlineMaskChannels.def"), std::string::npos);
    EXPECT_EQ(shaderInclude.find("// GroupId r 0"), std::string::npos);
    EXPECT_EQ(shaderInclude.find("#define SELECTION_OUTLINE_MASK_GROUP_SWIZZLE r"), std::string::npos);
    EXPECT_NE(shader.find("#include \"SelectionOutlineMaskChannels.hlsli\""), std::string::npos);
    EXPECT_NE(compositeCore.find("SelectionOutlineMaskGetGroupId"), std::string::npos);
    EXPECT_NE(shader.find("SelectionOutlineMaskSetGroupId"), std::string::npos);
    EXPECT_EQ(shader.find("SELECTION_OUTLINE_MASK_GROUP_SWIZZLE"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskChannelsKeepVisibleAndSelectedDistinct)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto channelTablePath = root / "App/Assets/Editor/Shaders/SelectionOutlineMaskChannels.def";
    const auto shaderIncludePath = root / "App/Assets/Editor/Shaders/SelectionOutlineMaskChannels.hlsli";

    ASSERT_TRUE(std::filesystem::is_regular_file(channelTablePath));
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderIncludePath));

    const auto channelTable = ReadSourceText(channelTablePath);
    const auto shaderInclude = ReadSourceText(shaderIncludePath);
    const auto channels = ParseSelectionMaskChannels(channelTable);

    ASSERT_TRUE(channels.contains("GroupId"));
    ASSERT_TRUE(channels.contains("Visible"));
    ASSERT_TRUE(channels.contains("Selected"));
    ASSERT_TRUE(channels.contains("Classification"));
    ASSERT_FALSE(channels.contains("Occluded"))
        << "Occlusion is derived as selected coverage minus visible coverage; it must not alias the visible channel.";

    EXPECT_EQ(channels.at("GroupId").swizzle, "r");
    EXPECT_EQ(channels.at("Visible").swizzle, "g");
    EXPECT_EQ(channels.at("Selected").swizzle, "b");
    EXPECT_EQ(channels.at("Classification").swizzle, "a");
    EXPECT_NE(channels.at("Visible").index, channels.at("Selected").index);
    EXPECT_NE(channels.at("Visible").swizzle, channels.at("Selected").swizzle);

    EXPECT_NE(shaderInclude.find("SelectionOutlineMask##name##Index"), std::string::npos);
    EXPECT_NE(shaderInclude.find("float SelectionOutlineMaskGet##name(float4 value)"), std::string::npos);
    EXPECT_NE(shaderInclude.find("void SelectionOutlineMaskSet##name(inout float4 value, float channelValue)"), std::string::npos);
    EXPECT_EQ(shaderInclude.find("#define SELECTION_OUTLINE_MASK_GROUP_INDEX 0"), std::string::npos);
    EXPECT_EQ(shaderInclude.find("#define SELECTION_OUTLINE_MASK_VISIBLE_INDEX 1"), std::string::npos);
    EXPECT_EQ(shaderInclude.find("#define SELECTION_OUTLINE_MASK_SELECTED_INDEX 2"), std::string::npos);
    EXPECT_EQ(shaderInclude.find("#define SELECTION_OUTLINE_MASK_CLASSIFICATION_INDEX 3"), std::string::npos);
    EXPECT_EQ(shaderInclude.find("SELECTION_OUTLINE_MASK_OCCLUDED_SWIZZLE"), std::string::npos);
    EXPECT_EQ(shaderInclude.find("SELECTION_OUTLINE_MASK_OCCLUDED_INDEX"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskTablesExpandToExpectedHlslConstants)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto channelTable = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMaskChannels.def");
    const auto passModeTable = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMaskPassModes.def");

    ASSERT_FALSE(channelTable.empty());
    ASSERT_FALSE(passModeTable.empty());

    const auto channelConstants =
        ParseHlslStaticIntConstants(ExpandSelectionOutlineChannelIndexConstantsForTest(channelTable));
    const auto passModeConstants =
        ParseHlslStaticIntConstants(ExpandSelectionOutlinePassModeConstantsForTest(passModeTable));

    ASSERT_EQ(channelConstants.size(), 4u);
    EXPECT_EQ(channelConstants.at("SelectionOutlineMaskGroupIdIndex"), 0);
    EXPECT_EQ(channelConstants.at("SelectionOutlineMaskVisibleIndex"), 1);
    EXPECT_EQ(channelConstants.at("SelectionOutlineMaskSelectedIndex"), 2);
    EXPECT_EQ(channelConstants.at("SelectionOutlineMaskClassificationIndex"), 3);

    ASSERT_EQ(passModeConstants.size(), 3u);
    EXPECT_EQ(passModeConstants.at("SelectionOutlinePassModeCaptureVisible"), 0);
    EXPECT_EQ(passModeConstants.at("SelectionOutlinePassModeCaptureOccluded"), 1);
    EXPECT_EQ(passModeConstants.at("SelectionOutlinePassModeComposite"), 2);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskMetadataPropagatesColorByPassOrderNotDebugName)
{
    EXPECT_FALSE(NLS::Editor::Rendering::SelectionOutlineMaskPassPropagatesColorOutput(
        NLS::Editor::Rendering::SelectionOutlineMaskPassKind::CaptureMask));
    EXPECT_TRUE(NLS::Editor::Rendering::SelectionOutlineMaskPassPropagatesColorOutput(
        NLS::Editor::Rendering::SelectionOutlineMaskPassKind::Composite))
        << "Color-output propagation is a frame-graph semantic; it must not depend on mutable profiler/debug names.";

    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());
    const auto buildMetadataStart = rendererSource.find(
        "std::vector<ThreadedRenderScenePassMetadata> SelectionOutlineMaskRenderer::BuildMetadata");
    ASSERT_NE(buildMetadataStart, std::string::npos);
    const auto buildPreparedStart = rendererSource.find(
        "SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput",
        buildMetadataStart);
    ASSERT_NE(buildPreparedStart, std::string::npos);
    const auto metadataBody = rendererSource.substr(buildMetadataStart, buildPreparedStart - buildMetadataStart);

    EXPECT_NE(metadataBody.find("for (size_t passIndex = 0u; passIndex < passInputs.size(); ++passIndex)"), std::string::npos);
    EXPECT_NE(metadataBody.find("SelectionOutlineMaskPassPropagatesColorOutput"), std::string::npos);
    EXPECT_EQ(metadataBody.find("passInput.debugName == kPassNames"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskMetadataPropagatesColorForCompositeOnlyCacheReuse)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(rendererHeader.find("std::vector<SelectionOutlineMaskPassKind> passKinds"), std::string::npos)
        << "Metadata must carry the semantic pass kind; cached-mask frames may emit only Composite.";
    EXPECT_NE(rendererSource.find("passKinds[passIndex]"), std::string::npos);

    const auto buildMetadataStart = rendererSource.find(
        "std::vector<ThreadedRenderScenePassMetadata> SelectionOutlineMaskRenderer::BuildMetadata");
    ASSERT_NE(buildMetadataStart, std::string::npos);
    const auto buildPreparedStart = rendererSource.find(
        "SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput",
        buildMetadataStart);
    ASSERT_NE(buildPreparedStart, std::string::npos);
    const auto metadataBody = rendererSource.substr(buildMetadataStart, buildPreparedStart - buildMetadataStart);
    EXPECT_EQ(metadataBody.find("static_cast<SelectionOutlineMaskPassKind>(passIndex)"), std::string::npos)
        << "Composite-only reuse must not be misclassified as CaptureMask by vector index.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskShaderMatchesUnityStylePassSemantics)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto shaderPath = root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl";
    const auto compositeCorePath = root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli";

    ASSERT_TRUE(std::filesystem::is_regular_file(shaderPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(compositeCorePath));
    const auto shader = ReadSourceText(shaderPath);
    const auto compositeCore = ReadSourceText(compositeCorePath);

    EXPECT_NE(shader.find("AlphaClip"), std::string::npos);
    EXPECT_NE(shader.find("CaptureMaskVisible"), std::string::npos);
    EXPECT_NE(shader.find("CaptureMaskOccluded"), std::string::npos);
    EXPECT_NE(compositeCore.find("ComputeIdEdge"), std::string::npos);
    EXPECT_EQ(shader.find("EdgeBlurHorizontal"), std::string::npos);
    EXPECT_EQ(shader.find("BlurVertical"), std::string::npos)
        << "Selection outline post-processing is fused into the final composite pass to reduce RHI helper-pass pressure.";
    EXPECT_NE(shader.find("Composite"), std::string::npos);
    EXPECT_NE(shader.find("u_ObjectId"), std::string::npos);
    EXPECT_NE(compositeCore.find("occlusion"), std::string::npos);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    ASSERT_FALSE(rendererSource.empty());
    EXPECT_NE(rendererSource.find("SelectionOutlineFallbackReason::UnsupportedMaterialMask"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskScreenSpacePassesCannotFakeSuccessWithEmptyDraws)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());
    EXPECT_NE(rendererSource.find("HasCompleteScreenSpacePasses"), std::string::npos);
    EXPECT_NE(rendererSource.find("SelectionOutlineFallbackReason::ScreenSpaceCommandCaptureFailed"), std::string::npos);
    EXPECT_NE(rendererSource.find("SetFallbackReason(SelectionOutlineFallbackReason::ScreenSpaceCommandCaptureFailed)"), std::string::npos);

    const auto completeCheck = rendererSource.find("HasCompleteScreenSpacePasses");
    const auto setFallback = rendererSource.find("SelectionOutlineFallbackReason::ScreenSpaceCommandCaptureFailed", completeCheck);
    const auto buildMetadata = rendererSource.find("output.metadata = BuildMetadata(output.passInputs, output.passKinds)", completeCheck);
    ASSERT_NE(completeCheck, std::string::npos);
    ASSERT_NE(setFallback, std::string::npos);
    ASSERT_NE(buildMetadata, std::string::npos);
    EXPECT_LT(setFallback, buildMetadata);

    const auto emptyCapture = rendererSource.find("if (maskDrawCommands.empty())");
    ASSERT_NE(emptyCapture, std::string::npos);
    const auto emptyCaptureBodyEnd = rendererSource.find("output.passKinds.push_back(SelectionOutlineMaskPassKind::CaptureMask)", emptyCapture);
    ASSERT_NE(emptyCaptureBodyEnd, std::string::npos);
    const auto emptyCaptureBody = rendererSource.substr(emptyCapture, emptyCaptureBodyEnd - emptyCapture);
    EXPECT_NE(emptyCaptureBody.find("SetFallbackReason(SelectionOutlineFallbackReason::ScreenSpaceCommandCaptureFailed)"), std::string::npos);
    EXPECT_NE(emptyCaptureBody.find("output.fallbackDecision = { m_lastFallbackReason, output.selectedItemCount }"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskPreparedOutputReservesBeforeAppendingPasses)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto buildPreparedStart = rendererSource.find(
        "SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput");
    ASSERT_NE(buildPreparedStart, std::string::npos);
    const auto captureStart = rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands", buildPreparedStart);
    ASSERT_NE(captureStart, std::string::npos);
    const auto buildPreparedBody = rendererSource.substr(buildPreparedStart, captureStart - buildPreparedStart);

    const auto reservePassInputs = buildPreparedBody.find("output.passInputs.reserve(kPassNames.size())");
    const auto reservePassKinds = buildPreparedBody.find("output.passKinds.reserve(kPassNames.size())");
    const auto firstPassInputPush = buildPreparedBody.find("output.passInputs.push_back");
    const auto firstPassKindPush = buildPreparedBody.find("output.passKinds.push_back");
    ASSERT_NE(reservePassInputs, std::string::npos);
    ASSERT_NE(reservePassKinds, std::string::npos);
    ASSERT_NE(firstPassInputPush, std::string::npos);
    ASSERT_NE(firstPassKindPush, std::string::npos);

    EXPECT_LT(reservePassInputs, firstPassInputPush)
        << "The hot path should reserve before the optional CaptureMask append, otherwise the first push may allocate.";
    EXPECT_LT(reservePassKinds, firstPassKindPush)
        << "Pass-kind metadata should mirror pass-input capacity from the start of assembly.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskResourceIdentityReportsMissingViewTexturesPrecisely)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto prepareStart = rendererSource.find("bool SelectionOutlineMaskRenderer::PrepareResources");
    ASSERT_NE(prepareStart, std::string::npos);
    const auto bindStart = rendererSource.find("void SelectionOutlineMaskRenderer::InvalidateSelectionOutlineTextureBindings", prepareStart);
    ASSERT_NE(bindStart, std::string::npos);
    const auto prepareBody = rendererSource.substr(prepareStart, bindStart - prepareStart);

    const auto missingDepthTexture = prepareBody.find("frameDescriptor.outputDepthStencilView->GetTexture() == nullptr");
    const auto missingOutputTexture = prepareBody.find("frameDescriptor.outputColorView->GetTexture() == nullptr");
    ASSERT_NE(missingDepthTexture, std::string::npos);
    ASSERT_NE(missingOutputTexture, std::string::npos);

    const auto depthReason = prepareBody.find("SelectionOutlineFallbackReason::MissingSceneDepth", missingDepthTexture);
    const auto outputReason = prepareBody.find("SelectionOutlineFallbackReason::MissingOutputColor", missingOutputTexture);
    ASSERT_NE(depthReason, std::string::npos);
    ASSERT_NE(outputReason, std::string::npos);
    EXPECT_LT(depthReason, missingOutputTexture);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskUsesDeferredGBufferDepthForValidationAndCapture)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(debugSceneSource.empty());
    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(debugSceneSource.find("ResolveDeferredSelectionOutlineDepthView"), std::string::npos);
    const auto buildInputStart = debugSceneSource.find("SelectionOutlinePreparedOutput BuildThreadedPassInput");
    ASSERT_NE(buildInputStart, std::string::npos);
    const auto buildInputEnd = debugSceneSource.find("private:", buildInputStart);
    ASSERT_NE(buildInputEnd, std::string::npos);
    const auto buildInputBody = debugSceneSource.substr(buildInputStart, buildInputEnd - buildInputStart);
    EXPECT_NE(buildInputBody.find("selectionOutlineDepthView"), std::string::npos);
    EXPECT_NE(buildInputBody.find("BuildPreparedOutput("), std::string::npos);
    EXPECT_NE(buildInputBody.find("selectionOutlineDepthView)"), std::string::npos);

    EXPECT_NE(rendererHeader.find("sceneDepthViewOverride"), std::string::npos);
    EXPECT_NE(rendererSource.find("sceneDepthViewOverride"), std::string::npos);
    EXPECT_NE(rendererSource.find("selectionFrameDescriptor.outputDepthStencilView = sceneDepthViewOverride"), std::string::npos);
    EXPECT_NE(rendererSource.find("selectionFrameDescriptor.outputDepthStencilTexture = sceneDepthViewOverride->GetTexture()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskRequiresDeferredGBufferDepthBeforePreparingScreenSpacePath)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    ASSERT_FALSE(debugSceneSource.empty());

    const auto buildInputStart = debugSceneSource.find("SelectionOutlinePreparedOutput BuildThreadedPassInput");
    ASSERT_NE(buildInputStart, std::string::npos);
    const auto buildInputEnd = debugSceneSource.find("private:", buildInputStart);
    ASSERT_NE(buildInputEnd, std::string::npos);
    const auto buildInputBody = debugSceneSource.substr(buildInputStart, buildInputEnd - buildInputStart);

    const auto depthResolve = buildInputBody.find("const auto selectionOutlineDepthView = ResolveDeferredSelectionOutlineDepthView(m_renderer)");
    const auto missingDepth = buildInputBody.find("selectionOutlineDepthView == nullptr", depthResolve);
    const auto buildPreparedOutput = buildInputBody.find("m_selectionOutlineMaskRenderer.BuildPreparedOutput", depthResolve);
    ASSERT_NE(depthResolve, std::string::npos);
    ASSERT_NE(missingDepth, std::string::npos);
    ASSERT_NE(buildPreparedOutput, std::string::npos);
    EXPECT_LT(missingDepth, buildPreparedOutput)
        << "Deferred threaded selection outline must not validate or capture against stale external depth when the prepared GBuffer depth is unavailable.";

    const auto missingDepthReturn = buildInputBody.find("return missingDepthOutput", missingDepth);
    ASSERT_NE(missingDepthReturn, std::string::npos);
    const auto missingDepthBlock = buildInputBody.substr(missingDepth, missingDepthReturn - missingDepth);
    EXPECT_NE(missingDepthBlock.find("SelectionOutlineFallbackReason::MissingSceneDepth"), std::string::npos);
    EXPECT_NE(missingDepthBlock.find("releaseSelectionOwnedPreparedFrameReservation()"), std::string::npos);
    EXPECT_EQ(missingDepthBlock.find("TryReservePreparedFrameResources"), std::string::npos)
        << "Missing-depth paths must not wait for a prepared-frame object-data slot before knowing a screen-space capture is possible.";
    EXPECT_EQ(missingDepthBlock.find("SelectionOutlineFallbackAction::LegacyShell"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskPassInputMovesRecordedMaskCommands)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(
        rendererHeader.find("std::vector<NLS::Render::Context::RecordedDrawCommandInput>&& maskDrawCommands"),
        std::string::npos);

    const auto buildOutputStart = rendererSource.find(
        "SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput");
    ASSERT_NE(buildOutputStart, std::string::npos);
    const auto captureStart = rendererSource.find(
        "void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands",
        buildOutputStart);
    ASSERT_NE(captureStart, std::string::npos);
    const auto buildOutputBody = rendererSource.substr(buildOutputStart, captureStart - buildOutputStart);
    EXPECT_NE(buildOutputBody.find("std::move(maskDrawCommands)"), std::string::npos);

    const auto buildPassStart = rendererSource.find("RenderPassCommandInput SelectionOutlineMaskRenderer::BuildPassInput");
    ASSERT_NE(buildPassStart, std::string::npos);
    const auto setFallbackStart = rendererSource.find("SelectionOutlineMaskRenderer::SetFallbackReason", buildPassStart);
    ASSERT_NE(setFallbackStart, std::string::npos);
    const auto buildPassBody = rendererSource.substr(buildPassStart, setFallbackStart - buildPassStart);
    EXPECT_NE(buildPassBody.find("passInput.recordedDrawCommands = std::move(maskDrawCommands)"), std::string::npos);
    EXPECT_EQ(buildPassBody.find("passInput.recordedDrawCommands = maskDrawCommands"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskFullscreenMaterialsSampleIntermediateTextures)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(shader.empty());
    EXPECT_NE(rendererHeader.find("std::unique_ptr<NLS::Render::Resources::Texture2D> m_maskTexture"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("std::unique_ptr<NLS::Render::Resources::Texture2D> m_edgeBlurHorizontalTexture"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("std::unique_ptr<NLS::Render::Resources::Texture2D> m_edgeTexture"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("std::unique_ptr<NLS::Render::Resources::Texture2D> m_blurHorizontalTexture"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("std::unique_ptr<NLS::Render::Resources::Texture2D> m_blurVerticalTexture"), std::string::npos);

    EXPECT_NE(rendererSource.find("Texture2D::WrapExternal"), std::string::npos);
    EXPECT_NE(rendererSource.find("WrapExternalInPlace"), std::string::npos);
    EXPECT_NE(rendererSource.find("BindScreenSpaceMaterialTextures"), std::string::npos);
    EXPECT_NE(rendererSource.find("SetMaterialTextureIfChanged(m_compositeMaterial, \"u_SelectionOutlineMask\", m_maskTexture.get())"), std::string::npos);
    EXPECT_EQ(rendererSource.find("SetMaterialTextureIfChanged(m_compositeMaterial, \"u_SelectionOutlineBlur\""), std::string::npos);
    EXPECT_EQ(shader.find("u_SelectionOutlineBlur"), std::string::npos);
    EXPECT_EQ(shader.find("u_SelectionOutlineEdge"), std::string::npos);

    for (const std::string textureName : {
        "u_SelectionOutlineMask",
        "u_MainTexture"
    })
    {
        ASSERT_NE(shader.find("Texture2D " + textureName), std::string::npos) << textureName;
        EXPECT_NE(rendererSource.find("SetMaterialTextureIfChanged(material, \"" + textureName + "\""), std::string::npos)
            << textureName << " must be bound on the material path so explicit binding creation does not fail and force legacy outline fallback.";
    }
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCaptureShaderSelectsPerPassModeAndCompositeUsesSplitShader)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl");
    const auto compositeShader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineComposite.hlsl");
    const auto passModeTable = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMaskPassModes.def");
    const auto projectPassModeWrapper = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskPassModes.def");

    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(shader.empty());
    ASSERT_FALSE(compositeShader.empty());
    ASSERT_FALSE(passModeTable.empty());
    EXPECT_NE(shader.find("u_SelectionOutlinePassMode"), std::string::npos);
    EXPECT_EQ(compositeShader.find("u_SelectionOutlinePassMode"), std::string::npos);
    EXPECT_NE(shader.find("SelectionOutlineMaskPassModes.def"), std::string::npos);
    EXPECT_EQ(compositeShader.find("SelectionOutlineMaskPassModes.def"), std::string::npos);
    EXPECT_NE(rendererSource.find("SelectionOutlineMaskPassModes.def"), std::string::npos);
    EXPECT_NE(projectPassModeWrapper.find("App/Assets/Editor/Shaders/SelectionOutlineMaskPassModes.def"), std::string::npos);
    EXPECT_NE(passModeTable.find("CaptureVisible"), std::string::npos);
    EXPECT_NE(passModeTable.find("CaptureOccluded"), std::string::npos);
    EXPECT_EQ(passModeTable.find("EdgeBlurHorizontal"), std::string::npos);
    EXPECT_EQ(passModeTable.find("DetectIdEdges"), std::string::npos);
    EXPECT_EQ(passModeTable.find("NLS_SELECTION_OUTLINE_MASK_PASS_MODE(BlurHorizontal"), std::string::npos);
    EXPECT_EQ(passModeTable.find("BlurVertical"), std::string::npos);
    EXPECT_NE(passModeTable.find("Composite"), std::string::npos);
    EXPECT_NE(shader.find("if (u_SelectionOutlinePassMode == SelectionOutlinePassModeCaptureOccluded)"), std::string::npos);
    EXPECT_EQ(shader.find("if (u_SelectionOutlinePassMode == SelectionOutlinePassModeEdgeBlurHorizontal)"), std::string::npos);
    EXPECT_NE(shader.find("return CaptureMaskOccluded(input)"), std::string::npos);
    EXPECT_EQ(shader.find("return EdgeBlurHorizontal(input)"), std::string::npos);
    EXPECT_EQ(shader.find("return BlurVertical(input)"), std::string::npos);
    EXPECT_NE(compositeShader.find("return Composite(input)"), std::string::npos);

    std::istringstream passModeStream(passModeTable);
    std::string passModeLine;
    std::regex passModePattern(R"(NLS_SELECTION_OUTLINE_MASK_PASS_MODE\((\w+),\s*(-?\d+)\))");
    size_t parsedPassModeCount = 0u;
    while (std::getline(passModeStream, passModeLine))
    {
        std::smatch match;
        if (!std::regex_search(passModeLine, match, passModePattern))
            continue;

        const auto constantName = "SelectionOutlinePassMode" + match[1].str();
        const auto expectedValue = std::stoi(match[2].str());
        EXPECT_NE(shader.find("SelectionOutlinePassMode##name = value"), std::string::npos)
            << "HLSL must define pass-mode constants from the shared table macro.";
        if (constantName != "SelectionOutlinePassModeComposite")
            EXPECT_NE(rendererSource.find(constantName), std::string::npos) << constantName;
        EXPECT_NE(passModeLine.find(std::to_string(expectedValue)), std::string::npos) << constantName;
        ++parsedPassModeCount;
    }
    EXPECT_EQ(parsedPassModeCount, 3u);

    EXPECT_NE(rendererSource.find("u_SelectionOutlinePassMode"), std::string::npos);
    EXPECT_NE(rendererHeader.find("NLS::Render::Resources::Material m_maskVisibleMaterial"), std::string::npos);
    EXPECT_NE(rendererHeader.find("NLS::Render::Resources::Material m_maskOccludedMaterial"), std::string::npos);
    EXPECT_NE(rendererSource.find("SetSelectionMaskPassModeIfChanged(m_maskVisibleMaterial, \"u_SelectionOutlinePassMode\", SelectionOutlinePassModeCaptureVisible)"), std::string::npos);
    EXPECT_NE(rendererSource.find("SetSelectionMaskPassModeIfChanged(m_maskOccludedMaterial, \"u_SelectionOutlinePassMode\", SelectionOutlinePassModeCaptureOccluded)"), std::string::npos);
    EXPECT_EQ(rendererSource.find("SetSelectionMaskPassModeIfChanged(m_edgeBlurHorizontalMaterial, \"u_SelectionOutlinePassMode\""), std::string::npos);
    EXPECT_EQ(rendererSource.find("SetSelectionMaskPassModeIfChanged(m_blurVerticalMaterial, \"u_SelectionOutlinePassMode\""), std::string::npos);
    EXPECT_EQ(rendererSource.find("SetSelectionMaskPassModeIfChanged(m_compositeMaterial, \"u_SelectionOutlinePassMode\""), std::string::npos);
    EXPECT_NE(rendererSource.find("EnsureSelectionOutlineCompositeMaterial"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCompositeBlendsOverSceneColor)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli");

    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(shader.empty());

    const auto constructorStart = rendererSource.find("SelectionOutlineMaskRenderer::SelectionOutlineMaskRenderer");
    ASSERT_NE(constructorStart, std::string::npos);
    const auto metadataStart = rendererSource.find("SelectionOutlineMaskRenderer::BuildMetadata", constructorStart);
    ASSERT_NE(metadataStart, std::string::npos);
    const auto constructorBody = rendererSource.substr(constructorStart, metadataStart - constructorStart);

    EXPECT_NE(constructorBody.find("m_compositeMaterial.SetBlendable(true)"), std::string::npos);
    EXPECT_EQ(constructorBody.find("m_maskVisibleMaterial.SetBlendable(true)"), std::string::npos);
    EXPECT_EQ(constructorBody.find("m_maskOccludedMaterial.SetBlendable(true)"), std::string::npos);
    EXPECT_EQ(constructorBody.find("m_edgeBlurHorizontalMaterial"), std::string::npos);
    EXPECT_EQ(constructorBody.find("m_blurVerticalMaterial.SetBlendable(true)"), std::string::npos);
    EXPECT_NE(rendererSource.find("CreateSelectionCompositePipelineState"), std::string::npos);
    EXPECT_NE(rendererSource.find("compositeOverrides.hasDepthAttachment = false"), std::string::npos);
    EXPECT_NE(rendererSource.find("CaptureRecordedDrawCommand"), std::string::npos);
    EXPECT_NE(rendererSource.find("compositeOverrides"), std::string::npos);
    EXPECT_NE(shader.find("const float outlineAlpha = saturate(softOutline.outline)"), std::string::npos);
    EXPECT_NE(shader.find("return float4(color.rgb, outlineAlpha)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCompositeForcesBlendedRecordedPipeline)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto materialHeader = ReadSourceText(root / "Runtime/Rendering/Resources/Material.h");
    const auto materialSource = ReadSourceText(root / "Runtime/Rendering/Resources/Material.cpp");
    const auto materialVariantKeySource = ReadSourceText(root / "Runtime/Rendering/Resources/MaterialVariantKey.cpp");

    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(materialHeader.empty());
    ASSERT_FALSE(materialSource.empty());
    ASSERT_FALSE(materialVariantKeySource.empty());

    EXPECT_NE(materialHeader.find("std::optional<bool> blending"), std::string::npos)
        << "Recorded helper passes need an explicit blend override because pipeline state application overwrites material blend state.";
    EXPECT_NE(materialSource.find("if (overrides.blending.has_value())"), std::string::npos);
    EXPECT_NE(materialVariantKeySource.find("overrideBlending"), std::string::npos);

    const auto recordFullscreenStart = rendererSource.find("void SelectionOutlineMaskRenderer::RecordFullscreenCommand");
    ASSERT_NE(recordFullscreenStart, std::string::npos);
    const auto buildResourceStart = rendererSource.find("SelectionOutlineMaskRenderer::BuildResourceIdentity", recordFullscreenStart);
    ASSERT_NE(buildResourceStart, std::string::npos);
    const auto recordFullscreenBody = rendererSource.substr(recordFullscreenStart, buildResourceStart - recordFullscreenStart);

    EXPECT_NE(recordFullscreenBody.find("compositeOverrides.blending = true"), std::string::npos)
        << "The fullscreen composite must alpha-blend over the existing Scene View color target.";
    EXPECT_NE(recordFullscreenBody.find("compositeOverrides.SetColorFormats"), std::string::npos)
        << "The recorded composite pipeline must match the actual Scene View output color format.";
    EXPECT_NE(recordFullscreenBody.find("frameDescriptor.outputColorView->GetDesc().format"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCompositeRecordsDepthlessPipeline)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto recordFullscreenStart = rendererSource.find("void SelectionOutlineMaskRenderer::RecordFullscreenCommand");
    ASSERT_NE(recordFullscreenStart, std::string::npos);
    const auto buildResourceStart = rendererSource.find("SelectionOutlineMaskRenderer::BuildResourceIdentity", recordFullscreenStart);
    ASSERT_NE(buildResourceStart, std::string::npos);
    const auto recordFullscreenBody = rendererSource.substr(recordFullscreenStart, buildResourceStart - recordFullscreenStart);

    EXPECT_NE(recordFullscreenBody.find("MaterialPipelineStateOverrides compositeOverrides"), std::string::npos);
    EXPECT_NE(recordFullscreenBody.find("compositeOverrides.hasDepthAttachment = false"), std::string::npos);
    EXPECT_NE(recordFullscreenBody.find("compositeOverrides.depthTest = false"), std::string::npos);
    EXPECT_NE(recordFullscreenBody.find("compositeOverrides.depthWrite = false"), std::string::npos);
    EXPECT_NE(recordFullscreenBody.find("CaptureRecordedDrawCommand"), std::string::npos);
    EXPECT_NE(recordFullscreenBody.find("compositeOverrides"), std::string::npos);
    EXPECT_EQ(recordFullscreenBody.find("CaptureRecordedDrawCommand(pso, drawable"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCompositeDiscardsTransparentPixelsBeforeBlend)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli");

    ASSERT_FALSE(shader.empty());

    const auto compositeStart = shader.find("float4 Composite(VSOutput input)");
    ASSERT_NE(compositeStart, std::string::npos);
    const auto compositeBody = shader.substr(compositeStart);

    EXPECT_NE(compositeBody.find("const float outlineAlpha = saturate(softOutline.outline)"), std::string::npos);
    EXPECT_NE(compositeBody.find("clip(outlineAlpha - 0.001f)"), std::string::npos)
        << "The fullscreen composite draw must not write transparent pixels over the Scene View color target.";
    EXPECT_NE(compositeBody.find("return float4(color.rgb, outlineAlpha"), std::string::npos);
    EXPECT_EQ(compositeBody.find("return float4(color.rgb, saturate(outline)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCompositeDerivesOcclusionFromSelectedMinusVisible)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli");

    ASSERT_FALSE(shader.empty());

    const auto softOutlineStart = shader.find("SelectionOutlineSoftOutline ComputeSoftOutline");
    ASSERT_NE(softOutlineStart, std::string::npos);
    const auto compositeStart = shader.find("float4 Composite(VSOutput input)");
    ASSERT_NE(compositeStart, std::string::npos);
    const auto softOutlineBody = shader.substr(softOutlineStart, compositeStart - softOutlineStart);
    EXPECT_NE(shader.find("SelectionOutlineMaskGetSelected(sourceMask)"), std::string::npos);
    EXPECT_NE(shader.find("SelectionOutlineMaskGetVisible(sourceMask)"), std::string::npos);
    EXPECT_NE(
        shader.find("saturate(SelectionOutlineMaskGetSelected(sourceMask) - SelectionOutlineMaskGetVisible(sourceMask))"),
        std::string::npos);
    EXPECT_EQ(softOutlineBody.find("SELECTION_OUTLINE_MASK_OCCLUDED_SWIZZLE"), std::string::npos);

    const auto compositeBody = shader.substr(compositeStart);

    EXPECT_NE(compositeBody.find("const SelectionOutlineSoftOutline softOutline = ComputeSoftOutline(uv)"), std::string::npos);
    EXPECT_NE(compositeBody.find("softOutline.occlusionFade"), std::string::npos);
    EXPECT_EQ(compositeBody.find("const float selected ="), std::string::npos)
        << "Composite must fade from the edge-source mask samples, not only from the current pixel.";
    EXPECT_EQ(compositeBody.find("SELECTION_OUTLINE_MASK_OCCLUDED_SWIZZLE"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskBindsTexelSizeForCompositeEdgeFilter)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli");
    const auto compositeShader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineComposite.hlsl");

    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(shader.empty());
    ASSERT_FALSE(compositeShader.empty());

    EXPECT_NE(compositeShader.find("float4 u_TexelSize"), std::string::npos);
    EXPECT_NE(shader.find("u_TexelSize.x"), std::string::npos);
    EXPECT_NE(shader.find("u_TexelSize.y"), std::string::npos);

    EXPECT_NE(rendererSource.find("const auto texelSize = Maths::Vector4("), std::string::npos);
    EXPECT_EQ(CountOccurrences(rendererSource, "\"u_TexelSize\", texelSize"), 3u);
    EXPECT_EQ(rendererSource.find("SetMaterialValueIfChanged(m_edgeBlurHorizontalMaterial, \"u_TexelSize\", texelSize)"), std::string::npos);
    EXPECT_NE(rendererSource.find("SetMaterialValueIfChanged(m_compositeMaterial, \"u_TexelSize\", texelSize)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCompositeReusesCachedNeighborhoodSamples)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli");

    ASSERT_FALSE(shader.empty());

    EXPECT_NE(shader.find("struct SelectionOutlineMaskNeighborhood"), std::string::npos);
    EXPECT_NE(shader.find("SelectionOutlineMaskNeighborhood BuildSelectionOutlineMaskNeighborhood(float2 uv)"), std::string::npos);
    EXPECT_NE(shader.find("ComputeIdEdgeFromMasks("), std::string::npos);

    const auto neighborhoodStart = shader.find("SelectionOutlineMaskNeighborhood BuildSelectionOutlineMaskNeighborhood(float2 uv)");
    ASSERT_NE(neighborhoodStart, std::string::npos);
    const auto chooseEdgeSourceStart = shader.find("float4 SelectionOutlineMaskChooseEdgeSource", neighborhoodStart);
    ASSERT_NE(chooseEdgeSourceStart, std::string::npos);
    const auto neighborhoodBody = shader.substr(neighborhoodStart, chooseEdgeSourceStart - neighborhoodStart);

    EXPECT_EQ(CountOccurrences(neighborhoodBody, "u_SelectionOutlineMask.Sample"), 13u);
    const std::map<std::string, std::string> expectedNeighborhoodSamples = {
        { "center", "uv)" },
        { "left", "uv + float2(-texel.x, 0.0f))" },
        { "right", "uv + float2(texel.x, 0.0f))" },
        { "up", "uv + float2(0.0f, -texel.y))" },
        { "down", "uv + float2(0.0f, texel.y))" },
        { "left2", "uv + float2(-2.0f * texel.x, 0.0f))" },
        { "right2", "uv + float2(2.0f * texel.x, 0.0f))" },
        { "up2", "uv + float2(0.0f, -2.0f * texel.y))" },
        { "down2", "uv + float2(0.0f, 2.0f * texel.y))" },
        { "leftUp", "uv + float2(-texel.x, -texel.y))" },
        { "leftDown", "uv + float2(-texel.x, texel.y))" },
        { "rightUp", "uv + float2(texel.x, -texel.y))" },
        { "rightDown", "uv + float2(texel.x, texel.y))" }
    };
    std::vector<std::string> sampleOffsets;
    for (const auto& [field, offset] : expectedNeighborhoodSamples)
    {
        const auto assignment = std::string("neighborhood.") + field + " = u_SelectionOutlineMask.Sample(u_LinearClampSampler, " + offset;
        EXPECT_NE(neighborhoodBody.find(assignment), std::string::npos)
            << field;
        sampleOffsets.push_back(offset);
    }
    std::sort(sampleOffsets.begin(), sampleOffsets.end());
    EXPECT_EQ(std::unique(sampleOffsets.begin(), sampleOffsets.end()), sampleOffsets.end())
        << "The cached neighborhood must keep 13 unique center/cardinal/diagonal/two-texel UV offsets.";

    const auto softOutlineStart = shader.find("SelectionOutlineSoftOutline ComputeSoftOutline");
    ASSERT_NE(softOutlineStart, std::string::npos);
    const auto compositeStart = shader.find("float4 Composite(VSOutput input)", softOutlineStart);
    ASSERT_NE(compositeStart, std::string::npos);
    const auto softOutlineBody = shader.substr(softOutlineStart, compositeStart - softOutlineStart);

    EXPECT_NE(
        softOutlineBody.find("const SelectionOutlineMaskNeighborhood neighborhood = BuildSelectionOutlineMaskNeighborhood(uv)"),
        std::string::npos);
    EXPECT_EQ(softOutlineBody.find("ComputeIdEdge(uv"), std::string::npos)
        << "The fused composite pass should not multiply mask texture fetches by nesting five edge-filter calls.";
    EXPECT_EQ(softOutlineBody.find("u_SelectionOutlineMask.Sample"), std::string::npos)
        << "Mask samples should be loaded once in BuildSelectionOutlineMaskNeighborhood and reused by the soft outline filter.";
    EXPECT_EQ(CountOccurrences(softOutlineBody, "ComputeIdEdgeFromMasks("), 5u);
    EXPECT_NE(
        softOutlineBody.find(
            "ComputeIdEdgeFromMasks(\n"
            "            neighborhood.center,\n"
            "            neighborhood.left,\n"
            "            neighborhood.right,\n"
            "            neighborhood.up,\n"
            "            neighborhood.down),\n"
            "        0.36f)"),
        std::string::npos);
    EXPECT_NE(
        softOutlineBody.find(
            "ComputeIdEdgeFromMasks(\n"
            "            neighborhood.right,\n"
            "            neighborhood.center,\n"
            "            neighborhood.right2,\n"
            "            neighborhood.rightUp,\n"
            "            neighborhood.rightDown),\n"
            "        0.16f)"),
        std::string::npos);
    EXPECT_NE(
        softOutlineBody.find(
            "ComputeIdEdgeFromMasks(\n"
            "            neighborhood.left,\n"
            "            neighborhood.left2,\n"
            "            neighborhood.center,\n"
            "            neighborhood.leftUp,\n"
            "            neighborhood.leftDown),\n"
            "        0.16f)"),
        std::string::npos);
    EXPECT_NE(
        softOutlineBody.find(
            "ComputeIdEdgeFromMasks(\n"
            "            neighborhood.down,\n"
            "            neighborhood.leftDown,\n"
            "            neighborhood.rightDown,\n"
            "            neighborhood.center,\n"
            "            neighborhood.down2),\n"
            "        0.16f)"),
        std::string::npos);
    EXPECT_NE(
        softOutlineBody.find(
            "ComputeIdEdgeFromMasks(\n"
            "            neighborhood.up,\n"
            "            neighborhood.leftUp,\n"
            "            neighborhood.rightUp,\n"
            "            neighborhood.up2,\n"
            "            neighborhood.center),\n"
            "        0.16f)"),
        std::string::npos);
    EXPECT_EQ(shader.find("SelectionOutlineEdgeSample ComputeIdEdge(float2 uv)"), std::string::npos)
        << "Keep the old five-tap edge helper removed so future shader edits cannot accidentally rebuild the full 13-sample neighborhood per offset.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCaptureWritesNonZeroObjectId)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto collectorHeader = ReadSourceText(root / "Project/Editor/Rendering/DebugGameObjectSelectionCollector.h");
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl");

    ASSERT_FALSE(collectorHeader.empty());
    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(shader.empty());

    EXPECT_NE(shader.find("SelectionOutlineMaskSetGroupId(mask, u_ObjectId)"), std::string::npos);
    EXPECT_NE(collectorHeader.find("float selectionGroupId"), std::string::npos);
    EXPECT_NE(collectorHeader.find("float selectionClassification"), std::string::npos);
    EXPECT_NE(collectorHeader.find("kSelectionOutlineParentClassification"), std::string::npos);
    EXPECT_NE(collectorHeader.find("kSelectionOutlineChildClassification"), std::string::npos);
    EXPECT_NE(collectorHeader.find("actorDepth == 0u"), std::string::npos);
    EXPECT_NE(rendererSource.find("ApplySelectionMaskGroupParameters"), std::string::npos);
    EXPECT_NE(rendererSource.find("group.selectionGroupId"), std::string::npos);
    EXPECT_NE(rendererSource.find("group.selectionClassification"), std::string::npos);
    EXPECT_EQ(rendererSource.find("SetMaterialValueIfChanged(material, \"u_ObjectId\", item.selectionGroupId)"), std::string::npos);
    EXPECT_EQ(rendererSource.find("constexpr float kSelectionOutlineDefaultObjectId"), std::string::npos);
    EXPECT_EQ(rendererSource.find("constexpr float kSelectionOutlineParentClassification"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskIntermediatePassesDisableBlendingAndCompositeOnlyBlends)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto visiblePsoStart = rendererSource.find("CreateSelectionMaskVisiblePipelineState");
    ASSERT_NE(visiblePsoStart, std::string::npos);
    const auto occludedPsoStart = rendererSource.find("CreateSelectionMaskOccludedPipelineState", visiblePsoStart);
    ASSERT_NE(occludedPsoStart, std::string::npos);
    const auto fullscreenPsoStart = rendererSource.find("CreateSelectionFullscreenPipelineState", occludedPsoStart);
    ASSERT_NE(fullscreenPsoStart, std::string::npos);
    const auto compositePsoStart = rendererSource.find("CreateSelectionCompositePipelineState", fullscreenPsoStart);
    ASSERT_NE(compositePsoStart, std::string::npos);
    const auto resourceIdentityStart = rendererSource.find("bool SameViewDesc", compositePsoStart);
    ASSERT_NE(resourceIdentityStart, std::string::npos);

    const auto visiblePsoBody = rendererSource.substr(visiblePsoStart, occludedPsoStart - visiblePsoStart);
    const auto occludedPsoBody = rendererSource.substr(occludedPsoStart, fullscreenPsoStart - occludedPsoStart);
    const auto fullscreenPsoBody = rendererSource.substr(fullscreenPsoStart, compositePsoStart - fullscreenPsoStart);
    const auto compositePsoBody = rendererSource.substr(compositePsoStart, resourceIdentityStart - compositePsoStart);

    EXPECT_NE(visiblePsoBody.find("pso.blending = false"), std::string::npos);
    EXPECT_NE(occludedPsoBody.find("pso.blending = false"), std::string::npos);
    EXPECT_NE(fullscreenPsoBody.find("pso.blending = false"), std::string::npos);
    EXPECT_NE(compositePsoBody.find("pso.blending = true"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCaptureBatchesSelectedMeshesWithObjectDataInstances)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(shader.empty());

    EXPECT_NE(rendererHeader.find("struct SelectionMaskCaptureGroup"), std::string::npos);
    EXPECT_NE(rendererSource.find("BuildSelectionMaskCaptureGroups"), std::string::npos);
    EXPECT_NE(rendererSource.find("AddSelectionMaskCaptureGroup"), std::string::npos);
    EXPECT_NE(rendererHeader.find("CaptureMaskDrawCommandsForGroups"), std::string::npos);
    EXPECT_NE(rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommandsForGroups"), std::string::npos);
    EXPECT_NE(rendererHeader.find("const std::vector<SelectionMaskCaptureGroup>& groups"), std::string::npos);

    EXPECT_NE(rendererSource.find("group.instanceModelMatrices"), std::string::npos);
    EXPECT_NE(rendererSource.find("drawable.instanceCount = static_cast<uint32_t>(group.instanceModelMatrices.size())"), std::string::npos);
    EXPECT_NE(rendererSource.find("descriptor.objectCount = drawable.instanceCount"), std::string::npos);
    EXPECT_NE(rendererSource.find("descriptor.instanceModelMatrices = group.instanceModelMatrices"), std::string::npos);
    EXPECT_NE(rendererSource.find("drawable.AddDescriptor(NLS::Engine::Rendering::EngineDrawableDescriptor{"), std::string::npos);

    const auto captureForGroupsStart = rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommandsForGroups");
    ASSERT_NE(captureForGroupsStart, std::string::npos);
    const auto fullscreenStart = rendererSource.find("void SelectionOutlineMaskRenderer::EnsureFullscreenResources", captureForGroupsStart);
    ASSERT_NE(fullscreenStart, std::string::npos);
    const auto captureForGroupsBody = rendererSource.substr(captureForGroupsStart, fullscreenStart - captureForGroupsStart);
    EXPECT_EQ(captureForGroupsBody.find("debugDrawItems.selectionMeshItems"), std::string::npos)
        << "Mask capture should consume prebuilt groups instead of recording one draw per selected item.";
    EXPECT_EQ(captureForGroupsBody.find("BuildSelectionMaskCaptureGroups"), std::string::npos)
        << "Visible and occluded mask passes must reuse one selected-tree grouping per frame.";
    EXPECT_EQ(rendererSource.find("ApplySelectionMaskItemParameters"), std::string::npos);
    EXPECT_NE(shader.find("ObjectData[u_ObjectIndex + instanceId]"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskReusesStableMaskWithoutCachingFrameDrawBindings)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(rendererHeader.find("struct MaskReuseSignature"), std::string::npos);
    EXPECT_NE(rendererHeader.find("m_cachedMaskSignature"), std::string::npos);
    EXPECT_NE(rendererSource.find("BuildMaskReuseSignature"), std::string::npos);
    EXPECT_NE(rendererSource.find("CanReuseCachedMask"), std::string::npos);
    EXPECT_NE(rendererSource.find("MarkCachedMaskPending"), std::string::npos);
    EXPECT_NE(rendererSource.find("CommitPendingCachedMask"), std::string::npos);
    EXPECT_NE(rendererSource.find("DiscardPendingCachedMask"), std::string::npos);
    EXPECT_NE(rendererSource.find("m_cachedMaskSignature.reset()"), std::string::npos);
    EXPECT_NE(rendererSource.find("signature->selectedItemCount > kSelectionOutlineDualDepthCaptureMaxItems"), std::string::npos)
        << "Small selections use depth-refined visibility and must recapture when scene depth changes.";
    EXPECT_EQ(rendererSource.find("GetLatestPublishedThreadedFrameId() + 1u"), std::string::npos)
        << "The cache target must use the actual published frame id rather than a global next-frame guess.";
    EXPECT_EQ(rendererHeader.find("GetLatestPublishedThreadedFrameId"), std::string::npos)
        << "Published-frame telemetry should not remain as dead cache-target helper state.";
    EXPECT_NE(rendererSource.find("m_cachedMaskRetirementTarget = publishedFrameId"), std::string::npos)
        << "The cached mask becomes reusable only after the actual frame that contains CaptureMask has retired.";
    EXPECT_NE(rendererSource.find("GetLatestRetiredThreadedFrameId() >= m_cachedMaskRetirementTarget"), std::string::npos)
        << "Cached-mask reuse must wait until the capture frame has retired, not just been prepared.";
    EXPECT_NE(rendererHeader.find("meshContentRevisionHash"), std::string::npos);
    EXPECT_NE(rendererSource.find("HashCombine(selectedTreeHash, group.mesh->GetContentRevision())"), std::string::npos)
        << "A stable Mesh pointer can still change geometry through reload/update; stale masks must be invalidated.";
    EXPECT_EQ(rendererSource.find("GetLatestPublishedThreadedFrameId() >"), std::string::npos);

    const auto buildPreparedStart = rendererSource.find(
        "SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput");
    ASSERT_NE(buildPreparedStart, std::string::npos);
    const auto captureStart = rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands", buildPreparedStart);
    ASSERT_NE(captureStart, std::string::npos);
    const auto buildPreparedBody = rendererSource.substr(buildPreparedStart, captureStart - buildPreparedStart);

    const auto reuseCheck = buildPreparedBody.find("CanReuseCachedMask");
    const auto captureCommands = buildPreparedBody.find("CaptureMaskDrawCommands");
    ASSERT_NE(reuseCheck, std::string::npos);
    ASSERT_NE(captureCommands, std::string::npos);
    EXPECT_LT(reuseCheck, captureCommands);
    EXPECT_NE(buildPreparedBody.find("SelectionOutlineMaskPassKind::Composite", reuseCheck), std::string::npos)
        << "Stable-mask frames should skip the expensive capture pass and emit only composite.";
    EXPECT_NE(buildPreparedBody.find("MarkCachedMaskPending", captureCommands), std::string::npos);

    EXPECT_EQ(rendererHeader.find("std::vector<NLS::Render::Context::RecordedDrawCommandInput> m_cachedMaskDrawCommands"), std::string::npos)
        << "Recorded draw commands carry per-frame binding sets/object indices and must not be cached across frames.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskReusesPreparedCaptureGroupsBeforeRebuildingThem)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(rendererHeader.find("struct SelectionSourceSignature"), std::string::npos);
    EXPECT_NE(rendererHeader.find("struct PreparedSelectionCaptureGroups"), std::string::npos);
    EXPECT_NE(rendererHeader.find("m_cachedSelectionSourceSignature"), std::string::npos);
    EXPECT_NE(rendererHeader.find("m_cachedSelectionCaptureGroups"), std::string::npos);
    EXPECT_NE(rendererHeader.find("m_cachedSelectionMaskReuseSignature"), std::string::npos);
    EXPECT_NE(rendererHeader.find("ResolveSelectionCaptureGroups"), std::string::npos);

    EXPECT_NE(rendererSource.find("BuildSelectionSourceSignature"), std::string::npos);
    EXPECT_NE(rendererSource.find("ResolveSelectionCaptureGroups"), std::string::npos);
    EXPECT_NE(rendererSource.find("m_cachedSelectionCaptureGroups.groups"), std::string::npos);
    EXPECT_NE(rendererSource.find("m_cachedSelectionMaskReuseSignature"), std::string::npos);
    EXPECT_NE(rendererSource.find("cameraMesh->GetContentRevision()"), std::string::npos)
        << "Camera helper meshes participate in the selected-tree mask and must invalidate prepared groups when reloaded.";
    EXPECT_NE(rendererHeader.find("hasCameraMesh"), std::string::npos);
    EXPECT_NE(rendererHeader.find("cameraMeshContentRevision"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("RecordedDrawCommandInput> m_cached"), std::string::npos)
        << "The prepared cache may keep selection grouping only; per-frame recorded bindings must still be captured fresh.";

    const auto buildPreparedStart = rendererSource.find(
        "SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput");
    ASSERT_NE(buildPreparedStart, std::string::npos);
    const auto captureStart = rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands", buildPreparedStart);
    ASSERT_NE(captureStart, std::string::npos);
    const auto buildPreparedBody = rendererSource.substr(buildPreparedStart, captureStart - buildPreparedStart);

    EXPECT_NE(buildPreparedBody.find("BuildSelectionSourceSignature"), std::string::npos);
    EXPECT_NE(buildPreparedBody.find("ResolveSelectionCaptureGroups"), std::string::npos);
    const auto resolvePrepared = buildPreparedBody.find("ResolveSelectionCaptureGroups");
    const auto buildGroups = buildPreparedBody.find("BuildSelectionMaskCaptureGroups");
    ASSERT_NE(resolvePrepared, std::string::npos);
    EXPECT_EQ(buildGroups, std::string::npos)
        << "Stable selections should hit a prepared-group cache instead of rebuilding hash groups in BuildPreparedOutput.";
    EXPECT_NE(buildPreparedBody.find("preparedSelection.maskReuseSignature"), std::string::npos);
    EXPECT_NE(buildPreparedBody.find("CanReuseCachedMask(maskReuseSignature)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCacheCommitsOnlyAfterSuccessfulPublish)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto baseRendererHeader = ReadSourceText(root / "Runtime/Rendering/Core/ABaseRenderer.h");
    const auto baseRendererSource = ReadSourceText(root / "Runtime/Rendering/Core/ABaseRenderer.cpp");
    const auto compositeSource = ReadSourceText(root / "Runtime/Rendering/Core/CompositeRenderer.cpp");
    const auto debugSceneHeader = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.h");
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(baseRendererHeader.empty());
    ASSERT_FALSE(baseRendererSource.empty());
    ASSERT_FALSE(compositeSource.empty());
    ASSERT_FALSE(debugSceneHeader.empty());
    ASSERT_FALSE(debugSceneSource.empty());
    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(baseRendererHeader.find("OnThreadedFramePublished(uint64_t publishedFrameId)"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("uint64_t publishedFrameId = 0u"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("&publishedFrameId"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("OnThreadedFramePublished(publishedFrameId)"), std::string::npos);

    EXPECT_NE(debugSceneHeader.find("OnThreadedFramePublished(uint64_t publishedFrameId)"), std::string::npos);
    EXPECT_NE(debugSceneHeader.find("OnThreadedFramePublishFailed()"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("CommitPendingSelectionOutlineMaskCache(publishedFrameId)"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("DiscardPendingSelectionOutlineMaskCache()"), std::string::npos);

    EXPECT_NE(rendererHeader.find("CommitPendingCachedMask(uint64_t publishedFrameId)"), std::string::npos);
    EXPECT_NE(rendererHeader.find("DiscardPendingCachedMask()"), std::string::npos);
    EXPECT_NE(rendererHeader.find("m_pendingCachedMaskSignature"), std::string::npos);
    EXPECT_NE(rendererSource.find("m_cachedMaskSignature = m_pendingCachedMaskSignature"), std::string::npos);
    EXPECT_NE(rendererSource.find("m_cachedMaskRetirementTarget = publishedFrameId"), std::string::npos);
    EXPECT_NE(compositeSource.find("CompositeRenderer::OnThreadedFramePublishFailed()"), std::string::npos);
    EXPECT_NE(compositeSource.find("ABaseRenderer::OnThreadedFramePublishFailed()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskPreservesRetirementTargetForIdenticalRecaptures)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(rendererHeader.find("ShouldKeepCachedMaskRetirementTarget"), std::string::npos)
        << "Repeated identical captures must not keep moving the cache retirement target forward forever.";
    EXPECT_NE(rendererHeader.find("m_keepCachedMaskRetirementTargetForPending"), std::string::npos);
    EXPECT_NE(rendererSource.find("bool SelectionOutlineMaskRenderer::ShouldKeepCachedMaskRetirementTarget"), std::string::npos);

    const auto keepStart = rendererSource.find("bool SelectionOutlineMaskRenderer::ShouldKeepCachedMaskRetirementTarget");
    ASSERT_NE(keepStart, std::string::npos);
    const auto markStart = rendererSource.find("void SelectionOutlineMaskRenderer::MarkCachedMaskPending", keepStart);
    ASSERT_NE(markStart, std::string::npos);
    const auto keepBody = rendererSource.substr(keepStart, markStart - keepStart);
    EXPECT_NE(keepBody.find("signature->Matches(*m_cachedMaskSignature)"), std::string::npos);
    EXPECT_NE(keepBody.find("GetLatestRetiredThreadedFrameId() < m_cachedMaskRetirementTarget"), std::string::npos);
    EXPECT_NE(keepBody.find("GetLatestFailedRetiredThreadedFrameId() < m_cachedMaskRetirementTarget"), std::string::npos)
        << "A failed later capture must break preservation so a new successful target is required.";

    const auto discardStart = rendererSource.find("void SelectionOutlineMaskRenderer::DiscardPendingCachedMask", markStart);
    ASSERT_NE(discardStart, std::string::npos);
    const auto markBody = rendererSource.substr(markStart, discardStart - markStart);
    const auto keepCall = markBody.find("ShouldKeepCachedMaskRetirementTarget(signature)");
    const auto assignPending = markBody.find("m_pendingCachedMaskSignature = signature");
    ASSERT_NE(keepCall, std::string::npos);
    ASSERT_NE(assignPending, std::string::npos);
    EXPECT_LT(keepCall, assignPending)
        << "The old retirement target must be preserved before staging a newer identical pending target.";

    const auto commitStart = rendererSource.find("void SelectionOutlineMaskRenderer::CommitPendingCachedMask", markStart);
    ASSERT_NE(commitStart, std::string::npos);
    ASSERT_LT(commitStart, discardStart);
    const auto commitBody = rendererSource.substr(commitStart, discardStart - commitStart);
    EXPECT_NE(commitBody.find("m_keepCachedMaskRetirementTargetForPending"), std::string::npos);
    EXPECT_NE(commitBody.find("keepRetirementTarget"), std::string::npos);
    EXPECT_NE(commitBody.find("m_cachedMaskRetirementTarget = publishedFrameId"), std::string::npos);
    EXPECT_NE(commitBody.find("!keepRetirementTarget"), std::string::npos)
        << "A repeated identical capture should update the signature but keep the older capture target until it retires or fails.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskRejectsFailedRetiredCacheTarget)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto lifecycleHeader = ReadSourceText(root / "Runtime/Rendering/Context/ThreadedRenderingLifecycle.h");
    const auto lifecycleSource = ReadSourceText(root / "Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp");
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(lifecycleHeader.empty());
    ASSERT_FALSE(lifecycleSource.empty());
    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(lifecycleHeader.find("latestFailedRetiredFrameId"), std::string::npos);
    EXPECT_NE(lifecycleHeader.find("m_latestFailedRetiredFrameId"), std::string::npos);
    EXPECT_NE(lifecycleSource.find("m_latestFailedRetiredFrameId = std::max("), std::string::npos)
        << "The lifecycle must retain the exact failed retired frame id for GPU-produced cache validation.";
    EXPECT_NE(lifecycleSource.find("m_telemetry.latestFailedRetiredFrameId = m_latestFailedRetiredFrameId"), std::string::npos);

    EXPECT_NE(rendererHeader.find("GetLatestFailedRetiredThreadedFrameId()"), std::string::npos);
    EXPECT_NE(rendererSource.find("uint64_t SelectionOutlineMaskRenderer::GetLatestFailedRetiredThreadedFrameId() const"), std::string::npos);
    EXPECT_NE(rendererSource.find("GetLatestFailedRetiredThreadedFrameId() < m_cachedMaskRetirementTarget"), std::string::npos)
        << "A later unrelated successful retirement must not validate a mask captured by a failed frame.";
}

TEST(EditorRenderPathContractTests, RhiFailurePathsPoisonSelectionMaskRetirement)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rhiSource = ReadSourceText(root / "Runtime/Rendering/Context/RhiThreadCoordinator.cpp");

    ASSERT_FALSE(rhiSource.empty());

    const auto serialRecordStart = rhiSource.find("void Detail::RecordThreadedRhiWork");
    ASSERT_NE(serialRecordStart, std::string::npos);
    const auto executeStart = rhiSource.find("void Detail::ExecuteThreadedSubmitPlan", serialRecordStart);
    ASSERT_NE(executeStart, std::string::npos);
    const auto serialRecordBody = rhiSource.substr(serialRecordStart, executeStart - serialRecordStart);

    const auto serialNoDispatch = serialRecordBody.find("No dispatches recorded but commands were expected");
    ASSERT_NE(serialNoDispatch, std::string::npos);
    EXPECT_NE(
        serialRecordBody.find("MarkCommandRecordingFailure", serialNoDispatch),
        std::string::npos)
        << "Serial compute passes with expected commands but zero dispatches must retire as failed.";

    const auto serialNoDraw = serialRecordBody.find("No draws recorded but commands were expected");
    ASSERT_NE(serialNoDraw, std::string::npos);
    EXPECT_NE(
        serialRecordBody.find("MarkCommandRecordingFailure", serialNoDraw),
        std::string::npos)
        << "External-output serial draw failures must poison the frame so cached selection masks are not reused.";

    const auto translateStart = rhiSource.find("TranslatedParallelCommandBufferBatch TranslateRecordedParallelCommandWorkUnits");
    ASSERT_NE(translateStart, std::string::npos);
    const auto namespaceEnd = rhiSource.find("Render::RHI::RHIFrameContext* Detail::BeginThreadedRhiFrame", translateStart);
    ASSERT_NE(namespaceEnd, std::string::npos);
    const auto translateBody = rhiSource.substr(translateStart, namespaceEnd - translateStart);

    const auto visibilityFailure = translateBody.find("Dependency visibility transition recording failed");
    ASSERT_NE(visibilityFailure, std::string::npos);
    EXPECT_NE(translateBody.find("MarkTranslatedParallelCommandFailure", visibilityFailure), std::string::npos);
    EXPECT_NE(translateBody.find("continue", visibilityFailure), std::string::npos)
        << "A work unit that needed an explicit barrier batch must not be submitted after that batch fails.";

    const auto postPassFailure = translateBody.find("Post-pass transition recording failed");
    ASSERT_NE(postPassFailure, std::string::npos);
    const auto postPassMarker = translateBody.rfind("MarkTranslatedParallelCommandFailure", postPassFailure);
    ASSERT_NE(postPassMarker, std::string::npos);
    const auto postPassCall = translateBody.rfind("RecordPostPassTransitionBatch", postPassMarker);
    ASSERT_NE(postPassCall, std::string::npos);
    EXPECT_LT(postPassCall, postPassMarker);
    EXPECT_NE(translateBody.rfind("HasResourceVisibilityTransitions(extractionVisibilityInput)", postPassCall), std::string::npos)
        << "An extracted texture list alone does not prove a post-pass barrier was needed.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCaptureGroupingUsesIndexedLookup)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());
    EXPECT_NE(rendererSource.find("#include <unordered_map>"), std::string::npos);
    EXPECT_NE(rendererSource.find("SelectionMaskCaptureGroupLookup"), std::string::npos);

    const auto addGroupStart = rendererSource.find("void AddSelectionMaskCaptureGroup");
    ASSERT_NE(addGroupStart, std::string::npos);
    const auto buildGroupsStart = rendererSource.find("std::vector<SelectionMaskCaptureGroup> BuildSelectionMaskCaptureGroups", addGroupStart);
    ASSERT_NE(buildGroupsStart, std::string::npos);
    const auto addGroupBody = rendererSource.substr(addGroupStart, buildGroupsStart - addGroupStart);

    EXPECT_NE(addGroupBody.find("groupLookup.find"), std::string::npos);
    EXPECT_NE(addGroupBody.find("groupLookup.emplace"), std::string::npos);
    EXPECT_EQ(addGroupBody.find("std::find_if("), std::string::npos)
        << "Grouping selected children must stay O(n) on large selected hierarchies.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskFullscreenPassesUseClipSpaceVertexPath)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl");

    ASSERT_FALSE(shader.empty());
    EXPECT_NE(shader.find("VSOutput BuildObjectMaskVertex"), std::string::npos);
    EXPECT_NE(shader.find("VSOutput BuildFullscreenVertex"), std::string::npos);
    EXPECT_NE(shader.find("output.PositionCS = float4(input.Position.xy, 0.0f, 1.0f)"), std::string::npos);
    EXPECT_NE(shader.find("if (u_SelectionOutlinePassMode == SelectionOutlinePassModeCaptureVisible"), std::string::npos);
    EXPECT_NE(shader.find("if (u_SelectionOutlinePassMode == SelectionOutlinePassModeCaptureOccluded"), std::string::npos);

    const auto fullscreenStart = shader.find("VSOutput BuildFullscreenVertex");
    ASSERT_NE(fullscreenStart, std::string::npos);
    const auto psStart = shader.find("float4 CaptureMaskVisible", fullscreenStart);
    ASSERT_NE(psStart, std::string::npos);
    const auto fullscreenBody = shader.substr(fullscreenStart, psStart - fullscreenStart);
    EXPECT_EQ(fullscreenBody.find("u_Model"), std::string::npos);
    EXPECT_EQ(fullscreenBody.find("u_ViewProjection"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskUnsupportedMaterialMaskReportsStructuredSkip)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto collectorHeader = ReadSourceText(root / "Project/Editor/Rendering/DebugGameObjectSelectionCollector.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto fallbackReasonTable = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineFallbackReasons.def");
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl");

    ASSERT_FALSE(collectorHeader.empty());
    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(fallbackReasonTable.empty());
    ASSERT_FALSE(shader.empty());
    EXPECT_NE(collectorHeader.find("sourceMaterial"), std::string::npos);
    EXPECT_NE(rendererSource.find("HasUnsupportedMaterialMask"), std::string::npos);
    EXPECT_NE(rendererSource.find("SelectionOutlineFallbackReason::UnsupportedMaterialMask"), std::string::npos);
    EXPECT_NE(rendererSource.find("output.fallbackDecision = { m_lastFallbackReason, output.selectedItemCount }"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("NLS_SELECTION_OUTLINE_FALLBACK_REASON(UnsupportedMaterialMask, SkipFrame)"), std::string::npos)
        << "Material alpha semantics are intentionally not routed back to the expensive shell path until SceneSelectionPass/alpha-cutout capture is implemented.";
    EXPECT_EQ(fallbackReasonTable.find("NLS_SELECTION_OUTLINE_FALLBACK_REASON(UnsupportedMaterialMask, LegacyShell)"), std::string::npos);
    EXPECT_EQ(rendererSource.find("m_maskVisibleMaterial.Set(\"u_AlphaClip\", 0.0f)"), std::string::npos);
    EXPECT_EQ(rendererSource.find("m_maskOccludedMaterial.Set(\"u_AlphaClip\", 0.0f)"), std::string::npos);
    EXPECT_EQ(shader.find("SceneSelectionPass equivalent"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskUnsupportedMaterialMaskDetectsActualMaskTexture)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto materialMaskStart = rendererSource.find("bool MaterialRequestsAlphaMask");
    ASSERT_NE(materialMaskStart, std::string::npos);
    const auto backendStart = rendererSource.find("NLS::Render::RHI::NativeBackendType ResolveSelectionOutlineBackend", materialMaskStart);
    ASSERT_NE(backendStart, std::string::npos);
    const auto materialMaskBody = rendererSource.substr(materialMaskStart, backendStart - materialMaskStart);

    EXPECT_NE(materialMaskBody.find("u_AlphaClip"), std::string::npos);
    EXPECT_NE(materialMaskBody.find("u_MaskMap"), std::string::npos)
        << "Standard.hlsl discards from u_MaskMap, so the structured skip must see real masked materials even when cutoff parameters are absent.";
    EXPECT_NE(materialMaskBody.find(":Generated/DefaultWhiteTexture"), std::string::npos)
        << "Default white mask-map bindings should not make every Standard material skip selection outline.";
    EXPECT_EQ(materialMaskBody.find("Contains(\"u_AlphaCutoff\") ||"), std::string::npos)
        << "A cutoff-capable shader parameter is not evidence that alpha clipping is enabled.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskReportsUnsupportedBackendForIndexedObjectData)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto fallbackReasonTable = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineFallbackReasons.def");

    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(fallbackReasonTable.empty());

    EXPECT_NE(rendererSource.find("IndexedObjectDataShaderSupport.h"), std::string::npos);
    EXPECT_NE(rendererSource.find("ShaderSupportsIndexedObjectData(*shader)"), std::string::npos);
    EXPECT_NE(rendererSource.find("BackendSupportsIndexedObjectDataPushConstants"), std::string::npos);
    EXPECT_NE(rendererSource.find("SelectionOutlineFallbackReason::UnsupportedBackend"), std::string::npos);
    EXPECT_NE(fallbackReasonTable.find("UnsupportedBackend"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskRebindsShaderOnlyWhenPointerChanges)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());
    EXPECT_NE(rendererSource.find("bool SetShaderIfChanged"), std::string::npos);
    EXPECT_NE(rendererSource.find("if (material.GetShader() == shader)"), std::string::npos);
    EXPECT_NE(rendererSource.find("return false"), std::string::npos);
    EXPECT_NE(rendererSource.find("material.SetShader(shader)"), std::string::npos);
    EXPECT_NE(rendererSource.find("return true"), std::string::npos);
    EXPECT_NE(rendererSource.find("EnsureSelectionOutlineMaterial"), std::string::npos);
    EXPECT_NE(rendererSource.find("if (SetShaderIfChanged(material, shader))"), std::string::npos);
    EXPECT_NE(rendererSource.find("BindSelectionOutlineTextureFallbacks(material, fallbackTexture)"), std::string::npos);
    EXPECT_EQ(rendererSource.find("m_maskVisibleMaterial.SetShader(shader)"), std::string::npos);
    EXPECT_EQ(rendererSource.find("m_edgeMaterial.SetShader(shader)"), std::string::npos);

    const auto buildPreparedStart = rendererSource.find("SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput");
    const auto captureStart = rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands", buildPreparedStart);
    ASSERT_NE(buildPreparedStart, std::string::npos);
    ASSERT_NE(captureStart, std::string::npos);
    const auto buildPreparedBody = rendererSource.substr(buildPreparedStart, captureStart - buildPreparedStart);
    EXPECT_NE(buildPreparedBody.find("EnsureSelectionOutlineMaterials("), std::string::npos);
    EXPECT_EQ(buildPreparedBody.find("BindSelectionOutlineTextureFallbacks("), std::string::npos)
        << "Default texture fallbacks must not dirty selection-outline material binding caches every frame.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCaptureEmitsVisibleAndOccludedContributions)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl");

    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(shader.empty());

    EXPECT_NE(rendererHeader.find("m_maskVisibleMaterial"), std::string::npos);
    EXPECT_NE(rendererHeader.find("m_maskOccludedMaterial"), std::string::npos);
    EXPECT_NE(rendererSource.find("SelectionOutlinePassModeCaptureOccluded"), std::string::npos);
    EXPECT_NE(rendererSource.find("CreateSelectionMaskVisiblePipelineState"), std::string::npos);
    EXPECT_NE(rendererSource.find("CreateSelectionMaskOccludedPipelineState"), std::string::npos);
    EXPECT_NE(rendererSource.find("CaptureMaskDrawCommandsForGroups"), std::string::npos);

    const auto visiblePsoStart = rendererSource.find("CreateSelectionMaskVisiblePipelineState");
    ASSERT_NE(visiblePsoStart, std::string::npos);
    const auto occludedPsoStart = rendererSource.find("CreateSelectionMaskOccludedPipelineState", visiblePsoStart);
    ASSERT_NE(occludedPsoStart, std::string::npos);
    const auto fullscreenPsoStart = rendererSource.find("CreateSelectionFullscreenPipelineState", occludedPsoStart);
    ASSERT_NE(fullscreenPsoStart, std::string::npos);
    const auto visiblePsoBody = rendererSource.substr(visiblePsoStart, occludedPsoStart - visiblePsoStart);
    const auto occludedPsoBody = rendererSource.substr(occludedPsoStart, fullscreenPsoStart - occludedPsoStart);

    EXPECT_NE(visiblePsoBody.find("pso.depthTest = true"), std::string::npos);
    EXPECT_NE(visiblePsoBody.find("pso.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL"), std::string::npos);
    EXPECT_NE(visiblePsoBody.find("pso.depthWriting = false"), std::string::npos);
    EXPECT_NE(occludedPsoBody.find("pso.depthTest = true"), std::string::npos);
    EXPECT_NE(occludedPsoBody.find("pso.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS"), std::string::npos);
    EXPECT_NE(occludedPsoBody.find("pso.depthWriting = false"), std::string::npos);
    EXPECT_NE(occludedPsoBody.find("pso.colorWriting.mask = 0x0F"), std::string::npos);

    const auto captureStart = rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands(");
    ASSERT_NE(captureStart, std::string::npos);
    const auto fullscreenStart = rendererSource.find("void SelectionOutlineMaskRenderer::EnsureFullscreenResources", captureStart);
    ASSERT_NE(fullscreenStart, std::string::npos);
    const auto captureBody = rendererSource.substr(captureStart, fullscreenStart - captureStart);

    const auto occludedCapture = captureBody.find("CreateSelectionMaskOccludedPipelineState(pso)");
    ASSERT_NE(occludedCapture, std::string::npos);
    const auto visibleCapture = captureBody.find("CreateSelectionMaskVisiblePipelineState(pso)");
    ASSERT_NE(visibleCapture, std::string::npos);
    EXPECT_LT(occludedCapture, visibleCapture)
        << "Without Unity's max-blend RHI override, capture all selected pixels first and overwrite visible pixels second.";
    EXPECT_NE(captureBody.find("m_maskVisibleMaterial"), std::string::npos);
    EXPECT_NE(captureBody.find("m_maskOccludedMaterial"), std::string::npos);

    EXPECT_NE(shader.find("SelectionOutlineMaskSetVisible(mask, 1.0f)"), std::string::npos);
    EXPECT_NE(shader.find("SelectionOutlineMaskSetSelected(mask, 1.0f)"), std::string::npos);
    EXPECT_EQ(shader.find("SelectionOutlineMaskSetOccluded(mask, 1.0f)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskKeepsSelectedCoverageForLargeSelectedTrees)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto captureStart = rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands(");
    ASSERT_NE(captureStart, std::string::npos);
    const auto fullscreenStart = rendererSource.find("void SelectionOutlineMaskRenderer::EnsureFullscreenResources", captureStart);
    ASSERT_NE(fullscreenStart, std::string::npos);
    const auto captureBody = rendererSource.substr(captureStart, fullscreenStart - captureStart);

    EXPECT_NE(rendererSource.find("kSelectionOutlineDualDepthCaptureMaxItems"), std::string::npos);
    EXPECT_NE(captureBody.find("enableVisibleDepthRefinement"), std::string::npos);
    EXPECT_NE(captureBody.find("selectedItemCount <= kSelectionOutlineDualDepthCaptureMaxItems"), std::string::npos);
    EXPECT_NE(captureBody.find("groups.size() * (enableVisibleDepthRefinement ? 2u : 1u)"), std::string::npos)
        << "Capture reservation should track grouped draw commands, not raw selected-item count.";

    const auto visibleRefinementGuard = captureBody.find("if (enableVisibleDepthRefinement)");
    ASSERT_NE(visibleRefinementGuard, std::string::npos);
    const auto smallBranchOccludedCapture = captureBody.find("m_maskOccludedMaterial", visibleRefinementGuard);
    const auto smallBranchVisibleCapture = captureBody.find("m_maskVisibleMaterial", visibleRefinementGuard);
    const auto largeBranchStart = captureBody.find("else", visibleRefinementGuard);
    ASSERT_NE(smallBranchOccludedCapture, std::string::npos);
    ASSERT_NE(smallBranchVisibleCapture, std::string::npos);
    ASSERT_NE(largeBranchStart, std::string::npos);
    EXPECT_LT(smallBranchOccludedCapture, smallBranchVisibleCapture)
        << "Small selections should capture all selected pixels first and then refine visible pixels.";

    const auto largeBranchBody = captureBody.substr(largeBranchStart);
    EXPECT_NE(largeBranchBody.find("m_maskVisibleMaterial"), std::string::npos)
        << "Large selected trees must keep a one-pass selected-coverage mask instead of dropping hidden selected pixels.";
    EXPECT_NE(largeBranchBody.find("SelectionOutlinePassModeCaptureVisible"), std::string::npos);
    EXPECT_NE(largeBranchBody.find("CreateSelectionMaskOccludedPipelineState(pso)"), std::string::npos)
        << "The one-pass large-selection capture still uses always-depth coverage.";
    EXPECT_EQ(largeBranchBody.find("m_maskOccludedMaterial"), std::string::npos)
        << "Large selections must not mark the coverage pass as occluded when the visible-depth refinement is skipped.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskTreatsLargeSelectionCoverageAsVisibleUnknown)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto captureStart = rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands(");
    ASSERT_NE(captureStart, std::string::npos);
    const auto fullscreenStart = rendererSource.find("void SelectionOutlineMaskRenderer::EnsureFullscreenResources", captureStart);
    ASSERT_NE(fullscreenStart, std::string::npos);
    const auto captureBody = rendererSource.substr(captureStart, fullscreenStart - captureStart);

    const auto refinementGuard = captureBody.find("if (enableVisibleDepthRefinement)");
    ASSERT_NE(refinementGuard, std::string::npos);
    const auto largeSelectionBranch = captureBody.find("else", refinementGuard);
    ASSERT_NE(largeSelectionBranch, std::string::npos);
    const auto largeBranchBody = captureBody.substr(largeSelectionBranch);

    EXPECT_NE(largeBranchBody.find("m_maskVisibleMaterial"), std::string::npos)
        << "Large stable selections should encode coverage as visible-unknown, not occluded, because the visible-depth refinement pass is intentionally skipped.";
    EXPECT_NE(largeBranchBody.find("SelectionOutlinePassModeCaptureVisible"), std::string::npos);
    EXPECT_NE(largeBranchBody.find("CreateSelectionMaskOccludedPipelineState(pso)"), std::string::npos)
        << "The large branch still captures all selected coverage with an always-depth state, but the shader must not tag it as occluded.";
    EXPECT_EQ(largeBranchBody.find("m_maskOccludedMaterial"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskEdgesUseClassificationKey)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto shader = ReadSourceText(root / "App/Assets/Editor/Shaders/SelectionOutlineCompositeCore.hlsli");

    ASSERT_FALSE(shader.empty());

    const auto accumulateStart = shader.find("void AccumulateSelectionOutlineEdge");
    ASSERT_NE(accumulateStart, std::string::npos);
    const auto computeStart = shader.find("SelectionOutlineEdgeSample ComputeIdEdgeFromMasks", accumulateStart);
    ASSERT_NE(computeStart, std::string::npos);
    const auto accumulateBody = shader.substr(accumulateStart, computeStart - accumulateStart);

    EXPECT_NE(accumulateBody.find("SelectionOutlineMaskGetGroupId(centerMask)"), std::string::npos);
    EXPECT_NE(accumulateBody.find("SelectionOutlineMaskGetClassification(centerMask)"), std::string::npos)
        << "Parent and child selected nodes share a group id today, so classification must be part of the edge key.";
    EXPECT_NE(accumulateBody.find("max("), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlinePreparedOutputRequiresMetadataPairingBeforeCacheCommit)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    const auto hasPassesStart = rendererHeader.find("bool HasScreenSpacePasses() const");
    ASSERT_NE(hasPassesStart, std::string::npos);
    const auto channelStart = rendererHeader.find("enum class SelectionOutlineMaskChannel", hasPassesStart);
    ASSERT_NE(channelStart, std::string::npos);
    const auto hasPassesBody = rendererHeader.substr(hasPassesStart, channelStart - hasPassesStart);
    EXPECT_NE(hasPassesBody.find("passInputs.size() == passKinds.size()"), std::string::npos);
    EXPECT_NE(hasPassesBody.find("passInputs.size() == metadata.size()"), std::string::npos)
        << "Deferred builder handoff must reject incomplete parallel vectors instead of letting cache commit against dropped metadata.";

    const auto buildPreparedStart = rendererSource.find("SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput");
    ASSERT_NE(buildPreparedStart, std::string::npos);
    const auto captureStart = rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands", buildPreparedStart);
    ASSERT_NE(captureStart, std::string::npos);
    const auto buildPreparedBody = rendererSource.substr(buildPreparedStart, captureStart - buildPreparedStart);
    const auto buildMetadata = buildPreparedBody.find("output.metadata = BuildMetadata(output.passInputs, output.passKinds)");
    const auto markPending = buildPreparedBody.find("MarkCachedMaskPending(maskReuseSignature)");
    ASSERT_NE(buildMetadata, std::string::npos);
    ASSERT_NE(markPending, std::string::npos);
    EXPECT_LT(buildMetadata, markPending)
        << "A pending cached mask should only be staged after pass inputs, pass kinds, and metadata are known to be paired.";
    EXPECT_NE(buildPreparedBody.find("output.metadata.size() != output.passInputs.size()"), std::string::npos);
    EXPECT_NE(buildPreparedBody.find("DiscardPendingCachedMask()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskKeepsDualDepthCaptureForSmallSelectionsOnly)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const std::regex thresholdPattern(
        R"(constexpr\s+size_t\s+kSelectionOutlineDualDepthCaptureMaxItems\s*=\s*(\d+)u)");
    std::smatch match;
    ASSERT_TRUE(std::regex_search(rendererSource, match, thresholdPattern));
    ASSERT_GE(match.size(), 2u);

    const auto threshold = static_cast<uint32_t>(std::stoul(match[1].str()));
    EXPECT_GT(threshold, 0u);
    EXPECT_LE(threshold, 8u)
        << "The extra visible-depth refinement pass doubles selected-mesh command preparation; keep it to tiny selections so complex prefab selection does not tank FPS.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskUsesColorOnlyIntermediateFramebuffer)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(rendererHeader.find("Rendering/Buffers/MultiFramebuffer.h"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("Rendering/Buffers/Framebuffer.h"), std::string::npos);
    EXPECT_NE(rendererHeader.find("NLS::Render::Buffers::MultiFramebuffer m_intermediateFramebuffer"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("m_maskFramebuffer"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("m_edgeFramebuffer"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("m_blurHorizontalFramebuffer"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("m_blurVerticalFramebuffer"), std::string::npos);

    EXPECT_NE(rendererSource.find("m_intermediateFramebuffer.Init("), std::string::npos);
    EXPECT_NE(rendererSource.find("false);"), std::string::npos);
    EXPECT_NE(rendererSource.find("GetOrCreateExplicitColorView(kSelectionOutlineMaskAttachmentIndex"), std::string::npos);
    EXPECT_EQ(rendererSource.find("GetOrCreateExplicitColorView(kSelectionOutlineEdgeBlurHorizontalAttachmentIndex"), std::string::npos);
    EXPECT_EQ(rendererSource.find("GetOrCreateExplicitColorView(kSelectionOutlineEdgeAttachmentIndex"), std::string::npos);
    EXPECT_EQ(rendererSource.find("GetOrCreateExplicitColorView(kSelectionOutlineBlurHorizontalAttachmentIndex"), std::string::npos);
    EXPECT_EQ(rendererSource.find("GetOrCreateExplicitColorView(kSelectionOutlineBlurVerticalAttachmentIndex"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskResourceMatrixDeclaresReadWriteChain)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto buildPassStart = rendererSource.find("RenderPassCommandInput SelectionOutlineMaskRenderer::BuildPassInput");
    ASSERT_NE(buildPassStart, std::string::npos);
    const auto setFallbackStart = rendererSource.find("SelectionOutlineMaskRenderer::SetFallbackReason", buildPassStart);
    ASSERT_NE(setFallbackStart, std::string::npos);
    const auto buildPassBody = rendererSource.substr(buildPassStart, setFallbackStart - buildPassStart);

    const auto captureMask = buildPassBody.find("case SelectionOutlineMaskPassKind::CaptureMask:");
    const auto composite = buildPassBody.find("case SelectionOutlineMaskPassKind::Composite:");
    ASSERT_NE(captureMask, std::string::npos);
    ASSERT_NE(composite, std::string::npos);
    EXPECT_EQ(buildPassBody.find("SelectionOutlineMaskPassKind::EdgeBlurHorizontal"), std::string::npos);
    EXPECT_EQ(buildPassBody.find("SelectionOutlineMaskPassKind::DetectIdEdges"), std::string::npos);
    EXPECT_EQ(buildPassBody.find("SelectionOutlineMaskPassKind::BlurHorizontal"), std::string::npos);
    EXPECT_EQ(buildPassBody.find("SelectionOutlineMaskPassKind::BlurVertical"), std::string::npos);

    const auto captureBody = buildPassBody.substr(captureMask, composite - captureMask);
    const auto compositeBody = buildPassBody.substr(composite);

    EXPECT_NE(captureBody.find("passInput.writesDepthStencilAttachment = false"), std::string::npos);
    EXPECT_NE(captureBody.find("passInput.depthStencilAttachmentView = frameDescriptor.outputDepthStencilView"), std::string::npos);
    EXPECT_NE(captureBody.find("frameDescriptor.outputDepthStencilView"), std::string::npos);
    EXPECT_NE(captureBody.find("ResourceAccessMode::Read"), std::string::npos);
    EXPECT_NE(captureBody.find("ResourceState::DepthRead"), std::string::npos);
    EXPECT_NE(captureBody.find("AccessMask::DepthStencilRead"), std::string::npos);
    EXPECT_NE(captureBody.find("resources.maskView"), std::string::npos);
    EXPECT_NE(captureBody.find("ResourceAccessMode::Write"), std::string::npos);

    EXPECT_EQ(compositeBody.find("resources.edgeBlurHorizontalView"), std::string::npos);
    EXPECT_NE(compositeBody.find("ResourceAccessMode::Read"), std::string::npos);
    EXPECT_NE(compositeBody.find("resources.maskView"), std::string::npos);
    EXPECT_NE(compositeBody.find("frameDescriptor.outputColorView"), std::string::npos);
    EXPECT_NE(compositeBody.find("AddTextureViewAccess"), std::string::npos);
    EXPECT_NE(compositeBody.find("ResourceAccessMode::Write"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskResourceMatrixUsesViewSubresourceRanges)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(rendererSource.find("AddTextureViewAccess"), std::string::npos);
    EXPECT_NE(rendererSource.find("NormalizeTextureSubresourceRange"), std::string::npos);

    const auto buildPassStart = rendererSource.find("RenderPassCommandInput SelectionOutlineMaskRenderer::BuildPassInput");
    ASSERT_NE(buildPassStart, std::string::npos);
    const auto setFallbackStart = rendererSource.find("SelectionOutlineMaskRenderer::SetFallbackReason", buildPassStart);
    ASSERT_NE(setFallbackStart, std::string::npos);
    const auto buildPassBody = rendererSource.substr(buildPassStart, setFallbackStart - buildPassStart);

    EXPECT_EQ(buildPassBody.find("AddTextureAccess("), std::string::npos)
        << "Access declarations must follow the view range, not the whole texture, or array/mip Scene View targets get over-broadened barriers.";
    EXPECT_NE(buildPassBody.find("AddTextureViewAccess(\n                passInput,\n                frameDescriptor.outputDepthStencilView"), std::string::npos);
    EXPECT_NE(buildPassBody.find("AddTextureViewAccess(\n                passInput,\n                resources.maskView"), std::string::npos);
    EXPECT_NE(buildPassBody.find("AddTextureViewAccess(\n                passInput,\n                frameDescriptor.outputColorView"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskResourceIdentityKeysStableReuse)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(rendererHeader.find("uint32_t width"), std::string::npos);
    EXPECT_NE(rendererHeader.find("uint32_t height"), std::string::npos);
    EXPECT_NE(rendererHeader.find("NLS::Render::RHI::TextureFormat maskFormat"), std::string::npos);
    EXPECT_EQ(rendererHeader.find("NLS::Render::RHI::TextureFormat colorFormat"), std::string::npos)
        << "ResourceIdentity stores the intermediate mask format; the Scene View color format is keyed by outputColorViewDesc.";
    EXPECT_NE(rendererHeader.find("uint32_t sampleCount"), std::string::npos);
    EXPECT_NE(rendererHeader.find("std::shared_ptr<NLS::Render::RHI::RHITexture> sceneDepthTexture"), std::string::npos);
    EXPECT_NE(rendererHeader.find("std::shared_ptr<NLS::Render::RHI::RHITexture> outputColorTexture"), std::string::npos);
    EXPECT_NE(rendererHeader.find("NLS::Render::RHI::RHITextureViewDesc sceneDepthViewDesc"), std::string::npos);
    EXPECT_NE(rendererHeader.find("NLS::Render::RHI::RHITextureViewDesc outputColorViewDesc"), std::string::npos);

    const auto matchesStart = rendererSource.find("bool SelectionOutlineMaskRenderer::ResourceIdentity::Matches");
    ASSERT_NE(matchesStart, std::string::npos);
    const auto ctorStart = rendererSource.find("SelectionOutlineMaskRenderer::SelectionOutlineMaskRenderer", matchesStart);
    ASSERT_NE(ctorStart, std::string::npos);
    const auto matchesBody = rendererSource.substr(matchesStart, ctorStart - matchesStart);
    EXPECT_NE(matchesBody.find("width == other.width"), std::string::npos);
    EXPECT_NE(matchesBody.find("height == other.height"), std::string::npos);
    EXPECT_NE(matchesBody.find("maskFormat == other.maskFormat"), std::string::npos);
    EXPECT_NE(matchesBody.find("sampleCount == other.sampleCount"), std::string::npos);
    EXPECT_NE(matchesBody.find("sceneDepthTexture == other.sceneDepthTexture"), std::string::npos);
    EXPECT_NE(matchesBody.find("outputColorTexture == other.outputColorTexture"), std::string::npos);
    EXPECT_NE(matchesBody.find("SameViewDesc(sceneDepthViewDesc, other.sceneDepthViewDesc)"), std::string::npos);
    EXPECT_NE(matchesBody.find("SameViewDesc(outputColorViewDesc, other.outputColorViewDesc)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskCacheSignaturesUseNamedHashSeed)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(rendererSource.find("kSelectionOutlineSignatureHashSeed"), std::string::npos);
    EXPECT_EQ(rendererSource.find("14695981039346656037ull"), rendererSource.rfind("14695981039346656037ull"))
        << "The FNV offset seed should have one named definition instead of duplicated literals across cache signatures.";

    const auto maskSignatureStart = rendererSource.find("SelectionOutlineMaskRenderer::BuildMaskReuseSignature");
    ASSERT_NE(maskSignatureStart, std::string::npos);
    const auto sourceSignatureStart = rendererSource.find("SelectionOutlineMaskRenderer::BuildSelectionSourceSignature", maskSignatureStart);
    ASSERT_NE(sourceSignatureStart, std::string::npos);
    const auto preparedGroupsStart = rendererSource.find("SelectionOutlineMaskRenderer::BuildPreparedSelectionCaptureGroups", sourceSignatureStart);
    ASSERT_NE(preparedGroupsStart, std::string::npos);
    const auto maskSignatureBody = rendererSource.substr(maskSignatureStart, sourceSignatureStart - maskSignatureStart);
    const auto sourceSignatureBody = rendererSource.substr(sourceSignatureStart, preparedGroupsStart - sourceSignatureStart);

    EXPECT_NE(maskSignatureBody.find("kSelectionOutlineSignatureHashSeed"), std::string::npos);
    EXPECT_NE(sourceSignatureBody.find("kSelectionOutlineSignatureHashSeed"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskRejectsUnsupportedSampleCountsBeforeAllocatingIntermediates)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(debugSceneSource.empty());

    EXPECT_NE(ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineFallbackReasons.def").find("UnsupportedSampleCount"), std::string::npos);
    EXPECT_NE(rendererHeader.find("uint32_t depthSampleCount"), std::string::npos);
    EXPECT_FALSE(NLS::Editor::Rendering::SelectionOutlineMaskSampleCountsAreSupported(4u, 4u));
    EXPECT_FALSE(NLS::Editor::Rendering::SelectionOutlineMaskSampleCountsAreSupported(1u, 4u));
    EXPECT_TRUE(NLS::Editor::Rendering::SelectionOutlineMaskSampleCountsAreSupported(1u, 1u));
    EXPECT_EQ(
        NLS::Editor::Rendering::ResolveSelectionOutlineFallbackAction(
            NLS::Editor::Rendering::SelectionOutlineFallbackReason::UnsupportedSampleCount),
        NLS::Editor::Rendering::SelectionOutlineFallbackAction::SkipFrame);
    EXPECT_STREQ(
        NLS::Editor::Rendering::SelectionOutlineFallbackReasonToString(
            NLS::Editor::Rendering::SelectionOutlineFallbackReason::UnsupportedSampleCount),
        "UnsupportedSampleCount");

    const auto buildIdentityStart = rendererSource.find("SelectionOutlineMaskRenderer::BuildResourceIdentity");
    ASSERT_NE(buildIdentityStart, std::string::npos);
    const auto prepareStart = rendererSource.find("bool SelectionOutlineMaskRenderer::PrepareResources", buildIdentityStart);
    ASSERT_NE(prepareStart, std::string::npos);
    const auto buildIdentityBody = rendererSource.substr(buildIdentityStart, prepareStart - buildIdentityStart);
    EXPECT_NE(buildIdentityBody.find("const auto outputSampleCount"), std::string::npos);
    EXPECT_NE(buildIdentityBody.find("const auto depthSampleCount"), std::string::npos);
    EXPECT_NE(buildIdentityBody.find("identity.sampleCount = outputSampleCount"), std::string::npos);
    EXPECT_NE(buildIdentityBody.find("identity.depthSampleCount = depthSampleCount"), std::string::npos);

    const auto bindStart = rendererSource.find("bool SelectionOutlineMaskRenderer::BindScreenSpaceMaterialTextures", prepareStart);
    ASSERT_NE(bindStart, std::string::npos);
    const auto prepareBody = rendererSource.substr(prepareStart, bindStart - prepareStart);
    const auto unsupportedSampleCount = prepareBody.find("SelectionOutlineFallbackReason::UnsupportedSampleCount");
    const auto resizeDecision = prepareBody.find("const bool needsResize");
    ASSERT_NE(unsupportedSampleCount, std::string::npos);
    ASSERT_NE(resizeDecision, std::string::npos);
    EXPECT_LT(unsupportedSampleCount, resizeDecision);
    EXPECT_NE(prepareBody.find("SelectionOutlineMaskSampleCountsAreSupported("), std::string::npos);
    EXPECT_NE(prepareBody.find("nextIdentity->sampleCount"), std::string::npos);
    EXPECT_NE(
        prepareBody.find("nextIdentity->depthSampleCount"),
        std::string::npos);
    EXPECT_NE(prepareBody.find("ResetResources()"), std::string::npos);

    EXPECT_NE(debugSceneSource.find("ResolveSelectionOutlineFallbackAction(maskOutput.fallbackDecision->reason)"), std::string::npos);
    EXPECT_EQ(debugSceneSource.find("case Editor::Rendering::SelectionOutlineFallbackReason::UnsupportedSampleCount"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineRejectsFrameAttachmentsWhoseExtentsDoNotMatchRenderTarget)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");

    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(rendererHeader.empty());

    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "SelectionOutlineExtentColor";
    colorDesc.extent = { 128u, 64u, 1u };
    colorDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    colorDesc.sampleCount = 1u;
    const auto colorTexture = std::make_shared<TestTexture>(colorDesc);

    NLS::Render::RHI::RHITextureDesc depthDesc;
    depthDesc.debugName = "SelectionOutlineExtentDepth";
    depthDesc.extent = { 128u, 64u, 1u };
    depthDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    depthDesc.sampleCount = 1u;
    const auto depthTexture = std::make_shared<TestTexture>(depthDesc);

    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.debugName = "SelectionOutlineExtentColorView";
    colorViewDesc.format = colorDesc.format;
    colorViewDesc.subresourceRange.mipLevelCount = 1u;
    colorViewDesc.subresourceRange.arrayLayerCount = 1u;
    const auto colorView = std::make_shared<TestTextureView>(colorTexture, colorViewDesc);

    NLS::Render::RHI::RHITextureViewDesc depthViewDesc;
    depthViewDesc.debugName = "SelectionOutlineExtentDepthView";
    depthViewDesc.format = depthDesc.format;
    depthViewDesc.subresourceRange.mipLevelCount = 1u;
    depthViewDesc.subresourceRange.arrayLayerCount = 1u;
    const auto depthView = std::make_shared<TestTextureView>(depthTexture, depthViewDesc);

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.outputColorView = colorView;
    frameDescriptor.outputDepthStencilView = depthView;

    EXPECT_TRUE(NLS::Editor::Rendering::SelectionOutlineFrameAttachmentsMatchRenderExtent(frameDescriptor));
    EXPECT_TRUE(NLS::Editor::Rendering::SelectionOutlineLegacyShellFallbackIsAttachmentCompatible(frameDescriptor));

    depthDesc.extent = { 64u, 64u, 1u };
    const auto staleDepthTexture = std::make_shared<TestTexture>(depthDesc);
    const auto staleDepthView = std::make_shared<TestTextureView>(staleDepthTexture, depthViewDesc);
    frameDescriptor.outputDepthStencilView = staleDepthView;
    EXPECT_FALSE(NLS::Editor::Rendering::SelectionOutlineFrameAttachmentsMatchRenderExtent(frameDescriptor));
    EXPECT_FALSE(NLS::Editor::Rendering::SelectionOutlineLegacyShellFallbackIsAttachmentCompatible(frameDescriptor));

    frameDescriptor.outputDepthStencilView = depthView;
    colorDesc.extent = { 128u, 32u, 1u };
    const auto staleColorTexture = std::make_shared<TestTexture>(colorDesc);
    const auto staleColorView = std::make_shared<TestTextureView>(staleColorTexture, colorViewDesc);
    frameDescriptor.outputColorView = staleColorView;
    EXPECT_FALSE(NLS::Editor::Rendering::SelectionOutlineFrameAttachmentsMatchRenderExtent(frameDescriptor));
    EXPECT_FALSE(NLS::Editor::Rendering::SelectionOutlineLegacyShellFallbackIsAttachmentCompatible(frameDescriptor));

    frameDescriptor.outputColorView = colorView;
    frameDescriptor.renderWidth = 256u;
    EXPECT_FALSE(NLS::Editor::Rendering::SelectionOutlineFrameAttachmentsMatchRenderExtent(frameDescriptor));

    EXPECT_NE(rendererHeader.find("SelectionOutlineFrameAttachmentsMatchRenderExtent"), std::string::npos);

    const auto prepareStart = rendererSource.find("bool SelectionOutlineMaskRenderer::PrepareResources");
    ASSERT_NE(prepareStart, std::string::npos);
    const auto bindStart = rendererSource.find("bool SelectionOutlineMaskRenderer::BindScreenSpaceMaterialTextures", prepareStart);
    ASSERT_NE(bindStart, std::string::npos);
    const auto prepareBody = rendererSource.substr(prepareStart, bindStart - prepareStart);
    const auto extentCheck = prepareBody.find("SelectionOutlineFrameAttachmentsMatchRenderExtent(frameDescriptor)");
    const auto buildIdentity = prepareBody.find("BuildResourceIdentity(frameDescriptor)");
    ASSERT_NE(extentCheck, std::string::npos);
    ASSERT_NE(buildIdentity, std::string::npos);
    EXPECT_LT(extentCheck, buildIdentity)
        << "Stale or resized Scene View attachments must be rejected before intermediate allocation or pass assembly.";
    EXPECT_NE(prepareBody.find("SelectionOutlineFallbackReason::StaleFrameAttachment", extentCheck), std::string::npos);
    EXPECT_NE(prepareBody.find("ResetResources()", extentCheck), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineLegacyFallbackRejectsMsaaFrameDescriptors)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(debugSceneSource.empty());

    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "SelectionOutlineLegacyMsaaColor";
    colorDesc.extent = { 128u, 128u, 1u };
    colorDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    colorDesc.sampleCount = 4u;
    const auto colorTexture = std::make_shared<TestTexture>(colorDesc);

    NLS::Render::RHI::RHITextureDesc depthDesc;
    depthDesc.debugName = "SelectionOutlineLegacyMsaaDepth";
    depthDesc.extent = { 128u, 128u, 1u };
    depthDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    depthDesc.sampleCount = 4u;
    const auto depthTexture = std::make_shared<TestTexture>(depthDesc);

    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.debugName = "SelectionOutlineLegacyMsaaColorView";
    colorViewDesc.format = colorDesc.format;
    colorViewDesc.subresourceRange.mipLevelCount = 1u;
    colorViewDesc.subresourceRange.arrayLayerCount = 1u;
    const auto colorView = std::make_shared<TestTextureView>(colorTexture, colorViewDesc);

    NLS::Render::RHI::RHITextureViewDesc depthViewDesc;
    depthViewDesc.debugName = "SelectionOutlineLegacyMsaaDepthView";
    depthViewDesc.format = depthDesc.format;
    depthViewDesc.subresourceRange.mipLevelCount = 1u;
    depthViewDesc.subresourceRange.arrayLayerCount = 1u;
    const auto depthView = std::make_shared<TestTextureView>(depthTexture, depthViewDesc);

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.outputColorView = colorView;
    frameDescriptor.outputDepthStencilView = depthView;

    EXPECT_FALSE(NLS::Editor::Rendering::SelectionOutlineLegacyShellFallbackIsAttachmentCompatible(frameDescriptor));

    depthDesc.sampleCount = 1u;
    const auto mismatchedDepthTexture = std::make_shared<TestTexture>(depthDesc);
    const auto mismatchedDepthView = std::make_shared<TestTextureView>(mismatchedDepthTexture, depthViewDesc);
    frameDescriptor.outputDepthStencilView = mismatchedDepthView;
    EXPECT_FALSE(NLS::Editor::Rendering::SelectionOutlineLegacyShellFallbackIsAttachmentCompatible(frameDescriptor));

    colorDesc.sampleCount = 1u;
    const auto singleSampleColorTexture = std::make_shared<TestTexture>(colorDesc);
    const auto singleSampleColorView = std::make_shared<TestTextureView>(singleSampleColorTexture, colorViewDesc);
    frameDescriptor.outputColorView = singleSampleColorView;
    EXPECT_TRUE(NLS::Editor::Rendering::SelectionOutlineLegacyShellFallbackIsAttachmentCompatible(frameDescriptor));

    const auto gateCall = debugSceneSource.find("SelectionOutlineLegacyShellFallbackIsAttachmentCompatible");
    const auto legacyCapture = debugSceneSource.find("m_outlineRenderer.CaptureOutlineDrawCommands");
    ASSERT_NE(gateCall, std::string::npos);
    ASSERT_NE(legacyCapture, std::string::npos);
    const auto gateBody = debugSceneSource.substr(gateCall, legacyCapture - gateCall);
    EXPECT_NE(gateBody.find("m_renderer.GetFrameDescriptor()"), std::string::npos);
    EXPECT_NE(gateBody.find("selectionOutlineDepthView"), std::string::npos);
    EXPECT_LT(gateCall, legacyCapture);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskAvoidsHotPathMaterialAndTextureWrapperChurn)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererHeader = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");
    const auto textureHeader = ReadSourceText(root / "Runtime/Rendering/Resources/Texture2D.h");
    const auto textureSource = ReadSourceText(root / "Runtime/Rendering/Resources/Texture2D.cpp");

    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(textureHeader.empty());
    ASSERT_FALSE(textureSource.empty());
    EXPECT_NE(textureHeader.find("WrapExternalInPlace"), std::string::npos);
    EXPECT_NE(textureSource.find("void Texture2D::WrapExternalInPlace"), std::string::npos);
    EXPECT_NE(rendererSource.find("SetMaterialValueIfChanged"), std::string::npos);
    EXPECT_NE(rendererSource.find("SetSelectionMaskPassModeIfChanged"), std::string::npos);
    EXPECT_NE(rendererSource.find("SetMaterialTextureIfChanged"), std::string::npos);
    EXPECT_NE(rendererSource.find("WrapSelectionOutlineExternalTexture"), std::string::npos);
    EXPECT_EQ(rendererSource.find("m_maskTexture = NLS::Render::Resources::Texture2D::WrapExternal(\n"), std::string::npos);
    EXPECT_EQ(rendererSource.find("m_edgeBlurHorizontalTexture"), std::string::npos);

    const auto buildPreparedStart = rendererSource.find("SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput");
    const auto captureStart = rendererSource.find("void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands", buildPreparedStart);
    ASSERT_NE(buildPreparedStart, std::string::npos);
    ASSERT_NE(captureStart, std::string::npos);
    const auto buildPreparedBody = rendererSource.substr(buildPreparedStart, captureStart - buildPreparedStart);
    EXPECT_EQ(buildPreparedBody.find(".Set<int>(\"u_SelectionOutlinePassMode\""), std::string::npos);
    EXPECT_EQ(buildPreparedBody.find(".Set(\"u_AlphaCutoff\""), std::string::npos);
    EXPECT_EQ(buildPreparedBody.find(".Set(\"u_TexelSize\""), std::string::npos);
    EXPECT_EQ(buildPreparedBody.find(".Set(\"u_OutlineColor\""), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskInvalidatesMaterialsWhenExternalTextureHandleChanges)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    EXPECT_NE(rendererSource.find("InvalidateSelectionOutlineTextureBindings"), std::string::npos);
    EXPECT_NE(rendererSource.find("textureHandleChanged"), std::string::npos);
    EXPECT_EQ(rendererSource.find("m_edgeBlurHorizontalMaterial.InvalidateExplicitBindingSetCache()"), std::string::npos);
    EXPECT_EQ(rendererSource.find("m_edgeMaterial.InvalidateExplicitBindingSetCache()"), std::string::npos);
    EXPECT_EQ(rendererSource.find("m_blurHorizontalMaterial.InvalidateExplicitBindingSetCache()"), std::string::npos);
    EXPECT_EQ(rendererSource.find("m_blurVerticalMaterial.InvalidateExplicitBindingSetCache()"), std::string::npos);
    EXPECT_NE(rendererSource.find("m_compositeMaterial.InvalidateExplicitBindingSetCache()"), std::string::npos);
    EXPECT_NE(rendererSource.find("BindSelectionOutlineTextureFallbacks("), std::string::npos)
        << "ResetResources must clear stale raw wrapper pointers after allocation or identity failures.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskPipelineStatesDisableDepthAndStencilWrites)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto maskPsoStart = rendererSource.find("CreateSelectionMaskVisiblePipelineState");
    ASSERT_NE(maskPsoStart, std::string::npos);
    const auto fullscreenPsoStart = rendererSource.find("CreateSelectionFullscreenPipelineState", maskPsoStart);
    ASSERT_NE(fullscreenPsoStart, std::string::npos);
    const auto sameViewStart = rendererSource.find("bool SameViewDesc", fullscreenPsoStart);
    ASSERT_NE(sameViewStart, std::string::npos);

    const auto maskPsoBody = rendererSource.substr(maskPsoStart, fullscreenPsoStart - maskPsoStart);
    const auto fullscreenPsoBody = rendererSource.substr(fullscreenPsoStart, sameViewStart - fullscreenPsoStart);

    EXPECT_NE(maskPsoBody.find("pso.depthTest = true"), std::string::npos);
    EXPECT_NE(maskPsoBody.find("pso.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL"), std::string::npos);
    EXPECT_NE(maskPsoBody.find("pso.depthWriting = false"), std::string::npos);
    EXPECT_NE(maskPsoBody.find("pso.stencilTest = false"), std::string::npos);
    EXPECT_NE(maskPsoBody.find("pso.stencilWriteMask = 0u"), std::string::npos);

    EXPECT_NE(fullscreenPsoBody.find("pso.depthTest = false"), std::string::npos);
    EXPECT_NE(fullscreenPsoBody.find("pso.depthWriting = false"), std::string::npos);
    EXPECT_NE(fullscreenPsoBody.find("pso.stencilTest = false"), std::string::npos);
    EXPECT_NE(fullscreenPsoBody.find("pso.stencilWriteMask = 0u"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskRetriesIntermediateAllocationAfterFailure)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(rendererSource.empty());

    const auto prepareStart = rendererSource.find("bool SelectionOutlineMaskRenderer::PrepareResources");
    ASSERT_NE(prepareStart, std::string::npos);
    const auto bindStart = rendererSource.find("bool SelectionOutlineMaskRenderer::BindScreenSpaceMaterialTextures", prepareStart);
    ASSERT_NE(bindStart, std::string::npos);
    const auto prepareBody = rendererSource.substr(prepareStart, bindStart - prepareStart);

    EXPECT_NE(prepareBody.find("!m_intermediateFramebuffer.IsInitialized()"), std::string::npos);

    const auto needsResize = prepareBody.find("const bool needsResize");
    const auto nullViewCheck = prepareBody.find("outResources.maskView == nullptr");
    const auto assignIdentity = prepareBody.find("m_resourceIdentity = nextIdentity");
    ASSERT_NE(needsResize, std::string::npos);
    ASSERT_NE(nullViewCheck, std::string::npos);
    ASSERT_NE(assignIdentity, std::string::npos);
    EXPECT_LT(needsResize, assignIdentity);
    EXPECT_LT(nullViewCheck, assignIdentity);
}

TEST(EditorRenderPathContractTests, PreparedEditorHelperInputsAvoidRecordedCommandCopies)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const auto gridHeader = ReadSourceText(root / "Project/Editor/Rendering/GridRenderPass.h");
    const auto pickingHeader = ReadSourceText(root / "Project/Editor/Rendering/PickingRenderPass.h");
    const auto deferredHeader = ReadSourceText(root / "Runtime/Engine/Rendering/DeferredSceneRenderer.h");
    const auto deferredSource = ReadSourceText(root / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");
    const auto deferredFrameGraphSource = ReadSourceText(root / "Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp");
    const auto threadedLifecycleSource = ReadSourceText(root / "Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp");

    ASSERT_FALSE(debugSceneSource.empty());
    ASSERT_FALSE(gridHeader.empty());
    ASSERT_FALSE(pickingHeader.empty());
    ASSERT_FALSE(deferredHeader.empty());
    ASSERT_FALSE(deferredSource.empty());
    ASSERT_FALSE(deferredFrameGraphSource.empty());
    ASSERT_FALSE(threadedLifecycleSource.empty());

    const auto helperGetterSignature =
        "const std::optional<NLS::Render::Context::RenderPassCommandInput>& GetPreparedThreadedPassInput() const";
    const auto helperConsumeSignature =
        "std::optional<NLS::Render::Context::RenderPassCommandInput> ConsumePreparedThreadedPassInput()";
    const auto selectionGetterSignature =
        "const std::vector<NLS::Render::Context::RenderPassCommandInput>& GetPreparedThreadedPassInputs() const";
    const auto selectionConsumeSignature =
        "std::vector<NLS::Render::Context::RenderPassCommandInput> ConsumePreparedThreadedPassInputs()";
    EXPECT_NE(gridHeader.find(helperGetterSignature), std::string::npos);
    EXPECT_NE(pickingHeader.find(helperGetterSignature), std::string::npos);
    EXPECT_NE(gridHeader.find(helperConsumeSignature), std::string::npos);
    EXPECT_NE(pickingHeader.find(helperConsumeSignature), std::string::npos);
    EXPECT_GE(CountOccurrences(debugSceneSource, helperGetterSignature), 2u);
    EXPECT_GE(CountOccurrences(debugSceneSource, helperConsumeSignature), 2u);
    EXPECT_NE(debugSceneSource.find(selectionGetterSignature), std::string::npos);
    EXPECT_NE(debugSceneSource.find(selectionConsumeSignature), std::string::npos);
    EXPECT_NE(debugSceneSource.find("GetPreparedThreadedPassMetadata()"), std::string::npos);

    EXPECT_NE(debugSceneSource.find("const auto& gridPassInput = GetPass<GridRenderPass>"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("const auto& cameraPassInput = GetPass<DebugCamerasRenderPass>"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("const auto& lightPassInput = GetPass<DebugLightsRenderPass>"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("const auto& selectionOutlinePassInputs = GetPass<DebugGameObjectRenderPass>"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("const auto& selectionOutlineMetadata = GetPass<DebugGameObjectRenderPass>"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("const auto& pickingPassInput = GetPass<PickingRenderPass>"), std::string::npos);
    EXPECT_EQ(debugSceneSource.find("const auto gridPassInput = GetPass<GridRenderPass>"), std::string::npos);
    EXPECT_EQ(debugSceneSource.find("const auto selectionPassInput = GetPass<DebugGameObjectRenderPass>"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("auto appendedPassInputs = BuildDebugDeferredAppendedPassInputs"), std::string::npos);
    EXPECT_EQ(debugSceneSource.find("const auto appendedPassInputs = BuildDebugDeferredAppendedPassInputs"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("GetPass<GridRenderPass>(\"Grid\").ConsumePreparedThreadedPassInput()"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("GetPass<DebugGameObjectRenderPass>(\"Debug GameObject\").ConsumePreparedThreadedPassInputs()"), std::string::npos);
    EXPECT_NE(debugSceneSource.find("passInputs.push_back(std::move(selectionPassInput))"), std::string::npos);
    EXPECT_EQ(debugSceneSource.find("passInputs.push_back(*selectionPassInput)"), std::string::npos);

    EXPECT_NE(deferredHeader.find(
        "std::vector<NLS::Render::Context::RenderPassCommandInput> appendedPassInputs"),
        std::string::npos);
    EXPECT_NE(deferredSource.find("appendedPassInputs = std::move(appendedPassInputs)"), std::string::npos);
    EXPECT_EQ(deferredSource.find("const auto* resolvedAppendedPassInputs = &appendedPassInputs"), std::string::npos);
    EXPECT_EQ(deferredSource.find("auto resolvedAppendedPassInputs = appendedPassInputs"), std::string::npos);
    EXPECT_EQ(deferredSource.find("auto* resolvedAppendedPassInputs = &appendedPassInputs"), std::string::npos);
    EXPECT_EQ(deferredSource.find("offscreenAppendedPassInputs = appendedPassInputs"), std::string::npos);
    EXPECT_NE(deferredSource.find("std::move(appendedPassInputs)"), std::string::npos);
    EXPECT_EQ(deferredSource.find("std::move(*resolvedAppendedPassInputs)"), std::string::npos);

    const auto deferredFrameGraphHeader = ReadSourceText(root / "Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h");
    ASSERT_FALSE(deferredFrameGraphHeader.empty());
    EXPECT_NE(deferredFrameGraphHeader.find("std::vector<NLS::Render::Context::RenderPassCommandInput>&& appendedPassInputs"), std::string::npos);
    EXPECT_EQ(deferredFrameGraphHeader.find("std::vector<NLS::Render::Context::RenderPassCommandInput> appendedPassInputs"), std::string::npos);
    EXPECT_NE(deferredFrameGraphSource.find("std::vector<NLS::Render::Context::RenderPassCommandInput>& appendedPassInputs"), std::string::npos);
    EXPECT_NE(deferredFrameGraphSource.find("passInputs.push_back(std::move(*appendedPassInput))"), std::string::npos);
    EXPECT_NE(deferredFrameGraphSource.find("appendedPassInputs.erase(appendedPassInput)"), std::string::npos);
    EXPECT_EQ(deferredFrameGraphSource.find("passInputs.push_back(appendedPassInput)"), std::string::npos);

    EXPECT_NE(threadedLifecycleSource.find("renderSceneBuilder = std::move(slot->preparedRenderSceneBuilder.value())"), std::string::npos);
    EXPECT_NE(threadedLifecycleSource.find("slot->preparedRenderSceneBuilder.reset()"), std::string::npos);
    EXPECT_EQ(threadedLifecycleSource.find("renderSceneBuilder = slot->preparedRenderSceneBuilder.value()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, PickingHelperDeclaresDepthStencilWrites)
{
    const auto source = ReadSourceText(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/PickingRenderPass.cpp");

    const auto buildInputStart = source.find(
        "std::optional<NLS::Render::Context::RenderPassCommandInput> Editor::Rendering::PickingRenderPass::BuildThreadedPassInput");
    ASSERT_NE(buildInputStart, std::string::npos);
    const auto captureModels = source.find("CapturePickableModels", buildInputStart);
    ASSERT_NE(captureModels, std::string::npos);
    const auto buildInputBody = source.substr(buildInputStart, captureModels - buildInputStart);

    const auto clearDepth = buildInputBody.find("passInput.clearDepth = true");
    const auto clearStencil = buildInputBody.find("passInput.clearStencil = true");
    const auto usesDepth = buildInputBody.find("passInput.usesDepthStencilAttachment = true");
    const auto writesDepth = buildInputBody.find("passInput.writesDepthStencilAttachment = true");

    ASSERT_NE(clearDepth, std::string::npos);
    ASSERT_NE(clearStencil, std::string::npos);
    ASSERT_NE(usesDepth, std::string::npos);
    ASSERT_NE(writesDepth, std::string::npos);
    EXPECT_LT(clearDepth, writesDepth);
    EXPECT_LT(clearStencil, writesDepth);
    EXPECT_LT(usesDepth, writesDepth);
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
    EXPECT_EQ(queueCode.find("FillEmptySlotsWithMaterial"), std::string::npos);
    EXPECT_EQ(queueCode.find("ApplyVisibleFallbackMaterial"), std::string::npos);
    EXPECT_NE(queueCode.find("state->restoreRootSelfActive = instance->instanceRoot->IsSelfActive();"), std::string::npos);
    EXPECT_EQ(queueCode.find("instance->instanceRoot->SetActive(false);"), std::string::npos)
        << "Ready generated model drops must stay visible while renderer resources resolve incrementally.";
    EXPECT_NE(queueCode.find("state->rootHiddenUntilRendererResourcesReady = false;"), std::string::npos);
    EXPECT_EQ(queueCode.find("state->rootHiddenUntilRendererResourcesReady = true;"), std::string::npos)
        << "The hidden-root path is a legacy fallback cleanup concern, not the primary ready-drop path.";
    EXPECT_NE(source.find("RestoreRendererResourceResolutionRootVisibility(*state);"), std::string::npos);
    EXPECT_EQ(source.find("if (!meshRenderer->GetMaterialPaths().empty())"), std::string::npos);
    EXPECT_NE(source.find("HasResolvedMaterialBindings(*meshRenderer)"), std::string::npos);
    const auto resolvedBegin = source.find("bool HasResolvedMaterialBindings(");
    ASSERT_NE(resolvedBegin, std::string::npos);
    const auto resolvedEnd = source.find("void CountResolvedRendererResources(", resolvedBegin);
    ASSERT_NE(resolvedEnd, std::string::npos);
    const auto resolvedCode = source.substr(resolvedBegin, resolvedEnd - resolvedBegin);
    EXPECT_NE(resolvedCode.find("!ResolvedMaterialPathMatches(material->path, paths[index])"), std::string::npos)
        << "Generated model readiness must accept equivalent absolute/Library material artifact paths.";
    EXPECT_NE(source.find("bool HasResolvedMaterialTextures("), std::string::npos)
        << "A generated model instance is not ready just because material pointers are bound; declared texture slots must be bound too.";
    EXPECT_NE(resolvedCode.find("!HasResolvedMaterialTextures(*material)"), std::string::npos)
        << "Already-resolved drops must keep the async resolution queue alive until material texture slots are bound.";
    EXPECT_NE(queueCode.find("resolvedPaths.push_back(materialPath.value_or(std::string {}))"), std::string::npos);
    EXPECT_EQ(queueCode.find("if (auto materialPath = ResolvePrefabAssetPath(prefab, value))\n                            resolvedPaths.push_back(*materialPath)"), std::string::npos);
    EXPECT_EQ(queueCode.find("materialManager[resolvedPaths[index]]"), std::string::npos);
    EXPECT_EQ(queueCode.find("materialManager.GetResource(resolvedPaths[index], false)"), std::string::npos);
    EXPECT_EQ(queueCode.find("materialManager.GetResource(task.materialPaths[index], true)"), std::string::npos);
    EXPECT_NE(queueCode.find("FindCachedMaterialByEquivalentPath(materialManager, task.materialPaths[index])"), std::string::npos)
        << "Generated model drops must reuse cached material artifacts across Library/absolute path aliases without cold-loading.";
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
    const auto meshArtifactLoad = loopCode.find("StartMeshArtifactLoad(actions, task, *state)");
    ASSERT_NE(cachedAliasCheck, std::string::npos);
    ASSERT_NE(meshArtifactLoad, std::string::npos);
    EXPECT_LT(cachedAliasCheck, meshArtifactLoad);
    EXPECT_NE(source.find("bool IsMeshTaskAlreadyCached("), std::string::npos);
    EXPECT_NE(source.find("std::unordered_map<std::string, std::shared_ptr<MeshArtifactLoadState>> meshLoadsByPath"), std::string::npos);
    EXPECT_NE(source.find("task.meshLoad = existingLoad->second"), std::string::npos);
    EXPECT_NE(source.find("resolutionState.meshLoadsByPath.emplace(task.modelPath, task.meshLoad)"), std::string::npos);
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
    EXPECT_EQ(bindCode.find("materialManager.LoadArtifactWithoutTextures(task.materialPaths[index])"), std::string::npos)
        << "Cold generated material artifacts must not be synchronously parsed during drop-time resource resolution.";
    EXPECT_NE(bindCode.find("FindCachedMaterialByEquivalentPath(materialManager, task.materialPaths[index])"), std::string::npos)
        << "Generated model material resolution should probe equivalent cached artifacts without synchronously loading cold misses.";
    EXPECT_NE(bindCode.find("if (!material || !material->IsValid())"), std::string::npos);
    EXPECT_LT(
        bindCode.find("if (!material || !material->IsValid())"),
        bindCode.find("meshRenderer.SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *material)"));
    EXPECT_NE(bindCode.find("meshRenderer.SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *material)"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetMaterialAtIndex(static_cast<uint8_t>(index), *material)"), std::string::npos);
    EXPECT_NE(bindCode.find("materialManager.RequestAsyncArtifact(task.materialPaths[index], true)"), std::string::npos)
        << "Cold generated material artifacts should be requested asynchronously instead of parsed on the drop path.";
    EXPECT_NE(bindCode.find("TrackRendererResourceAsyncInterest("), std::string::npos)
        << "A renderer resolution state must register one cancelable material interest per path instead of incrementing the global async request count on every retry.";
    EXPECT_NE(bindCode.find("materialManager.IsAsyncArtifactLoadPending(task.materialPaths[index])"), std::string::npos);
    EXPECT_NE(bindCode.find("materialManager.IsAsyncArtifactLoadFailed(task.materialPaths[index])"), std::string::npos);
    EXPECT_NE(bindCode.find("++stats->failedMaterialSlots"), std::string::npos)
        << "A material slot should fail only after the async artifact reader reports failure, not on a cold cache miss.";
    const auto textureBind = bindCode.find("if (!BindDeferredMaterialTextures(*material, task, state, stats, frameBudgetExpired))");
    ASSERT_NE(textureBind, std::string::npos);
    const auto taskFailedGuard = bindCode.find("if (task.failed)", textureBind);
    ASSERT_NE(taskFailedGuard, std::string::npos)
        << "A material with failed texture dependencies must not be exposed to rendering.";
    EXPECT_NE(bindCode.find("state.failed = true;", taskFailedGuard), std::string::npos);
    EXPECT_LT(taskFailedGuard, bindCode.find("meshRenderer.SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *material)"));
}

TEST(EditorRenderPathContractTests, GeneratedModelMaterialResolutionBindsCachedTexturesAndQueuesAsyncArtifactLoads)
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
    EXPECT_NE(textureBindCode.find("FindCachedTextureByEquivalentPath(textureManager, texturePath)"), std::string::npos)
        << "Generated model texture binding must reuse cached texture artifacts across Library/absolute path aliases.";
    EXPECT_EQ(textureBindCode.find("textureManager.GetResource(texturePath, true)"), std::string::npos);
    EXPECT_EQ(textureBindCode.find("LoadResource(texturePath)"), std::string::npos);
    EXPECT_EQ(textureBindCode.find("PumpAsyncLoads("), std::string::npos);
    EXPECT_NE(textureBindCode.find("textureManager.IsAsyncArtifactLoadPending(texturePath)"), std::string::npos);
    EXPECT_NE(textureBindCode.find("textureManager.IsAsyncArtifactLoadFailed(texturePath)"), std::string::npos);
    EXPECT_NE(textureBindCode.find("textureManager.RequestAsyncArtifact(texturePath, true)"), std::string::npos);
    EXPECT_NE(textureBindCode.find("TrackRendererResourceAsyncInterest("), std::string::npos)
        << "A renderer resolution state must register one cancelable texture interest per path instead of incrementing the global async request count on every retry.";
    const auto asyncRequest = textureBindCode.find("textureManager.RequestAsyncArtifact(texturePath, true)");
    const auto pendingTextureGuard = textureBindCode.find("if (!texture && textureManager.IsAsyncArtifactLoadPending(texturePath))", asyncRequest);
    const auto pendingTextureReturn = textureBindCode.find("return false;", pendingTextureGuard);
    const auto failedTextureGuard = textureBindCode.find("if (!texture && textureManager.IsAsyncArtifactLoadFailed(texturePath))", pendingTextureReturn);
    const auto failedTextureCount = textureBindCode.find("++stats->failedMaterialSlots", failedTextureGuard);
    const auto nextTextureSlotAdvance = textureBindCode.find("task.nextTextureSlot = textureIndex", asyncRequest);
    ASSERT_NE(asyncRequest, std::string::npos);
    ASSERT_NE(pendingTextureGuard, std::string::npos);
    ASSERT_NE(pendingTextureReturn, std::string::npos);
    ASSERT_NE(failedTextureGuard, std::string::npos);
    ASSERT_NE(failedTextureCount, std::string::npos);
    ASSERT_NE(nextTextureSlotAdvance, std::string::npos);
    EXPECT_LT(pendingTextureGuard, nextTextureSlotAdvance);
    EXPECT_LT(pendingTextureReturn, nextTextureSlotAdvance);
    EXPECT_LT(failedTextureGuard, nextTextureSlotAdvance);
    EXPECT_LT(failedTextureCount, nextTextureSlotAdvance);
    EXPECT_NE(textureBindCode.find("material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture)"), std::string::npos);
    EXPECT_NE(textureBindCode.find("frameBudgetExpired()"), std::string::npos);

    const auto resolutionStepBegin = actionsSource.find("void RunRendererResourceResolutionStep(");
    ASSERT_NE(resolutionStepBegin, std::string::npos);
    const auto resolutionStepEnd = actionsSource.find("void Editor::Core::EditorActions::LoadEmptyScene()", resolutionStepBegin);
    ASSERT_NE(resolutionStepEnd, std::string::npos);
    const auto resolutionStepCode = actionsSource.substr(resolutionStepBegin, resolutionStepEnd - resolutionStepBegin);
    EXPECT_NE(resolutionStepCode.find("textureManager.PumpAsyncLoads(kRendererResourceResolutionTextureBindsPerFrame)"), std::string::npos);
    EXPECT_EQ(
        resolutionStepCode.find(
            "textureManager.PumpAsyncLoads(kRendererResourceResolutionTextureBindsPerFrame)",
            resolutionStepCode.find("textureManager.PumpAsyncLoads(kRendererResourceResolutionTextureBindsPerFrame)") + 1u),
        std::string::npos);

    const auto textureManagerSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/ResourceManagement/TextureManager.cpp";
    const std::string textureManagerSource = ReadSourceText(textureManagerSourcePath);
    ASSERT_FALSE(textureManagerSource.empty());
    EXPECT_NE(textureManagerSource.find("std::optional<std::filesystem::file_time_type> writeTime;"), std::string::npos);
    EXPECT_NE(textureManagerSource.find("request.writeTime = writeTime;"), std::string::npos);
    EXPECT_NE(textureManagerSource.find("std::string runtimeSignature;"), std::string::npos);
    EXPECT_NE(textureManagerSource.find("failed->second.runtimeSignature == runtimeSignature"), std::string::npos);
    EXPECT_NE(textureManagerSource.find("{ request.realPath, request.writeTime, CurrentTextureRuntimeSignature() }"), std::string::npos);
    EXPECT_NE(textureManagerSource.find("bool TextureManager::IsAsyncArtifactLoadFailed"), std::string::npos);
    EXPECT_EQ(textureManagerSource.find("g_failedAsyncTextureArtifacts[request.path] = TryGetLastWriteTime"), std::string::npos);

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

TEST(EditorRenderPathContractTests, GeneratedModelMaterialResolutionFailsUnqueueableTextureMisses)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto textureBindBegin = source.find("bool BindDeferredMaterialTextures(");
    ASSERT_NE(textureBindBegin, std::string::npos);
    const auto textureBindEnd = source.find("template<typename FrameBudgetExpired>", textureBindBegin);
    ASSERT_NE(textureBindEnd, std::string::npos);
    const auto textureBindCode = source.substr(textureBindBegin, textureBindEnd - textureBindBegin);

    const auto request = textureBindCode.find("textureManager.RequestAsyncArtifact(texturePath, true)");
    const auto pendingGuard = textureBindCode.find("textureManager.IsAsyncArtifactLoadPending(texturePath)", request);
    const auto failedGuard = textureBindCode.find("textureManager.IsAsyncArtifactLoadFailed(texturePath)", pendingGuard);
    const auto unqueueableGuard = textureBindCode.find("if (!texture)", failedGuard + 1u);
    ASSERT_NE(request, std::string::npos);
    ASSERT_NE(pendingGuard, std::string::npos);
    ASSERT_NE(failedGuard, std::string::npos);
    ASSERT_NE(unqueueableGuard, std::string::npos);
    EXPECT_NE(textureBindCode.find("++stats->failedMaterialSlots", unqueueableGuard), std::string::npos);
    EXPECT_NE(textureBindCode.find("task.failed = true", unqueueableGuard), std::string::npos);
    EXPECT_NE(textureBindCode.find("task.nextTextureSlot = textureIndex", unqueueableGuard), std::string::npos);
    EXPECT_LT(unqueueableGuard, textureBindCode.find("material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture)"));
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionRestoresHiddenRootOnLiveCancellation)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto stepBegin = source.find("void RunRendererResourceResolutionStep(");
    ASSERT_NE(stepBegin, std::string::npos);
    const auto stepEnd = source.find("void Editor::Core::EditorActions::LoadEmptyScene()", stepBegin);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepCode = source.substr(stepBegin, stepEnd - stepBegin);

    EXPECT_NE(stepCode.find("auto finishCancelled = [&actions, &tracker, &state]"), std::string::npos);
    EXPECT_NE(stepCode.find("if (restoreRootVisibility)"), std::string::npos);
    EXPECT_NE(stepCode.find("RestoreRendererResourceResolutionRootVisibility(*state);"), std::string::npos);
    EXPECT_NE(stepCode.find("finishCancelled(false);"), std::string::npos);
    EXPECT_NE(stepCode.find("finishCancelled(true);"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionRollsBackHiddenRootOnFailure)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto rollbackBegin = source.find("void RollbackHiddenRendererResourceResolutionRoot(");
    ASSERT_NE(rollbackBegin, std::string::npos);
    const auto rollbackEnd = source.find("template<typename ComponentType>", rollbackBegin);
    ASSERT_NE(rollbackEnd, std::string::npos);
    const auto rollbackCode = source.substr(rollbackBegin, rollbackEnd - rollbackBegin);

    EXPECT_NE(rollbackCode.find("state.rootHiddenUntilRendererResourcesReady"), std::string::npos);
    EXPECT_NE(rollbackCode.find("actions.GetSelectedGameObject() == root"), std::string::npos);
    EXPECT_NE(rollbackCode.find("actions.UnselectGameObject();"), std::string::npos);
    EXPECT_NE(rollbackCode.find("scene->DestroyGameObject(*root);"), std::string::npos);
    EXPECT_NE(rollbackCode.find("state.rootHiddenUntilRendererResourcesReady = false;"), std::string::npos);
    EXPECT_EQ(rollbackCode.find("root->SetActive(state.restoreRootSelfActive);"), std::string::npos);

    const auto stepBegin = source.find("void RunRendererResourceResolutionStep(");
    ASSERT_NE(stepBegin, std::string::npos);
    const auto stepEnd = source.find("void Editor::Core::EditorActions::LoadEmptyScene()", stepBegin);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepCode = source.substr(stepBegin, stepEnd - stepBegin);

    const auto finishFailed = stepCode.find("auto finishFailed = [&actions, &tracker, &state]");
    ASSERT_NE(finishFailed, std::string::npos);
    const auto rollbackCall = stepCode.find("RollbackFailedRendererResourceResolutionRoot(actions, *state);", finishFailed);
    const auto releaseListener = stepCode.find("actions.ReleaseGameObjectDestroyedListener(state->destroyedListener);", finishFailed);
    ASSERT_NE(rollbackCall, std::string::npos);
    ASSERT_NE(releaseListener, std::string::npos);
    EXPECT_LT(rollbackCall, releaseListener);
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionPreservesVisibleCommittedRootOnFailure)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto rollbackBegin = source.find("void RollbackFailedRendererResourceResolutionRoot(");
    ASSERT_NE(rollbackBegin, std::string::npos);
    const auto rollbackEnd = source.find("template<typename ComponentType>", rollbackBegin);
    ASSERT_NE(rollbackEnd, std::string::npos);
    const auto rollbackCode = source.substr(rollbackBegin, rollbackEnd - rollbackBegin);

    const auto hiddenGuard = rollbackCode.find("if (state.rootHiddenUntilRendererResourcesReady)");
    const auto hiddenRollbackCall = rollbackCode.find("RollbackHiddenRendererResourceResolutionRoot(actions, state);");
    ASSERT_NE(hiddenGuard, std::string::npos);
    ASSERT_NE(hiddenRollbackCall, std::string::npos);
    EXPECT_LT(hiddenGuard, hiddenRollbackCall)
        << "Renderer-resource failure may only destroy the legacy hidden atomic root, never a visible saved/restored prefab instance.";
    EXPECT_EQ(rollbackCode.find("scene->DestroyGameObject(*root);"), std::string::npos)
        << "Visible saved/restored prefab instances must not be destroyed by the generic failure path.";
    EXPECT_NE(rollbackCode.find("RestoreRendererResourceResolutionRootVisibility(state);"), std::string::npos)
        << "Visible committed/restored instances must remain in the scene and simply be unhidden if needed.";

    const auto stepBegin = source.find("void RunRendererResourceResolutionStep(");
    ASSERT_NE(stepBegin, std::string::npos);
    const auto stepEnd = source.find("void Editor::Core::EditorActions::LoadEmptyScene()", stepBegin);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepCode = source.substr(stepBegin, stepEnd - stepBegin);

    const auto finishFailed = stepCode.find("auto finishFailed = [&actions, &tracker, &state]");
    ASSERT_NE(finishFailed, std::string::npos);
    EXPECT_NE(stepCode.find("RollbackFailedRendererResourceResolutionRoot(actions, *state);", finishFailed), std::string::npos);
    EXPECT_EQ(stepCode.find("RollbackHiddenRendererResourceResolutionRoot(actions, *state);", finishFailed), std::string::npos)
        << "Failure rollback must preserve visible committed/restored roots while still cleaning up hidden legacy roots.";
}

TEST(EditorRenderPathContractTests, MeshRendererRuntimeMaterialResolveDoesNotSynchronouslyLoadArtifacts)
{
    const auto meshRendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Components/MeshRenderer.cpp";

    const std::string source = ReadSourceText(meshRendererSourcePath);

    ASSERT_FALSE(source.empty());
    const auto resolveBegin = source.find("MeshRenderer::Material* MeshRenderer::ResolveMaterialSlot(");
    ASSERT_NE(resolveBegin, std::string::npos);
    const auto resolveEnd = source.find("MeshRenderer::Material* MeshRenderer::ResolveMaterialAtIndex(", resolveBegin);
    ASSERT_NE(resolveEnd, std::string::npos);
    const auto resolveCode = source.substr(resolveBegin, resolveEnd - resolveBegin);

    EXPECT_NE(resolveCode.find("FindCachedMaterialByEquivalentPath(materialManager, path)"), std::string::npos);
    EXPECT_EQ(resolveCode.find("LoadArtifactWithoutTextures(path)"), std::string::npos)
        << "Normal render/picking paths must not synchronously load cold material artifacts; generated model material loading belongs to the frame-budgeted editor resolution queue.";
    EXPECT_EQ(resolveCode.find("MaterialArtifactPathExists(path)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionCancelsMarkedDestroyedInstancesBeforeMoreWork)
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
    EXPECT_NE(resolveCode.find("!root || !root->IsAlive()"), std::string::npos)
        << "A prefab root marked for destruction must cancel renderer resource resolution before scheduling or binding more mesh/material work.";

    const auto taskObjectBegin = source.find("NLS::Engine::GameObject* ResolveLiveTaskObject(");
    ASSERT_NE(taskObjectBegin, std::string::npos);
    const auto taskObjectEnd = source.find("const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*>* EnsureRendererResourceLiveObjectIndex", taskObjectBegin);
    ASSERT_NE(taskObjectEnd, std::string::npos);
    const auto taskObjectCode = source.substr(taskObjectBegin, taskObjectEnd - taskObjectBegin);
    EXPECT_NE(taskObjectCode.find("!object || !object->IsAlive()"), std::string::npos)
        << "Child objects marked for destruction must be skipped even before Scene::CollectGarbages deletes them.";

    const auto meshLoadBegin = source.find("bool StartMeshArtifactLoad(");
    ASSERT_NE(meshLoadBegin, std::string::npos);
    const auto meshLoadEnd = source.find("std::optional<RendererResourceResolutionTask> PopNextRemainingTask(", meshLoadBegin);
    ASSERT_NE(meshLoadEnd, std::string::npos);
    const auto meshLoadCode = source.substr(meshLoadBegin, meshLoadEnd - meshLoadBegin);
    EXPECT_NE(meshLoadCode.find("cancelled"), std::string::npos)
        << "Background mesh artifact loads must observe cancellation so deleting a prefab does not keep CPU-heavy artifact work alive.";
}

TEST(EditorRenderPathContractTests, ReadyGeneratedModelDropDefersAssetResolution)
{
    const auto bridgeSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/EditorAssetDragDropBridge.cpp";
    const std::string source = ReadSourceText(bridgeSourcePath);

    ASSERT_FALSE(source.empty());
    const auto instantiateBegin = source.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::InstantiateImportedAsset(");
    ASSERT_NE(instantiateBegin, std::string::npos);
    const auto instantiateEnd = source.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::InstantiateImportedAsset(\n    AssetDatabaseFacade&", instantiateBegin);
    ASSERT_NE(instantiateEnd, std::string::npos);
    const auto instantiateCode = source.substr(instantiateBegin, instantiateEnd - instantiateBegin);

    EXPECT_EQ(instantiateCode.find("{}," "\n        assetType == NLS::Core::Assets::AssetType::ModelScene"), std::string::npos)
        << "Ready generated model drops should instantiate from stabilized import results without entering the legacy deferred-resolution path.";
    const auto deferredResolutionComment = instantiateCode.find("Keep Scene View mouse release cheap");
    ASSERT_NE(deferredResolutionComment, std::string::npos);
    EXPECT_NE(instantiateCode.find("true\n    });", deferredResolutionComment), std::string::npos)
        << "Ready imported model drops should defer mesh/material resolution so final mouse release stays non-blocking.";
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionAllowsColdMaterialSlots)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto stepBegin = source.find("void RunRendererResourceResolutionStep(");
    ASSERT_NE(stepBegin, std::string::npos);
    const auto stepEnd = source.find("void Editor::Core::EditorActions::LoadEmptyScene()", stepBegin);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepCode = source.substr(stepBegin, stepEnd - stepBegin);

    EXPECT_EQ(stepCode.find("state->stats->unresolvedMaterialSlots > 0u"), std::string::npos)
        << "Cold material artifacts must not fail or roll back an otherwise visible generated-model drop.";
    EXPECT_NE(stepCode.find("state->stats->failedMaterialSlots > 0u"), std::string::npos)
        << "Explicit material texture failures should still fail the resolution job.";
    EXPECT_NE(stepCode.find("tracker.ReportProgress(state->job, NLS::Editor::Assets::ImportPhase::Postprocess, 1.0, \"Renderer resources ready\")"), std::string::npos);
}

TEST(EditorRenderPathContractTests, MaterialArtifactPrewarmDoesNotSynchronouslyLoadShaderDependencies)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/ResourceManagement/MaterialManager.cpp";

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

TEST(EditorRenderPathContractTests, ResourcePathUpdatesDoNotUseOffsetReinterpretCasts)
{
    const std::array<std::filesystem::path, 2u> sourcePaths = {
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/ResourceManagement/MaterialManager.cpp",
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp"
    };

    for (const auto& sourcePath : sourcePaths)
    {
        const std::string source = ReadSourceText(sourcePath);
        ASSERT_FALSE(source.empty()) << sourcePath.string();
        EXPECT_EQ(source.find("reinterpret_cast<std::string*>"), std::string::npos)
            << sourcePath.string();
        EXPECT_EQ(source.find("offsetof("), std::string::npos)
            << sourcePath.string();
        EXPECT_EQ(source.find("const_cast<std::string&>"), std::string::npos)
            << sourcePath.string();
    }
}

TEST(EditorRenderPathContractTests, PreparedObjectDataReservationUsesFenceGatedPublicationPath)
{
    const auto providerSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp";
    const std::string source = ReadSourceText(providerSourcePath);

    ASSERT_FALSE(source.empty());
    const auto resolveBegin = source.find("std::optional<size_t> EngineFrameObjectBindingProvider::ResolveActiveObjectDataSlotIndex()");
    ASSERT_NE(resolveBegin, std::string::npos);
    const auto resolveEnd = source.find("EngineFrameObjectBindingProvider::ObjectDataFrameSlot* EngineFrameObjectBindingProvider::ResolveActiveObjectDataSlot()", resolveBegin);
    ASSERT_NE(resolveEnd, std::string::npos);
    const auto resolveCode = source.substr(resolveBegin, resolveEnd - resolveBegin);

    EXPECT_NE(
        resolveCode.find("ReserveReusableFrameContextSlotIndexForPreparedPublication"),
        std::string::npos);
    EXPECT_EQ(
        resolveCode.find("ReserveReusableFrameContextSlotIndex(m_renderer.GetDriver())"),
        std::string::npos);
}

TEST(EditorRenderPathContractTests, MaterialArtifactLoadWithoutTexturesLoadsShaderButDefersTextureDependencies)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/ResourceManagement/MaterialManager.cpp";

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
    const auto unresolvedMaterialBegin = bindCode.find("if (!material || !material->IsValid())");
    ASSERT_NE(unresolvedMaterialBegin, std::string::npos);
    const auto unresolvedMaterialEnd = bindCode.find("if (!BindDeferredMaterialTextures", unresolvedMaterialBegin);
    ASSERT_NE(unresolvedMaterialEnd, std::string::npos);
    const auto unresolvedMaterialCode = bindCode.substr(
        unresolvedMaterialBegin,
        unresolvedMaterialEnd - unresolvedMaterialBegin);
    EXPECT_EQ(unresolvedMaterialCode.find("++stats->unresolvedMaterialSlots"), std::string::npos)
        << "A material path that is neither cached nor pending is terminal; unresolved success leaves RenderScene suppressing the draw forever.";
    EXPECT_NE(unresolvedMaterialCode.find("++stats->failedMaterialSlots"), std::string::npos);
    EXPECT_NE(unresolvedMaterialCode.find("task.failed = true"), std::string::npos);
    EXPECT_NE(unresolvedMaterialCode.find("state.failed = true"), std::string::npos);
    EXPECT_NE(source.find("kRendererResourceResolutionMaterialSlotsPerTask"), std::string::npos);
    EXPECT_NE(bindCode.find("visitedSlots < kRendererResourceResolutionMaterialSlotsPerTask"), std::string::npos);
    EXPECT_EQ(bindCode.find("materialManager.PrewarmArtifact("), std::string::npos);
    EXPECT_NE(bindCode.find("frameBudgetExpired()"), std::string::npos);
    EXPECT_NE(bindCode.find("return false;"), std::string::npos);
    EXPECT_NE(bindCode.find("return task.nextMaterialSlot >= task.materialPaths.size()"), std::string::npos);

    EXPECT_EQ(unresolvedMaterialCode.find("break;"), std::string::npos);
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
    EXPECT_NE(loadCode.find("NLS::Render::Assets::LoadMeshArtifact(artifactPath, state->cancelled.get())"), std::string::npos);
    EXPECT_EQ(loadCode.find("NLS::Render::Assets::LoadMeshArtifact(artifactPath)"), std::string::npos);
    EXPECT_NE(loadCode.find("RendererResourceResolutionState& resolutionState"), std::string::npos);
    EXPECT_NE(loadCode.find("std::lock_guard lock(resolutionState.asyncLoadsMutex)"), std::string::npos);
    EXPECT_NE(loadCode.find("resolutionState.meshLoadsByPath.find(task.modelPath)"), std::string::npos);
    EXPECT_NE(loadCode.find("resolutionState.meshLoadsByPath.emplace(task.modelPath, task.meshLoad)"), std::string::npos);
    EXPECT_NE(loadCode.find("task.meshLoad = existingLoad->second"), std::string::npos);
    EXPECT_NE(loadCode.find("catch (const std::exception& exception)"), std::string::npos);
    EXPECT_NE(loadCode.find("state->completed = true"), std::string::npos);
    EXPECT_EQ(bindCode.find("Render::Resources::MeshBufferUploadMode::GpuOnly"), std::string::npos);
    EXPECT_EQ(bindCode.find("modelManager[task.modelPath]"), std::string::npos);
    EXPECT_EQ(bindCode.find("meshRenderer.SetModel(gpuModel)"), std::string::npos);
    EXPECT_NE(bindCode.find("FindCachedMeshByEquivalentPath(meshManager, task.modelPath)"), std::string::npos)
        << "Generated model mesh binding must reuse cached mesh artifacts across Library/absolute path aliases.";
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
    const auto headerPath = std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.h";
    const std::string header = ReadSourceText(headerPath);
    ASSERT_FALSE(header.empty());
    const auto stateBegin = header.find("struct PrefabInstanceMeshArtifactLoadState");
    ASSERT_NE(stateBegin, std::string::npos);
    const auto stateEnd = header.find("struct PrefabInstancePreviewResourceHandoff", stateBegin);
    ASSERT_NE(stateEnd, std::string::npos);
    const auto stateCode = header.substr(stateBegin, stateEnd - stateBegin);
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
    EXPECT_NE(bindCode.find("auto* cached = FindCachedMeshByEquivalentPath(meshManager, task.modelPath)"), std::string::npos);
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
    const auto headerPath = std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.h";
    const std::string header = ReadSourceText(headerPath);
    ASSERT_FALSE(header.empty());

    const auto taskBegin = source.find("struct RendererResourceResolutionTask");
    ASSERT_NE(taskBegin, std::string::npos);
    const auto rendererStateBegin = source.find("struct RendererResourceResolutionState", taskBegin);
    ASSERT_NE(rendererStateBegin, std::string::npos);
    const auto taskCode = source.substr(taskBegin, rendererStateBegin - taskBegin);
    const auto loadStateBegin = header.find("struct PrefabInstanceMeshArtifactLoadState");
    ASSERT_NE(loadStateBegin, std::string::npos);
    const auto loadStateEnd = header.find("struct PrefabInstancePreviewResourceHandoff", loadStateBegin);
    ASSERT_NE(loadStateEnd, std::string::npos);
    const auto loadStateCode = header.substr(loadStateBegin, loadStateEnd - loadStateBegin);

    EXPECT_NE(taskCode.find("bool failed = false"), std::string::npos);
    EXPECT_NE(taskCode.find("std::shared_ptr<MeshArtifactLoadState> meshLoad"), std::string::npos);
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
    EXPECT_NE(finalCode.find("if (state->failed ||"), std::string::npos);
    EXPECT_NE(finalCode.find("state->stats->failedMaterialSlots > 0u"), std::string::npos);
    EXPECT_EQ(finalCode.find("state->stats->unresolvedMaterialSlots > 0u"), std::string::npos)
        << "Missing cold material slots should keep the mesh visible with fallback material instead of failing the drop.";
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
    EXPECT_NE(payloadCode.find("return CreateGameObjectFromAsset(resourcePath, focusOnCreation, p_parent, placementOverride)"), std::string::npos);
    EXPECT_EQ(payloadCode.find("Asset drag handle is not imported yet; waiting for background preimport"), std::string::npos);
    EXPECT_EQ(payloadCode.find("return nullptr;\n    }\n\n    if (!result.handled"), std::string::npos);
}

TEST(EditorRenderPathContractTests, ImportedPayloadDropFailuresLogBridgeDiagnostics)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto payloadBegin =
        source.find("Engine::GameObject* NLS::Editor::Core::EditorActions::CreateGameObjectFromAsset(\n    const NLS::Editor::Assets::EditorAssetDragPayload& payload");
    ASSERT_NE(payloadBegin, std::string::npos);
    const auto completionBegin =
        source.find("void NLS::Editor::Core::EditorActions::CompletePendingAssetDrop(", payloadBegin);
    ASSERT_NE(completionBegin, std::string::npos);
    const auto payloadCode = source.substr(payloadBegin, completionBegin - payloadBegin);

    EXPECT_NE(payloadCode.find("for (const auto& diagnostic : result.dragDrop.diagnostics)"), std::string::npos);
    EXPECT_NE(payloadCode.find("Imported asset drag diagnostic code="), std::string::npos);
    EXPECT_NE(payloadCode.find("payloadSubAssetKey="), std::string::npos);
    EXPECT_NE(payloadCode.find("generatedModelPrefab="), std::string::npos);
    EXPECT_NE(payloadCode.find("dragDropStatus="), std::string::npos);
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
    EXPECT_NE(primitiveFactorySource.find("builtin:Primitive/"), std::string::npos);
    EXPECT_EQ(actionsSource.find("\":Models\\\\Cube.fbx\""), std::string::npos);
    EXPECT_EQ(menuSource.find("\":Models\\\\"), std::string::npos);
    EXPECT_EQ(sceneManagerSource.find("Validation Cube"), std::string::npos);
    EXPECT_EQ(sceneManagerSource.find("PrimitiveType::Cube"), std::string::npos);
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
    EXPECT_NE(stateCode.find("std::atomic_bool cancelled = false"), std::string::npos);
    EXPECT_NE(stateCode.find("ListenerID destroyedListener ="), std::string::npos);
    EXPECT_NE(stateCode.find("NLS::Editor::Core::EditorActions::SceneMutationToken sceneToken"), std::string::npos);
    EXPECT_NE(stateCode.find("const NLS::Editor::Assets::PrefabInstanceRecord* cachedLiveInstance"), std::string::npos);
    EXPECT_NE(stateCode.find("RendererResourceLiveObjectIndex liveObjects"), std::string::npos);
    EXPECT_EQ(stateCode.find("NLS::Engine::GameObject* instanceRoot"), std::string::npos);
    EXPECT_EQ(stepCode.find("const auto liveObjectsBySourceId = BuildPrefabInstanceObjectIndex(*liveInstance)"), std::string::npos);
    EXPECT_NE(stepCode.find("auto* liveInstance = ResolveLivePrefabInstance(actions, *state)"), std::string::npos);
    EXPECT_NE(stepCode.find("const auto* liveObjectsBySourceId = EnsureRendererResourceLiveObjectIndex(*state, *liveInstance)"), std::string::npos);
    EXPECT_NE(stepCode.find("RunRendererResourceResolutionTask(\n            actions,\n            *liveInstance,\n            task,\n            *state,\n            *liveObjectsBySourceId"), std::string::npos);
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

    const auto sceneFileTarget = sceneViewSource.find("AcceptDragDropPayload(\"File\"");
    const auto sceneAssetTarget =
        sceneViewSource.find("AcceptDragDropPayload(\n        NLS::Editor::Assets::kEditorAssetDragPayloadType");
    ASSERT_NE(sceneFileTarget, std::string::npos);
    ASSERT_NE(sceneAssetTarget, std::string::npos);
    const auto sceneLegacyDropEnd = sceneViewSource.find("UI::EndDragDropTarget();", sceneFileTarget);
    ASSERT_NE(sceneLegacyDropEnd, std::string::npos);
    const auto sceneLegacyDropCode = sceneViewSource.substr(sceneFileTarget, sceneLegacyDropEnd - sceneFileTarget);
    EXPECT_NE(sceneLegacyDropCode.find("EFileType::SCENE"), std::string::npos);
    EXPECT_EQ(sceneLegacyDropCode.find("EFileType::PREFAB"), std::string::npos)
        << "Project prefab drops in Scene View must use the EditorAsset payload so hover preview and resource handoff run before mouse release.";
    EXPECT_EQ(sceneLegacyDropCode.find("EFileType::MODEL"), std::string::npos)
        << "Project model drops in Scene View must use the EditorAsset payload; the legacy File path has no before-delivery mesh preview.";
    EXPECT_NE(sceneLegacyDropCode.find("IsBuiltInResourcePath(path)"), std::string::npos)
        << "The legacy File path should only preserve built-in primitive resource drops.";
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

    const auto assetPayloadDeclaration = assetBrowserSource.find("const auto assetPayload = p_isEngineItem");
    ASSERT_NE(assetPayloadDeclaration, std::string::npos);
    EXPECT_NE(assetBrowserSource.find("const bool assetPayloadReplacesFileDrag"), std::string::npos);
    EXPECT_NE(assetBrowserSource.find("fileType == Utils::PathParser::EFileType::MODEL"), std::string::npos);
    EXPECT_NE(assetBrowserSource.find("fileType == Utils::PathParser::EFileType::PREFAB"), std::string::npos);
    EXPECT_NE(assetBrowserSource.find("if (!assetPayloadReplacesFileDrag)"), std::string::npos)
        << "Only model/prefab EditorAsset drags should replace the legacy File payload; texture/material/shader rows still need File for AssetView legacy drop.";
    const auto fileDragSource = assetBrowserSource.find(
        "fileDragSource = &clickableText.AddPlugin<UI::DDSource<std::pair<std::string, Group*>>>",
        assetPayloadDeclaration);
    ASSERT_NE(fileDragSource, std::string::npos);
    const auto editorAssetDragSource = assetBrowserSource.find(
        "clickableText.AddPlugin<UI::DDSource<NLS::Editor::Assets::EditorAssetDragPayload>>",
        fileDragSource);
    ASSERT_NE(editorAssetDragSource, std::string::npos);
    const auto browserDragCode = assetBrowserSource.substr(
        assetPayloadDeclaration,
        editorAssetDragSource - assetPayloadDeclaration);
    EXPECT_NE(browserDragCode.find("if (!assetPayloadReplacesFileDrag)"), std::string::npos)
        << "Model/prefab rows with EditorAsset payloads must not also publish the legacy File payload, otherwise Scene View preview cannot receive AcceptBeforeDelivery EditorAsset events.";
    EXPECT_NE(assetBrowserSource.find("editorAssetDragSource->hasTooltip = !assetPayloadReplacesFileDrag;"), std::string::npos)
        << "Model/prefab Scene View drags should rely on the mesh ghost instead of keeping the AssetBrowser text tooltip stuck to the cursor.";
    EXPECT_NE(assetBrowserSource.find("subAssetDragSource.hasTooltip = false;"), std::string::npos)
        << "Generated model sub-asset drags should not obscure the Scene View mesh preview with the source tooltip.";

    const auto renameHandler = assetBrowserSource.find("contextMenu->RenamedEvent +=");
    ASSERT_NE(renameHandler, std::string::npos);
    const auto duplicateHandler = assetBrowserSource.find("contextMenu->DuplicateEvent +=", renameHandler);
    ASSERT_NE(duplicateHandler, std::string::npos);
    const auto renameCode = assetBrowserSource.substr(renameHandler, duplicateHandler - renameHandler);
    EXPECT_NE(renameCode.find("editorAssetDragSource"), std::string::npos);
    EXPECT_NE(renameCode.find("BuildEditorAssetDragPayloadForFile("), std::string::npos);
    EXPECT_NE(renameCode.find("editorAssetDragSource->data = *updatedPayload;"), std::string::npos);
    EXPECT_NE(renameCode.find("editorAssetDragSource->tooltip = newResourceFormatPath;"), std::string::npos);

    const auto assetViewFileTarget =
        assetViewSource.find("DDTarget<std::pair<std::string, UI::Widgets::Group*>>>(\"File\")");
    ASSERT_NE(assetViewFileTarget, std::string::npos);
    const auto assetViewLegacyDropCode = assetViewSource.substr(assetViewFileTarget);
    EXPECT_EQ(assetViewLegacyDropCode.find("EFileType::MODEL"), std::string::npos);
    EXPECT_EQ(assetViewLegacyDropCode.find("ModelManager>().GetResource(path"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DragDropTargetExposesPreviewPayloadBeforeDelivery)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto dragDropHeaderPath = root / "Runtime/UI/Plugins/DragDrop.h";
    const auto dragDropSourcePath = root / "Runtime/UI/Plugins/DragDrop.cpp";
    const auto ddTargetPath = root / "Runtime/UI/Plugins/DDTarget.h";

    const std::string dragDropHeader = ReadSourceText(dragDropHeaderPath);
    const std::string dragDropSource = ReadSourceText(dragDropSourcePath);
    const std::string ddTarget = ReadSourceText(ddTargetPath);

    ASSERT_FALSE(dragDropHeader.empty());
    ASSERT_FALSE(dragDropSource.empty());
    ASSERT_FALSE(ddTarget.empty());

    EXPECT_NE(dragDropHeader.find("bool delivered = false;"), std::string::npos);
    EXPECT_NE(dragDropHeader.find("AcceptBeforeDelivery"), std::string::npos);
    EXPECT_NE(dragDropSource.find("ImGuiDragDropFlags_AcceptBeforeDelivery"), std::string::npos);
    EXPECT_NE(dragDropSource.find("payload->IsDelivery()"), std::string::npos);
    EXPECT_NE(ddTarget.find("Event<T> PreviewReceivedEvent;"), std::string::npos);
    EXPECT_NE(ddTarget.find("bool acceptBeforeDelivery = false;"), std::string::npos);
    EXPECT_NE(ddTarget.find("bool m_isHovered = false;"), std::string::npos);
    EXPECT_NE(ddTarget.find("if (payload.delivered)"), std::string::npos);
    EXPECT_NE(ddTarget.find("DataReceivedEvent.Invoke(data);"), std::string::npos);
    EXPECT_NE(ddTarget.find("PreviewReceivedEvent.Invoke(data);"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewAssetDropUsesBeforeDeliveryPreviewLifecycle)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string sceneViewHeader = ReadSourceText(root / "Project/Editor/Panels/SceneView.h");
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");
    const std::string dragDropHeader = ReadSourceText(root / "Runtime/UI/Plugins/DragDrop.h");
    const std::string dragDropSource = ReadSourceText(root / "Runtime/UI/Plugins/DragDrop.cpp");

    ASSERT_FALSE(sceneViewHeader.empty());
    ASSERT_FALSE(sceneViewSource.empty());
    ASSERT_FALSE(dragDropHeader.empty());
    ASSERT_FALSE(dragDropSource.empty());

    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewPayload"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewMousePos"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewPlacement"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("UpdateImportedAssetDragPreview"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("PumpImportedAssetDragPreviewBeforeRender"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("DrawImportedAssetDragPreview"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("ResolveImportedAssetDragPreviewPlacement"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("ClearImportedAssetDragPreview"), std::string::npos);

    EXPECT_NE(sceneViewSource.find("HandleViewportAssetDragDrop();"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("UI::DragDropTargetFlags::AcceptBeforeDelivery"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("payloadView.delivered"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("UpdateImportedAssetDragPreview(payload);"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("if (!UI::BeginDragDropTarget())"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("ClearImportedAssetDragPreview();"), std::string::npos);
    EXPECT_NE(dragDropHeader.find("PeekDragDropPayload"), std::string::npos);
    EXPECT_NE(dragDropSource.find("ImGui::GetDragDropPayload()"), std::string::npos);

    const auto initFrameBegin = sceneViewSource.find("void Editor::Panels::SceneView::InitFrame()");
    ASSERT_NE(initFrameBegin, std::string::npos);
    const auto initFrameEnd = sceneViewSource.find("Engine::SceneSystem::Scene* Editor::Panels::SceneView::GetScene()", initFrameBegin);
    ASSERT_NE(initFrameEnd, std::string::npos);
    const auto initFrameCode = sceneViewSource.substr(initFrameBegin, initFrameEnd - initFrameBegin);
    EXPECT_NE(initFrameCode.find("PumpImportedAssetDragPreviewBeforeRender();"), std::string::npos)
        << "Scene View must peek the active drag payload before AView::InitFrame builds the SceneDescriptor so the mesh ghost scene is rendered on the same frame.";
    EXPECT_LT(
        initFrameCode.find("PumpImportedAssetDragPreviewBeforeRender();"),
        initFrameCode.find("AViewControllable::InitFrame();"));

    const auto preRenderBegin = sceneViewSource.find("void Editor::Panels::SceneView::DrawPreRenderViewportOverlay()");
    ASSERT_NE(preRenderBegin, std::string::npos);
    const auto preRenderEnd = sceneViewSource.find("void Editor::Panels::SceneView::OnAfterDrawWidgets()", preRenderBegin);
    ASSERT_NE(preRenderEnd, std::string::npos);
    const auto preRenderCode = sceneViewSource.substr(preRenderBegin, preRenderEnd - preRenderBegin);
    EXPECT_EQ(preRenderCode.find("PumpImportedAssetDragPreviewBeforeRender();"), std::string::npos)
        << "Overlay-stage preview pumping is too late for the current frame's CreateSceneDescriptor.";

    const auto previewBegin = sceneViewSource.find("void Editor::Panels::SceneView::UpdateImportedAssetDragPreview(");
    ASSERT_NE(previewBegin, std::string::npos);
    const auto previewEnd = sceneViewSource.find("bool Editor::Panels::SceneView::EnsureImportedAssetDragPreviewMeshGhost(", previewBegin);
    ASSERT_NE(previewEnd, std::string::npos);
    const auto previewCode = sceneViewSource.substr(previewBegin, previewEnd - previewBegin);
    EXPECT_NE(previewCode.find("m_importedAssetDragPreviewPayload = payload;"), std::string::npos);
    EXPECT_NE(previewCode.find("GetMousePosition()"), std::string::npos);
    EXPECT_NE(previewCode.find("ResolveImportedAssetDragPreviewPlacement"), std::string::npos);
    EXPECT_EQ(previewCode.find("CreateGameObjectFromAsset"), std::string::npos);
    EXPECT_EQ(previewCode.find("Scene::AddGameObject"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewAssetDropUsesSingleViewportDragTargetForPreviewAndLegacyDrop)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string sceneViewHeader = ReadSourceText(root / "Project/Editor/Panels/SceneView.h");
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");

    ASSERT_FALSE(sceneViewHeader.empty());
    ASSERT_FALSE(sceneViewSource.empty());

    EXPECT_NE(sceneViewHeader.find("HandleViewportAssetDragDrop"), std::string::npos);
    EXPECT_EQ(sceneViewSource.find("m_image->AddPlugin<UI::DDTarget"), std::string::npos)
        << "Scene View must not stack multiple DDTarget plugins on the same viewport image; the first target can consume the ImGui target scope and starve EditorAsset AcceptBeforeDelivery preview events.";
    EXPECT_NE(sceneViewSource.find("void Editor::Panels::SceneView::HandleViewportAssetDragDrop()"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("BeginDragDropTarget()"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("AcceptDragDropPayload(\n        NLS::Editor::Assets::kEditorAssetDragPayloadType"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("DragDropTargetFlags::AcceptBeforeDelivery"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("UpdateImportedAssetDragPreview(payload);"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("AcceptDragDropPayload(\"File\""), std::string::npos);
    EXPECT_NE(sceneViewSource.find("CreateGameObjectFromAsset(path, true)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewAssetDropTargetExecutesOnViewportImageItem)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string sceneViewHeader = ReadSourceText(root / "Project/Editor/Panels/SceneView.h");
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");

    ASSERT_FALSE(sceneViewHeader.empty());
    ASSERT_FALSE(sceneViewSource.empty());

    EXPECT_NE(sceneViewHeader.find("class ViewportDragDropTarget;"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("class Editor::Panels::SceneView::ViewportDragDropTarget final : public UI::IPlugin"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("m_image->AddPlugin<ViewportDragDropTarget>(*this);"), std::string::npos)
        << "The Scene View drop target must execute as the viewport image plugin so ImGui::BeginDragDropTarget binds to the image item, not to a later overlay/panel item.";

    const auto afterBegin = sceneViewSource.find("void Editor::Panels::SceneView::OnAfterDrawWidgets()");
    ASSERT_NE(afterBegin, std::string::npos);
    const auto afterEnd = sceneViewSource.find("void Editor::Panels::SceneView::AfterRenderFrame()", afterBegin);
    ASSERT_NE(afterEnd, std::string::npos);
    const auto afterCode = sceneViewSource.substr(afterBegin, afterEnd - afterBegin);
    EXPECT_EQ(afterCode.find("HandleViewportAssetDragDrop();"), std::string::npos)
        << "Calling BeginDragDropTarget after DrawWidgets can miss the viewport image because ImGui uses the last submitted item.";
}

TEST(EditorRenderPathContractTests, SceneViewAssetDropUsesOnlyReadyMeshMaterialTexturePreviewAndCommitsAtPreviewPlacement)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string sceneViewHeader = ReadSourceText(root / "Project/Editor/Panels/SceneView.h");
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");
    const std::string editorActionsHeader = ReadSourceText(root / "Project/Editor/Core/EditorActions.h");
    const std::string editorActionsSource = ReadSourceText(root / "Project/Editor/Core/EditorActions.cpp");

    const auto drawBegin = sceneViewSource.find("void Editor::Panels::SceneView::DrawImportedAssetDragPreview()");
    ASSERT_NE(drawBegin, std::string::npos);
    const auto drawEnd = sceneViewSource.find("void Editor::Panels::SceneView::DrawViewportOverlay()", drawBegin);
    ASSERT_NE(drawEnd, std::string::npos);
    const auto drawCode = sceneViewSource.substr(drawBegin, drawEnd - drawBegin);
    EXPECT_EQ(drawCode.find("GetDebugDrawService()"), std::string::npos);
    EXPECT_EQ(drawCode.find("SubmitBox"), std::string::npos);
    EXPECT_EQ(drawCode.find("AddText"), std::string::npos)
        << "Scene View imported model/prefab drags must not fall back to a filename label.";
    EXPECT_EQ(drawCode.find("submittedSceneGhost"), std::string::npos);
    EXPECT_EQ(drawCode.find("AddCircle"), std::string::npos);
    EXPECT_EQ(drawCode.find("AddCircleFilled"), std::string::npos);
    EXPECT_EQ(drawCode.find("AddLine"), std::string::npos)
        << "Scene View imported model/prefab drags must not draw a UI crosshair/proxy; only the ready mesh/material/texture preview scene may be visible.";
    EXPECT_EQ(drawCode.find("const bool previewHasBoundMesh ="), std::string::npos);
    EXPECT_EQ(drawCode.find("if (!previewHasBoundMesh)"), std::string::npos);
    EXPECT_EQ(drawCode.find("CreateGameObjectFromAsset"), std::string::npos);
    EXPECT_EQ(drawCode.find("Scene::AddGameObject"), std::string::npos);

    const auto dropHandler = sceneViewSource.find("void Editor::Panels::SceneView::HandleViewportAssetDragDrop()");
    ASSERT_NE(dropHandler, std::string::npos);
    const auto dropEnd = sceneViewSource.find("void Editor::Panels::SceneView::DrawImportedAssetDragPreview()", dropHandler);
    ASSERT_NE(dropEnd, std::string::npos);
    const auto dropCode = sceneViewSource.substr(dropHandler, dropEnd - dropHandler);
    EXPECT_NE(dropCode.find("m_importedAssetDragPreviewPlacement"), std::string::npos);
    EXPECT_NE(dropCode.find("ResolveImportedAssetDragPreviewPlacement(EDITOR_CONTEXT(inputManager)->GetMousePosition())"), std::string::npos);
    EXPECT_NE(dropCode.find("CollectImportedAssetDragPreviewResourceHandoff()"), std::string::npos);
    EXPECT_NE(dropCode.find("std::move(previewResourceHandoff)"), std::string::npos);
    EXPECT_NE(dropCode.find("CreateGameObjectFromImportedPrefabArtifact("), std::string::npos);
    EXPECT_NE(dropCode.find("CreateGameObjectFromAsset(payload, true, nullptr, previewPlacement)"), std::string::npos);

    EXPECT_NE(editorActionsHeader.find("std::optional<Maths::Vector3> placementOverride"), std::string::npos);
    EXPECT_NE(editorActionsSource.find("placementOverride.has_value()"), std::string::npos);
    EXPECT_NE(editorActionsSource.find("instance.GetTransform()->SetWorldPosition(*placementOverride);"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewFinalDropReusesPreviewPrefabArtifactWhenAvailable)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string sceneViewHeader = ReadSourceText(root / "Project/Editor/Panels/SceneView.h");
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");
    const std::string editorActionsHeader = ReadSourceText(root / "Project/Editor/Core/EditorActions.h");
    const std::string bridgeHeader = ReadSourceText(root / "Project/Editor/Assets/EditorAssetDragDropBridge.h");
    const std::string bridgeSource = ReadSourceText(root / "Project/Editor/Assets/EditorAssetDragDropBridge.cpp");

    ASSERT_FALSE(sceneViewHeader.empty());
    ASSERT_FALSE(sceneViewSource.empty());
    ASSERT_FALSE(editorActionsHeader.empty());
    ASSERT_FALSE(bridgeHeader.empty());
    ASSERT_FALSE(bridgeSource.empty());

    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewArtifact"), std::string::npos);
    EXPECT_NE(editorActionsHeader.find("CreateGameObjectFromImportedPrefabArtifact"), std::string::npos);
    EXPECT_NE(bridgeHeader.find("DropImportedPrefabArtifactIntoHierarchy"), std::string::npos);

    const auto ensureBegin = sceneViewSource.find("bool Editor::Panels::SceneView::EnsureImportedAssetDragPreviewMeshGhost(");
    ASSERT_NE(ensureBegin, std::string::npos);
    const auto ensureEnd = sceneViewSource.find("std::optional<Maths::Vector3> Editor::Panels::SceneView::ResolveImportedAssetDragPreviewPlacement(", ensureBegin);
    ASSERT_NE(ensureEnd, std::string::npos);
    const auto ensureCode = sceneViewSource.substr(ensureBegin, ensureEnd - ensureBegin);
    EXPECT_NE(ensureCode.find("m_importedAssetDragPreviewArtifact = std::move(*prefab);"), std::string::npos);
    EXPECT_NE(ensureCode.find("InstantiatePrefabArtifact("), std::string::npos);
    EXPECT_NE(ensureCode.find("*m_importedAssetDragPreviewArtifact"), std::string::npos);
    EXPECT_NE(ensureCode.find("previewLoadPolicy"), std::string::npos);
    EXPECT_NE(ensureCode.find("previewLoadPolicy.deferAssetReferenceResolution = true;"), std::string::npos)
        << "Scene View hover preview must not synchronously prewarm large mesh/material artifacts.";

    const auto dropBegin = sceneViewSource.find("void Editor::Panels::SceneView::HandleViewportAssetDragDrop()");
    ASSERT_NE(dropBegin, std::string::npos);
    const auto dropEnd = sceneViewSource.find("void Editor::Panels::SceneView::DrawImportedAssetDragPreview()", dropBegin);
    ASSERT_NE(dropEnd, std::string::npos);
    const auto dropCode = sceneViewSource.substr(dropBegin, dropEnd - dropBegin);
    EXPECT_NE(dropCode.find("auto previewArtifact = std::move(m_importedAssetDragPreviewArtifact);"), std::string::npos);
    EXPECT_NE(dropCode.find("if (previewArtifact.has_value())"), std::string::npos);
    EXPECT_NE(dropCode.find("CollectImportedAssetDragPreviewResourceHandoff()"), std::string::npos);
    EXPECT_NE(dropCode.find("CreateGameObjectFromImportedPrefabArtifact("), std::string::npos);
    EXPECT_NE(dropCode.find("std::move(previewResourceHandoff)"), std::string::npos)
        << "Final drop must transfer completed preview mesh loads to the formal instance instead of discarding them and reloading on mouse release.";
    EXPECT_NE(dropCode.find("CreateGameObjectFromAsset(payload, true, nullptr, previewPlacement)"), std::string::npos)
        << "A fallback keeps not-yet-previewed payload drops working.";

    EXPECT_NE(bridgeSource.find("bool IsImportedPrefabArtifactCurrentForPayload("), std::string::npos);
    const auto freshnessBegin = bridgeSource.find("bool IsImportedPrefabArtifactCurrentForPayload(");
    ASSERT_NE(freshnessBegin, std::string::npos);
    const auto freshnessEnd = bridgeSource.find("}\n\n}", freshnessBegin);
    ASSERT_NE(freshnessEnd, std::string::npos);
    const auto freshnessCode = bridgeSource.substr(freshnessBegin, freshnessEnd - freshnessBegin);
    EXPECT_NE(freshnessCode.find("validateRendererDependencies"), std::string::npos);
    EXPECT_NE(freshnessCode.find("ValidateGeneratedModelRendererArtifactsReady("), std::string::npos)
        << "Explicit deep validation is allowed only for non-release paths that can afford synchronous artifact reads.";
    EXPECT_NE(freshnessCode.find("GeneratedModelRendererArtifactFilesExist"), std::string::npos)
        << "Cached preview drops may skip deep artifact parsing, but must still reject missing renderer artifact files.";

    const auto cachedDropBegin = bridgeSource.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropImportedPrefabArtifactIntoHierarchy(");
    ASSERT_NE(cachedDropBegin, std::string::npos);
    const auto cachedDropEnd = bridgeSource.find("std::optional<NLS::Engine::Assets::PrefabArtifact> EditorAssetDragDropBridge::TryLoadPreviewPrefabArtifact(", cachedDropBegin);
    ASSERT_NE(cachedDropEnd, std::string::npos);
    const auto cachedDropCode = bridgeSource.substr(cachedDropBegin, cachedDropEnd - cachedDropBegin);
    EXPECT_NE(cachedDropCode.find("IsImportedPrefabArtifactCurrentForPayload(ProjectRoot(), payload, prefab, assetPath, prefabSubAssetKey, false)"), std::string::npos)
        << "A cached preview artifact must be rejected when the manifest or dependency stamps changed during the drag, but final release must not synchronously re-parse mesh/material/texture artifacts already validated during preview.";
    EXPECT_NE(cachedDropCode.find("dragdrop-cached-artifact-stale"), std::string::npos);

    const auto instantiateBegin = bridgeSource.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::InstantiateImportedAsset(");
    ASSERT_NE(instantiateBegin, std::string::npos);
    const auto instantiateEnd = bridgeSource.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::InstantiateImportedAsset(\n    AssetDatabaseFacade& database", instantiateBegin);
    ASSERT_NE(instantiateEnd, std::string::npos);
    const auto instantiateCode = bridgeSource.substr(instantiateBegin, instantiateEnd - instantiateBegin);
    EXPECT_NE(instantiateCode.find("Keep Scene View mouse release cheap"), std::string::npos)
        << "Imported prefab/model drops should defer asset reference resolution so mouse release does not synchronously prewarm large artifacts.";
    EXPECT_NE(instantiateCode.find("},\n        // Keep Scene View mouse release cheap"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewPreviewAttemptsCurrentArtifactEvenWhenPayloadReadinessIsStale)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");

    ASSERT_FALSE(sceneViewSource.empty());
    const auto ensureBegin = sceneViewSource.find("bool Editor::Panels::SceneView::EnsureImportedAssetDragPreviewMeshGhost(");
    ASSERT_NE(ensureBegin, std::string::npos);
    const auto ensureEnd = sceneViewSource.find("std::optional<Maths::Vector3> Editor::Panels::SceneView::ResolveImportedAssetDragPreviewPlacement(", ensureBegin);
    ASSERT_NE(ensureEnd, std::string::npos);
    const auto ensureCode = sceneViewSource.substr(ensureBegin, ensureEnd - ensureBegin);

    EXPECT_EQ(ensureCode.find("IsEditorAssetDragPayloadPreviewPrefabReady"), std::string::npos)
        << "AssetBrowser payload readiness can be stale after import; Scene View should ask the bridge to validate current artifacts instead of suppressing preview.";
    EXPECT_NE(ensureCode.find("TryLoadPreviewPrefabArtifact(payload)"), std::string::npos);
    EXPECT_NE(ensureCode.find("m_importedAssetDragPreviewMeshGhostUnavailable"), std::string::npos);
    EXPECT_NE(ensureCode.find("m_importedAssetDragPreviewNextMeshGhostRetryTime"), std::string::npos)
        << "A transient artifact read failure during hover must not suppress the mesh ghost for the entire drag.";
    EXPECT_NE(sceneViewSource.find("kSceneViewDragPreviewRetryDelay"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewPreviewDoesNotRequirePayloadSubAssetKeyBeforeFastPreviewLoad)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");
    const std::string bridgeSource = ReadSourceText(root / "Project/Editor/Assets/EditorAssetDragDropBridge.cpp");

    ASSERT_FALSE(sceneViewSource.empty());
    ASSERT_FALSE(bridgeSource.empty());

    const auto ensureBegin = sceneViewSource.find("bool Editor::Panels::SceneView::EnsureImportedAssetDragPreviewMeshGhost(");
    ASSERT_NE(ensureBegin, std::string::npos);
    const auto ensureEnd = sceneViewSource.find("std::optional<Maths::Vector3> Editor::Panels::SceneView::ResolveImportedAssetDragPreviewPlacement(", ensureBegin);
    ASSERT_NE(ensureEnd, std::string::npos);
    const auto ensureCode = sceneViewSource.substr(ensureBegin, ensureEnd - ensureBegin);

    const auto assetPathGuard = ensureCode.find("assetPath.empty()");
    ASSERT_NE(assetPathGuard, std::string::npos);
    EXPECT_EQ(ensureCode.find("subAssetKey.empty()", assetPathGuard), std::string::npos)
        << "Scene View hover must let EditorAssetDragDropBridge derive the default model/prefab sub-asset key; otherwise whole-file drags fall back to the text label instead of a mesh ghost.";
    EXPECT_NE(ensureCode.find("TryLoadPreviewPrefabArtifact(payload)"), std::string::npos);

    const auto previewHandleBegin = bridgeSource.find("std::optional<ImportedAssetHandle> ResolveImportedAssetHandleForPreview(");
    ASSERT_NE(previewHandleBegin, std::string::npos);
    const auto previewHandleEnd = bridgeSource.find("bool IsImportedPrefabArtifactCurrentForPayload(", previewHandleBegin);
    ASSERT_NE(previewHandleEnd, std::string::npos);
    const auto previewHandleCode = bridgeSource.substr(previewHandleBegin, previewHandleEnd - previewHandleBegin);
    EXPECT_NE(previewHandleCode.find("prefabSubAssetKey.empty()"), std::string::npos);
    EXPECT_NE(previewHandleCode.find("DefaultGeneratedPrefabSubAssetKeyForAssetPath(assetPath)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewPreviewLoadsPrefabGraphWithoutRendererArtifactDeepValidation)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");
    const std::string bridgeHeader = ReadSourceText(root / "Project/Editor/Assets/EditorAssetDragDropBridge.h");
    const std::string bridgeSource = ReadSourceText(root / "Project/Editor/Assets/EditorAssetDragDropBridge.cpp");

    ASSERT_FALSE(sceneViewSource.empty());
    ASSERT_FALSE(bridgeHeader.empty());
    ASSERT_FALSE(bridgeSource.empty());

    const auto ensureBegin = sceneViewSource.find("bool Editor::Panels::SceneView::EnsureImportedAssetDragPreviewMeshGhost(");
    ASSERT_NE(ensureBegin, std::string::npos);
    const auto ensureEnd = sceneViewSource.find("std::optional<Maths::Vector3> Editor::Panels::SceneView::ResolveImportedAssetDragPreviewPlacement(", ensureBegin);
    ASSERT_NE(ensureEnd, std::string::npos);
    const auto ensureCode = sceneViewSource.substr(ensureBegin, ensureEnd - ensureBegin);
    EXPECT_NE(ensureCode.find("TryLoadPreviewPrefabArtifact(payload)"), std::string::npos);
    EXPECT_NE(ensureCode.find("previewLoadPolicy.deferAssetReferenceResolution = true;"), std::string::npos)
        << "Hover preview should create the object graph without mesh/material prewarm.";

    const auto previewLoadBegin = bridgeSource.find("std::optional<NLS::Engine::Assets::PrefabArtifact> EditorAssetDragDropBridge::TryLoadPreviewPrefabArtifact(");
    ASSERT_NE(previewLoadBegin, std::string::npos);
    const auto previewLoadEnd = bridgeSource.find("\n}", previewLoadBegin);
    ASSERT_NE(previewLoadEnd, std::string::npos);
    const auto previewLoadCode = bridgeSource.substr(previewLoadBegin, previewLoadEnd - previewLoadBegin);
    EXPECT_NE(previewLoadCode.find("ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles"), std::string::npos)
        << "Hover preview must share the formal drop's lightweight renderer artifact file readiness gate, otherwise stale artifacts create an invisible preview scene.";
    EXPECT_EQ(previewLoadCode.find("ValidateGeneratedModelRendererArtifactsReady("), std::string::npos)
        << "Preview loading should not synchronously read every generated mesh/material/texture artifact.";

    const auto fastLoadBegin = bridgeSource.find("FastImportedPrefabLoadResult LoadImportedPrefabFast(");
    ASSERT_NE(fastLoadBegin, std::string::npos);
    const auto fastLoadEnd = bridgeSource.find("EditorAssetDragDropBridgeResult MakePendingImportedPrefabResult(", fastLoadBegin);
    ASSERT_NE(fastLoadEnd, std::string::npos);
    const auto fastLoadCode = bridgeSource.substr(fastLoadBegin, fastLoadEnd - fastLoadBegin);
    EXPECT_NE(fastLoadCode.find("ImportedPrefabArtifactLoadMode"), std::string::npos);
    EXPECT_NE(fastLoadCode.find("RequireRendererArtifactFiles"), std::string::npos);
    EXPECT_NE(fastLoadCode.find("ValidateRendererDependencies"), std::string::npos);
    EXPECT_NE(fastLoadCode.find("ValidateGeneratedModelRendererArtifactsReady("), std::string::npos)
        << "Deep validation may exist for explicit validation paths.";
    const auto requireRendererFilesBegin =
        fastLoadCode.find("loadMode == ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles");
    ASSERT_NE(requireRendererFilesBegin, std::string::npos);
    const auto requireRendererFilesEnd =
        fastLoadCode.find("loadMode == ImportedPrefabArtifactLoadMode::ValidateRendererDependencies", requireRendererFilesBegin);
    ASSERT_NE(requireRendererFilesEnd, std::string::npos);
    const auto requireRendererFilesCode =
        fastLoadCode.substr(requireRendererFilesBegin, requireRendererFilesEnd - requireRendererFilesBegin);
    EXPECT_NE(requireRendererFilesCode.find("GeneratedModelRendererArtifactFilesExist("), std::string::npos)
        << "Mouse-release paths should do lightweight artifact-file readiness checks.";
    EXPECT_EQ(requireRendererFilesCode.find("ValidateGeneratedModelRendererArtifactsReady("), std::string::npos)
        << "Mouse-release paths must not synchronously parse mesh/material/texture artifacts.";
    const auto lightweightRendererFilesBegin =
        bridgeSource.find("FastImportedPrefabLoadResult GeneratedModelRendererArtifactFilesExist(");
    ASSERT_NE(lightweightRendererFilesBegin, std::string::npos);
    const auto lightweightRendererFilesEnd =
        bridgeSource.find("FastImportedPrefabLoadResult LoadImportedPrefabFast(", lightweightRendererFilesBegin);
    ASSERT_NE(lightweightRendererFilesEnd, std::string::npos);
    const auto lightweightRendererFilesCode =
        bridgeSource.substr(lightweightRendererFilesBegin, lightweightRendererFilesEnd - lightweightRendererFilesBegin);
    EXPECT_NE(lightweightRendererFilesCode.find("HasNativeArtifactHeader("), std::string::npos)
        << "Release/drop readiness should only perform bounded header validation.";
    EXPECT_EQ(lightweightRendererFilesCode.find("ComputeArtifactFileContentHash("), std::string::npos)
        << "Release/drop readiness must not scan whole mesh/material/texture files.";
    EXPECT_EQ(lightweightRendererFilesCode.find("LoadMeshArtifact("), std::string::npos);
    EXPECT_EQ(lightweightRendererFilesCode.find("LoadTextureArtifact("), std::string::npos);
    EXPECT_EQ(lightweightRendererFilesCode.find("IsReadableMaterialArtifact("), std::string::npos);
    EXPECT_EQ(lightweightRendererFilesCode.find("ValidateGeneratedModelRendererArtifactsReady("), std::string::npos);
    const auto dropModelBegin = bridgeSource.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropModelAssetIntoHierarchy(");
    ASSERT_NE(dropModelBegin, std::string::npos);
    const auto dropModelEnd = bridgeSource.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropModelAssetIntoHierarchyAsync(", dropModelBegin);
    ASSERT_NE(dropModelEnd, std::string::npos);
    const auto dropModelCode = bridgeSource.substr(dropModelBegin, dropModelEnd - dropModelBegin);
    EXPECT_NE(dropModelCode.find("ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles"), std::string::npos)
        << "Mouse-release drops must only check committed artifact presence; synchronous mesh/texture parsing causes visible release stalls.";
    EXPECT_EQ(dropModelCode.find("ValidateGeneratedModelRendererArtifactsReady("), std::string::npos);

    const auto handleDropBegin = bridgeSource.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropImportedAssetHandleIntoHierarchy(");
    ASSERT_NE(handleDropBegin, std::string::npos);
    const auto handleDropEnd = bridgeSource.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropImportedPrefabArtifactIntoHierarchy(", handleDropBegin);
    ASSERT_NE(handleDropEnd, std::string::npos);
    const auto handleDropCode = bridgeSource.substr(handleDropBegin, handleDropEnd - handleDropBegin);
    EXPECT_NE(handleDropCode.find("ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles"), std::string::npos)
        << "Scene View payload drops must not deep-validate renderer artifacts on release.";
    EXPECT_EQ(handleDropCode.find("ValidateGeneratedModelRendererArtifactsReady("), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewAssetDropPreviewRayMatchesRenderedScreenRight)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");

    ASSERT_FALSE(sceneViewSource.empty());

    const auto placementBegin = sceneViewSource.find(
        "std::optional<Maths::Vector3> Editor::Panels::SceneView::ResolveImportedAssetDragPreviewPlacement(");
    ASSERT_NE(placementBegin, std::string::npos);
    const auto placementEnd = sceneViewSource.find(
        "void Editor::Panels::SceneView::DrawImportedAssetDragPreview()", placementBegin);
    ASSERT_NE(placementEnd, std::string::npos);
    const auto placementCode = sceneViewSource.substr(placementBegin, placementEnd - placementBegin);

    EXPECT_NE(placementCode.find("const float screenRightNdcX = -ndcX;"), std::string::npos);
    EXPECT_NE(placementCode.find("GetWorldRight() * (screenRightNdcX * tanHalfFov * aspect)"), std::string::npos);
    EXPECT_NE(placementCode.find("view matrix maps world right to screen-left"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewAssetDropUsesPreviewOnlyMeshGhostScene)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string sceneViewHeader = ReadSourceText(root / "Project/Editor/Panels/SceneView.h");
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");
    const std::string debugRendererHeader = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.h");
    const std::string debugRendererSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const std::string baseRendererHeader = ReadSourceText(root / "Runtime/Engine/Rendering/BaseSceneRenderer.h");
    const std::string baseRendererSource = ReadSourceText(root / "Runtime/Engine/Rendering/BaseSceneRenderer.cpp");

    ASSERT_FALSE(sceneViewHeader.empty());
    ASSERT_FALSE(sceneViewSource.empty());
    ASSERT_FALSE(debugRendererHeader.empty());
    ASSERT_FALSE(debugRendererSource.empty());
    ASSERT_FALSE(baseRendererHeader.empty());
    ASSERT_FALSE(baseRendererSource.empty());

    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewScene"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewRoot"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewMeshGhostUnavailable"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewPrewarmedResources"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewMaterialRequests"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewTextureRequests"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewMeshLoads"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_importedAssetDragPreviewRenderableReady"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("EnsureImportedAssetDragPreviewMeshGhost"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("PumpImportedAssetDragPreviewResources"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("void ClearImportedAssetDragPreview(bool cancelAsyncResourceRequests = true);"), std::string::npos);

    const auto previewBegin = sceneViewSource.find("bool Editor::Panels::SceneView::EnsureImportedAssetDragPreviewMeshGhost(");
    ASSERT_NE(previewBegin, std::string::npos);
    const auto previewEnd = sceneViewSource.find("std::optional<Maths::Vector3> Editor::Panels::SceneView::ResolveImportedAssetDragPreviewPlacement(", previewBegin);
    ASSERT_NE(previewEnd, std::string::npos);
    const auto previewCode = sceneViewSource.substr(previewBegin, previewEnd - previewBegin);
    EXPECT_NE(previewCode.find("EditorAssetDragDropBridge"), std::string::npos);
    EXPECT_EQ(previewCode.find("IsEditorAssetDragPayloadPreviewPrefabReady"), std::string::npos);
    EXPECT_NE(previewCode.find("TryLoadPreviewPrefabArtifact"), std::string::npos);
    EXPECT_NE(previewCode.find("InstantiatePrefabArtifact"), std::string::npos);
    EXPECT_NE(previewCode.find("m_importedAssetDragPreviewMeshGhostUnavailable"), std::string::npos);
    EXPECT_NE(previewCode.find("m_importedAssetDragPreviewScene"), std::string::npos);
    EXPECT_NE(previewCode.find("m_importedAssetDragPreviewRoot"), std::string::npos);
    EXPECT_NE(previewCode.find("previewLoadPolicy.deferAssetReferenceResolution = true;"), std::string::npos);
    EXPECT_EQ(previewCode.find("AssetDatabaseFacade"), std::string::npos);
    EXPECT_EQ(previewCode.find(".Refresh("), std::string::npos);
    EXPECT_EQ(previewCode.find("LoadPrefabArtifactAtPath"), std::string::npos);
    EXPECT_EQ(previewCode.find("CreateGameObjectFromAsset"), std::string::npos);
    EXPECT_EQ(previewCode.find("EDITOR_CONTEXT(sceneManager)"), std::string::npos);
    EXPECT_EQ(previewCode.find("prefabInstanceRegistry"), std::string::npos);

    const auto descriptorBegin = sceneViewSource.find("debugRenderer->AddDescriptor<Rendering::DebugSceneRenderer::DebugSceneDescriptor>");
    ASSERT_NE(descriptorBegin, std::string::npos);
    const auto descriptorCode = sceneViewSource.substr(descriptorBegin, 600);
    EXPECT_EQ(descriptorCode.find("m_importedAssetDragPreviewRenderableReady ?"), std::string::npos)
        << "A large preview scene must not stay globally hidden until every mesh/material/texture is ready.";
    EXPECT_NE(descriptorCode.find("m_importedAssetDragPreviewScene.get()"), std::string::npos)
        << "Scene View should submit the preview scene immediately; per-renderer readiness gates suppress unfinished meshes.";

    const auto sceneDescriptorBegin = sceneViewSource.find("Engine::Rendering::BaseSceneRenderer::SceneDescriptor Editor::Panels::SceneView::CreateSceneDescriptor()");
    ASSERT_NE(sceneDescriptorBegin, std::string::npos);
    const auto sceneDescriptorCode = sceneViewSource.substr(sceneDescriptorBegin, 700);
    EXPECT_EQ(sceneDescriptorCode.find("m_importedAssetDragPreviewRenderableReady && m_importedAssetDragPreviewScene"), std::string::npos)
        << "Whole-prefab readiness gating makes large imported model drags invisible for too long.";
    EXPECT_NE(sceneDescriptorCode.find("if (m_importedAssetDragPreviewScene)"), std::string::npos);
    EXPECT_NE(sceneDescriptorCode.find("descriptor.additiveScenes.push_back(m_importedAssetDragPreviewScene.get())"), std::string::npos);

    EXPECT_NE(baseRendererHeader.find("std::vector<SceneSystem::Scene*> additiveScenes;"), std::string::npos);
    EXPECT_NE(baseRendererHeader.find("std::unordered_map<SceneSystem::Scene*, RenderScene> m_additiveRenderScenes;"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("appendSceneDrawables(sceneDescriptor.scene, m_renderScene, true, false);"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("for (auto* additiveScene : sceneDescriptor.additiveScenes)"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("m_additiveRenderScenes.erase(it)"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("auto& additiveRenderScene = m_additiveRenderScenes[additiveScene];"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("appendSceneDrawables(*additiveScene, additiveRenderScene, false, true);"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("uint32_t nextObjectIndex = 0u;"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("reassignObjectIndices(opaques);"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("reassignObjectIndices(skyboxes);"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("reassignObjectIndices(transparents);"), std::string::npos);
    EXPECT_NE(baseRendererSource.find("EngineDrawableDescriptor::kInvalidObjectIndex"), std::string::npos);

    EXPECT_NE(debugRendererHeader.find("Engine::SceneSystem::Scene* previewScene = nullptr;"), std::string::npos);
    EXPECT_NE(debugRendererSource.find("previewScene"), std::string::npos);

    const auto pumpBegin = sceneViewSource.find("void Editor::Panels::SceneView::PumpImportedAssetDragPreviewResources()");
    ASSERT_NE(pumpBegin, std::string::npos);
    const auto pumpEnd = sceneViewSource.find("void Editor::Panels::SceneView::DrawImportedAssetDragPreview()", pumpBegin);
    ASSERT_NE(pumpEnd, std::string::npos);
    const auto pumpCode = sceneViewSource.substr(pumpBegin, pumpEnd - pumpBegin);
    const auto consumeBegin = sceneViewSource.find("TryConsumeImportedAssetDragPreviewMeshLoad(");
    ASSERT_NE(consumeBegin, std::string::npos);
    const auto consumeEnd = sceneViewSource.find("bool StartImportedAssetDragPreviewMeshLoad(", consumeBegin);
    ASSERT_NE(consumeEnd, std::string::npos);
    const auto consumeCode = sceneViewSource.substr(consumeBegin, consumeEnd - consumeBegin);
    EXPECT_NE(pumpCode.find("kSceneViewDragPreviewMeshPrewarmsPerFrame"), std::string::npos);
    EXPECT_NE(pumpCode.find("kSceneViewDragPreviewMaterialPrewarmsPerFrame"), std::string::npos);
    EXPECT_EQ(pumpCode.find("PrewarmArtifact(resolved.artifactPath)"), std::string::npos)
        << "Scene View drag preview must not synchronously read large mesh artifacts on the hover frame.";
    EXPECT_NE(sceneViewSource.find("BuildImportedAssetDragPreviewMeshData"), std::string::npos)
        << "Cold Scene View previews need a capped preview mesh so the real model can follow the mouse without uploading the full artifact.";
    EXPECT_NE(sceneViewSource.find("kSceneViewDragPreviewMaxMeshVertices"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("kSceneViewDragPreviewMaxMeshIndices"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("kSceneViewDragPreviewMeshPrewarmsPerFrame"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("kSceneViewDragPreviewMaterialPrewarmsPerFrame"), std::string::npos);
    EXPECT_NE(consumeCode.find("CreateImportedAssetDragPreviewMesh(*data)"), std::string::npos)
        << "Completed cold mesh CPU data should become a lightweight preview mesh, not stay proxy forever.";
    EXPECT_EQ(consumeCode.find("found->second->data.reset()"), std::string::npos)
        << "Preview mesh consumption must keep the full artifact data available for release handoff; otherwise the formal drop restarts the heavy mesh path.";
    EXPECT_NE(sceneViewSource.find("MeshBufferUploadMode::CpuToGpu"), std::string::npos)
        << "Preview mesh upload is allowed only after the CPU artifact is ready and capped by the preview budget.";
    EXPECT_NE(sceneViewSource.find("StartImportedAssetDragPreviewMeshLoad"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("TrackBackgroundTask"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("LoadMeshArtifact"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("SetResolvedTransientMeshFromReference"), std::string::npos);
    EXPECT_EQ(pumpCode.find("LoadArtifactWithoutTextures(resolved.artifactPath)"), std::string::npos)
        << "Scene View drag preview must not synchronously parse generated material artifacts on the hover frame.";
    EXPECT_NE(sceneViewSource.find("materialManager.RequestAsyncArtifact(resolved.artifactPath, true)"), std::string::npos)
        << "Cold Scene View drag previews should request generated materials as cancellable interests that can be handed off to the formal instance.";
    EXPECT_NE(sceneViewSource.find("FindImportedAssetDragPreviewCachedMaterial("), std::string::npos)
        << "Scene View drag preview must reuse already loaded material artifacts across equivalent absolute/Library paths.";
    EXPECT_NE(sceneViewSource.find("meshRenderer.GetMaterialReferences()"), std::string::npos)
        << "Scene View drag preview must prefer the prefab PPtr-bound material before equivalent-path cache hits so duplicate artifact registrations cannot keep the preview invisible.";
    EXPECT_NE(sceneViewSource.find("FindImportedAssetDragPreviewCachedTexture("), std::string::npos)
        << "Scene View drag preview must reuse already loaded texture artifacts across equivalent absolute/Library paths.";
    EXPECT_NE(sceneViewSource.find("FindImportedAssetDragPreviewCachedMesh("), std::string::npos)
        << "Scene View drag preview must reuse already loaded mesh artifacts across equivalent absolute/Library paths.";
    EXPECT_NE(sceneViewSource.find("bool RequestImportedAssetDragPreviewMaterial("), std::string::npos);
    EXPECT_NE(pumpCode.find("const bool accepted = RequestImportedAssetDragPreviewMaterial("), std::string::npos)
        << "Scene View preview should only mark a material artifact prewarmed when the cached material exists or an async request was accepted.";
    EXPECT_NE(pumpCode.find("if (accepted)"), std::string::npos);
    EXPECT_EQ(pumpCode.find("if (prewarmedThisFrame >= kSceneViewDragPreviewResourcePrewarmsPerFrame)\n            break;\n    }\n    for (const auto& resolved : m_importedAssetDragPreviewArtifact->resolvedAssets)"), std::string::npos)
        << "Large imported model previews must not let hundreds of mesh artifacts consume the shared prewarm budget before material/texture requests can start.";
    EXPECT_NE(pumpCode.find("PumpImportedAssetDragPreviewResourceManagers(m_importedAssetDragPreviewTextureRequests);"), std::string::npos)
        << "Scene View drag preview should drain async resource completions before binding preview materials.";
    EXPECT_NE(sceneViewSource.find("materialManager.PumpAsyncLoads("), std::string::npos)
        << "Scene View drag preview must drain async material completions while hovering.";
    EXPECT_NE(sceneViewSource.find("textureManager.PumpAsyncLoadsForPaths("), std::string::npos)
        << "Cold texture previews need a strictly budgeted, preview-owned ready-completion pump or they can stay as proxy forever.";
    EXPECT_EQ(sceneViewSource.find("textureManager.PumpAsyncLoads(kSceneViewDragPreviewTextureCompletionsPerFrame)"), std::string::npos)
        << "Scene View preview must not drain arbitrary global texture completions on the hover frame.";
    EXPECT_NE(sceneViewSource.find("BindImportedAssetDragPreviewMaterialTextures"), std::string::npos)
        << "When a preview material becomes available, Scene View should asynchronously fill its texture slots instead of relying on the white fallback.";
    EXPECT_NE(sceneViewSource.find("textureManager.RequestAsyncArtifact(texturePath, true)"), std::string::npos)
        << "Preview material textures must be requested as cancellable interests so cancelling a hover does not leak pending async state.";
    EXPECT_NE(pumpCode.find("m_importedAssetDragPreviewMaterialRequests"), std::string::npos)
        << "Preview material async requests must be tracked so cancellation does not leak CPU work.";
    EXPECT_NE(pumpCode.find("m_importedAssetDragPreviewTextureRequests"), std::string::npos)
        << "Preview texture async requests must be tracked so cancellation does not leak CPU work.";
    EXPECT_EQ(pumpCode.find("ImportedAssetDragPreviewRenderableResourcesReady"), std::string::npos)
        << "Preview readiness should be evaluated per renderer; a whole-tree readiness pre-pass can leave large drags invisible too long.";
    EXPECT_NE(pumpCode.find("BindImportedAssetDragPreviewReadyRenderers"), std::string::npos);
    EXPECT_NE(pumpCode.find("m_importedAssetDragPreviewRenderableReady = BindImportedAssetDragPreviewReadyRenderers"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("ResolveMesh()"), std::string::npos);
    EXPECT_EQ(sceneViewSource.find("ResolveMaterials()"), std::string::npos)
        << "Preview material binding must stay within the explicit per-frame budget instead of synchronously resolving every slot.";
    EXPECT_NE(sceneViewSource.find("GetMaterialPaths()"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("SetResolvedMaterialFromReference"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("CancelImportedAssetDragPreviewMeshLoads"), std::string::npos);

    const auto cancelRequestsBegin = sceneViewSource.find("void CancelImportedAssetDragPreviewAsyncResourceRequests(");
    ASSERT_NE(cancelRequestsBegin, std::string::npos);
    const auto cancelRequestsEnd = sceneViewSource.find("ImGuizmo::OPERATION ToNativeImGuizmoOperation", cancelRequestsBegin);
    ASSERT_NE(cancelRequestsEnd, std::string::npos);
    const auto cancelRequestsCode = sceneViewSource.substr(cancelRequestsBegin, cancelRequestsEnd - cancelRequestsBegin);
    EXPECT_NE(cancelRequestsCode.find("materialManager.CancelAsyncArtifact(path)"), std::string::npos)
        << "Cancelling a preview hover must release its material async interests so they do not leak indefinitely.";
    EXPECT_NE(cancelRequestsCode.find("textureManager.CancelAsyncArtifact(path)"), std::string::npos)
        << "Cancelling a preview hover must release its texture async interests so they do not leak indefinitely.";
    EXPECT_NE(cancelRequestsCode.find("materialRequests.clear()"), std::string::npos);
    EXPECT_NE(cancelRequestsCode.find("textureRequests.clear()"), std::string::npos);

    const auto dropBegin = sceneViewSource.find("void Editor::Panels::SceneView::HandleViewportAssetDragDrop()");
    ASSERT_NE(dropBegin, std::string::npos);
    const auto dropEnd = sceneViewSource.find("void Editor::Panels::SceneView::PumpImportedAssetDragPreviewBeforeRender()", dropBegin);
    ASSERT_NE(dropEnd, std::string::npos);
    const auto dropCode = sceneViewSource.substr(dropBegin, dropEnd - dropBegin);
    EXPECT_NE(dropCode.find("CollectImportedAssetDragPreviewResourceHandoff()"), std::string::npos);
    EXPECT_NE(dropCode.find("ClearImportedAssetDragPreview(false)"), std::string::npos)
        << "Drop commit must hand off preview work without cancelling async resources needed by the formal instance.";

    const auto clearBegin = sceneViewSource.find("void Editor::Panels::SceneView::ClearImportedAssetDragPreview(const bool cancelAsyncResourceRequests)");
    ASSERT_NE(clearBegin, std::string::npos);
    const auto clearEnd = sceneViewSource.find("void Editor::Panels::SceneView::EnsureRenderer()", clearBegin);
    ASSERT_NE(clearEnd, std::string::npos);
    const auto clearCode = sceneViewSource.substr(clearBegin, clearEnd - clearBegin);
    EXPECT_NE(clearCode.find("if (cancelAsyncResourceRequests)"), std::string::npos);
    EXPECT_NE(clearCode.find("CancelImportedAssetDragPreviewAsyncResourceRequests("), std::string::npos);
    EXPECT_NE(clearCode.find("m_importedAssetDragPreviewMaterialRequests.clear()"), std::string::npos);
    EXPECT_NE(clearCode.find("m_importedAssetDragPreviewTextureRequests.clear()"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneViewAssetDropDoesNotShowMeshBeforePreviewMaterialsAndTexturesAreBound)
{
    const auto sceneViewSource = ReadSourceText("Project/Editor/Panels/SceneView.cpp");
    const auto baseSceneRendererSource = ReadSourceText("Runtime/Engine/Rendering/BaseSceneRenderer.cpp");
    ASSERT_FALSE(sceneViewSource.empty());
    ASSERT_FALSE(baseSceneRendererSource.empty());

    const auto resourcesReadyBegin = sceneViewSource.find("bool BindImportedAssetDragPreviewReadyRenderers(");
    ASSERT_NE(resourcesReadyBegin, std::string::npos);
    const auto resourcesReadyEnd = sceneViewSource.find("bool RequestImportedAssetDragPreviewMaterial(", resourcesReadyBegin);
    ASSERT_NE(resourcesReadyEnd, std::string::npos);
    const auto resourcesReadyCode = sceneViewSource.substr(resourcesReadyBegin, resourcesReadyEnd - resourcesReadyBegin);

    const auto materialGate = resourcesReadyCode.find("ImportedAssetDragPreviewMaterialsReady(");
    ASSERT_NE(materialGate, std::string::npos)
        << "Preview must gate mesh visibility until the renderer's material slots have real materials.";
    EXPECT_NE(resourcesReadyCode.find("ImportedAssetDragPreviewMeshResourceReady"), std::string::npos)
        << "Preview visibility must wait for mesh CPU data or cached mesh resources too.";

    const auto materialsReadyBegin = sceneViewSource.find("bool ImportedAssetDragPreviewMaterialsReady(");
    ASSERT_NE(materialsReadyBegin, std::string::npos);
    const auto materialsReadyEnd = sceneViewSource.find("bool ImportedAssetDragPreviewMeshResourceReady(", materialsReadyBegin);
    ASSERT_NE(materialsReadyEnd, std::string::npos);
    const auto materialsReadyCode = sceneViewSource.substr(materialsReadyBegin, materialsReadyEnd - materialsReadyBegin);
    EXPECT_NE(materialsReadyCode.find("BindImportedAssetDragPreviewMaterialTextures(*"), std::string::npos)
        << "The material-ready gate must also validate texture slots so preview meshes do not appear as white models.";

    const auto textureBindBegin = sceneViewSource.find("bool BindImportedAssetDragPreviewMaterialTextures(");
    ASSERT_NE(textureBindBegin, std::string::npos);
    const auto textureBindEnd = sceneViewSource.find("bool ImportedAssetDragPreviewMaterialsReady(", textureBindBegin);
    ASSERT_NE(textureBindEnd, std::string::npos);
    const auto textureBindCode = sceneViewSource.substr(textureBindBegin, textureBindEnd - textureBindBegin);
    EXPECT_NE(textureBindCode.find("bool ready = true;"), std::string::npos);
    EXPECT_NE(textureBindCode.find("ready = false;"), std::string::npos)
        << "Texture binding must report missing or budget-deferred textures so mesh visibility can wait.";
    EXPECT_NE(textureBindCode.find("texturePathMatches"), std::string::npos)
        << "An existing texture pointer only satisfies preview readiness if it resolves to the material-declared texture artifact.";
    EXPECT_NE(textureBindCode.find("ResolveResourcePath"), std::string::npos)
        << "Preview readiness must accept equivalent absolute/Library texture artifact paths, not raw string equality only.";
    EXPECT_NE(textureBindCode.find("return ready;"), std::string::npos);

    const auto pumpBegin = sceneViewSource.find("void Editor::Panels::SceneView::PumpImportedAssetDragPreviewResources()");
    ASSERT_NE(pumpBegin, std::string::npos);
    const auto pumpEnd = sceneViewSource.find("NLS::Editor::Core::PrefabInstancePreviewResourceHandoff", pumpBegin);
    ASSERT_NE(pumpEnd, std::string::npos);
    const auto pumpCode = sceneViewSource.substr(pumpBegin, pumpEnd - pumpBegin);
    EXPECT_EQ(pumpCode.find("if (!resourcesReady || !foundRenderable)\n        return;"), std::string::npos)
        << "Large preview scenes must keep progressively binding ready renderers instead of waiting for the whole prefab.";
    EXPECT_EQ(pumpCode.find("(void)resourcesReady"), std::string::npos)
        << "Preview must not compute readiness and then ignore it before binding meshes.";
    EXPECT_NE(pumpCode.find("if (!foundRenderable)\n        m_importedAssetDragPreviewRenderableReady = false;"), std::string::npos);
    EXPECT_NE(pumpCode.find("m_importedAssetDragPreviewRenderableReady = BindImportedAssetDragPreviewReadyRenderers"), std::string::npos)
        << "Ready renderer binding must only expose meshes after that renderer's material and texture slots are bound.";

    const auto sceneDescriptorBegin = sceneViewSource.find("Engine::Rendering::BaseSceneRenderer::SceneDescriptor Editor::Panels::SceneView::CreateSceneDescriptor()");
    ASSERT_NE(sceneDescriptorBegin, std::string::npos);
    const auto sceneDescriptorCode = sceneViewSource.substr(sceneDescriptorBegin, 700);
    EXPECT_EQ(sceneDescriptorCode.find("m_importedAssetDragPreviewRenderableReady && m_importedAssetDragPreviewScene"), std::string::npos)
        << "The additive preview scene must be available early; RenderScene suppresses unresolved renderer slots.";
    EXPECT_NE(sceneDescriptorCode.find("if (m_importedAssetDragPreviewScene)"), std::string::npos);

    EXPECT_NE(baseSceneRendererSource.find("appendSceneDrawables(sceneDescriptor.scene, m_renderScene, true, false)"), std::string::npos)
        << "The main saved scene must not use preview-only strict texture gating or existing prefab instances can disappear while drag resources are pending.";
    EXPECT_NE(baseSceneRendererSource.find("appendSceneDrawables(*additiveScene, additiveRenderScene, false, true)"), std::string::npos)
        << "Only the additive drag preview scene should require explicit material textures before submitting draws, preventing white preview meshes without hiding the main scene.";

    const auto drawBegin = sceneViewSource.find("void Editor::Panels::SceneView::DrawImportedAssetDragPreview()");
    ASSERT_NE(drawBegin, std::string::npos);
    const auto drawEnd = sceneViewSource.find("void Editor::Panels::SceneView::ClearImportedAssetDragPreview", drawBegin);
    ASSERT_NE(drawEnd, std::string::npos);
    const auto drawCode = sceneViewSource.substr(drawBegin, drawEnd - drawBegin);
    EXPECT_EQ(drawCode.find("AddCircle"), std::string::npos);
    EXPECT_EQ(drawCode.find("AddCircleFilled"), std::string::npos);
    EXPECT_EQ(drawCode.find("AddLine"), std::string::npos);
    EXPECT_EQ(drawCode.find("SubmitBox"), std::string::npos)
        << "Pending preview resources should never fall back to UI proxies or a white mesh.";
}

TEST(EditorRenderPathContractTests, DeferredMaterialResolutionBindsMaterialOnlyAfterTexturesAreReady)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const std::string source = ReadSourceText(sourcePath);

    ASSERT_FALSE(source.empty());
    const auto bindFunction = source.find("bool BindDeferredMaterialPaths(");
    ASSERT_NE(bindFunction, std::string::npos);
    const auto nextFunction = source.find("bool BindDeferredMeshPath(", bindFunction);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto body = source.substr(bindFunction, nextFunction - bindFunction);

    const auto textureBind = body.find("if (!BindDeferredMaterialTextures(*material, task, state, stats, frameBudgetExpired))");
    ASSERT_NE(textureBind, std::string::npos)
        << "Generated model material resolution must validate texture slots before exposing the material to rendering.";
    const auto resolvedBind = body.find("meshRenderer.SetResolvedMaterialFromReference", textureBind);
    ASSERT_NE(resolvedBind, std::string::npos);
    EXPECT_LT(textureBind, resolvedBind)
        << "Binding the material before textures are ready renders the dropped model as a white fallback material.";
    EXPECT_NE(body.find("meshRenderer.GetMaterialAtIndex(static_cast<uint8_t>(index)) == material", resolvedBind), std::string::npos)
        << "Deferred resolution must only count a material slot as bound after MeshRenderer actually accepted the asset reference.";
    const auto materialBoundGuard = body.find("if (!materialBound)", resolvedBind);
    ASSERT_NE(materialBoundGuard, std::string::npos);
    EXPECT_NE(body.find("++stats->failedMaterialSlots", materialBoundGuard), std::string::npos)
        << "A rejected material PPtr binding is terminal; retrying forever keeps generated models invisible and leaves the progress job running.";
    EXPECT_NE(body.find("task.failed = true", materialBoundGuard), std::string::npos);
    EXPECT_NE(body.find("state.failed = true", materialBoundGuard), std::string::npos);
    const auto materialBoundBlockEnd = body.find("if (stats)\n            ++stats->boundMaterialSlots;", materialBoundGuard);
    ASSERT_NE(materialBoundBlockEnd, std::string::npos);
    const auto materialBoundBlock = body.substr(materialBoundGuard, materialBoundBlockEnd - materialBoundGuard);
    EXPECT_EQ(materialBoundBlock.find("return false;"), std::string::npos)
        << "Rejected material reference binding must not requeue the same material slot indefinitely.";
    EXPECT_NE(body.find("meshRenderer.GetMaterialReferences()"), std::string::npos)
        << "Deferred resolution must prefer an already PPtr-bound material before equivalent-path cache hits; otherwise duplicate artifact registrations can leave the drop permanently pending.";
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionCancelsAsyncMaterialAndTextureRequests)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string actionsSource = ReadSourceText(root / "Project/Editor/Core/EditorActions.cpp");
    const std::string materialHeader = ReadSourceText(root / "Runtime/Core/ResourceManagement/MaterialManager.h");
    const std::string materialSource = ReadSourceText(root / "Runtime/Rendering/ResourceManagement/MaterialManager.cpp");
    const std::string textureHeader = ReadSourceText(root / "Runtime/Core/ResourceManagement/TextureManager.h");
    const std::string textureSource = ReadSourceText(root / "Runtime/Rendering/ResourceManagement/TextureManager.cpp");
    const std::string sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");

    ASSERT_FALSE(actionsSource.empty());
    ASSERT_FALSE(materialHeader.empty());
    ASSERT_FALSE(materialSource.empty());
    ASSERT_FALSE(textureHeader.empty());
    ASSERT_FALSE(textureSource.empty());
    ASSERT_FALSE(sceneViewSource.empty());

    EXPECT_NE(materialHeader.find("CancelAsyncArtifact"), std::string::npos);
    EXPECT_NE(materialSource.find("void MaterialManager::CancelAsyncArtifact"), std::string::npos);
    EXPECT_NE(textureHeader.find("CancelAsyncArtifact"), std::string::npos);
    EXPECT_NE(textureSource.find("void TextureManager::CancelAsyncArtifact"), std::string::npos);
    EXPECT_NE(materialHeader.find("RequestAsyncArtifact(const std::string& p_path, bool p_cancelableInterest = false)"), std::string::npos)
        << "Formal drops and renderer paths should remain shared async interests by default.";
    EXPECT_NE(textureHeader.find("RequestAsyncArtifact(const std::string& p_path, bool p_cancelableInterest = false)"), std::string::npos)
        << "Formal drops and renderer paths should remain shared async interests by default.";
    EXPECT_NE(materialSource.find("size_t cancelableInterestCount = 0u"), std::string::npos);
    EXPECT_NE(materialSource.find("bool hasSharedInterest = false"), std::string::npos);
    EXPECT_NE(textureSource.find("size_t cancelableInterestCount = 0u"), std::string::npos);
    EXPECT_NE(textureSource.find("bool hasSharedInterest = false"), std::string::npos);
    EXPECT_NE(materialSource.find("g_asyncMaterialRequests.emplace(path, std::move(request));"), std::string::npos)
        << "Async material requests must reserve the path before launching std::async.";
    EXPECT_LT(
        materialSource.find("g_asyncMaterialRequests.emplace(path, std::move(request));"),
        materialSource.find("std::async("));
    EXPECT_NE(textureSource.find("g_asyncTextureRequests.emplace(path, std::move(request));"), std::string::npos)
        << "Async texture requests must reserve the path before launching std::async.";
    EXPECT_LT(
        textureSource.find("g_asyncTextureRequests.emplace(path, std::move(request));"),
        textureSource.find("std::async("));
    const auto materialPendingBegin = materialSource.find("bool MaterialManager::IsAsyncArtifactLoadPending");
    ASSERT_NE(materialPendingBegin, std::string::npos);
    const auto materialPendingEnd = materialSource.find("bool MaterialManager::IsAsyncArtifactLoadFailed", materialPendingBegin);
    ASSERT_NE(materialPendingEnd, std::string::npos);
    const auto materialPendingCode = materialSource.substr(materialPendingBegin, materialPendingEnd - materialPendingBegin);
    EXPECT_EQ(materialPendingCode.find("g_cancelledAsyncMaterialArtifacts"), std::string::npos)
        << "Cancelled in-flight material requests must still look pending until they drain.";
    const auto texturePendingBegin = textureSource.find("bool TextureManager::IsAsyncArtifactLoadPending");
    ASSERT_NE(texturePendingBegin, std::string::npos);
    const auto texturePendingEnd = textureSource.find("bool TextureManager::IsAsyncArtifactLoadFailed", texturePendingBegin);
    ASSERT_NE(texturePendingEnd, std::string::npos);
    const auto texturePendingCode = textureSource.substr(texturePendingBegin, texturePendingEnd - texturePendingBegin);
    EXPECT_EQ(texturePendingCode.find("g_cancelledAsyncTextureArtifacts"), std::string::npos)
        << "Cancelled in-flight texture requests must still look pending until they drain.";
    const auto materialCancelBegin = materialSource.find("void MaterialManager::CancelAsyncArtifact");
    ASSERT_NE(materialCancelBegin, std::string::npos);
    const auto materialCancelEnd = materialSource.find("bool MaterialManager::IsAsyncArtifactLoadPending", materialCancelBegin);
    ASSERT_NE(materialCancelEnd, std::string::npos);
    const auto materialCancelCode = materialSource.substr(materialCancelBegin, materialCancelEnd - materialCancelBegin);
    EXPECT_NE(materialCancelCode.find("--found->second.cancelableInterestCount"), std::string::npos);
    EXPECT_EQ(materialCancelCode.find("found->second.cancelled->store"), std::string::npos)
        << "Preview material cancellation must release only the preview interest; aborting a shared path request can hide existing prefab instances.";
    EXPECT_EQ(materialCancelCode.find("g_cancelledAsyncMaterialArtifacts.insert"), std::string::npos)
        << "Preview material cancellation must not mark the equivalent artifact path globally cancelled.";

    const auto textureCancelBegin = textureSource.find("void TextureManager::CancelAsyncArtifact");
    ASSERT_NE(textureCancelBegin, std::string::npos);
    const auto textureCancelEnd = textureSource.find("bool TextureManager::IsAsyncArtifactLoadPending", textureCancelBegin);
    ASSERT_NE(textureCancelEnd, std::string::npos);
    const auto textureCancelCode = textureSource.substr(textureCancelBegin, textureCancelEnd - textureCancelBegin);
    EXPECT_NE(textureCancelCode.find("--found->second.cancelableInterestCount"), std::string::npos);
    EXPECT_EQ(textureCancelCode.find("found->second.cancelled->store"), std::string::npos)
        << "Preview texture cancellation must release only the preview interest; aborting a shared path request can hide existing prefab instances.";
    EXPECT_EQ(textureCancelCode.find("g_cancelledAsyncTextureArtifacts.insert"), std::string::npos)
        << "Preview texture cancellation must not mark the equivalent artifact path globally cancelled.";
    const auto materialPumpBegin = materialSource.find("void MaterialManager::PumpAsyncLoads");
    ASSERT_NE(materialPumpBegin, std::string::npos);
    const auto materialPumpEnd = materialSource.find("void MaterialManager::DestroyResource", materialPumpBegin);
    ASSERT_NE(materialPumpEnd, std::string::npos);
    const auto materialPumpCode = materialSource.substr(materialPumpBegin, materialPumpEnd - materialPumpBegin);
    EXPECT_NE(materialPumpCode.find("request.future.get()"), std::string::npos);
    EXPECT_NE(materialPumpCode.find("catch (const std::exception& exception)"), std::string::npos)
        << "Async material task exceptions must be converted into failed artifact state instead of escaping the editor resolution pump.";
    EXPECT_NE(materialPumpCode.find("g_failedAsyncMaterialArtifacts[request.path]"), std::string::npos);
    const auto materialCreateBegin = materialPumpCode.find("MaterialLoader::CreateFromSerializedPayload");
    ASSERT_NE(materialCreateBegin, std::string::npos);
    EXPECT_NE(materialPumpCode.find("RegisterResource(request.path, material)", materialCreateBegin), std::string::npos);
    EXPECT_NE(materialPumpCode.find("catch (const std::exception& exception)", materialCreateBegin), std::string::npos)
        << "Async material runtime creation/register failures must not escape the editor resolution pump.";

    const auto texturePumpBegin = textureSource.find("void PumpAsyncTextureArtifactLoads(");
    ASSERT_NE(texturePumpBegin, std::string::npos);
    const auto texturePumpEnd = textureSource.find("void TextureManager::PumpAsyncLoads", texturePumpBegin);
    ASSERT_NE(texturePumpEnd, std::string::npos);
    const auto texturePumpCode = textureSource.substr(texturePumpBegin, texturePumpEnd - texturePumpBegin);
    EXPECT_NE(texturePumpCode.find("request.future.get()"), std::string::npos);
    EXPECT_NE(texturePumpCode.find("catch (const std::exception& exception)"), std::string::npos)
        << "Async texture task exceptions must be converted into failed artifact state instead of escaping the editor resolution pump.";
    EXPECT_NE(texturePumpCode.find("g_failedAsyncTextureArtifacts[request.path]"), std::string::npos);
    const auto textureCreateBegin = texturePumpCode.find("TextureLoader::CreateFromArtifact");
    ASSERT_NE(textureCreateBegin, std::string::npos);
    EXPECT_NE(texturePumpCode.find("RegisterResource(request.path, texture)", textureCreateBegin), std::string::npos);
    EXPECT_NE(texturePumpCode.find("catch (const std::exception& exception)", textureCreateBegin), std::string::npos)
        << "Async texture runtime creation/register failures must not escape the editor resolution pump.";

    const auto cancelInterestsBegin = actionsSource.find("void CancelRendererResourceMaterialAndTextureInterests(");
    ASSERT_NE(cancelInterestsBegin, std::string::npos);
    const auto cancelInterestsEnd = actionsSource.find("void RegisterRendererResourceResolutionState(", cancelInterestsBegin);
    ASSERT_NE(cancelInterestsEnd, std::string::npos);
    const auto cancelInterestsCode = actionsSource.substr(cancelInterestsBegin, cancelInterestsEnd - cancelInterestsBegin);
    EXPECT_NE(cancelInterestsCode.find("materialManager.CancelAsyncArtifact(path)"), std::string::npos)
        << "Released renderer-resolution material interests must cancel their cancelable async requests.";
    EXPECT_NE(cancelInterestsCode.find("textureManager.CancelAsyncArtifact(path)"), std::string::npos)
        << "Released renderer-resolution texture interests must cancel their cancelable async requests.";

    const auto cancelBegin = actionsSource.find("void CancelRendererResourceResolutionForMarkedObject(");
    ASSERT_NE(cancelBegin, std::string::npos);
    const auto cancelEnd = actionsSource.find("std::string ToGenericPath(", cancelBegin);
    ASSERT_NE(cancelEnd, std::string::npos);
    const auto cancelCode = actionsSource.substr(cancelBegin, cancelEnd - cancelBegin);
    EXPECT_NE(cancelCode.find("CancelRendererResourceMaterialAndTextureInterests"), std::string::npos)
        << "Deleting one prefab instance must release only that state's material/texture interests.";
    EXPECT_NE(actionsSource.find("std::mutex asyncLoadsMutex"), std::string::npos);
    EXPECT_NE(actionsSource.find("std::mutex lifecycleMutex"), std::string::npos);
    EXPECT_NE(actionsSource.find("class PreviewResourceHandoffCancelGuard"), std::string::npos)
        << "Preview handoff must be RAII-cancelled on every drop/queue early return so hover async work cannot leak CPU after removal.";

    const auto createImportedBegin = actionsSource.find("Engine::GameObject* NLS::Editor::Core::EditorActions::CreateGameObjectFromImportedPrefabArtifact(");
    ASSERT_NE(createImportedBegin, std::string::npos);
    const auto createImportedEnd = actionsSource.find("void NLS::Editor::Core::EditorActions::CompletePendingAssetDrop(", createImportedBegin);
    ASSERT_NE(createImportedEnd, std::string::npos);
    const auto createImportedCode = actionsSource.substr(createImportedBegin, createImportedEnd - createImportedBegin);
    EXPECT_NE(createImportedCode.find("PreviewResourceHandoffCancelGuard previewHandoffGuard(previewResourceHandoff);"), std::string::npos);
    EXPECT_NE(createImportedCode.find("previewHandoffGuard.Disarm();"), std::string::npos)
        << "The cached-prefab drop path may disarm only after the handoff is passed into formal resolution.";

    const auto queueBegin = actionsSource.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(");
    ASSERT_NE(queueBegin, std::string::npos);
    const auto queueEnd = actionsSource.find("bool Editor::Core::EditorActions::DestroyGameObject(", queueBegin);
    ASSERT_NE(queueEnd, std::string::npos);
    const auto queueCode = actionsSource.substr(queueBegin, queueEnd - queueBegin);
    EXPECT_NE(queueCode.find("PreviewResourceHandoffCancelGuard previewHandoffGuard(previewResourceHandoff);"), std::string::npos);
    const auto registerState = queueCode.find("RegisterRendererResourceResolutionState(state);");
    ASSERT_NE(registerState, std::string::npos);
    EXPECT_NE(queueCode.find("previewHandoffGuard.Disarm();", registerState), std::string::npos)
        << "Queue early returns before state registration must keep the guard armed and cancel preview mesh/material/texture interests.";

    const auto adoptBegin = actionsSource.find("void AdoptPreviewResourceHandoff(");
    ASSERT_NE(adoptBegin, std::string::npos);
    const auto adoptEnd = actionsSource.find("NLS::Engine::GameObject* FindLiveGameObjectByAddress(", adoptBegin);
    ASSERT_NE(adoptEnd, std::string::npos);
    const auto adoptCode = actionsSource.substr(adoptBegin, adoptEnd - adoptBegin);
    EXPECT_EQ(adoptCode.find("meshFilter->SetResolvedTransientMeshFromReference"), std::string::npos)
        << "Formal prefab instances must never adopt the capped Scene View preview mesh as their committed mesh.";

    const auto collectPreviewBegin = sceneViewSource.find("NLS::Editor::Core::PrefabInstancePreviewResourceHandoff CollectImportedAssetDragPreviewMeshes(");
    ASSERT_NE(collectPreviewBegin, std::string::npos);
    const auto collectPreviewEnd = sceneViewSource.find("std::string NormalizeImportedAssetDragPreviewResourcePath", collectPreviewBegin);
    ASSERT_NE(collectPreviewEnd, std::string::npos);
    const auto collectPreviewCode = sceneViewSource.substr(collectPreviewBegin, collectPreviewEnd - collectPreviewBegin);
    EXPECT_NE(collectPreviewCode.find("handoff.meshLoadsByPath = std::move(loads)"), std::string::npos)
        << "Drop handoff must transfer in-flight or completed mesh artifact loads so release does not cancel hover work and restart the heavy mesh path.";
    EXPECT_EQ(collectPreviewCode.find("CancelImportedAssetDragPreviewMeshLoads(loads)"), std::string::npos)
        << "Release handoff must not cancel preview mesh loads that the formal prefab instance is about to adopt.";
    EXPECT_NE(collectPreviewCode.find("entry.second->transientMesh.reset()"), std::string::npos)
        << "Formal prefab instances may reuse loaded artifact data, but must not inherit the capped Scene View preview mesh.";

    const auto finishCancelled = actionsSource.find("auto finishCancelled =");
    ASSERT_NE(finishCancelled, std::string::npos);
    const auto finishFailed = actionsSource.find("auto finishFailed =", finishCancelled);
    ASSERT_NE(finishFailed, std::string::npos);
    const auto finishCancelledCode = actionsSource.substr(finishCancelled, finishFailed - finishCancelled);
    EXPECT_NE(finishCancelledCode.find("CancelRendererResourceMaterialAndTextureInterests"), std::string::npos);

    const auto finishFailedEnd = actionsSource.find("if (state->cancelled.load", finishFailed);
    ASSERT_NE(finishFailedEnd, std::string::npos);
    const auto finishFailedCode = actionsSource.substr(finishFailed, finishFailedEnd - finishFailed);
    EXPECT_NE(finishFailedCode.find("CancelRendererResourceMaterialAndTextureInterests"), std::string::npos);
}

TEST(EditorRenderPathContractTests, ResourceManagerCacheAccessIsMutexProtectedForAsyncPumps)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string header = ReadSourceText(root / "Runtime/Core/ResourceManagement/AResourceManager.h");
    const std::string implementation = ReadSourceText(root / "Runtime/Core/ResourceManagement/AResourceManager.inl");
    const std::string textureManager = ReadSourceText(root / "Runtime/Rendering/ResourceManagement/TextureManager.cpp");
    const std::string materialManager = ReadSourceText(root / "Runtime/Rendering/ResourceManagement/MaterialManager.cpp");

    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(implementation.empty());
    ASSERT_FALSE(textureManager.empty());
    ASSERT_FALSE(materialManager.empty());

    EXPECT_NE(header.find("mutable std::recursive_mutex m_resourcesMutex"), std::string::npos);
    EXPECT_NE(header.find("std::unordered_map<std::string, T*> GetResources() const"), std::string::npos)
        << "Resource iteration must use a snapshot so async pumps cannot race callers over the internal cache map.";
    EXPECT_EQ(header.find("std::unordered_map<std::string, T*>& GetResources()"), std::string::npos);

    const auto getResourceBegin = implementation.find("inline T* AResourceManager<T>::GetResource(");
    ASSERT_NE(getResourceBegin, std::string::npos);
    const auto getResourceEnd = implementation.find("inline T* AResourceManager<T>::operator[]", getResourceBegin);
    ASSERT_NE(getResourceEnd, std::string::npos);
    const auto getResourceCode = implementation.substr(getResourceBegin, getResourceEnd - getResourceBegin);
    EXPECT_NE(getResourceCode.find("std::lock_guard lock(m_resourcesMutex)"), std::string::npos);

    const auto registerBegin = implementation.find("inline T* AResourceManager<T>::RegisterResource(");
    ASSERT_NE(registerBegin, std::string::npos);
    const auto registerEnd = implementation.find("inline void AResourceManager<T>::UnregisterResource", registerBegin);
    ASSERT_NE(registerEnd, std::string::npos);
    const auto registerCode = implementation.substr(registerBegin, registerEnd - registerBegin);
    EXPECT_NE(registerCode.find("std::lock_guard lock(m_resourcesMutex)"), std::string::npos);
    EXPECT_NE(registerCode.find("m_resources[p_path] = p_instance"), std::string::npos);

    const auto snapshotBegin = implementation.find("inline std::unordered_map<std::string, T*> AResourceManager<T>::GetResources() const");
    ASSERT_NE(snapshotBegin, std::string::npos);
    const auto snapshotEnd = implementation.find("std::string AResourceManager<T>::GetRealPath", snapshotBegin);
    ASSERT_NE(snapshotEnd, std::string::npos);
    const auto snapshotCode = implementation.substr(snapshotBegin, snapshotEnd - snapshotBegin);
    EXPECT_NE(snapshotCode.find("std::lock_guard lock(m_resourcesMutex)"), std::string::npos);
    EXPECT_NE(snapshotCode.find("return m_resources;"), std::string::npos);

    EXPECT_NE(textureManager.find("RegisterResource(request.path, texture)"), std::string::npos);
    EXPECT_NE(materialManager.find("RegisterResource(request.path, material)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SceneLoadPrefabRestoreRefreshesAssetDatabaseOnceAndCachesArtifacts)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const auto actionsHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.h";
    const std::string source = ReadSourceText(actionsSourcePath);
    const std::string header = ReadSourceText(actionsHeaderPath);

    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(header.empty());
    EXPECT_NE(header.find("bool RestorePrefabInstancesForCurrentSceneFromDisk();"), std::string::npos);

    const auto helperBegin = source.find("bool Editor::Core::EditorActions::RestorePrefabInstancesForCurrentSceneFromDisk()");
    ASSERT_NE(helperBegin, std::string::npos);
    const auto helperEnd = source.find("void Editor::Core::EditorActions::LoadSceneFromDisk(", helperBegin);
    ASSERT_NE(helperEnd, std::string::npos);
    const auto helperCode = source.substr(helperBegin, helperEnd - helperBegin);

    const auto loadBegin = source.find("void Editor::Core::EditorActions::LoadSceneFromDisk(");
    ASSERT_NE(loadBegin, std::string::npos);
    const auto loadEnd = source.find("bool Editor::Core::EditorActions::IsCurrentSceneLoadedFromDisk() const", loadBegin);
    ASSERT_NE(loadEnd, std::string::npos);
    const auto loadCode = source.substr(loadBegin, loadEnd - loadBegin);

    EXPECT_NE(helperCode.find("AssetDatabaseFacade prefabDatabase"), std::string::npos);
    EXPECT_NE(helperCode.find("const auto prefabDatabaseReady = prefabDatabase.Refresh();"), std::string::npos);
    EXPECT_NE(helperCode.find("prefabArtifactCache"), std::string::npos);
    EXPECT_NE(loadCode.find("RestorePrefabInstancesForCurrentSceneFromDisk();"), std::string::npos);
    EXPECT_NE(loadCode.find("SceneLoadProgress& progress"), std::string::npos)
        << "Manual scene switching should surface SceneManager progress instead of appearing frozen.";
    EXPECT_NE(loadCode.find("PresentTaskProgress"), std::string::npos);
    EXPECT_NE(loadCode.find("Restoring scene prefab instances"), std::string::npos);
    EXPECT_NE(loadCode.find("const bool prefabRestoreSucceeded = RestorePrefabInstancesForCurrentSceneFromDisk();"), std::string::npos);
    EXPECT_NE(loadCode.find("CompleteTaskProgress(kSceneLoadProgressTaskKey"), std::string::npos);
    EXPECT_NE(loadCode.find("prefabRestoreSucceeded ? \"Scene loaded\" : \"Scene loaded with prefab restore warnings\""), std::string::npos);
    EXPECT_NE(helperCode.find("QueuePrefabInstanceAssetResolution("), std::string::npos)
        << "Restored generated model prefab instances must re-enter the renderer resource queue; otherwise cold scene loads can restore prefab metadata but leave meshes hidden by strict material/texture gates.";
    EXPECT_NE(helperCode.find("FindCachedRestoredPrefabArtifact"), std::string::npos)
        << "Scene restore must queue renderer resolution with the full prefab artifact loaded from the asset database, not only the registry source graph.";
    EXPECT_EQ(helperCode.find("registeredInstance,\n                nullptr,\n                m_context.sceneManager.GetCurrentSceneSourcePath()"), std::string::npos)
        << "Passing nullptr drops manifest-derived resolvedAssets and reproduces resolvedAssets=0/no renderer tasks on restored generated model prefab instances.";
    EXPECT_NE(helperCode.find("registeredInstance->generatedReadOnly"), std::string::npos);
    EXPECT_NE(helperCode.find("registeredInstance->instanceRoot == object"), std::string::npos);

    const auto restoreBegin = helperCode.find("RestorePrefabInstancesFromSceneDocument(");
    ASSERT_NE(restoreBegin, std::string::npos);
    const auto resolverBegin = helperCode.find("-> std::optional<NLS::Engine::Assets::PrefabArtifact>", restoreBegin);
    ASSERT_NE(resolverBegin, std::string::npos);
    const auto resolverEnd = helperCode.find("});", resolverBegin);
    ASSERT_NE(resolverEnd, std::string::npos);
    const auto resolverCode = helperCode.substr(resolverBegin, resolverEnd - resolverBegin);

    EXPECT_EQ(resolverCode.find(".Refresh("), std::string::npos)
        << "Prefab restore resolver should not refresh the asset database once per prefab instance.";
    EXPECT_NE(resolverCode.find("prefabArtifactCache.find(cacheKey)"), std::string::npos);
    EXPECT_NE(resolverCode.find("prefabArtifactCache.emplace(cacheKey, artifact)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionReloadsPrefabArtifactWhenSourcePrefabMissing)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";
    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("TryLoadPrefabArtifactForRendererResourceResolution"), std::string::npos)
        << "Queueing generated model renderer resolution without a source prefab must reload the full prefab artifact so resolvedAssets are available.";

    const auto queue = source.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(");
    ASSERT_NE(queue, std::string::npos);
    const auto queueEnd = source.find("bool Editor::Core::EditorActions::DestroyGameObject(", queue);
    ASSERT_NE(queueEnd, std::string::npos);
    const auto queueCode = source.substr(queue, queueEnd - queue);

    EXPECT_NE(queueCode.find("TryLoadPrefabArtifactForRendererResourceResolution"), std::string::npos);
    EXPECT_NE(queueCode.find("loadedSourcePrefab"), std::string::npos);
    EXPECT_EQ(queueCode.find("prefab.assetId = instance->prefabAssetId;\n        prefab.graph = instance->sourceGraph;\n        prefab.generatedModelPrefab = instance->generatedReadOnly;"), std::string::npos)
        << "Graph-only fallback leaves resolvedAssets empty and causes no renderer resource tasks.";
    EXPECT_NE(queueCode.find("prefab.resolvedAssets.empty()"), std::string::npos)
        << "Generated model resource resolution must fail early with context if the loaded prefab artifact has no resolved renderer assets.";
}

TEST(EditorRenderPathContractTests, StartupSceneRestoreUsesSharedPrefabRestoreAndProgress)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::string editorSource = ReadSourceText(root / "Project/Editor/Core/Editor.cpp");

    ASSERT_FALSE(editorSource.empty());
    const auto restoreBegin = editorSource.find("void Editor::Core::Editor::RestoreStartupScene()");
    ASSERT_NE(restoreBegin, std::string::npos);
    const auto restoreEnd = editorSource.find("void Editor::Core::Editor::RefreshProjectAssetBrowser()", restoreBegin);
    ASSERT_NE(restoreEnd, std::string::npos);
    const auto restoreCode = editorSource.substr(restoreBegin, restoreEnd - restoreBegin);

    EXPECT_NE(restoreCode.find("RestorePrefabInstancesForCurrentSceneFromDisk();"), std::string::npos)
        << "Startup scene restore must use the same prefab restore path as manual scene open; otherwise saved PrefabInstances remain stripped until the user double-clicks the scene.";
    EXPECT_NE(restoreCode.find("const bool prefabRestoreSucceeded = m_editorActions.RestorePrefabInstancesForCurrentSceneFromDisk();"), std::string::npos);
    EXPECT_NE(restoreCode.find("Restoring startup scene prefab instances"), std::string::npos);
    EXPECT_NE(restoreCode.find("prefabRestoreSucceeded ? \"Startup scene loaded\" : \"Startup scene loaded with prefab restore warnings\""), std::string::npos);
}

TEST(EditorRenderPathContractTests, UnityStylePrefabRestoreReordersAndRebuildsRuntimeCachesOncePerBatch)
{
    const auto facadeSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/PrefabUtilityFacade.cpp";
    const std::string source = ReadSourceText(facadeSourcePath);

    ASSERT_FALSE(source.empty());
    const auto restoreBegin = source.find("PrefabOperationResult RestoreUnityStylePrefabInstancesFromSceneDocument(");
    ASSERT_NE(restoreBegin, std::string::npos);
    const auto restoreEnd = source.find("PrefabAssetType PrefabUtilityFacade::GetPrefabAssetType(", restoreBegin);
    ASSERT_NE(restoreEnd, std::string::npos);
    const auto restoreCode = source.substr(restoreBegin, restoreEnd - restoreBegin);

    EXPECT_EQ(CountOccurrences(restoreCode, "scene.RebuildRuntimeCachesAfterLoad();"), 1u)
        << "Unity-style prefab restore should rebuild scene runtime caches once after the whole batch.";
    EXPECT_EQ(CountOccurrences(restoreCode, "ReorderLiveObjectsBySceneDocumentOrder(sceneDocument, scene, sceneObjectsById);"), 1u)
        << "Unity-style prefab restore should reorder live objects once after all restored prefab instances are known.";

    const auto recoveryBegin = source.find("NLS::Engine::GameObject* RestoreStrippedRecoveryRoot(");
    ASSERT_NE(recoveryBegin, std::string::npos);
    const auto recoveryEnd = source.find("PrefabOperationResult InstantiateStrippedPrefabInstance(", recoveryBegin);
    ASSERT_NE(recoveryEnd, std::string::npos);
    const auto recoveryCode = source.substr(recoveryBegin, recoveryEnd - recoveryBegin);
    EXPECT_EQ(recoveryCode.find("RebuildRuntimeCachesAfterLoad"), std::string::npos);

    const auto instantiateBegin = recoveryEnd;
    const auto instantiateEnd = source.find("PrefabOperationResult RestoreUnityStylePrefabInstancesFromSceneDocument(", instantiateBegin);
    ASSERT_NE(instantiateEnd, std::string::npos);
    const auto instantiateCode = source.substr(instantiateBegin, instantiateEnd - instantiateBegin);
    EXPECT_EQ(instantiateCode.find("RebuildRuntimeCachesAfterLoad"), std::string::npos);
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

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionUsesMultiBindBudgetsForVisibleDropLatency)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("constexpr auto kRendererResourceResolutionFrameBudget = std::chrono::milliseconds(12);"), std::string::npos)
        << "Generated model drops should remain bounded by time instead of dumping all resource binding into one UI tick.";
    EXPECT_NE(source.find("constexpr size_t kRendererResourceResolutionMeshBindsPerFrame = 32u;"), std::string::npos)
        << "Large ready prefabs need enough per-step mesh binding throughput to become visible quickly after mouse release.";
    EXPECT_NE(source.find("constexpr size_t kRendererResourceResolutionTextureBindsPerFrame = 64u;"), std::string::npos)
        << "Texture binding throughput must be high enough that material-ready models do not remain invisible for dozens of steps.";
    EXPECT_EQ(source.find("constexpr size_t kRendererResourceResolutionMeshBindsPerFrame = 4u;"), std::string::npos)
        << "A 4-mesh hard cap makes 400+ mesh prefabs visibly stall even when the frame time budget has room.";
    EXPECT_EQ(source.find("constexpr size_t kRendererResourceResolutionTextureBindsPerFrame = 8u;"), std::string::npos)
        << "The old texture cap was tuned for safety but is too low for ready generated model drops.";
}

TEST(EditorRenderPathContractTests, GeneratedModelResourceResolutionQueuesMeshesBeforeMaterials)
{
    const auto actionsSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp";

    const std::string source = ReadSourceText(actionsSourcePath);

    ASSERT_FALSE(source.empty());
    const auto queue = source.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(");
    ASSERT_NE(queue, std::string::npos);
    const auto constructor = source.find("auto state = std::make_shared<RendererResourceResolutionState>();", queue);
    ASSERT_NE(constructor, std::string::npos);
    const auto queueCode = source.substr(queue, constructor - queue);

    const auto meshEnqueue = queueCode.find("meshTasks.rbegin()");
    const auto materialEnqueue = queueCode.find("materialTasks.begin()");
    ASSERT_NE(meshEnqueue, std::string::npos);
    ASSERT_NE(materialEnqueue, std::string::npos);
    EXPECT_LT(meshEnqueue, materialEnqueue);
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

TEST(EditorRenderPathContractTests, NativeProgressDialogCloseDestroysWindowBeforeQuittingThread)
{
    const auto contextSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Context.cpp";

    std::ifstream contextStream(contextSourcePath, std::ios::binary);
    const std::string contextSource{
        std::istreambuf_iterator<char>(contextStream),
        std::istreambuf_iterator<char>()};
    ASSERT_FALSE(contextSource.empty());

    const auto requestClose = contextSource.find("void RequestClose()");
    const auto runDialogThread = contextSource.find("void RunDialogThread()", requestClose);
    ASSERT_NE(requestClose, std::string::npos);
    ASSERT_NE(runDialogThread, std::string::npos);
    const auto requestCloseSource = contextSource.substr(requestClose, runDialogThread - requestClose);
    EXPECT_NE(requestCloseSource.find("SendMessageW(windowHandle, kProgressCloseMessage"), std::string::npos);
    EXPECT_NE(requestCloseSource.find("else if (m_threadId != 0)"), std::string::npos);

    const auto closeMessage = contextSource.find("message == kProgressCloseMessage");
    const auto destroyMessage = contextSource.find("message == WM_DESTROY", closeMessage);
    const auto wmClose = contextSource.find("message == WM_CLOSE", closeMessage);
    ASSERT_NE(closeMessage, std::string::npos);
    ASSERT_NE(destroyMessage, std::string::npos);
    ASSERT_NE(wmClose, std::string::npos);
    const auto closeMessageSource = contextSource.substr(closeMessage, destroyMessage - closeMessage);
    EXPECT_NE(closeMessageSource.find("DestroyWindow(windowHandle)"), std::string::npos);
    EXPECT_EQ(closeMessageSource.find("PostQuitMessage(0)"), std::string::npos);

    EXPECT_NE(contextSource.substr(destroyMessage, wmClose - destroyMessage).find("PostQuitMessage(0)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, NativeProgressDialogDoesNotShowDefaultStartingEditorBeforeFirstUpdate)
{
    const auto contextSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Context.cpp";

    std::ifstream contextStream(contextSourcePath, std::ios::binary);
    const std::string contextSource{
        std::istreambuf_iterator<char>(contextStream),
        std::istreambuf_iterator<char>()};
    ASSERT_FALSE(contextSource.empty());

    const auto runDialogThread = contextSource.find("void RunDialogThread()");
    const auto markReady = contextSource.find("MarkReady();", runDialogThread);
    const auto applyPendingState = contextSource.find("void ApplyPendingState()", markReady);
    ASSERT_NE(runDialogThread, std::string::npos);
    ASSERT_NE(markReady, std::string::npos);
    ASSERT_NE(applyPendingState, std::string::npos);

    const auto constructionSource = contextSource.substr(runDialogThread, applyPendingState - runDialogThread);
    EXPECT_EQ(constructionSource.find("ShowWindow(m_windowHandle, SW_SHOWNORMAL)"), std::string::npos);

    const auto applyOwnerBlocking = contextSource.find("void ApplyOwnerBlocking", applyPendingState);
    ASSERT_NE(applyOwnerBlocking, std::string::npos);
    const auto applySource = contextSource.substr(applyPendingState, applyOwnerBlocking - applyPendingState);
    EXPECT_NE(applySource.find("!m_visible"), std::string::npos);
    EXPECT_NE(applySource.find("ShowWindow(m_windowHandle, SW_SHOWNORMAL)"), std::string::npos);
    EXPECT_NE(applySource.find("m_visible = true"), std::string::npos);
}

TEST(EditorRenderPathContractTests, FileWatcherPreimportUsesGlobalProgressTracker)
{
    const auto assetBrowserSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/AssetBrowser.cpp";

    const std::string source = ReadSourceText(assetBrowserSourcePath);
    ASSERT_FALSE(source.empty());

    const auto schedulerMethod = source.find(
        "void Editor::Panels::AssetBrowser::ScheduleProjectAssetPreimport(");
    const auto refreshMethod = source.find(
        "void Editor::Panels::AssetBrowser::RefreshPreservingExpandedFolders()",
        schedulerMethod);
    ASSERT_NE(schedulerMethod, std::string::npos);
    ASSERT_NE(refreshMethod, std::string::npos);

    const auto methodSource = source.substr(schedulerMethod, refreshMethod - schedulerMethod);
    EXPECT_NE(methodSource.find("auto& tracker = EDITOR_CONTEXT(importProgressTracker);"), std::string::npos);
    EXPECT_NE(methodSource.find("preimportScheduler.Run(database, tracker, request)"), std::string::npos);
    EXPECT_EQ(methodSource.find("ImportProgressTracker tracker;"), std::string::npos);
    EXPECT_EQ(methodSource.find("std::make_shared<ImportProgressTracker>()"), std::string::npos);
    EXPECT_NE(methodSource.find("LogAssetPreimportFailureDetails"), std::string::npos);
}

TEST(EditorRenderPathContractTests, NativeTaskProgressDoesNotDisableEditorWindow)
{
    const auto contextSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Context.cpp";

    const std::string contextSource = ReadSourceText(contextSourcePath);
    ASSERT_FALSE(contextSource.empty());

    const auto presentTaskProgress = contextSource.find(
        "void Editor::Core::Context::PresentTaskProgress(");
    const auto completeTaskProgress = contextSource.find(
        "void Editor::Core::Context::CompleteTaskProgress(",
        presentTaskProgress);
    ASSERT_NE(presentTaskProgress, std::string::npos);
    ASSERT_NE(completeTaskProgress, std::string::npos);

    const auto methodSource = contextSource.substr(
        presentTaskProgress,
        completeTaskProgress - presentTaskProgress);
    EXPECT_NE(methodSource.find("m_nativeProgressDialog->Update("), std::string::npos);
    EXPECT_NE(
        methodSource.find("window != nullptr ? window->GetNativeWindowHandle() : nullptr"),
        std::string::npos);
    EXPECT_NE(methodSource.find("false);"), std::string::npos);
    EXPECT_EQ(methodSource.find("true);"), std::string::npos);
}

TEST(EditorRenderPathContractTests, NativeTaskProgressCompletionIsScopedToActiveTaskKey)
{
    const auto contextHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Context.h";
    const auto contextSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Context.cpp";

    const std::string contextHeader = ReadSourceText(contextHeaderPath);
    const std::string contextSource = ReadSourceText(contextSourcePath);
    ASSERT_FALSE(contextHeader.empty());
    ASSERT_FALSE(contextSource.empty());

    EXPECT_NE(contextHeader.find("void CompleteTaskProgress(uint64_t taskKey, const std::string& label);"), std::string::npos);

    const auto keyedComplete = contextSource.find(
        "void Editor::Core::Context::CompleteTaskProgress(\n    const uint64_t taskKey,");
    ASSERT_NE(keyedComplete, std::string::npos);
    const auto unkeyedComplete = contextSource.find(
        "void Editor::Core::Context::CompleteTaskProgress(const std::string& label)",
        keyedComplete);
    ASSERT_NE(unkeyedComplete, std::string::npos);
    const auto keyedCompleteCode = contextSource.substr(keyedComplete, unkeyedComplete - keyedComplete);
    EXPECT_NE(keyedCompleteCode.find("m_activeTaskProgressKey != taskKey"), std::string::npos)
        << "Scene loading must not close an unrelated import progress task.";
    EXPECT_NE(keyedCompleteCode.find("m_nativeProgressDialog->Update(label, 1.0f)"), std::string::npos);
    EXPECT_NE(keyedCompleteCode.find("m_activeTaskProgressKey = 0u"), std::string::npos);
    EXPECT_NE(keyedCompleteCode.find("dialog->Close();"), std::string::npos);
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
    EXPECT_NE(source.find("FindCachedMeshByEquivalentPath(meshManager, task.modelPath)"), std::string::npos);
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

TEST(EditorRenderPathContractTests, DeferredThreadedSkipsPublishWhenOpaqueSceneCannotQueueGBuffer)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto deferredHeader = ReadSourceText(root / "Runtime/Engine/Rendering/DeferredSceneRenderer.h");
    const auto deferredSource = ReadSourceText(root / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    ASSERT_FALSE(deferredHeader.empty());
    ASSERT_FALSE(deferredSource.empty());

    EXPECT_NE(deferredHeader.find("bool TryPublishThreadedFrame() override"), std::string::npos);
    EXPECT_NE(deferredHeader.find("m_skipThreadedFramePublish"), std::string::npos);
    EXPECT_NE(deferredSource.find("ShouldSkipThreadedDeferredFramePublish"), std::string::npos);
    EXPECT_NE(deferredSource.find("snapshot.visibleOpaqueDrawCount > 0u"), std::string::npos);
    EXPECT_NE(deferredSource.find("queuedGBufferDrawCount == 0u"), std::string::npos);
    EXPECT_NE(deferredSource.find("m_skipThreadedFramePublish = true"), std::string::npos);
    EXPECT_NE(deferredSource.find("bool DeferredSceneRenderer::TryPublishThreadedFrame()"), std::string::npos);
    EXPECT_NE(deferredSource.find("if (m_skipThreadedFramePublish)"), std::string::npos);
}

TEST(EditorRenderPathContractTests, DeferredThreadedPreflightsPreparedFrameSlotBeforeGBufferCapture)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto deferredSource = ReadSourceText(root / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    ASSERT_FALSE(deferredSource.empty());

    const auto beginFrame = deferredSource.find("void DeferredSceneRenderer::BeginFrame");
    ASSERT_NE(beginFrame, std::string::npos);
    const auto ensureGBuffer = deferredSource.find("EnsureGBufferTargets", beginFrame);
    ASSERT_NE(ensureGBuffer, std::string::npos);
    const auto preflight = deferredSource.find("TryReservePreparedFrameResources", beginFrame);
    ASSERT_NE(preflight, std::string::npos);
    EXPECT_LT(preflight, ensureGBuffer);

    const auto captureLoop = deferredSource.find("for (const auto& entry : drawables.opaques)", ensureGBuffer);
    ASSERT_NE(captureLoop, std::string::npos);
    const auto preflightBlock = deferredSource.substr(preflight, captureLoop - preflight);
    EXPECT_NE(preflightBlock.find("m_skipThreadedFramePublish = true"), std::string::npos);
    EXPECT_NE(preflightBlock.find("queuedGBufferDrawCount == 0u"), std::string::npos);
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskDelaysPreparedFrameSlotReservationUntilDrawCaptureNeedsIt)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");
    const auto rendererSource = ReadSourceText(root / "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    ASSERT_FALSE(debugSceneSource.empty());
    ASSERT_FALSE(rendererSource.empty());

    const auto buildThreadedPassInput = debugSceneSource.find("SelectionOutlinePreparedOutput BuildThreadedPassInput");
    ASSERT_NE(buildThreadedPassInput, std::string::npos);
    const auto buildThreadedPassInputEnd = debugSceneSource.find("private:", buildThreadedPassInput);
    ASSERT_NE(buildThreadedPassInputEnd, std::string::npos);
    const auto buildThreadedPassInputBody =
        debugSceneSource.substr(buildThreadedPassInput, buildThreadedPassInputEnd - buildThreadedPassInput);

    const auto buildPreparedOutput = buildThreadedPassInputBody.find("BuildPreparedOutput");
    ASSERT_NE(buildPreparedOutput, std::string::npos);
    const auto earlyReservation = buildThreadedPassInputBody.find(
        "TryReservePreparedFrameResources",
        0u);
    if (earlyReservation != std::string::npos)
        EXPECT_GT(earlyReservation, buildPreparedOutput)
            << "Composite-only cached-mask frames must not preflight or wait for an object-data slot before BuildPreparedOutput can decide whether CaptureMask is needed.";

    EXPECT_NE(buildThreadedPassInputBody.find("releaseSelectionOwnedPreparedFrameReservation"), std::string::npos)
        << "Failure paths must still release a slot if draw capture reserved one later.";
    EXPECT_NE(rendererSource.find("CaptureMaskDrawCommands("), std::string::npos)
        << "Mask capture remains the point where indexed object-data draws can reserve prepared-frame resources.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskReleasesOnlySelectionOwnedPreparedFrameReservationWhenNoHelperPassIsEmitted)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    ASSERT_FALSE(debugSceneSource.empty());

    const auto buildThreadedPassInput = debugSceneSource.find("SelectionOutlinePreparedOutput BuildThreadedPassInput");
    ASSERT_NE(buildThreadedPassInput, std::string::npos);
    const auto buildThreadedPassInputEnd = debugSceneSource.find("private:", buildThreadedPassInput);
    ASSERT_NE(buildThreadedPassInputEnd, std::string::npos);
    const auto buildThreadedPassInputBody =
        debugSceneSource.substr(buildThreadedPassInput, buildThreadedPassInputEnd - buildThreadedPassInput);

    const auto reservationSnapshot =
        buildThreadedPassInputBody.find("hadPreparedFrameReservationBeforeSelection");
    const auto releaseHelper = buildThreadedPassInputBody.find("releaseSelectionOwnedPreparedFrameReservation");
    const auto buildPreparedOutput = buildThreadedPassInputBody.find("BuildPreparedOutput");
    ASSERT_NE(reservationSnapshot, std::string::npos);
    ASSERT_NE(releaseHelper, std::string::npos);
    ASSERT_NE(buildPreparedOutput, std::string::npos);
    EXPECT_LT(reservationSnapshot, releaseHelper);
    EXPECT_LT(releaseHelper, buildPreparedOutput);
    EXPECT_EQ(
        buildThreadedPassInputBody.find("TryReservePreparedFrameResources()", 0u),
        std::string::npos)
        << "Stable composite-only selection frames must not pre-reserve a prepared object-data slot.";

    EXPECT_NE(
        buildThreadedPassInputBody.find("frameObjectBindingProvider->HasReservedPreparedFrameResources()"),
        std::string::npos);
    EXPECT_NE(
        buildThreadedPassInputBody.find("!hadPreparedFrameReservationBeforeSelection"),
        std::string::npos)
        << "Selection fallback paths must not release a GBuffer reservation that existed before the selected-outline pass.";
    EXPECT_NE(
        buildThreadedPassInputBody.find("frameObjectBindingProvider->ReleaseReservedPreparedFrameResources()"),
        std::string::npos);

    const auto fallbackAction = buildThreadedPassInputBody.find("ResolveSelectionOutlineFallbackAction");
    const auto legacyCapture = buildThreadedPassInputBody.find("m_outlineRenderer.CaptureOutlineDrawCommands");
    const auto zeroDraw = buildThreadedPassInputBody.find("if (passInput.drawCount == 0u)");
    ASSERT_NE(fallbackAction, std::string::npos);
    ASSERT_NE(legacyCapture, std::string::npos);
    ASSERT_NE(zeroDraw, std::string::npos);

    const auto noFallbackDecision = buildThreadedPassInputBody.find("!maskOutput.fallbackDecision.has_value()");
    ASSERT_NE(noFallbackDecision, std::string::npos);
    EXPECT_LT(buildPreparedOutput, noFallbackDecision);
    EXPECT_LT(noFallbackDecision, fallbackAction);

    const auto noFallbackReturn = buildThreadedPassInputBody.find("return maskOutput;", noFallbackDecision);
    ASSERT_NE(noFallbackReturn, std::string::npos);
    const auto noFallbackBlock = buildThreadedPassInputBody.substr(
        noFallbackDecision,
        noFallbackReturn - noFallbackDecision);
    EXPECT_NE(noFallbackBlock.find("releaseSelectionOwnedPreparedFrameReservation()"), std::string::npos)
        << "No-output paths must release only a reservation created by selected-object draw capture.";

    const auto skipActionReturn = buildThreadedPassInputBody.find("return maskOutput;", fallbackAction);
    ASSERT_NE(skipActionReturn, std::string::npos);
    const auto skipActionBlock = buildThreadedPassInputBody.substr(
        fallbackAction,
        skipActionReturn - fallbackAction);
    EXPECT_NE(skipActionBlock.find("releaseSelectionOwnedPreparedFrameReservation()"), std::string::npos)
        << "Fallback actions that emit no helper pass must release only a selected-object-owned reservation before returning.";

    const auto legacyCompatibility = buildThreadedPassInputBody.find(
        "SelectionOutlineLegacyShellFallbackIsAttachmentCompatible",
        fallbackAction);
    ASSERT_NE(legacyCompatibility, std::string::npos);
    ASSERT_LT(legacyCompatibility, legacyCapture);
    const auto compatibilityReturn = buildThreadedPassInputBody.find("return maskOutput;", legacyCompatibility);
    ASSERT_NE(compatibilityReturn, std::string::npos);
    ASSERT_LT(compatibilityReturn, legacyCapture);
    const auto compatibilityBlock = buildThreadedPassInputBody.substr(
        legacyCompatibility,
        compatibilityReturn - legacyCompatibility);
    EXPECT_NE(compatibilityBlock.find("releaseSelectionOwnedPreparedFrameReservation()"), std::string::npos)
        << "Legacy fallback paths rejected before command capture must release only a selected-object-owned reservation.";

    const auto zeroDrawReturn = buildThreadedPassInputBody.find("return maskOutput;", zeroDraw);
    ASSERT_NE(zeroDrawReturn, std::string::npos);
    const auto zeroDrawBlock = buildThreadedPassInputBody.substr(
        zeroDraw,
        zeroDrawReturn - zeroDraw);
    EXPECT_NE(zeroDrawBlock.find("releaseSelectionOwnedPreparedFrameReservation()"), std::string::npos);

    const auto legacyCaptureBlock = buildThreadedPassInputBody.substr(
        compatibilityReturn,
        legacyCapture - compatibilityReturn);
    EXPECT_EQ(legacyCaptureBlock.find("releaseSelectionOwnedPreparedFrameReservation()"), std::string::npos)
        << "The reservation must remain live while legacy fallback draw commands are captured.";
}

TEST(EditorRenderPathContractTests, SelectionOutlineMaskSkipsSelectedObjectWorkWhenDeferredPublishIsSkipped)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto deferredHeader = ReadSourceText(root / "Runtime/Engine/Rendering/DeferredSceneRenderer.h");
    const auto debugSceneSource = ReadSourceText(root / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    ASSERT_FALSE(deferredHeader.empty());
    ASSERT_FALSE(debugSceneSource.empty());

    EXPECT_NE(
        deferredHeader.find("bool IsThreadedFramePublishSkippedForCurrentFrame() const"),
        std::string::npos);

    const auto passStart = debugSceneSource.find("class DebugGameObjectRenderPass");
    ASSERT_NE(passStart, std::string::npos);
    const auto drawStart = debugSceneSource.find("virtual void Draw(Render::Data::PipelineState p_pso) override", passStart);
    ASSERT_NE(drawStart, std::string::npos);
    const auto drawEnd = debugSceneSource.find("static bool ShouldSubmitDebugGameObjectElements", drawStart);
    ASSERT_NE(drawEnd, std::string::npos);
    const auto drawBody = debugSceneSource.substr(drawStart, drawEnd - drawStart);
    const auto drawSkip = drawBody.find("IsDeferredThreadedFramePublishSkipped");
    const auto collectSelectedTree = drawBody.find("PrepareDebugGameObjectDebugDrawItems");
    ASSERT_NE(drawSkip, std::string::npos);
    ASSERT_NE(collectSelectedTree, std::string::npos);
    EXPECT_LT(drawSkip, collectSelectedTree);

    const auto buildThreadedPassInput = debugSceneSource.find("SelectionOutlinePreparedOutput BuildThreadedPassInput", passStart);
    ASSERT_NE(buildThreadedPassInput, std::string::npos);
    const auto buildPreparedOutput = debugSceneSource.find("BuildPreparedOutput", buildThreadedPassInput);
    ASSERT_NE(buildPreparedOutput, std::string::npos);
    const auto buildSkip = debugSceneSource.find("IsDeferredThreadedFramePublishSkipped", buildThreadedPassInput);
    ASSERT_NE(buildSkip, std::string::npos);
    EXPECT_LT(buildSkip, buildPreparedOutput);
}

TEST(EditorRenderPathContractTests, SceneValidationAssetReadbackWaitsForFocusedCreatedSelection)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto sceneViewSource = ReadSourceText(root / "Project/Editor/Panels/SceneView.cpp");

    ASSERT_FALSE(sceneViewSource.empty());

    const auto validationCreateAssetCheck = sceneViewSource.find("diagnostics.editorValidationCreateAsset.empty()");
    ASSERT_NE(validationCreateAssetCheck, std::string::npos);
    const auto stableFrameCheck = sceneViewSource.find("kStableFramesAfterAssetCreation", validationCreateAssetCheck);
    ASSERT_NE(stableFrameCheck, std::string::npos);
    const auto readinessBlock = sceneViewSource.substr(
        validationCreateAssetCheck,
        stableFrameCheck - validationCreateAssetCheck);

    EXPECT_NE(readinessBlock.find("EDITOR_EXEC(GetSelectedGameObject())"), std::string::npos);
    EXPECT_NE(readinessBlock.find("selectedGameObject == nullptr"), std::string::npos);
    EXPECT_EQ(readinessBlock.find("std::filesystem::path(diagnostics.editorValidationCreateAsset).stem()"), std::string::npos);
    EXPECT_EQ(readinessBlock.find("FindGameObjectByName(expectedName)"), std::string::npos);
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

    auto resources = MakeCompleteDeferredPreparedSceneResources();

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
        MakeCompleteDeferredPreparedSceneResources(),
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

    auto resources = MakeCompleteDeferredPreparedSceneResources();

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

    auto resources = MakeCompleteDeferredPreparedSceneResources();

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

    auto resources = MakeCompleteDeferredPreparedSceneResources();

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

    auto resources = MakeCompleteDeferredPreparedSceneResources();

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
        MakeCompleteDeferredPreparedSceneResources(),
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
        MakeCompleteDeferredPreparedSceneResources(),
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

TEST(EditorRenderPathContractTests, DeferredEditorOverlayPassInputIsConsumedOnceForDuplicateMetadata)
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
        MakeCompleteDeferredPreparedSceneResources(),
        { gridPass },
        { gridMetadata, gridMetadata });

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    EXPECT_EQ(package.passCommandInputs[2].debugName, "EditorGridPass");
    ASSERT_EQ(package.passCommandInputs[2].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[2].recordedDrawCommands[0].instanceCount, 23u);
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

TEST(EditorRenderPathContractTests, DebugCameraPassKeepsBackfaceCullingForCameraMesh)
{
    const std::filesystem::path debugSceneRendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp";

    const std::string source = ReadSourceText(debugSceneRendererSourcePath);

    ASSERT_FALSE(source.empty());
    const auto camerasPassStart = source.find("class DebugCamerasRenderPass");
    ASSERT_NE(camerasPassStart, std::string::npos);
    const auto lightsPassStart = source.find("class DebugLightsRenderPass", camerasPassStart);
    ASSERT_NE(lightsPassStart, std::string::npos);

    const auto camerasPassSource = source.substr(camerasPassStart, lightsPassStart - camerasPassStart);
    EXPECT_NE(camerasPassSource.find("CreateEditorOverlayPipelineState"), std::string::npos);
    EXPECT_NE(camerasPassSource.find("p_pso.culling = true;"), std::string::npos);
    EXPECT_NE(
        camerasPassSource.find("p_pso.cullFace = NLS::Render::Settings::ECullFace::BACK;"),
        std::string::npos);
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
        MakeCompleteDeferredPreparedSceneResources());

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
    gbufferDepthDesc.format = NLS::Render::FrameGraph::kDeferredGBufferDepthFormat;
    gbufferDepthDesc.usage = NLS::Render::FrameGraph::kDeferredGBufferDepthUsage;
    auto gbufferDepthTexture = std::make_shared<TestTexture>(gbufferDepthDesc);

    NLS::Render::RHI::RHITextureViewDesc gbufferDepthViewDesc;
    gbufferDepthViewDesc.debugName = "DeferredGBufferDepthView";
    gbufferDepthViewDesc.format = gbufferDepthDesc.format;
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

    auto resources = MakeCompleteDeferredPreparedSceneResources(gbufferDepthView);

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
    gbufferDepthDesc.format = NLS::Render::FrameGraph::kDeferredGBufferDepthFormat;
    gbufferDepthDesc.usage = NLS::Render::FrameGraph::kDeferredGBufferDepthUsage;
    auto gbufferDepthTexture = std::make_shared<TestTexture>(gbufferDepthDesc);

    NLS::Render::RHI::RHITextureViewDesc gbufferDepthViewDesc;
    gbufferDepthViewDesc.debugName = "DeferredGBufferDepthView";
    gbufferDepthViewDesc.format = gbufferDepthDesc.format;
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

    std::vector<NLS::Render::Context::RenderPassCommandInput> appendedPassInputs {
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

    auto resources = MakeCompleteDeferredPreparedSceneResources(gbufferDepthView);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        std::move(appendedPassInputs),
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
    gbufferDepthDesc.format = NLS::Render::FrameGraph::kDeferredGBufferDepthFormat;
    gbufferDepthDesc.usage = NLS::Render::FrameGraph::kDeferredGBufferDepthUsage;
    auto gbufferDepthTexture = std::make_shared<TestTexture>(gbufferDepthDesc);

    NLS::Render::RHI::RHITextureViewDesc gbufferDepthViewDesc;
    gbufferDepthViewDesc.debugName = "DeferredGBufferDepthView";
    gbufferDepthViewDesc.format = gbufferDepthDesc.format;
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

    auto resources = MakeCompleteDeferredPreparedSceneResources(gbufferDepthView);

    NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
        package,
        lightGridContext,
        resources,
        std::move(appendedPassInputs),
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
