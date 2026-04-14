#pragma once

namespace NLS::Editor::Core
{
inline bool ShouldTickResizeImmediately(bool isTicking, bool isPollingEvents, bool isResizeTicking)
{
    if (isResizeTicking)
        return false;

    return !isTicking || isPollingEvents;
}

inline bool ShouldRunResizeFollowUpFrame(
    bool platformResizeTriggered,
    bool resizeCursorActive,
    bool primaryMouseDown)
{
    return platformResizeTriggered || (resizeCursorActive && primaryMouseDown);
}
} // namespace NLS::Editor::Core
