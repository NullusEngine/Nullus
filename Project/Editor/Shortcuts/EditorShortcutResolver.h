#pragma once

#include <vector>

#include "EditorShortcutCommand.h"
#include "EditorShortcutContext.h"

namespace NLS::Editor::Shortcuts
{
    struct ShortcutResolutionContext
    {
        std::vector<ShortcutContext> contexts;
        bool textInputActive = false;
    };

    class EditorShortcutResolver
    {
    public:
        EditorShortcutResolver() = default;

        static const ShortcutCommand* Resolve(
            const std::vector<ShortcutCommand>& p_commands,
            const std::function<ShortcutBinding(const std::string&)>& p_bindingProvider,
            const ShortcutBinding& p_pressedBinding,
            const std::vector<ShortcutContext>& p_contexts);
    };
}
