#pragma once

namespace NLS::Editor::Core
{
    inline int& EditorModalSceneInputBlockCount()
    {
        static int count = 0;
        return count;
    }

    inline void SetEditorModalSceneInputBlocked(const char*, const bool p_blocked)
    {
        auto& count = EditorModalSceneInputBlockCount();
        if (p_blocked)
            ++count;
        else if (count > 0)
            --count;
    }

    inline void SetShortcutSettingsWindowBlocksSceneInput(const bool p_blocked)
    {
        static bool blocked = false;
        if (blocked == p_blocked)
            return;
        blocked = p_blocked;
        SetEditorModalSceneInputBlocked("Shortcuts", p_blocked);
    }

    inline void SetSettingsWindowBlocksSceneInput(const bool p_blocked)
    {
        static bool blocked = false;
        if (blocked == p_blocked)
            return;
        blocked = p_blocked;
        SetEditorModalSceneInputBlocked("Settings", p_blocked);
    }

    inline bool DoesEditorModalBlockSceneInput()
    {
        return EditorModalSceneInputBlockCount() > 0;
    }

    inline bool DoesShortcutSettingsWindowBlockSceneInput()
    {
        return DoesEditorModalBlockSceneInput();
    }
}
