#include <gtest/gtest.h>

#include "Rendering/PickingReadbackLifecycle.h"

namespace
{
    struct TestScene {};

    TEST(PickingReadbackLifecycleTests, PendingFrameIsNotReadableUntilReadbackIsAvailable)
    {
        TestScene scene;
        NLS::Editor::Rendering::PickingReadbackLifecycle<TestScene> lifecycle;

        lifecycle.QueueSubmittedFrame({ &scene, 640u, 480u, 3u, nullptr });
        lifecycle.PromotePendingFrameIfReadbackAvailable(false);

        EXPECT_EQ(lifecycle.GetReadableFrame(), nullptr);
    }

    TEST(PickingReadbackLifecycleTests, PromotesPendingFrameWhenReadbackBecomesAvailable)
    {
        TestScene scene;
        NLS::Editor::Rendering::PickingReadbackLifecycle<TestScene> lifecycle;

        lifecycle.QueueSubmittedFrame({ &scene, 640u, 480u, 5u, nullptr });
        lifecycle.PromotePendingFrameIfReadbackAvailable(true);

        const auto* readableFrame = lifecycle.GetReadableFrame();
        ASSERT_NE(readableFrame, nullptr);
        EXPECT_EQ(readableFrame->scene, &scene);
        EXPECT_EQ(readableFrame->width, 640u);
        EXPECT_EQ(readableFrame->height, 480u);
        EXPECT_EQ(readableFrame->serial, 5u);
    }

    TEST(PickingReadbackLifecycleTests, ImmediateReadableFrameSupportsSynchronousRendering)
    {
        TestScene scene;
        NLS::Editor::Rendering::PickingReadbackLifecycle<TestScene> lifecycle;

        lifecycle.MarkSubmittedFrameImmediatelyReadable({ &scene, 320u, 240u, 7u, nullptr });

        const auto* readableFrame = lifecycle.GetReadableFrame();
        ASSERT_NE(readableFrame, nullptr);
        EXPECT_EQ(readableFrame->scene, &scene);
        EXPECT_EQ(readableFrame->width, 320u);
        EXPECT_EQ(readableFrame->height, 240u);
        EXPECT_EQ(readableFrame->serial, 7u);
    }

    TEST(PickingReadbackLifecycleTests, ResetSubmittedFrameClearsStaleReadableFrame)
    {
        TestScene scene;
        NLS::Editor::Rendering::PickingReadbackLifecycle<TestScene> lifecycle;

        lifecycle.MarkSubmittedFrameImmediatelyReadable({ &scene, 320u, 240u, 9u, nullptr });
        lifecycle.ResetSubmittedFrame();

        EXPECT_EQ(lifecycle.GetReadableFrame(), nullptr);
    }
}
