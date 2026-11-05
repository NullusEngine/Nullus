#pragma once
#include "EditorShortcutRegistry.h"
#include "EditorInputRouter.h"
#include "EditorInputActions.h"
#include <Input/InputDomain.h>
#include <span>

namespace NLS::Editor::Shortcuts
{
class EditorShortcutAdapter final
{
public:
    EditorShortcutAdapter(Input::InputSystem& system, EditorShortcutRegistry& commands);
    void Rebuild(const std::vector<ShortcutContext>& contexts);
    void SetDefaults(std::shared_ptr<const Input::CompiledInput> defaults) { m_defaults = std::move(defaults); m_generation = UINT64_MAX; }
    void Update(const std::vector<ShortcutContext>& contexts, EditorInputRoute route);
    void Prepare(const std::vector<ShortcutContext>& contexts, EditorInputRoute route);
    void Dispatch(EditorInputRoute route);
    Input::InputUser& GetUser() { return *m_user; }
    Input::InputSystem& GetSystem() { return m_system; }
    const std::vector<Input::BindingId>& GetBindingSlots(const std::string& id) const;
    Input::InputValue GetNavigationValue() const { return m_navigationUser->GetActionValue(m_navigationUser->FindAction(NavigationAction())); }
    Input::ActionSnapshot GetInputState(EditorInputAction action);
    void RefreshContinuousInput(bool force = false, bool checkRestoredPress = false);
    static Input::ActionId ActionForCommand(const std::string& command);
    static Input::BindingId BindingForCommand(const std::string& command, size_t slot);
    static Input::ContextId ContextForScope(const std::string& scope);
    static Input::Chord ConvertChord(const ShortcutBinding& binding);
    static Input::Chord NavigationChord(const ShortcutBinding& binding);
    static bool IsSessionControl(const std::string& id);
    static Input::ActionId NavigationAction();
    static Input::InputValue NavigationContribution(const std::string& id);
    static std::span<const ShortcutDescriptor> NavigationSettings();
    static std::span<const ShortcutDescriptor> ContinuousSettings();
private:
    Input::InputSystem& m_system;
    EditorShortcutRegistry& m_commands;
    Input::InputDomain m_domain;
    std::shared_ptr<Input::InputUser> m_user;
    std::shared_ptr<Input::InputUser> m_navigationUser;
    uint64_t m_navigationProfileRevision = 0;
    std::shared_ptr<const Input::CompiledInput> m_defaults;
    std::vector<Input::InputSubscription> m_subscriptions;
    std::vector<std::string> m_scopes;
    std::vector<ShortcutContext> m_contexts;
    std::vector<Input::DeviceId> m_gamepads;
    std::unordered_map<std::string, std::vector<Input::BindingId>> m_bindingSlots;
    EditorInputRouter m_router;
    uint64_t m_generation = UINT64_MAX;
    std::array<Input::ActionHandle, static_cast<size_t>(EditorInputAction::Count)> m_continuousHandles {};
    std::array<Input::ActionSnapshot, static_cast<size_t>(EditorInputAction::Count)> m_continuousStates {};
    uint64_t m_continuousFrame = UINT64_MAX;
    uint64_t m_continuousSequence = UINT64_MAX;
    uint64_t m_preparedFrame = UINT64_MAX;
};
}
