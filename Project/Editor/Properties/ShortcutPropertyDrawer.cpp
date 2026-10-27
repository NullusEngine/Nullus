#include "Properties/ShortcutPropertyDrawer.h"
#include "Properties/PropertyDrawerRegistry.h"
#include "Properties/PropertyControlState.h"
#include "Shortcuts/EditorShortcutService.h"
#include "Undo/EditorUndoService.h"
#include <Reflection/ExternalReflectionRegistration.h>
#include <ServiceLocator.h>
#include <array>

namespace NLS::Editor::Shortcuts
{
inline void RegisterShortcutPropertyReflection(meta::ReflectionDatabase& db, meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Windowing::Inputs::EKey)
    metaExternalType.SetEnum<NLS::Windowing::Inputs::EKey>(metaExternalTypeName,
                                                           {{"Unknown", NLS::Windowing::Inputs::EKey::KEY_UNKNOWN}});
    NLS_META_EXTERNAL_END();
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Shortcuts::ShortcutBinding)
    NLS_META_EXTERNAL_FIELD(bool, assigned);
    // Key codes persist as the platform-independent engine key enum's
    // underlying numeric layout, with its own reflected enum descriptor.
    NLS_META_EXTERNAL_FIELD(Windowing::Inputs::EKey, primaryKey);
    NLS_META_EXTERNAL_FIELD(uint8_t, modifiers);
    NLS_META_EXTERNAL_END();
}
} // namespace NLS::Editor::Shortcuts
NLS_META_EXTERNAL_MODULE(NLS::Editor::Shortcuts::RegisterShortcutPropertyReflection)

namespace NLS::Editor::Properties
{
namespace
{
using namespace Shortcuts;
using Windowing::Inputs::EKey;
using namespace UI::Controls;
ImGuiKey ToUi(EKey code)
{
    const int key = static_cast<int>(code);
    if (key >= 65 && key <= 90)
        return static_cast<ImGuiKey>(ImGuiKey_A + key - 65);
    if (key >= 48 && key <= 57)
        return static_cast<ImGuiKey>(ImGuiKey_0 + key - 48);
    if (key >= 290 && key <= 313)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + key - 290);
    if (key >= 320 && key <= 329)
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + key - 320);
    const std::pair<EKey, ImGuiKey> keys[] = {{EKey::KEY_SPACE, ImGuiKey_Space},
                                              {EKey::KEY_ENTER, ImGuiKey_Enter},
                                              {EKey::KEY_TAB, ImGuiKey_Tab},
                                              {EKey::KEY_BACKSPACE, ImGuiKey_Backspace},
                                              {EKey::KEY_INSERT, ImGuiKey_Insert},
                                              {EKey::KEY_DELETE, ImGuiKey_Delete},
                                              {EKey::KEY_LEFT, ImGuiKey_LeftArrow},
                                              {EKey::KEY_RIGHT, ImGuiKey_RightArrow},
                                              {EKey::KEY_UP, ImGuiKey_UpArrow},
                                              {EKey::KEY_DOWN, ImGuiKey_DownArrow},
                                              {EKey::KEY_PAGE_UP, ImGuiKey_PageUp},
                                              {EKey::KEY_PAGE_DOWN, ImGuiKey_PageDown},
                                              {EKey::KEY_HOME, ImGuiKey_Home},
                                              {EKey::KEY_END, ImGuiKey_End},
                                              {EKey::KEY_APOSTROPHE, ImGuiKey_Apostrophe},
                                              {EKey::KEY_COMMA, ImGuiKey_Comma},
                                              {EKey::KEY_MINUS, ImGuiKey_Minus},
                                              {EKey::KEY_PERIOD, ImGuiKey_Period},
                                              {EKey::KEY_SLASH, ImGuiKey_Slash},
                                              {EKey::KEY_SEMICOLON, ImGuiKey_Semicolon},
                                              {EKey::KEY_EQUAL, ImGuiKey_Equal},
                                              {EKey::KEY_LEFT_BRACKET, ImGuiKey_LeftBracket},
                                              {EKey::KEY_BACKSLASH, ImGuiKey_Backslash},
                                              {EKey::KEY_RIGHT_BRACKET, ImGuiKey_RightBracket},
                                              {EKey::KEY_GRAVE_ACCENT, ImGuiKey_GraveAccent},
                                              {EKey::KEY_ESCAPE, ImGuiKey_Escape},
                                              {EKey::KEY_CAPS_LOCK, ImGuiKey_CapsLock},
                                              {EKey::KEY_SCROLL_LOCK, ImGuiKey_ScrollLock},
                                              {EKey::KEY_NUM_LOCK, ImGuiKey_NumLock},
                                              {EKey::KEY_PRINT_SCREEN, ImGuiKey_PrintScreen},
                                              {EKey::KEY_PAUSE, ImGuiKey_Pause},
                                              {EKey::KEY_KP_DECIMAL, ImGuiKey_KeypadDecimal},
                                              {EKey::KEY_KP_DIVIDE, ImGuiKey_KeypadDivide},
                                              {EKey::KEY_KP_MULTIPLY, ImGuiKey_KeypadMultiply},
                                              {EKey::KEY_KP_SUBTRACT, ImGuiKey_KeypadSubtract},
                                              {EKey::KEY_KP_ADD, ImGuiKey_KeypadAdd},
                                              {EKey::KEY_KP_ENTER, ImGuiKey_KeypadEnter},
                                              {EKey::KEY_KP_EQUAL, ImGuiKey_KeypadEqual}};
    for (const auto& [engine, ui] : keys)
        if (engine == code)
            return ui;
    return ImGuiKey_None;
}
EKey FromUi(ImGuiKey ui)
{
    for (int key = 32; key <= 348; ++key)
        if (ToUi(static_cast<EKey>(key)) == ui)
            return static_cast<EKey>(key);
    return EKey::KEY_UNKNOWN;
}
class BindingCommand final : public Undo::IEditorUndoCommand
{
public:
    BindingCommand(std::string id, ShortcutBinding before, ShortcutBinding after)
        : m_id(std::move(id)), m_before(before), m_after(after)
    {
    }
    Undo::UndoCommandInfo GetInfo() const override
    {
        Undo::UndoCommandInfo info;
        info.label = "Change shortcut " + m_id;
        info.sourceModule = "Properties";
        info.scopes = {{Undo::UndoScopeKind::ProjectSettings, "EditorShortcuts", 0}};
        info.affected = {m_id};
        info.estimatedBytes = sizeof(*this) + m_id.size();
        return info;
    }
    Undo::UndoValidationResult Validate(Undo::UndoDirection direction,
                                        const Undo::UndoExecutionContext&) const override
    {
        if (!NLS::Core::ServiceLocator::Contains<EditorShortcutService>())
            return Undo::UndoValidationResult::Stale("Shortcut service is closed");
        const auto current = NLS::Core::ServiceLocator::Get<EditorShortcutService>().GetBinding(m_id);
        return current == (direction == Undo::UndoDirection::Undo ? m_after : m_before) || (direction == Undo::UndoDirection::Redo && current == m_after) ? Undo::UndoValidationResult::Ready() : Undo::UndoValidationResult::Conflict("Shortcut changed outside this edit");
    }
    Undo::UndoStartResult Apply(Undo::UndoDirection direction, const Undo::UndoExecutionContext&,
                                Undo::UndoCompletion) override
    {
        return NLS::Core::ServiceLocator::Get<EditorShortcutService>().SetBinding(
                   m_id, direction == Undo::UndoDirection::Undo ? m_before : m_after) ?
                   Undo::UndoStartResult::Completed() :
                   Undo::UndoStartResult::Failed("Shortcut is no longer available");
    }
    bool TryMergeFrom(const Undo::IEditorUndoCommand&) override
    {
        return false;
    }

private:
    std::string m_id;
    ShortcutBinding m_before, m_after;
};
} // namespace
void RegisterShortcutPropertyDrawer()
{
    static auto token = []
    {
        PropertyDrawerRegistration registration;
        registration.scope = DrawerScope::ExactType;
        registration.identifier = "builtin.shortcut";
        registration.valueType = NLS_TYPEOF(ShortcutBinding);
        registration.draw = [](auto& c)
        {
            auto value = c.value.template GetValue<ShortcutBinding>();
            KeyChord chord;
            chord.key = value.assigned ? ToUi(value.primaryKey) : ImGuiKey_None;
            chord.ctrl = HasModifier(value.modifiers, EShortcutModifier::Ctrl);
            chord.shift = HasModifier(value.modifiers, EShortcutModifier::Shift);
            chord.alt = HasModifier(value.modifiers, EShortcutModifier::Alt);
            chord.super = HasModifier(value.modifiers, EShortcutModifier::Super);
            auto& state = static_cast<PropertyControlState*>(c.state)->keyChord;
            c.interaction = KeyChordField(c.widgetId.c_str(), chord, state, {c.property.readOnly || c.property.metadata.readOnly, false, c.uiScale});
            if (c.interaction.changed)
            {
                value = ShortcutBinding::Unassigned();
                if (chord.key != ImGuiKey_None)
                {
                    uint8_t modifiers = 0;
                    if (chord.ctrl)
                        modifiers |= ToModifierMask(EShortcutModifier::Ctrl);
                    if (chord.shift)
                        modifiers |= ToModifierMask(EShortcutModifier::Shift);
                    if (chord.alt)
                        modifiers |= ToModifierMask(EShortcutModifier::Alt);
                    if (chord.super)
                        modifiers |= ToModifierMask(EShortcutModifier::Super);
                    value = ShortcutBinding::FromKey(FromUi(chord.key), modifiers);
                }
                c.changed = c.preview(meta::Variant(value), "Change shortcut");
            }
            return true;
        };
        return PropertyDrawerRegistry::Get().Register(std::move(registration));
    }();
}
bool ApplyShortcutBinding(const std::string& commandId, const ShortcutBinding& binding, std::string& error)
{
    if (!NLS::Core::ServiceLocator::Contains<EditorShortcutService>())
        return false;
    auto& service = NLS::Core::ServiceLocator::Get<EditorShortcutService>();
    const auto before = service.GetBinding(commandId);
    if (before == binding)
        return true;
    const auto conflicts = service.ValidateBinding(commandId, binding);
    if (!conflicts.empty())
    {
        error = conflicts.front().message;
        return false;
    }
    if (!service.SetBinding(commandId, binding))
        return false;
    if (NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>())
    {
        const auto result = NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>().RecordApplied(
            std::make_unique<BindingCommand>(commandId, before, binding));
        if (!result.Succeeded())
        {
            service.SetBinding(commandId, before);
            error = result.message;
            return false;
        }
    }
    return true;
}
} // namespace NLS::Editor::Properties
