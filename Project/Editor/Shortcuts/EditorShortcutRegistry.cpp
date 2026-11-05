#include "Shortcuts/EditorShortcutRegistry.h"

#include <algorithm>

namespace NLS::Editor::Shortcuts
{
    bool EditorShortcutRegistry::RegisterCommand(ShortcutCommand p_command)
    {
        if (p_command.id.empty() || m_commandIndices.contains(p_command.id))
            return false;

        const auto index = m_commands.size();
        m_commandIndices.emplace(p_command.id, index);
        m_commands.emplace_back(std::move(p_command));
        ++m_generation;
        m_commandGenerations[m_commands.back().id] = m_generation;
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

    bool EditorShortcutRegistry::CanExecute(const std::string& id, const InvocationContext& context) const
    {
        const auto* command = FindCommand(id);
        if (!command || (!command->invoke && !command->execute)) return false;
        const auto generation = GetCommandGeneration(id);
        const auto availability = command->availability;
        const auto canExecute = command->canExecute;
        if (availability && !availability()) return false;
        if (GetCommandGeneration(id) != generation) return false;
        if (canExecute && !canExecute(context)) return false;
        return GetCommandGeneration(id) == generation;
    }

    CommandResult EditorShortcutRegistry::TryExecuteCommand(const std::string& id, const InvocationContext& context)
    {
        const auto generation = GetCommandGeneration(id);
        if (!FindCommand(id)) return CommandResult::UnknownCommand;
        const bool available = CanExecute(id, context);
        if (GetCommandGeneration(id) != generation) return CommandResult::TargetExpired;
        if (!available) return CommandResult::Disabled;
        // CanExecute and Execute may register/unregister commands. Never retain
        // a vector element or callback reference across either call.
        const auto* command = FindCommand(id);
        if (!command) return CommandResult::TargetExpired;
        auto invoke = command->invoke;
        auto execute = command->execute;
        if (invoke) return invoke(context);
        if (!execute) return CommandResult::Disabled;
        execute();
        return CommandResult::Completed;
    }

    bool EditorShortcutRegistry::UnregisterCommand(const std::string& id)
    {
        const auto it = m_commandIndices.find(id);
        if (it == m_commandIndices.end()) return false;
        m_commandGenerations.erase(id);
        m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(it->second));
        m_commandIndices.clear();
        for (size_t i = 0; i < m_commands.size(); ++i) m_commandIndices.emplace(m_commands[i].id, i);
        ++m_generation;
        return true;
    }
    uint64_t EditorShortcutRegistry::GetCommandGeneration(const std::string& id) const
    { const auto it=m_commandGenerations.find(id); return it==m_commandGenerations.end()?0:it->second; }

}
