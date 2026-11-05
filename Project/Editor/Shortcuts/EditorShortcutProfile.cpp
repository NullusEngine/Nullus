#include "Shortcuts/EditorShortcutProfile.h"

#include <fstream>
#include <charconv>

#include <Json/json.hpp>

namespace NLS::Editor::Shortcuts
{
    namespace
    {
        using json = nlohmann::json;

        std::optional<ShortcutBinding> BindingFromJson(const json& p_json)
        {
            if (!p_json.is_object())
                return std::nullopt;

            ShortcutBinding binding;
            const auto assigned = p_json.find("assigned"), key = p_json.find("key"), modifiers = p_json.find("modifiers");
            if (assigned == p_json.end() || !assigned->is_boolean() || key == p_json.end() || !key->is_string() ||
                modifiers == p_json.end() || !modifiers->is_number_integer()) return std::nullopt;
            const auto keyText = key->get<std::string>();
            int keyCode = -1;
            const auto converted = std::from_chars(keyText.data(), keyText.data() + keyText.size(), keyCode);
            if (converted.ec != std::errc{} || converted.ptr != keyText.data() + keyText.size()) return std::nullopt;
            const auto modifierBits = modifiers->get<int64_t>();
            if (modifierBits < 0 || modifierBits > 31 || keyCode < 0 || keyCode >= 512) return std::nullopt;
            binding.assigned = assigned->get<bool>();
            binding.primaryKey = static_cast<Windowing::Inputs::EKey>(keyCode);
            binding.modifiers = static_cast<ShortcutModifiers>(modifierBits);

            if (!binding.IsValid())
                return std::nullopt;

            return binding;
        }
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
            if (!input.is_open())
                return false;

            // Parse without throwing so malformed user profiles remain a
            // recoverable configuration error even in builds where the JSON
            // library is compiled with exceptions disabled.
            const json root = json::parse(input, nullptr, false);
            if (root.is_discarded() || !root.is_object())
                return false;
            if (const auto version = root.find("version"); version != root.end() &&
                (!version->is_number_integer() || version->get<int64_t>() != 1)) return false;

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

}
