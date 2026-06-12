#include <gtest/gtest.h>

#include "Rendering/PickingReadbackLifecycle.h"

namespace
{
    struct TestScene {};

    NLS::Editor::Panels::HitProxyPickingSignature MakeSignature()
    {
        NLS::Editor::Panels::HitProxyPickingSignature signature;
        signature.renderWidth = 640u;
        signature.renderHeight = 480u;
        signature.cameraViewHash = 11u;
        signature.pickableSceneRevision = 22u;
        signature.pickableDrawSourceHash = 33u;
        signature.viewId = 44u;
        return signature;
    }

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

    TEST(PickingReadbackLifecycleTests, PixelReadbackInFlightSuppressesRepeatedAttemptsUntilCompleted)
    {
        NLS::Editor::Rendering::PickingReadbackLifecycle<TestScene> lifecycle;

        EXPECT_TRUE(lifecycle.TryBeginPixelReadback());
        EXPECT_FALSE(lifecycle.TryBeginPixelReadback());

        lifecycle.EndPixelReadback();

        EXPECT_TRUE(lifecycle.TryBeginPixelReadback());
    }

    TEST(PickingReadbackLifecycleTests, PromotedReadableFramePreservesPickingSignature)
    {
        TestScene scene;
        NLS::Editor::Rendering::PickingReadbackLifecycle<TestScene> lifecycle;
        const auto signature = MakeSignature();

        lifecycle.QueueSubmittedFrame({ &scene, 640u, 480u, 13u, nullptr, 0u, {}, signature });
        lifecycle.PromotePendingFrameIfReadbackAvailable(true);

        const auto* readableFrame = lifecycle.GetReadableFrame();
        ASSERT_NE(readableFrame, nullptr);
        EXPECT_TRUE(NLS::Editor::Panels::HitProxyPickingSignaturesMatch(
            signature,
            readableFrame->signature));
    }
}
