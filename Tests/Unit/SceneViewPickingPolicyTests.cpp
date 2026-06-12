#include <gtest/gtest.h>

#include "Panels/SceneViewPickingPolicy.h"

namespace
{
    NLS::Editor::Panels::HitProxyPickingSignature MakeSignature()
    {
        return {
            1920u,
            1080u,
            0x1000u,
            0x2000u,
            0x3000u,
            0x4000u
        };
    }
}

TEST(SceneViewPickingPolicyTests, ReusesReadablePickingFrameWhenSignatureMatches)
{
    const auto signature = MakeSignature();

    EXPECT_TRUE(NLS::Editor::Panels::ShouldReuseHitProxyPickingFrame(
        true,
        signature,
        signature));
}

TEST(SceneViewPickingPolicyTests, RejectsPickingFrameReuseWhenAnySignatureFieldDiffers)
{
    const auto signature = MakeSignature();

    auto differentExtent = signature;
    differentExtent.renderWidth += 1u;
    EXPECT_FALSE(NLS::Editor::Panels::ShouldReuseHitProxyPickingFrame(
        true,
        signature,
        differentExtent));

    auto differentCamera = signature;
    differentCamera.cameraViewHash += 1u;
    EXPECT_FALSE(NLS::Editor::Panels::ShouldReuseHitProxyPickingFrame(
        true,
        signature,
        differentCamera));

    auto differentRevision = signature;
    differentRevision.pickableSceneRevision += 1u;
    EXPECT_FALSE(NLS::Editor::Panels::ShouldReuseHitProxyPickingFrame(
        true,
        signature,
        differentRevision));

    auto differentDrawSources = signature;
    differentDrawSources.pickableDrawSourceHash += 1u;
    EXPECT_FALSE(NLS::Editor::Panels::ShouldReuseHitProxyPickingFrame(
        true,
        signature,
        differentDrawSources));

    auto differentView = signature;
    differentView.viewId += 1u;
    EXPECT_FALSE(NLS::Editor::Panels::ShouldReuseHitProxyPickingFrame(
        true,
        signature,
        differentView));
}

TEST(SceneViewPickingPolicyTests, DoesNotReuseWhenNoReadablePickingFrameExists)
{
    const auto signature = MakeSignature();

    EXPECT_FALSE(NLS::Editor::Panels::ShouldReuseHitProxyPickingFrame(
        false,
        signature,
        signature));
}

TEST(SceneViewPickingPolicyTests, HoverBudgetCanSkipHoverButNeverClick)
{
    constexpr uint64_t visiblePickableDrawCount = 2048u;
    constexpr uint64_t hoverBudget = 1024u;

    EXPECT_TRUE(NLS::Editor::Panels::ShouldSkipHitProxyPickingForVisibleDrawBudget(
        NLS::Editor::Panels::HitProxyPickingRequestKind::Hover,
        visiblePickableDrawCount,
        hoverBudget));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldSkipHitProxyPickingForVisibleDrawBudget(
        NLS::Editor::Panels::HitProxyPickingRequestKind::Click,
        visiblePickableDrawCount,
        hoverBudget));
}

TEST(SceneViewPickingPolicyTests, ZeroHoverBudgetDisablesBudgetSkip)
{
    EXPECT_FALSE(NLS::Editor::Panels::ShouldSkipHitProxyPickingForVisibleDrawBudget(
        NLS::Editor::Panels::HitProxyPickingRequestKind::Hover,
        2048u,
        0u));
}

TEST(SceneViewPickingPolicyTests, ClickResolveRequiresFreshReadableSerialAndMatchingSignature)
{
    const auto signature = MakeSignature();
    auto differentSignature = signature;
    differentSignature.cameraViewHash += 1u;

    EXPECT_FALSE(NLS::Editor::Panels::ShouldResolveHitProxyPickingRequest(
        NLS::Editor::Panels::HitProxyPickingRequestKind::Click,
        false,
        8u,
        8u,
        signature,
        signature));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldResolveHitProxyPickingRequest(
        NLS::Editor::Panels::HitProxyPickingRequestKind::Click,
        true,
        7u,
        8u,
        signature,
        signature));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldResolveHitProxyPickingRequest(
        NLS::Editor::Panels::HitProxyPickingRequestKind::Click,
        true,
        8u,
        8u,
        signature,
        differentSignature));

    EXPECT_TRUE(NLS::Editor::Panels::ShouldResolveHitProxyPickingRequest(
        NLS::Editor::Panels::HitProxyPickingRequestKind::Click,
        true,
        8u,
        8u,
        signature,
        signature));
}

TEST(SceneViewPickingPolicyTests, HoverResolveRequiresReadableMatchingSignatureButNoMinimumSerial)
{
    const auto signature = MakeSignature();

    EXPECT_TRUE(NLS::Editor::Panels::ShouldResolveHitProxyPickingRequest(
        NLS::Editor::Panels::HitProxyPickingRequestKind::Hover,
        true,
        1u,
        99u,
        signature,
        signature));
}

TEST(SceneViewPickingPolicyTests, PendingClickMinimumSerialRequiresNextSubmittedFrame)
{
    EXPECT_EQ(
        NLS::Editor::Panels::ComputePendingClickMinimumReadablePickingFrameSerial(0u, true),
        0u);
    EXPECT_EQ(
        NLS::Editor::Panels::ComputePendingClickMinimumReadablePickingFrameSerial(41u, true),
        41u);
    EXPECT_EQ(
        NLS::Editor::Panels::ComputePendingClickMinimumReadablePickingFrameSerial(41u, false),
        42u);
}

TEST(SceneViewPickingPolicyTests, ClickPickingForcesStaticFrameRenderEvenWhenHoverSampleExists)
{
    EXPECT_TRUE(NLS::Editor::Panels::ShouldForceSceneViewStaticFrameRenderForPicking(
        true,
        true,
        true));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldForceSceneViewStaticFrameRenderForPicking(
        true,
        true,
        false));
}

TEST(SceneViewPickingPolicyTests, SubmittedPendingClickDoesNotRequestAnotherClickPickingFrame)
{
    EXPECT_TRUE(NLS::Editor::Panels::ShouldRequestHitProxyPickingFrameForClick(
        true,
        false,
        false));

    EXPECT_TRUE(NLS::Editor::Panels::ShouldRequestHitProxyPickingFrameForClick(
        false,
        true,
        false));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldRequestHitProxyPickingFrameForClick(
        false,
        true,
        true));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldRequestHitProxyPickingFrameForClick(
        false,
        false,
        false));
}

TEST(SceneViewPickingPolicyTests, SubmittedPendingClickBlocksHoverPickingUntilClickReadbackResolves)
{
    EXPECT_FALSE(NLS::Editor::Panels::ShouldRequestHitProxyPickingFrameWhileClickReadbackPending(
        false,
        true,
        true));
    EXPECT_TRUE(NLS::Editor::Panels::ShouldForceSceneViewStaticFrameRenderForPendingClick(
        true));

    EXPECT_TRUE(NLS::Editor::Panels::ShouldRequestHitProxyPickingFrameWhileClickReadbackPending(
        true,
        true,
        true));

    EXPECT_TRUE(NLS::Editor::Panels::ShouldRequestHitProxyPickingFrameWhileClickReadbackPending(
        false,
        true,
        false));

    EXPECT_TRUE(NLS::Editor::Panels::ShouldRequestHitProxyPickingFrameWhileClickReadbackPending(
        false,
        false,
        false));
    EXPECT_FALSE(NLS::Editor::Panels::ShouldForceSceneViewStaticFrameRenderForPendingClick(
        false));
}
