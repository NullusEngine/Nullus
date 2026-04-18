#include <gtest/gtest.h>

#include "Rendering/Debug/DebugDrawService.h"

TEST(DebugDrawTypesTests, SubmittedLinesRespectCategoryVisibilityAndLifetimeRules)
{
    using namespace NLS::Render::Debug;

    DebugDrawService service(16u);
    NLS::Render::Data::PipelineState pipelineState;

    DebugDrawSubmitOptions oneFrameOptions;
    oneFrameOptions.category = DebugDrawCategory::General;
    oneFrameOptions.lifetime = DebugDrawLifetime::OneFrame();

    DebugDrawSubmitOptions persistentOptions;
    persistentOptions.category = DebugDrawCategory::Grid;
    persistentOptions.lifetime = DebugDrawLifetime::Persistent();

    DebugDrawSubmitOptions timedOptions;
    timedOptions.category = DebugDrawCategory::Bounds;
    timedOptions.lifetime = DebugDrawLifetime::Frames(2u);

    ASSERT_TRUE(service.SubmitLine(
        pipelineState,
        { 0.0f, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f },
        1.0f,
        oneFrameOptions));
    ASSERT_TRUE(service.SubmitLine(
        pipelineState,
        { 0.0f, 1.0f, 0.0f },
        { 1.0f, 1.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        1.0f,
        persistentOptions));
    ASSERT_TRUE(service.SubmitLine(
        pipelineState,
        { 0.0f, 2.0f, 0.0f },
        { 1.0f, 2.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f },
        1.0f,
        timedOptions));

    EXPECT_EQ(service.CollectVisibleLines().size(), 3u);

    service.SetCategoryEnabled(DebugDrawCategory::Grid, false);
    EXPECT_EQ(service.CollectVisibleLines().size(), 2u);

    service.SetCategoryEnabled(DebugDrawCategory::Grid, true);
    service.EndFrame();
    EXPECT_EQ(service.CollectVisibleLines().size(), 2u);

    service.EndFrame();
    EXPECT_EQ(service.CollectVisibleLines().size(), 1u);
    EXPECT_EQ(service.CollectVisibleLines().front().get().options.category, DebugDrawCategory::Grid);
}

TEST(DebugDrawTypesTests, OverflowRejectsNewLinesWithoutDiscardingQueuedEntries)
{
    using namespace NLS::Render::Debug;

    DebugDrawService service(2u);
    NLS::Render::Data::PipelineState pipelineState;

    EXPECT_TRUE(service.SubmitLine(
        pipelineState,
        { 0.0f, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f },
        1.0f));
    EXPECT_TRUE(service.SubmitLine(
        pipelineState,
        { 0.0f, 1.0f, 0.0f },
        { 1.0f, 1.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        1.0f));

    EXPECT_FALSE(service.SubmitLine(
        pipelineState,
        { 0.0f, 2.0f, 0.0f },
        { 1.0f, 2.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f },
        1.0f));
    EXPECT_EQ(service.GetLimitState(), DebugDrawLimitState::OverflowRejected);
    EXPECT_EQ(service.CollectVisibleLines().size(), 2u);
}

TEST(DebugDrawTypesTests, PrimitivesCanBeSubmittedWithoutPipelineStateAndFilteredByUnifiedSettings)
{
    using namespace NLS::Render::Debug;

    DebugDrawService service(8u);

    DebugDrawSubmitOptions lightingOptions;
    lightingOptions.category = DebugDrawCategory::Lighting;
    lightingOptions.style.color = { 1.0f, 1.0f, 0.0f };
    lightingOptions.style.lineWidth = 2.0f;

    ASSERT_TRUE(service.SubmitPoint({ 0.0f, 0.0f, 0.0f }, lightingOptions));
    ASSERT_TRUE(service.SubmitLine({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, lightingOptions));

    DebugDrawSubmitOptions generalOptions;
    generalOptions.category = DebugDrawCategory::General;
    generalOptions.style.color = { 0.0f, 1.0f, 0.0f };

    ASSERT_TRUE(service.SubmitTriangle(
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f },
        generalOptions));

    EXPECT_EQ(service.GetQueuedPrimitiveCount(), 3u);
    EXPECT_EQ(service.CollectVisiblePrimitives().size(), 3u);

    service.SetEnabled(false);
    EXPECT_TRUE(service.CollectVisiblePrimitives().empty());
    EXPECT_EQ(service.GetQueuedPrimitiveCount(), 3u);

    service.SetEnabled(true);
    service.SetCategoryEnabled(DebugDrawCategory::Lighting, false);

    auto visiblePrimitives = service.CollectVisiblePrimitives();
    ASSERT_EQ(visiblePrimitives.size(), 1u);
    EXPECT_EQ(visiblePrimitives.front().get().type, DebugDrawPrimitiveType::Triangle);
    EXPECT_EQ(visiblePrimitives.front().get().options.category, DebugDrawCategory::General);
}
