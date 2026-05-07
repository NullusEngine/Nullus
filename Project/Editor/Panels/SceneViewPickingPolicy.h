#pragma once

namespace NLS::Editor::Panels
{
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
        bool rightMousePressed,
        bool sampleExpired,
        bool mouseMoved,
        bool hasPickingSample)
    {
        if (!mouseOverView || isResizing || pickingSuppressedByGizmo)
            return false;

        return hasPendingClickPick ||
            leftClicked ||
            (!rightMousePressed && sampleExpired && (mouseMoved || !hasPickingSample));
    }
}
