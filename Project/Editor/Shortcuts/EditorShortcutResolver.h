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

        // Backward-compatible single-binding resolver used by callers that do
        // not need aliases.
        static const ShortcutCommand* Resolve(
            const std::vector<ShortcutCommand>& p_commands,
            const std::function<ShortcutBinding(const std::string&)>& p_bindingProvider,
            const ShortcutBinding& p_pressedBinding,
            const std::vector<ShortcutContext>& p_contexts);

        // Resolves a command against any of its effective gestures while still
        // applying the same context priority and text-input rules.
        static const ShortcutCommand* ResolveAny(
            const std::vector<ShortcutCommand>& p_commands,
            const std::function<std::vector<ShortcutBinding>(const std::string&)>& p_bindingProvider,
            const ShortcutBinding& p_pressedBinding,
            const std::vector<ShortcutContext>& p_contexts);
    };
}
