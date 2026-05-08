#include <gtest/gtest.h>

#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/GridRenderPass.h"
#include "Rendering/EditorHelperLifecycle.h"
#include "Rendering/OutlineRenderer.h"

namespace
{
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
    helperState.hasSelectedActor = true;
    helperState.hasVisibleDebugDrawPrimitives = true;

    EXPECT_EQ(NLS::Editor::Rendering::CountThreadedEditorHelperPasses(helperState), 6u);
}

TEST(DebugSceneLifecycleTests, SkipsDisabledOrEmptyEditorHelpersForThreadedSnapshotPlanning)
{
    NLS::Editor::Rendering::ThreadedEditorHelperState helperState;
    helperState.gridEnabled = false;
    helperState.sceneCameraCount = 0u;
    helperState.sceneLightCount = 0u;
    helperState.hasSelectedActor = false;
    helperState.hasVisibleDebugDrawPrimitives = false;

    EXPECT_EQ(NLS::Editor::Rendering::CountThreadedEditorHelperPasses(helperState), 0u);
}

TEST(DebugSceneLifecycleTests, SkipsEditorHelpersWhosePassesAreDisabledForThreadedPlanning)
{
    NLS::Editor::Rendering::ThreadedEditorHelperState helperState;
    helperState.gridPassEnabled = false;
    helperState.cameraPassEnabled = false;
    helperState.lightPassEnabled = false;
    helperState.actorPassEnabled = false;
    helperState.debugDrawPassEnabled = false;
    helperState.gridEnabled = true;
    helperState.sceneCameraCount = 2u;
    helperState.sceneLightCount = 3u;
    helperState.hasSelectedActor = true;
    helperState.hasVisibleDebugDrawPrimitives = true;

    EXPECT_EQ(NLS::Editor::Rendering::CountThreadedEditorHelperPasses(helperState), 0u);
}

TEST(DebugSceneLifecycleTests, SceneVisibilityRefreshPreservesEditorHelperDrawCount)
{
    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.visibleHelperDrawCount = 3u;

    NLS::Engine::Rendering::BaseSceneRenderer::AllDrawables drawables;
    NLS::Render::Entities::Drawable opaqueDrawable;
    drawables.opaques.emplace(0.0f, opaqueDrawable);

    SceneSnapshotVisibilityHarness::RefreshFrameSnapshotVisibility(snapshot, drawables);

    EXPECT_EQ(snapshot.visibleOpaqueDrawCount, 1u);
    EXPECT_EQ(snapshot.visibleHelperDrawCount, 3u);
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

TEST(DebugSceneLifecycleTests, SelectionHelpersRequireActorPassAndSelectedActor)
{
    using NLS::Editor::Rendering::OutlineRenderer;

    NLS::Engine::GameObject actor("Selected", "Editor");

    EXPECT_FALSE(OutlineRenderer::ShouldIncludeInThreadedFrame(false, &actor));
    EXPECT_FALSE(OutlineRenderer::ShouldIncludeInThreadedFrame(true, nullptr));
    EXPECT_TRUE(OutlineRenderer::ShouldIncludeInThreadedFrame(true, &actor));
}

TEST(DebugSceneLifecycleTests, SceneDestructionBroadcastsActorDestroyedBeforeDeletingActors)
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
