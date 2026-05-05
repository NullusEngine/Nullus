#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "EditorShortcutCommand.h"

namespace NLS::Editor::Shortcuts
{
    class EditorShortcutRegistry
    {
    public:
        EditorShortcutRegistry() = default;

        bool RegisterCommand(ShortcutCommand p_command);
        const ShortcutCommand* FindCommand(const std::string& p_id) const;
        ShortcutCommand* FindCommand(const std::string& p_id);
        const std::vector<ShortcutCommand>& GetCommands() const;

        bool SetBinding(const std::string& p_id, const ShortcutBinding& p_binding);
        ShortcutBinding GetBinding(const std::string& p_id) const;
        bool ClearBinding(const std::string& p_id);
        bool ResetBinding(const std::string& p_id);
        std::vector<ShortcutConflict> ValidateBinding(
            const std::string& p_id,
            const ShortcutBinding& p_binding,
            const std::function<bool(const ShortcutContextId&, const ShortcutContextId&)>& p_areContextsMutuallyExclusive) const;

    private:
        std::vector<ShortcutCommand> m_commands;
        std::unordered_map<std::string, size_t> m_commandIndices;
        std::unordered_map<std::string, ShortcutBinding> m_bindingOverrides;
    };
}
