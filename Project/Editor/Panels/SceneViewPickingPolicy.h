#pragma once

#include <cstdint>

namespace NLS::Editor::Panels
{
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
        bool hasPickingSample)
    {
        return shouldRequestPickingFrame && !hasPickingSample;
    }

    inline bool ShouldSkipSceneHoverPickingForVisibleDrawBudget(
        bool isClickPickingFrame,
        uint64_t visiblePickableDrawCount,
        uint64_t hoverPickingVisibleDrawBudget)
    {
        return !isClickPickingFrame &&
            hoverPickingVisibleDrawBudget > 0u &&
            visiblePickableDrawCount > hoverPickingVisibleDrawBudget;
    }
}
