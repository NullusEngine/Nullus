#include <gtest/gtest.h>

#include "Core/ResizeRefreshPolicy.h"

TEST(ResizeRefreshPolicyTests, TicksImmediatelyWhenIdle)
{
    EXPECT_TRUE(NLS::Editor::Core::ShouldTickResizeImmediately(
        false,
        false,
        false));
}

TEST(ResizeRefreshPolicyTests, TicksImmediatelyWhilePollingEventsInsideOuterFrame)
{
    EXPECT_TRUE(NLS::Editor::Core::ShouldTickResizeImmediately(
        true,
        true,
        false));
}

TEST(ResizeRefreshPolicyTests, DefersWhenAlreadyRunningNonPollingFrameWork)
{
    EXPECT_FALSE(NLS::Editor::Core::ShouldTickResizeImmediately(
        true,
        false,
        false));
}

TEST(ResizeRefreshPolicyTests, DefersWhenResizeTickIsAlreadyRunning)
{
    EXPECT_FALSE(NLS::Editor::Core::ShouldTickResizeImmediately(
        false,
        true,
        true));
}

TEST(ResizeRefreshPolicyTests, RunsFollowUpFrameAfterPlatformResize)
{
    EXPECT_TRUE(NLS::Editor::Core::ShouldRunResizeFollowUpFrame(
        true,
        false,
        false));
}

TEST(ResizeRefreshPolicyTests, RunsFollowUpFrameWhileDraggingResizeSplitter)
{
    EXPECT_TRUE(NLS::Editor::Core::ShouldRunResizeFollowUpFrame(
        false,
        true,
        true));
}

TEST(ResizeRefreshPolicyTests, DoesNotRunFollowUpFrameWhenOnlyHoveringResizeSplitter)
{
    EXPECT_FALSE(NLS::Editor::Core::ShouldRunResizeFollowUpFrame(
        false,
        true,
        false));
}

TEST(ResizeRefreshPolicyTests, DoesNotRunFollowUpFrameWithoutResizeSignal)
{
    EXPECT_FALSE(NLS::Editor::Core::ShouldRunResizeFollowUpFrame(
        false,
        false,
        true));
}
