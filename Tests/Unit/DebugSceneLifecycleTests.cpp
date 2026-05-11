#include <gtest/gtest.h>

#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/GridRenderPass.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/EditorHelperLifecycle.h"
#include "Rendering/OutlineRenderer.h"

namespace
{
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
