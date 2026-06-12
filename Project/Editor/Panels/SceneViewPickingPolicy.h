#pragma once

#include <cstdint>

namespace NLS::Editor::Panels
{
    enum class HitProxyPickingRequestKind : uint8_t
    {
        Hover,
        Click
    };

    struct HitProxyPickingSignature
    {
        uint32_t renderWidth = 0u;
        uint32_t renderHeight = 0u;
        uint64_t cameraViewHash = 0u;
        uint64_t pickableSceneRevision = 0u;
        uint64_t pickableDrawSourceHash = 0u;
        uint64_t viewId = 0u;
    };

    inline bool HitProxyPickingSignaturesMatch(
        const HitProxyPickingSignature& lhs,
        const HitProxyPickingSignature& rhs)
    {
        return lhs.renderWidth == rhs.renderWidth &&
            lhs.renderHeight == rhs.renderHeight &&
            lhs.cameraViewHash == rhs.cameraViewHash &&
            lhs.pickableSceneRevision == rhs.pickableSceneRevision &&
            lhs.pickableDrawSourceHash == rhs.pickableDrawSourceHash &&
            lhs.viewId == rhs.viewId;
    }

    inline bool ShouldReuseHitProxyPickingFrame(
        bool hasReadablePickingFrame,
        const HitProxyPickingSignature& requestSignature,
        const HitProxyPickingSignature& readableFrameSignature)
    {
        return hasReadablePickingFrame &&
            HitProxyPickingSignaturesMatch(requestSignature, readableFrameSignature);
    }

    inline bool ShouldSkipHitProxyPickingForVisibleDrawBudget(
        HitProxyPickingRequestKind requestKind,
        uint64_t visiblePickableDrawCount,
        uint64_t hoverPickingVisibleDrawBudget)
    {
        return requestKind == HitProxyPickingRequestKind::Hover &&
            hoverPickingVisibleDrawBudget > 0u &&
            visiblePickableDrawCount > hoverPickingVisibleDrawBudget;
    }

    inline bool ShouldResolveHitProxyPickingRequest(
        HitProxyPickingRequestKind requestKind,
        bool hasReadablePickingFrame,
        uint64_t readablePickingFrameSerial,
        uint64_t minimumReadablePickingFrameSerial,
        const HitProxyPickingSignature& requestSignature,
        const HitProxyPickingSignature& readableFrameSignature)
    {
        if (!ShouldReuseHitProxyPickingFrame(
            hasReadablePickingFrame,
            requestSignature,
            readableFrameSignature))
        {
            return false;
        }

        return requestKind != HitProxyPickingRequestKind::Click ||
            readablePickingFrameSerial >= minimumReadablePickingFrameSerial;
    }

    inline uint64_t ComputePendingClickMinimumReadablePickingFrameSerial(
        const uint64_t submittedPickingFrameSerial,
        const bool clickPickingFrameSubmitted)
    {
        return clickPickingFrameSubmitted
            ? submittedPickingFrameSerial
            : submittedPickingFrameSerial + 1u;
    }

    inline bool ShouldSceneViewBlockCameraInput(
        bool shortcutsWindowOpen,
        bool isAnyItemActive,
        bool mouseOverView,
        bool wantTextInput)
    {
        return shortcutsWindowOpen ||
            wantTextInput ||
            (isAnyItemActive && !mouseOverView);
    }

    inline bool ShouldResolvePendingSceneClickPick(
        bool hasPendingClickPick,
        bool queuedClickPickThisFrame,
        bool rightMousePressed,
        uint64_t pendingClickRequestedPickingFrame,
        uint64_t readablePickingFrame)
    {
        (void)queuedClickPickThisFrame;
        return hasPendingClickPick &&
            !rightMousePressed &&
            pendingClickRequestedPickingFrame > 0u &&
            readablePickingFrame >= pendingClickRequestedPickingFrame;
    }

    inline bool ShouldRenderScenePickingFrame(
        bool mouseOverView,
        bool isResizing,
        bool pickingSuppressedByGizmo,
        bool hasPendingClickPick,
        bool leftClicked,
        bool cameraControlActive,
        bool cameraMovedThisFrame,
        bool sampleExpired,
        bool mouseMoved,
        bool hasPickingSample)
    {
        if (!mouseOverView || isResizing || pickingSuppressedByGizmo || cameraControlActive)
            return false;

        if (cameraMovedThisFrame && !hasPendingClickPick && !leftClicked)
            return false;

        return hasPendingClickPick ||
            leftClicked ||
            (sampleExpired && (mouseMoved || !hasPickingSample));
    }

    inline bool ShouldCancelScenePickingWhileCameraControlIsActive(
        bool cameraControlActive,
        bool hasPendingClickPick)
    {
        return cameraControlActive && hasPendingClickPick;
    }

    inline bool ShouldForceSceneViewStaticFrameRenderForPicking(
        bool shouldRequestPickingFrame,
        bool hasPickingSample,
        bool isClickPickingFrame)
    {
        return shouldRequestPickingFrame && (isClickPickingFrame || !hasPickingSample);
    }

    inline bool ShouldSkipSceneHoverPickingForVisibleDrawBudget(
        bool isClickPickingFrame,
        uint64_t visiblePickableDrawCount,
        uint64_t hoverPickingVisibleDrawBudget)
    {
        return ShouldSkipHitProxyPickingForVisibleDrawBudget(
            isClickPickingFrame
                ? HitProxyPickingRequestKind::Click
                : HitProxyPickingRequestKind::Hover,
            visiblePickableDrawCount,
            hoverPickingVisibleDrawBudget);
    }
}
