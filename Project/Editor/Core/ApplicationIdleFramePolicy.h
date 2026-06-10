#pragma once

#include <cstdint>

namespace NLS::Editor::Core
{
inline bool ShouldPaceIdleEditorFrame(
    const bool powerSavingEnabled,
    const bool hasTransientInput,
    const bool hasMouseButtonDown,
    const bool hasPendingResizeTick,
    const bool isResizeTicking,
    const bool profilerRecording)
{
    return powerSavingEnabled &&
        !hasTransientInput &&
        !hasMouseButtonDown &&
        !hasPendingResizeTick &&
        !isResizeTicking &&
        !profilerRecording;
}

inline uint32_t GetIdleEditorFramePacingMilliseconds()
{
    return 2u;
}
} // namespace NLS::Editor::Core
