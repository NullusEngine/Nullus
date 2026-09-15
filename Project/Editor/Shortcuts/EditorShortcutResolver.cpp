#include "Shortcuts/EditorShortcutResolver.h"

#include <algorithm>

namespace NLS::Editor::Shortcuts
{
    namespace
    {
        int GetContextPriority(
            const ShortcutContextId& p_contextId,
            const std::vector<ShortcutContext>& p_contexts)
        {
            if (p_contextId == ShortcutContexts::Global)
                return 0;

            for (const auto& context : p_contexts)
            {
                if (context.id == p_contextId && context.IsActive())
                    return context.priority;
            }

            return -1;
        }

        bool IsTextInputActive(const std::vector<ShortcutContext>& p_contexts)
        {
            return std::any_of(
                p_contexts.begin(),
                p_contexts.end(),
                [](const ShortcutContext& p_context)
                {
                    return p_context.id == ShortcutContexts::TextInput && p_context.IsActive();
                });
        }
    }

    const ShortcutCommand* EditorShortcutResolver::Resolve(
        const std::vector<ShortcutCommand>& p_commands,
        const std::function<ShortcutBinding(const std::string&)>& p_bindingProvider,
        const ShortcutBinding& p_pressedBinding,
        const std::vector<ShortcutContext>& p_contexts)
    {
        return ResolveAny(
            p_commands,
            [&p_bindingProvider](const std::string& p_commandId)
            {
                return std::vector<ShortcutBinding> { p_bindingProvider(p_commandId) };
            },
            p_pressedBinding,
            p_contexts);
    }

    const ShortcutCommand* EditorShortcutResolver::ResolveAny(
        const std::vector<ShortcutCommand>& p_commands,
        const std::function<std::vector<ShortcutBinding>(const std::string&)>& p_bindingProvider,
        const ShortcutBinding& p_pressedBinding,
        const std::vector<ShortcutContext>& p_contexts)
    {
        if (!p_pressedBinding.IsValid())
            return nullptr;

        const bool textInputActive = IsTextInputActive(p_contexts);
        const ShortcutCommand* bestCommand = nullptr;
        int bestPriority = -1;

        for (const auto& command : p_commands)
        {
            if (textInputActive && !command.allowDuringTextInput)
                continue;
            if (!command.IsAvailable())
                continue;

            const auto bindings = p_bindingProvider(command.id);
            const auto matchingBinding = std::find_if(
                bindings.begin(),
                bindings.end(),
                [&p_pressedBinding](const ShortcutBinding& binding)
                {
                    return binding.IsValid() &&
                        AreShortcutBindingsEquivalentForCurrentPlatform(binding, p_pressedBinding);
                });
            if (matchingBinding == bindings.end())
                continue;

            const int priority = GetContextPriority(command.context, p_contexts);
            if (priority < 0)
                continue;

            if (priority > bestPriority)
            {
                bestCommand = &command;
                bestPriority = priority;
            }
        }

        return bestCommand;
    }
}
