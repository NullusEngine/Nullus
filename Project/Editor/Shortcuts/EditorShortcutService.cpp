#include "Shortcuts/EditorShortcutService.h"

#include <algorithm>
#include <cctype>

namespace NLS::Editor::Shortcuts
{
    namespace
    {
        bool IsModifierDown(
            const KeyStateProvider& p_getKeyState,
            const Windowing::Inputs::EKey p_left,
            const Windowing::Inputs::EKey p_right)
        {
            using Windowing::Inputs::EKeyState;
            return p_getKeyState(p_left) == EKeyState::KEY_DOWN ||
                p_getKeyState(p_right) == EKeyState::KEY_DOWN;
        }

        bool ModifiersMatch(
            const ShortcutBinding& p_binding,
            const KeyStateProvider& p_getKeyState)
        {
            using Windowing::Inputs::EKey;
            return HasModifier(p_binding.modifiers, EShortcutModifier::Ctrl) ==
                    IsModifierDown(p_getKeyState, EKey::KEY_LEFT_CONTROL, EKey::KEY_RIGHT_CONTROL) &&
                HasModifier(p_binding.modifiers, EShortcutModifier::Shift) ==
                    IsModifierDown(p_getKeyState, EKey::KEY_LEFT_SHIFT, EKey::KEY_RIGHT_SHIFT) &&
                HasModifier(p_binding.modifiers, EShortcutModifier::Alt) ==
                    IsModifierDown(p_getKeyState, EKey::KEY_LEFT_ALT, EKey::KEY_RIGHT_ALT) &&
                HasModifier(p_binding.modifiers, EShortcutModifier::Super) ==
                    IsModifierDown(p_getKeyState, EKey::KEY_LEFT_SUPER, EKey::KEY_RIGHT_SUPER);
        }
    }

    bool EditorShortcutService::RegisterCommand(ShortcutCommand p_command)
    {
        return m_registry.RegisterCommand(std::move(p_command));
    }

    void EditorShortcutService::RegisterContext(ShortcutContext p_context)
    {
        m_contexts.emplace_back(std::move(p_context));
    }

    bool EditorShortcutService::ExecuteShortcut(const ShortcutBinding& p_binding)
    {
        const auto* command = EditorShortcutResolver::Resolve(
            m_registry.GetCommands(),
            [this](const std::string& p_commandId)
            {
                return m_registry.GetBinding(p_commandId);
            },
            p_binding,
            m_contexts);

        if (command == nullptr || !command->execute)
            return false;

        command->execute();
        return true;
    }

    bool EditorShortcutService::ExecutePressedShortcut(
        const KeyPressedPredicate& p_isKeyPressed,
        const KeyStateProvider& p_getKeyState)
    {
        for (const auto& command : m_registry.GetCommands())
        {
            const auto binding = m_registry.GetBinding(command.id);
            if (!binding.IsValid() ||
                !p_isKeyPressed(binding.primaryKey) ||
                !ModifiersMatch(binding, p_getKeyState))
            {
                continue;
            }

            return ExecuteShortcut(binding);
        }

        return false;
    }

    std::vector<ShortcutConflict> EditorShortcutService::ValidateBinding(
        const std::string& p_commandId,
        const ShortcutBinding& p_binding) const
    {
        return m_registry.ValidateBinding(
            p_commandId,
            p_binding,
            [this](const ShortcutContextId& p_first, const ShortcutContextId& p_second)
            {
                return AreContextsMutuallyExclusive(p_first, p_second);
            });
    }

    bool EditorShortcutService::LoadProfile(const std::filesystem::path& p_path)
    {
        const bool loaded = m_profile.Load(p_path);

        for (const auto& [commandId, binding] : m_profile.GetBindingOverrides())
            m_registry.SetBinding(commandId, binding);

        for (const auto& commandId : m_profile.GetUnassignedCommands())
            m_registry.ClearBinding(commandId);

        return loaded;
    }

    bool EditorShortcutService::SaveProfile(const std::filesystem::path& p_path) const
    {
        return m_profile.Save(p_path);
    }

    bool EditorShortcutService::SetBinding(const std::string& p_commandId, const ShortcutBinding& p_binding)
    {
        const auto conflicts = ValidateBinding(p_commandId, p_binding);
        if (!conflicts.empty())
            return false;

        if (!m_registry.SetBinding(p_commandId, p_binding))
            return false;

        m_profile.SetBindingOverride(p_commandId, p_binding);
        return true;
    }

    bool EditorShortcutService::ClearBinding(const std::string& p_commandId)
    {
        const auto* command = m_registry.FindCommand(p_commandId);
        if (command != nullptr && command->requiredBinding)
            return false;

        if (!m_registry.ClearBinding(p_commandId))
            return false;

        m_profile.SetCommandUnassigned(p_commandId);
        return true;
    }

    bool EditorShortcutService::ResetBinding(const std::string& p_commandId)
    {
        if (!m_registry.ResetBinding(p_commandId))
            return false;

        m_profile.RemoveBindingOverride(p_commandId);
        m_profile.RemoveCommandUnassigned(p_commandId);
        return true;
    }

    std::vector<const ShortcutCommand*> EditorShortcutService::SearchCommands(const std::string& p_query) const
    {
        auto normalize = [](std::string value)
        {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](unsigned char ch)
                {
                    return static_cast<char>(std::tolower(ch));
                });
            return value;
        };

        const auto query = normalize(p_query);
        std::vector<const ShortcutCommand*> result;
        for (const auto& command : m_registry.GetCommands())
        {
            const auto haystack = normalize(
                command.id + " " +
                command.displayName + " " +
                command.category + " " +
                command.context + " " +
                GetBindingDisplayText(command.id));

            if (query.empty() || haystack.find(query) != std::string::npos)
                result.push_back(&command);
        }

        return result;
    }

    std::vector<const ShortcutCommand*> EditorShortcutService::GetCommands() const
    {
        std::vector<const ShortcutCommand*> result;
        for (const auto& command : m_registry.GetCommands())
            result.push_back(&command);
        return result;
    }

    std::string EditorShortcutService::GetBindingDisplayText(const std::string& p_commandId) const
    {
        return ToDisplayString(m_registry.GetBinding(p_commandId));
    }

    ShortcutBinding EditorShortcutService::GetBinding(const std::string& p_commandId) const
    {
        return m_registry.GetBinding(p_commandId);
    }

    const EditorShortcutRegistry& EditorShortcutService::GetRegistry() const
    {
        return m_registry;
    }

    EditorShortcutRegistry& EditorShortcutService::GetRegistry()
    {
        return m_registry;
    }

    bool EditorShortcutService::AreContextsMutuallyExclusive(
        const ShortcutContextId& p_first,
        const ShortcutContextId& p_second) const
    {
        if (p_first == p_second)
            return false;
        if (p_first == ShortcutContexts::Global || p_second == ShortcutContexts::Global)
            return false;

        const ShortcutContext* first = nullptr;
        const ShortcutContext* second = nullptr;
        for (const auto& context : m_contexts)
        {
            if (context.id == p_first)
                first = &context;
            if (context.id == p_second)
                second = &context;
        }

        return first != nullptr &&
            second != nullptr &&
            !first->exclusiveGroup.empty() &&
            first->exclusiveGroup == second->exclusiveGroup;
    }
}
