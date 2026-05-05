#include "Shortcuts/EditorShortcutProfile.h"

#include <fstream>

#include <Json/json.hpp>

namespace NLS::Editor::Shortcuts
{
    namespace
    {
        using json = nlohmann::json;

        std::string KeyToStorageString(const Windowing::Inputs::EKey p_key)
        {
            return std::to_string(static_cast<int>(p_key));
        }

        Windowing::Inputs::EKey KeyFromStorageString(const std::string& p_value)
        {
            return static_cast<Windowing::Inputs::EKey>(std::stoi(p_value));
        }

        json BindingToJson(const ShortcutBinding& p_binding)
        {
            return json::object({
                { "assigned", p_binding.assigned },
                { "key", KeyToStorageString(p_binding.primaryKey) },
                { "modifiers", p_binding.modifiers }
            });
        }

        std::optional<ShortcutBinding> BindingFromJson(const json& p_json)
        {
            if (!p_json.is_object())
                return std::nullopt;

            ShortcutBinding binding;
            binding.assigned = p_json.value("assigned", false);
            binding.primaryKey = KeyFromStorageString(p_json.value("key", "-1"));
            binding.modifiers = static_cast<ShortcutModifiers>(p_json.value("modifiers", 0));

            if (!binding.IsValid())
                return std::nullopt;

            return binding;
        }
    }

    void EditorShortcutProfile::SetBindingOverride(const std::string& p_commandId, const ShortcutBinding& p_binding)
    {
        m_bindingOverrides[p_commandId] = p_binding;
        m_unassignedCommands.erase(p_commandId);
    }

    void EditorShortcutProfile::RemoveBindingOverride(const std::string& p_commandId)
    {
        m_bindingOverrides.erase(p_commandId);
    }

    std::optional<ShortcutBinding> EditorShortcutProfile::GetBindingOverride(const std::string& p_commandId) const
    {
        const auto it = m_bindingOverrides.find(p_commandId);
        if (it == m_bindingOverrides.end())
            return std::nullopt;
        return it->second;
    }

    const std::unordered_map<std::string, ShortcutBinding>& EditorShortcutProfile::GetBindingOverrides() const
    {
        return m_bindingOverrides;
    }

    void EditorShortcutProfile::SetCommandUnassigned(const std::string& p_commandId)
    {
        m_bindingOverrides.erase(p_commandId);
        m_unassignedCommands.insert(p_commandId);
    }

    void EditorShortcutProfile::RemoveCommandUnassigned(const std::string& p_commandId)
    {
        m_unassignedCommands.erase(p_commandId);
    }

    bool EditorShortcutProfile::IsCommandUnassigned(const std::string& p_commandId) const
    {
        return m_unassignedCommands.contains(p_commandId);
    }

    const std::unordered_set<std::string>& EditorShortcutProfile::GetUnassignedCommands() const
    {
        return m_unassignedCommands;
    }

    bool EditorShortcutProfile::Load(const std::filesystem::path& p_path)
    {
        m_bindingOverrides.clear();
        m_unassignedCommands.clear();

        if (!std::filesystem::exists(p_path))
            return true;

        try
        {
            std::ifstream input(p_path);
            json root;
            input >> root;

            if (const auto bindingsIt = root.find("bindings");
                bindingsIt != root.end() && bindingsIt->is_object())
            {
                for (const auto& [commandId, bindingJson] : bindingsIt->items())
                {
                    if (auto binding = BindingFromJson(bindingJson))
                        m_bindingOverrides[commandId] = *binding;
                }
            }

            if (const auto unassignedIt = root.find("unassigned");
                unassignedIt != root.end() && unassignedIt->is_array())
            {
                for (const auto& commandId : *unassignedIt)
                {
                    if (commandId.is_string())
                        m_unassignedCommands.insert(commandId.get<std::string>());
                }
            }
        }
        catch (...)
        {
            m_bindingOverrides.clear();
            m_unassignedCommands.clear();
            return false;
        }

        return true;
    }

    bool EditorShortcutProfile::Save(const std::filesystem::path& p_path) const
    {
        try
        {
            std::filesystem::create_directories(p_path.parent_path());

            json bindings = json::object();
            for (const auto& [commandId, binding] : m_bindingOverrides)
                bindings[commandId] = BindingToJson(binding);

            json unassigned = json::array();
            for (const auto& commandId : m_unassignedCommands)
                unassigned.push_back(commandId);

            const json root = json::object({
                { "version", 1 },
                { "profileName", "Default" },
                { "bindings", bindings },
                { "unassigned", unassigned }
            });

            std::ofstream output(p_path);
            output << root.dump(4);
        }
        catch (...)
        {
            return false;
        }

        return true;
    }
}
