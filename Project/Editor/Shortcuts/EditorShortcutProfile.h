#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "EditorShortcutBinding.h"

namespace NLS::Editor::Shortcuts
{
    // Read-only importer for the former shortcuts.json format. Runtime state
    // and all writes use InputUser / InputProfile instead.
    class EditorShortcutProfile
    {
    public:
        EditorShortcutProfile() = default;

        std::optional<ShortcutBinding> GetBindingOverride(const std::string& p_commandId) const;
        const std::unordered_map<std::string, ShortcutBinding>& GetBindingOverrides() const;

        bool IsCommandUnassigned(const std::string& p_commandId) const;
        const std::unordered_set<std::string>& GetUnassignedCommands() const;

        bool Load(const std::filesystem::path& p_path);

    private:
        std::unordered_map<std::string, ShortcutBinding> m_bindingOverrides;
        std::unordered_set<std::string> m_unassignedCommands;
    };
}
