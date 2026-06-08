#include <gtest/gtest.h>

#include "GameObject.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MeshFilter.h"
#include "SceneSystem/Scene.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/GridRenderPass.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/EditorHelperLifecycle.h"
#include "Rendering/OutlineRenderer.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"

#include <fstream>
#include <filesystem>
#include <sstream>

namespace
{
    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        std::ostringstream stream;
        stream << file.rdbuf();
        return stream.str();
    }

    class TestBindingSet final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        explicit TestBindingSet(std::string debugName)
        {
            m_desc.debugName = std::move(debugName);
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingSetDesc m_desc{};
    };

    struct SceneSnapshotVisibilityHarness : NLS::Engine::Rendering::BaseSceneRenderer
    {
        using NLS::Engine::Rendering::BaseSceneRenderer::RefreshFrameSnapshotVisibility;
    };

}

TEST(DebugSceneLifecycleTests, CountsActiveEditorHelpersForThreadedSnapshotPlanning)
{
    NLS::Editor::Rendering::ThreadedEditorHelperState helperState;
    helperState.gridEnabled = true;
    helperState.sceneCameraCount = 2u;
    helperState.sceneLightCount = 3u;
    helperState.hasSelectedGameObject = true;
    helperState.hasVisibleDebugDrawPrimitives = true;

    EXPECT_EQ(NLS::Editor::Rendering::CountThreadedEditorHelperPasses(helperState), 6u);
}

TEST(DebugSceneLifecycleTests, SkipsDisabledOrEmptyEditorHelpersForThreadedSnapshotPlanning)
{
    NLS::Editor::Rendering::ThreadedEditorHelperState helperState;
    helperState.gridEnabled = false;
    helperState.sceneCameraCount = 0u;
    helperState.sceneLightCount = 0u;
    helperState.hasSelectedGameObject = false;
    helperState.hasVisibleDebugDrawPrimitives = false;

    EXPECT_EQ(NLS::Editor::Rendering::CountThreadedEditorHelperPasses(helperState), 0u);
}

TEST(DebugSceneLifecycleTests, SkipsEditorHelpersWhosePassesAreDisabledForThreadedPlanning)
{
    NLS::Editor::Rendering::ThreadedEditorHelperState helperState;
    helperState.gridPassEnabled = false;
    helperState.cameraPassEnabled = false;
    helperState.lightPassEnabled = false;
    helperState.gameObjectPassEnabled = false;
    helperState.debugDrawPassEnabled = false;
    helperState.gridEnabled = true;
    helperState.sceneCameraCount = 2u;
    helperState.sceneLightCount = 3u;
    helperState.hasSelectedGameObject = true;
    helperState.hasVisibleDebugDrawPrimitives = true;

    EXPECT_EQ(NLS::Editor::Rendering::CountThreadedEditorHelperPasses(helperState), 0u);
}

TEST(DebugSceneLifecycleTests, SceneVisibilityRefreshPreservesEditorHelperDrawCount)
{
    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.visibleHelperDrawCount = 3u;

    NLS::Engine::Rendering::BaseSceneRenderer::AllDrawables drawables;
    NLS::Render::Entities::Drawable opaqueDrawable;
    drawables.opaques.emplace_back(0.0f, opaqueDrawable);

    SceneSnapshotVisibilityHarness::RefreshFrameSnapshotVisibility(snapshot, drawables);

    EXPECT_EQ(snapshot.visibleOpaqueDrawCount, 1u);
    EXPECT_EQ(snapshot.visibleHelperDrawCount, 3u);
}

TEST(DebugSceneLifecycleTests, CullingOverlayItemsAreBuiltFromFrameSnapshotOnlyWhenEnabled)
{
    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.largeSceneCullReasonSnapshot.entries.push_back({
        0x90u,
        7u,
        3u,
        5u,
        0u,
        10u,
        11u,
        false
    });
    snapshot.largeSceneCullReasonSnapshot.entries.push_back({
        0x90u,
        8u,
        3u,
        0u,
        0u,
        11u,
        12u,
        true
    });

    NLS::Editor::Rendering::DebugSceneRenderer::CullingOverlayOptions options;
    EXPECT_TRUE(NLS::Editor::Rendering::BuildDebugSceneCullingOverlayItems(snapshot, options).empty());
    EXPECT_FALSE(NLS::Editor::Rendering::ShouldPublishDebugSceneCullReasonSnapshots(options));

    options.enabled = true;
    options.maxItems = 0u;
    EXPECT_FALSE(NLS::Editor::Rendering::ShouldPublishDebugSceneCullReasonSnapshots(options));

    options.includeVisiblePrimitives = false;
    options.maxItems = 4u;

    const auto hiddenOnlyItems =
        NLS::Editor::Rendering::BuildDebugSceneCullingOverlayItems(snapshot, options);
    ASSERT_EQ(hiddenOnlyItems.size(), 1u);
    EXPECT_EQ(hiddenOnlyItems[0].sceneId, 0x90u);
    EXPECT_EQ(hiddenOnlyItems[0].primitiveIndex, 7u);
    EXPECT_EQ(hiddenOnlyItems[0].primitiveGeneration, 3u);
    EXPECT_EQ(hiddenOnlyItems[0].reason, 5u);
    EXPECT_FALSE(hiddenOnlyItems[0].visible);
    EXPECT_TRUE(NLS::Editor::Rendering::ShouldPublishDebugSceneCullReasonSnapshots(options));

    options.includeVisiblePrimitives = true;
    EXPECT_EQ(NLS::Editor::Rendering::BuildDebugSceneCullingOverlayItems(snapshot, options).size(), 2u);
}

TEST(DebugSceneLifecycleTests, GridHelperRequiresPassDescriptorAndDebugDrawSettings)
{
    using NLS::Editor::Rendering::GridRenderPass;

    EXPECT_TRUE(GridRenderPass::ShouldIncludeInThreadedFrame(true, true, true, true));
    EXPECT_FALSE(GridRenderPass::ShouldIncludeInThreadedFrame(false, true, true, true));
    EXPECT_FALSE(GridRenderPass::ShouldIncludeInThreadedFrame(true, false, true, true));
    EXPECT_FALSE(GridRenderPass::ShouldIncludeInThreadedFrame(true, true, false, true));
    EXPECT_FALSE(GridRenderPass::ShouldIncludeInThreadedFrame(true, true, true, false));
}

TEST(DebugSceneLifecycleTests, SelectionHelpersRequireGameObjectPassAndSelectedGameObject)
{
    using NLS::Editor::Rendering::OutlineRenderer;

    NLS::Engine::GameObject actor("Selected", "Editor");

    EXPECT_FALSE(OutlineRenderer::ShouldIncludeInThreadedFrame(false, &actor));
    EXPECT_FALSE(OutlineRenderer::ShouldIncludeInThreadedFrame(true, nullptr));
    EXPECT_TRUE(OutlineRenderer::ShouldIncludeInThreadedFrame(true, &actor));
}

TEST(DebugSceneLifecycleTests, EditorHelperDrawsDoNotReceiveLightGridPassBindings)
{
    auto placeholder = NLS::Engine::Rendering::BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder();
    auto lightGridBindingSet = std::make_shared<TestBindingSet>("LightGridGraphicsBindingSet");

    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.helperDrawCount = 1u;
    package.recordedDrawCommands.resize(3u);
    for (auto& drawCommand : package.recordedDrawCommands)
        drawCommand.passBindingSet = placeholder;

    NLS::Render::Context::RenderPassCommandInput opaqueInput;
    opaqueInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaqueInput.recordedDrawCommands.resize(1u);
    opaqueInput.recordedDrawCommands[0].passBindingSet = placeholder;

    NLS::Render::Context::RenderPassCommandInput transparentInput;
    transparentInput.kind = NLS::Render::Context::RenderPassCommandKind::Transparent;
    transparentInput.recordedDrawCommands.resize(1u);
    transparentInput.recordedDrawCommands[0].passBindingSet = placeholder;

    NLS::Render::Context::RenderPassCommandInput helperInput;
    helperInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    helperInput.debugName = "EditorHelperPass";
    helperInput.recordedDrawCommands.resize(1u);
    helperInput.recordedDrawCommands[0].passBindingSet = placeholder;

    package.passCommandInputs = {
        std::move(opaqueInput),
        std::move(transparentInput),
        std::move(helperInput)
    };

    NLS::Engine::Rendering::BaseSceneRenderer::ResolvePreparedScenePassBindingSetPlaceholders(
        package,
        lightGridBindingSet,
        package.opaqueDrawCount + package.skyboxDrawCount + package.transparentDrawCount);

    EXPECT_EQ(package.recordedDrawCommands[0].passBindingSet, lightGridBindingSet);
    EXPECT_EQ(package.recordedDrawCommands[1].passBindingSet, lightGridBindingSet);
    EXPECT_EQ(package.recordedDrawCommands[2].passBindingSet, nullptr);
    EXPECT_EQ(package.passCommandInputs[0].recordedDrawCommands[0].passBindingSet, lightGridBindingSet);
    EXPECT_EQ(package.passCommandInputs[1].recordedDrawCommands[0].passBindingSet, lightGridBindingSet);
    EXPECT_EQ(package.passCommandInputs[2].recordedDrawCommands[0].passBindingSet, nullptr);
}

TEST(DebugSceneLifecycleTests, DeferredGBufferDrawsReceiveLightGridPassBindingPlaceholders)
{
    auto placeholder = NLS::Engine::Rendering::BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder();
    auto lightGridBindingSet = std::make_shared<TestBindingSet>("LightGridGraphicsBindingSet");

    NLS::Render::Context::RenderScenePackage package;
    package.opaqueDrawCount = 1u;
    package.recordedDrawCommands.resize(2u);
    package.recordedDrawCommands[0].passBindingSet = placeholder;
    package.recordedDrawCommands[1].passBindingSet = placeholder;

    NLS::Render::Context::RenderPassCommandInput gbufferInput;
    gbufferInput.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    gbufferInput.recordedDrawCommands.resize(1u);
    gbufferInput.recordedDrawCommands[0].passBindingSet = placeholder;

    NLS::Render::Context::RenderPassCommandInput lightingInput;
    lightingInput.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingInput.recordedDrawCommands.resize(1u);
    lightingInput.recordedDrawCommands[0].passBindingSet = placeholder;

    package.passCommandInputs = {
        std::move(gbufferInput),
        std::move(lightingInput)
    };

    NLS::Engine::Rendering::BaseSceneRenderer::ResolvePreparedScenePassBindingSetPlaceholders(
        package,
        lightGridBindingSet,
        package.opaqueDrawCount);

    EXPECT_EQ(package.recordedDrawCommands[0].passBindingSet, lightGridBindingSet);
    EXPECT_EQ(package.recordedDrawCommands[1].passBindingSet, nullptr);
    EXPECT_EQ(package.passCommandInputs[0].recordedDrawCommands[0].passBindingSet, lightGridBindingSet);
    EXPECT_EQ(package.passCommandInputs[1].recordedDrawCommands[0].passBindingSet, lightGridBindingSet);
}

TEST(DebugSceneLifecycleTests, SceneDestructionBroadcastsGameObjectDestroyedBeforeDeletingGameObjects)
{
    size_t destroyedCount = 0u;
    const auto listener = NLS::Engine::GameObject::DestroyedEvent +=
        [&destroyedCount](NLS::Engine::GameObject& actor)
        {
            if (actor.GetName() == "Selected")
            {
                ++destroyedCount;
                EXPECT_NE(actor.GetTransform(), nullptr);
            }
        };

    {
        NLS::Engine::SceneSystem::Scene scene;
        scene.CreateGameObject("Selected");
    }

    NLS::Engine::GameObject::DestroyedEvent -= listener;
    EXPECT_EQ(destroyedCount, 1u);
}

TEST(DebugSceneLifecycleTests, DestroyGameObjectRebuildsFastAccessAfterRemovedPointerLeavesSceneList)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& first = scene.CreateGameObject("First");
    first.AddComponent<NLS::Engine::Components::MeshRenderer>();
    auto& second = scene.CreateGameObject("Second");
    second.AddComponent<NLS::Engine::Components::MeshRenderer>();

    ASSERT_EQ(scene.GetFastAccessComponents().modelRenderers.size(), 2u);

    ASSERT_TRUE(scene.DestroyGameObject(first));

    const auto& fastAccess = scene.GetFastAccessComponents();
    ASSERT_EQ(fastAccess.modelRenderers.size(), 1u);
    ASSERT_NE(fastAccess.modelRenderers[0], nullptr);
    EXPECT_EQ(fastAccess.modelRenderers[0]->gameobject(), &second);
}

TEST(DebugSceneLifecycleTests, FastAccessRebuildKeepsDuplicateComponentsOnSameGameObject)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("Renderer");
    auto* first = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    auto* second = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const auto& fastAccess = scene.GetFastAccessComponents();
    ASSERT_EQ(fastAccess.modelRenderers.size(), 2u);
    EXPECT_EQ(fastAccess.modelRenderers[0], first);
    EXPECT_EQ(fastAccess.modelRenderers[1], second);
}

TEST(DebugSceneLifecycleTests, DestroyGameObjectRemovesPointerBeforeDeletingAndRebuildingFastAccess)
{
    const auto sceneSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/SceneSystem/Scene.cpp");
    const auto destroyFunction = sceneSource.substr(sceneSource.find("bool Scene::DestroyGameObject"));

    const auto removePosition = destroyFunction.find("RemoveGameObjectsFromSceneList(collected)");
    const auto destroyPosition = destroyFunction.find("DestroyCollectedGameObjects(gameObjects)");

    ASSERT_NE(removePosition, std::string::npos);
    ASSERT_NE(destroyPosition, std::string::npos);
    EXPECT_LT(removePosition, destroyPosition);
}

TEST(DebugSceneLifecycleTests, DestroyGameObjectDestroysChildSubtreeOnceAndRebuildsFastAccess)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("Parent");
    parent.AddComponent<NLS::Engine::Components::MeshRenderer>();
    auto& child = scene.CreateGameObject("Child");
    child.AddComponent<NLS::Engine::Components::CameraComponent>();
    child.SetParent(parent);

    ASSERT_EQ(scene.GetGameObjects().size(), 2u);
    ASSERT_EQ(scene.GetFastAccessComponents().modelRenderers.size(), 1u);
    ASSERT_EQ(scene.GetFastAccessComponents().cameras.size(), 1u);

    size_t parentDestroyed = 0u;
    size_t childDestroyed = 0u;
    const auto listener = NLS::Engine::GameObject::DestroyedEvent +=
        [&parentDestroyed, &childDestroyed](NLS::Engine::GameObject& actor)
        {
            if (actor.GetName() == "Parent")
                ++parentDestroyed;
            if (actor.GetName() == "Child")
                ++childDestroyed;
        };

    ASSERT_TRUE(scene.DestroyGameObject(parent));

    NLS::Engine::GameObject::DestroyedEvent -= listener;
    EXPECT_EQ(parentDestroyed, 1u);
    EXPECT_EQ(childDestroyed, 1u);
    EXPECT_TRUE(scene.GetGameObjects().empty());
    EXPECT_TRUE(scene.GetFastAccessComponents().modelRenderers.empty());
    EXPECT_TRUE(scene.GetFastAccessComponents().cameras.empty());
}

TEST(DebugSceneLifecycleTests, CollectGarbagesDestroysMarkedChildSubtreeOnce)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("Parent");
    auto& child = scene.CreateGameObject("Child");
    child.SetParent(parent);

    size_t parentDestroyed = 0u;
    size_t childDestroyed = 0u;
    const auto listener = NLS::Engine::GameObject::DestroyedEvent +=
        [&parentDestroyed, &childDestroyed](NLS::Engine::GameObject& actor)
        {
            if (actor.GetName() == "Parent")
                ++parentDestroyed;
            if (actor.GetName() == "Child")
                ++childDestroyed;
        };

    parent.MarkAsDestroy();
    scene.CollectGarbages();

    NLS::Engine::GameObject::DestroyedEvent -= listener;
    EXPECT_EQ(parentDestroyed, 1u);
    EXPECT_EQ(childDestroyed, 1u);
    EXPECT_TRUE(scene.GetGameObjects().empty());
}

TEST(DebugSceneLifecycleTests, MeshRendererUpdateMaterialListClampsMeshMaterialSlotsToCapacity)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("Renderer");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);

    NLS::Render::Resources::Mesh mesh(
        std::vector<NLS::Render::Geometry::Vertex> {},
        std::vector<uint32_t> {},
        static_cast<uint32_t>(NLS::Engine::Components::MeshRenderer::kMaxMaterialCount));

    meshFilter->SetMesh(&mesh);
    meshRenderer->UpdateMaterialList();
    NLS::Render::Resources::Material fallbackMaterial;
    meshRenderer->FillWithMaterial(fallbackMaterial);

    const auto& resolvedMaterials = meshRenderer->GetMaterials();
    EXPECT_EQ(
        std::count(resolvedMaterials.begin(), resolvedMaterials.end(), &fallbackMaterial),
        static_cast<std::ptrdiff_t>(NLS::Engine::Components::MeshRenderer::kMaxMaterialCount));
}
