#pragma once

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
        bool CanExecute(const std::string& id, const InvocationContext& context = {}) const;
        CommandResult TryExecuteCommand(const std::string& id, const InvocationContext& context = {});
        uint64_t GetGeneration() const { return m_generation; }
        uint64_t GetCommandGeneration(const std::string& id) const;
        bool UnregisterCommand(const std::string& id);

    private:
        std::vector<ShortcutCommand> m_commands;
        std::unordered_map<std::string, size_t> m_commandIndices;
        std::unordered_map<std::string, uint64_t> m_commandGenerations;
        uint64_t m_generation = 0;
    };
}
