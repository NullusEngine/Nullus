#include <gtest/gtest.h>

#include "Core/ApplicationIdleFramePolicy.h"
#include "Core/ResizeRefreshPolicy.h"

TEST(ResizeRefreshPolicyTests, TicksImmediatelyWhenIdle)
{
    EXPECT_TRUE(NLS::Editor::Core::ShouldTickResizeImmediately(
        true,
        false,
        false,
        false));
}

TEST(ResizeRefreshPolicyTests, TicksImmediatelyWhilePollingEventsInsideOuterFrame)
{
    EXPECT_TRUE(NLS::Editor::Core::ShouldTickResizeImmediately(
        true,
        true,
        true,
        false));
}

TEST(ResizeRefreshPolicyTests, DefersNonNativeResizeCallbacksToMainLoop)
{
    EXPECT_FALSE(NLS::Editor::Core::ShouldTickResizeImmediately(
        false,
        false,
        false,
        false));
}

TEST(ResizeRefreshPolicyTests, DefersWhenAlreadyRunningNonPollingFrameWork)
{
    EXPECT_FALSE(NLS::Editor::Core::ShouldTickResizeImmediately(
        true,
        true,
        false,
        false));
}

TEST(ResizeRefreshPolicyTests, DefersWhenResizeTickIsAlreadyRunning)
{
    EXPECT_FALSE(NLS::Editor::Core::ShouldTickResizeImmediately(
        true,
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

TEST(ApplicationIdleFramePolicyTests, PacesOnlyTrulyIdleEditorFrames)
{
    EXPECT_FALSE(NLS::Editor::Core::ShouldPaceIdleEditorFrame(
        false,
        false,
        false,
        false,
        false,
        false));

    EXPECT_TRUE(NLS::Editor::Core::ShouldPaceIdleEditorFrame(
        true,
        false,
        false,
        false,
        false,
        false));

    EXPECT_FALSE(NLS::Editor::Core::ShouldPaceIdleEditorFrame(
        true,
        true,
        false,
        false,
        false,
        false));
    EXPECT_FALSE(NLS::Editor::Core::ShouldPaceIdleEditorFrame(
        true,
        false,
        true,
        false,
        false,
        false));
    EXPECT_FALSE(NLS::Editor::Core::ShouldPaceIdleEditorFrame(
        true,
        false,
        false,
        true,
        false,
        false));
    EXPECT_FALSE(NLS::Editor::Core::ShouldPaceIdleEditorFrame(
        true,
        false,
        false,
        false,
        true,
        false));
    EXPECT_FALSE(NLS::Editor::Core::ShouldPaceIdleEditorFrame(
        true,
        false,
        false,
        false,
        false,
        true));

    EXPECT_GT(NLS::Editor::Core::GetIdleEditorFramePacingMilliseconds(), 0u);
}
