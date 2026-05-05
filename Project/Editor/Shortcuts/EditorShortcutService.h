#pragma once

#include <vector>

#include "EditorShortcutRegistry.h"
#include "EditorShortcutResolver.h"
#include "EditorShortcutProfile.h"
#include <Windowing/Inputs/EKeyState.h>

namespace NLS::Editor::Shortcuts
{
    using KeyPressedPredicate = std::function<bool(Windowing::Inputs::EKey)>;
    using KeyStateProvider = std::function<Windowing::Inputs::EKeyState(Windowing::Inputs::EKey)>;

    class EditorShortcutService
    {
    public:
        EditorShortcutService() = default;

        bool RegisterCommand(ShortcutCommand p_command);
        void RegisterContext(ShortcutContext p_context);
        bool ExecuteShortcut(const ShortcutBinding& p_binding);
        bool ExecutePressedShortcut(
            const KeyPressedPredicate& p_isKeyPressed,
            const KeyStateProvider& p_getKeyState);
        std::vector<ShortcutConflict> ValidateBinding(
            const std::string& p_commandId,
            const ShortcutBinding& p_binding) const;
        bool LoadProfile(const std::filesystem::path& p_path);
        bool SaveProfile(const std::filesystem::path& p_path) const;
        bool SetBinding(const std::string& p_commandId, const ShortcutBinding& p_binding);
        bool ClearBinding(const std::string& p_commandId);
        bool ResetBinding(const std::string& p_commandId);
        std::vector<const ShortcutCommand*> SearchCommands(const std::string& p_query) const;
        std::vector<const ShortcutCommand*> GetCommands() const;
        std::string GetBindingDisplayText(const std::string& p_commandId) const;
        ShortcutBinding GetBinding(const std::string& p_commandId) const;

        const EditorShortcutRegistry& GetRegistry() const;
        EditorShortcutRegistry& GetRegistry();

    private:
        bool AreContextsMutuallyExclusive(const ShortcutContextId& p_first, const ShortcutContextId& p_second) const;

    private:
        EditorShortcutRegistry m_registry;
        std::vector<ShortcutContext> m_contexts;
        EditorShortcutProfile m_profile;
    };
}
