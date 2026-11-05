#pragma once

#include <vector>
#include <filesystem>

#include "EditorShortcutRegistry.h"
#include "EditorInputRouter.h"
#include "EditorInputActions.h"
#include <memory>
#include <Math/Vector3.h>
#include <Input/InputRebinding.h>

namespace NLS::Input { class InputSystem; class InputUser; }

namespace NLS::Editor::Shortcuts
{
    class EditorShortcutAdapter;

    class EditorShortcutService
    {
    public:
        EditorShortcutService();
        ~EditorShortcutService();
        void InitializeInput(Input::InputSystem& system);
        bool SetInputDefaults(std::shared_ptr<const Input::CompiledInput> definition);
        bool LoadDefaults(const std::filesystem::path& path, std::vector<std::string>& diagnostics);
        void UpdateInput(EditorInputRoute route);
        void PrepareInput(EditorInputRoute route);
        void DispatchInput(EditorInputRoute route);
        Maths::Vector3 GetNavigationValue() const;
        Input::ActionSnapshot GetInputState(EditorInputAction action);
        Input::InputUser* GetInputUser();
        const std::vector<Input::BindingId>& GetBindingSlots(const std::string& id) const;
        CommandResult TryExecuteCommand(const std::string& id, const InvocationContext& context = {});
        std::string CaptureBindingState(const std::string& id) const;
        bool RestoreBindingState(const std::string& id, const std::string& snapshot);
        std::unique_ptr<Input::RebindingSession> BeginRebinding(const std::string& id, size_t slot);
        bool SetBindingSlot(const std::string& id, size_t slot, Input::BindingOverride value, std::string& error);
        std::string GetBindingSlotDisplayText(const std::string& id, size_t slot) const;

        bool RegisterCommand(ShortcutCommand p_command);
        void RegisterContext(ShortcutContext p_context);
        std::vector<ShortcutConflict> ValidateBinding(
            const std::string& p_commandId,
            const ShortcutBinding& p_binding) const;
        // Legacy JSON is read only when the new profile does not exist.
        bool LoadProfile(const std::filesystem::path& p_path, const std::filesystem::path& legacyPath = {});
        const std::vector<std::string>& GetProfileDiagnostics() const { return m_profileDiagnostics; }
        bool SaveProfile(const std::filesystem::path& p_path) const;
        bool SetBinding(const std::string& p_commandId, const ShortcutBinding& p_binding);
        bool ClearBinding(const std::string& p_commandId);
        bool ResetBinding(const std::string& p_commandId);
        std::vector<const ShortcutDescriptor*> SearchCommands(const std::string& p_query) const;
        std::vector<const ShortcutDescriptor*> GetCommands() const;
        std::string GetBindingDisplayText(const std::string& p_commandId) const;
        ShortcutBinding GetBinding(const std::string& p_commandId) const;

        const EditorShortcutRegistry& GetRegistry() const;
        EditorShortcutRegistry& GetRegistry();

    private:
        const ShortcutDescriptor* FindDescriptor(const std::string& id) const;
        const ShortcutDescriptor* FindBindingDescriptor(Input::BindingId binding) const;
        bool ImportLegacyProfile(const std::filesystem::path& path);
        bool AllowsBindingOverlap(const ShortcutDescriptor& first, const ShortcutDescriptor& second) const;
        bool AreContextsMutuallyExclusive(const ShortcutContextId& p_first, const ShortcutContextId& p_second) const;
        bool AreInputContextsMutuallyExclusive(Input::ContextId first, Input::ContextId second) const;

    private:
        EditorShortcutRegistry m_registry;
        std::vector<ShortcutContext> m_contexts;
        std::vector<std::string> m_profileDiagnostics;
        std::unique_ptr<EditorShortcutAdapter> m_adapter;
        std::filesystem::path m_inputProfilePath;
        std::filesystem::path m_defaultsPath;
        std::filesystem::file_time_type m_defaultsTime;
        double m_nextDefaultsPoll = 0;
    };
}
