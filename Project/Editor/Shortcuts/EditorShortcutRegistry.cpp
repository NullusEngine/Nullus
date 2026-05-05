#include "Shortcuts/EditorShortcutRegistry.h"

#include <sstream>

namespace NLS::Editor::Shortcuts
{
    bool EditorShortcutRegistry::RegisterCommand(ShortcutCommand p_command)
    {
        if (p_command.id.empty() || m_commandIndices.contains(p_command.id))
            return false;

        const auto index = m_commands.size();
        m_commandIndices.emplace(p_command.id, index);
        m_commands.emplace_back(std::move(p_command));
        return true;
    }

    const ShortcutCommand* EditorShortcutRegistry::FindCommand(const std::string& p_id) const
    {
        const auto it = m_commandIndices.find(p_id);
        if (it == m_commandIndices.end())
            return nullptr;
        return &m_commands[it->second];
    }

    ShortcutCommand* EditorShortcutRegistry::FindCommand(const std::string& p_id)
    {
        const auto it = m_commandIndices.find(p_id);
        if (it == m_commandIndices.end())
            return nullptr;
        return &m_commands[it->second];
    }

    const std::vector<ShortcutCommand>& EditorShortcutRegistry::GetCommands() const
    {
        return m_commands;
    }

    bool EditorShortcutRegistry::SetBinding(const std::string& p_id, const ShortcutBinding& p_binding)
    {
        if (FindCommand(p_id) == nullptr)
            return false;

        m_bindingOverrides[p_id] = p_binding;
        return true;
    }

    bool EditorShortcutRegistry::ClearBinding(const std::string& p_id)
    {
        if (FindCommand(p_id) == nullptr)
            return false;

        m_bindingOverrides[p_id] = ShortcutBinding::Unassigned();
        return true;
    }

    bool EditorShortcutRegistry::ResetBinding(const std::string& p_id)
    {
        if (FindCommand(p_id) == nullptr)
            return false;

        m_bindingOverrides.erase(p_id);
        return true;
    }

    ShortcutBinding EditorShortcutRegistry::GetBinding(const std::string& p_id) const
    {
        const auto overrideIt = m_bindingOverrides.find(p_id);
        if (overrideIt != m_bindingOverrides.end())
            return overrideIt->second;

        if (const auto* command = FindCommand(p_id))
            return command->defaultBinding;

        return ShortcutBinding::Unassigned();
    }

    std::vector<ShortcutConflict> EditorShortcutRegistry::ValidateBinding(
        const std::string& p_id,
        const ShortcutBinding& p_binding,
        const std::function<bool(const ShortcutContextId&, const ShortcutContextId&)>& p_areContextsMutuallyExclusive) const
    {
        std::vector<ShortcutConflict> conflicts;

        const auto* targetCommand = FindCommand(p_id);
        if (targetCommand == nullptr)
            return conflicts;

        const auto addConflict = [&conflicts, &p_binding](
            const EShortcutConflictType p_type,
            const std::string& p_firstCommandId,
            const std::string& p_secondCommandId,
            const std::string& p_message)
        {
            ShortcutConflict conflict;
            conflict.type = p_type;
            conflict.binding = p_binding;
            conflict.commandIds = { p_firstCommandId, p_secondCommandId };
            conflict.message = p_message;
            conflict.blocking = true;
            conflicts.emplace_back(std::move(conflict));
        };

        if (!p_binding.IsValid())
        {
            ShortcutConflict conflict;
            conflict.type = EShortcutConflictType::InvalidBinding;
            conflict.binding = p_binding;
            conflict.commandIds = { p_id };
            conflict.message = "Shortcut binding is invalid.";
            conflicts.emplace_back(std::move(conflict));
            return conflicts;
        }

        for (const auto& otherCommand : m_commands)
        {
            if (otherCommand.id == p_id)
                continue;

            const auto otherBinding = GetBinding(otherCommand.id);
            if (!otherBinding.IsValid() || otherBinding != p_binding)
                continue;

            const bool targetGlobal = targetCommand->context == ShortcutContexts::Global;
            const bool otherGlobal = otherCommand.context == ShortcutContexts::Global;

            if (targetGlobal && otherGlobal)
            {
                addConflict(
                    EShortcutConflictType::DuplicateGlobal,
                    p_id,
                    otherCommand.id,
                    "Shortcut is already assigned to another global command.");
                continue;
            }

            if (targetCommand->context == otherCommand.context)
            {
                addConflict(
                    EShortcutConflictType::DuplicateContext,
                    p_id,
                    otherCommand.id,
                    "Shortcut is already assigned in this context.");
                continue;
            }

            if (!targetGlobal && !otherGlobal &&
                p_areContextsMutuallyExclusive(targetCommand->context, otherCommand.context))
            {
                continue;
            }

            if (targetGlobal || otherGlobal)
            {
                const auto& contextCommand = targetGlobal ? otherCommand : *targetCommand;
                if (contextCommand.allowContextOverride)
                    continue;

                addConflict(
                    EShortcutConflictType::GlobalContextCollision,
                    p_id,
                    otherCommand.id,
                    "Shortcut collides with a global command.");
            }
        }

        return conflicts;
    }
}
