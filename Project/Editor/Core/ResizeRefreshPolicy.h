#pragma once

#include <chrono>

namespace NLS::Editor::Core
{
inline bool ShouldRefreshNativeResizeFrame(
    bool sizeChanged,
    std::chrono::steady_clock::duration elapsedSinceLastFrame)
{
    return sizeChanged || elapsedSinceLastFrame >= std::chrono::milliseconds(16);
}

inline bool ShouldTickResizeImmediately(
    bool nativeResizeInProgress,
    bool isTicking,
    bool isPollingEvents,
    bool isResizeTicking)
{
    if (isResizeTicking)
        return false;

    return nativeResizeInProgress && (!isTicking || isPollingEvents);
}

inline bool ShouldRunResizeFollowUpFrame(
    bool platformResizeTriggered,
    bool resizeCursorActive,
    bool primaryMouseDown,
    bool nativeResizeInProgress = false)
{
    if (nativeResizeInProgress)
        return false;
    return platformResizeTriggered || (resizeCursorActive && primaryMouseDown);
}
} // namespace NLS::Editor::Core
