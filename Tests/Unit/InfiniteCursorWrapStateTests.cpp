#include <gtest/gtest.h>

#include "Windowing/InfiniteCursorWrapState.h"

namespace
{
using NLS::Windowing::InfiniteCursorWrapState;
}

TEST(InfiniteCursorWrapStateTests, DisabledStateDoesNotQueueWarp)
{
    InfiniteCursorWrapState state;
    state.SeedCursorPosition(0.0, 12.0);
    state.BeginFrame();

    const auto warp = state.Evaluate({100.0f, 50.0f}, true);

    EXPECT_FALSE(warp.has_value());
    EXPECT_FLOAT_EQ(state.GetFrameCompensation().x, 0.0f);
    EXPECT_FLOAT_EQ(state.GetFrameCompensation().y, 0.0f);
}

TEST(InfiniteCursorWrapStateTests, LeftEdgeQueuesOppositeSideWarpAndCompensation)
{
    InfiniteCursorWrapState state;
    state.SetEnabled(true);
    state.SeedCursorPosition(0.0, 12.0);
    state.BeginFrame();

    const auto warp = state.Evaluate({100.0f, 50.0f}, true);

    ASSERT_TRUE(warp.has_value());
    EXPECT_FLOAT_EQ(warp->targetPosition.x, 95.0f);
    EXPECT_FLOAT_EQ(warp->targetPosition.y, 12.0f);
    EXPECT_FLOAT_EQ(warp->compensation.x, 95.0f);
    EXPECT_FLOAT_EQ(warp->compensation.y, 0.0f);
    EXPECT_FLOAT_EQ(state.GetFrameCompensation().x, 95.0f);
    EXPECT_FLOAT_EQ(state.GetFrameCompensation().y, 0.0f);
}

TEST(InfiniteCursorWrapStateTests, WarpUpdatesTrackedPositionSoSameFrameReevaluationDoesNotLoop)
{
    InfiniteCursorWrapState state;
    state.SetEnabled(true);
    state.SeedCursorPosition(0.0, 12.0);
    state.BeginFrame();

    const auto firstWarp = state.Evaluate({100.0f, 50.0f}, true);
    const auto secondWarp = state.Evaluate({100.0f, 50.0f}, true);

    ASSERT_TRUE(firstWarp.has_value());
    EXPECT_FALSE(secondWarp.has_value());
    EXPECT_FLOAT_EQ(state.GetFrameCompensation().x, 95.0f);
    EXPECT_FLOAT_EQ(state.GetFrameCompensation().y, 0.0f);
}

TEST(InfiniteCursorWrapStateTests, BeginFrameClearsPreviousFrameCompensation)
{
    InfiniteCursorWrapState state;
    state.SetEnabled(true);
    state.SeedCursorPosition(0.0, 12.0);
    state.BeginFrame();
    ASSERT_TRUE(state.Evaluate({100.0f, 50.0f}, true).has_value());

    state.BeginFrame();

    EXPECT_FLOAT_EQ(state.GetFrameCompensation().x, 0.0f);
    EXPECT_FLOAT_EQ(state.GetFrameCompensation().y, 0.0f);
}

TEST(InfiniteCursorWrapStateTests, RightEdgeQueuesOppositeSideWarpAndNegativeCompensation)
{
    InfiniteCursorWrapState state;
    state.SetEnabled(true);
    state.SeedCursorPosition(99.0, 12.0);
    state.BeginFrame();

    const auto warp = state.Evaluate({100.0f, 50.0f}, true);

    ASSERT_TRUE(warp.has_value());
    EXPECT_FLOAT_EQ(warp->targetPosition.x, 5.0f);
    EXPECT_FLOAT_EQ(warp->targetPosition.y, 12.0f);
    EXPECT_FLOAT_EQ(warp->compensation.x, -94.0f);
    EXPECT_FLOAT_EQ(warp->compensation.y, 0.0f);
}

TEST(InfiniteCursorWrapStateTests, NearRightEdgeQueuesWarpBeforeOsDisplayClampStopsDelta)
{
    InfiniteCursorWrapState state;
    state.SetEnabled(true);
    state.SeedCursorPosition(96.0, 12.0);
    state.BeginFrame();

    const auto warp = state.Evaluate({100.0f, 50.0f}, true);

    ASSERT_TRUE(warp.has_value());
    EXPECT_FLOAT_EQ(warp->targetPosition.x, 5.0f);
    EXPECT_FLOAT_EQ(warp->targetPosition.y, 12.0f);
    EXPECT_FLOAT_EQ(warp->compensation.x, -91.0f);
    EXPECT_FLOAT_EQ(warp->compensation.y, 0.0f);
}
