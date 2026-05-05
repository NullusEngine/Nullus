#pragma once

#include <functional>
#include <string>

namespace NLS::Editor::Shortcuts
{
    using ShortcutContextId = std::string;

    struct ShortcutContext
    {
        ShortcutContextId id;
        std::string displayName;
        int priority = 0;
        std::string exclusiveGroup;
        std::function<bool()> isActive;

        bool IsActive() const;
    };

    namespace ShortcutContexts
    {
        inline const ShortcutContextId Global = "global";
        inline const ShortcutContextId SceneView = "scene-view";
        inline const ShortcutContextId SceneViewFlyMode = "scene-view-fly-mode";
        inline const ShortcutContextId Hierarchy = "hierarchy";
        inline const ShortcutContextId AssetBrowser = "asset-browser";
        inline const ShortcutContextId Inspector = "inspector";
        inline const ShortcutContextId GameView = "game-view";
        inline const ShortcutContextId Menu = "menu";
        inline const ShortcutContextId TextInput = "text-input";
    }

    inline bool ShortcutContext::IsActive() const
    {
        return !isActive || isActive();
    }
}
