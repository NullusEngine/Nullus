#pragma once

namespace NLS::Editor::Core
{
    inline bool& ShortcutSettingsWindowBlocksSceneInput()
    {
        static bool blocked = false;
        return blocked;
    }

    inline void SetShortcutSettingsWindowBlocksSceneInput(const bool p_blocked)
    {
        ShortcutSettingsWindowBlocksSceneInput() = p_blocked;
    }

    inline bool DoesShortcutSettingsWindowBlockSceneInput()
    {
        return ShortcutSettingsWindowBlocksSceneInput();
    }
}
