#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "EditorShortcutBinding.h"

namespace NLS::Editor::Shortcuts
{
    class EditorShortcutProfile
    {
    public:
        EditorShortcutProfile() = default;

        void SetBindingOverride(const std::string& p_commandId, const ShortcutBinding& p_binding);
        void RemoveBindingOverride(const std::string& p_commandId);
        std::optional<ShortcutBinding> GetBindingOverride(const std::string& p_commandId) const;
        const std::unordered_map<std::string, ShortcutBinding>& GetBindingOverrides() const;

        void SetCommandUnassigned(const std::string& p_commandId);
        void RemoveCommandUnassigned(const std::string& p_commandId);
        bool IsCommandUnassigned(const std::string& p_commandId) const;
        const std::unordered_set<std::string>& GetUnassignedCommands() const;

        bool Load(const std::filesystem::path& p_path);
        bool Save(const std::filesystem::path& p_path) const;

    private:
        std::unordered_map<std::string, ShortcutBinding> m_bindingOverrides;
        std::unordered_set<std::string> m_unassignedCommands;
    };
}
