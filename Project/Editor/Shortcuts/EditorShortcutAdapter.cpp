#include "EditorShortcutAdapter.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace NLS::Editor::Shortcuts
{
namespace
{
const std::string ControlScope = "editor-session-controls";
const std::string GameControlScope = "game-input-controls";
}
std::span<const ShortcutDescriptor> EditorShortcutAdapter::ContinuousSettings()
{
    static const auto settings = []
    {
        std::array<ShortcutDescriptor, static_cast<size_t>(EditorInputAction::Count)> result;
        const char* names[] = {"Fly Navigation", "Pan Navigation", "Orbit Navigation", "Navigation Boost",
            "Navigation Pointer Delta", "Navigation Wheel Zoom", "Temporary Gizmo Snap", "Cancel Scene Gesture", "Suspend Scene Selection",
            "Pan Canvas", "Canvas Wheel Zoom", "Canvas Horizontal Scroll", "Canvas Vertical Scroll",
            "Bypass Canvas Snapping", "Constrain Canvas Resize", "Toggle Scene Selection", "Add Scene Selection",
            "Toggle Canvas Selection", "Add Canvas Selection", "Timeline Wheel Zoom"};
        for (size_t i = 0; i < result.size(); ++i)
        {
            result[i].id = EditorInputActionIds[i]; result[i].displayName = names[i];
            result[i].context = i < 6 ? "camera-navigation" : i < 9 ? "scene-pointer-controls" : "ui-designer-pointer-controls";
            result[i].category = i < 6 ? "Viewport Navigation" : i < 9 ? "Scene View/Gestures" : "UI Designer/Canvas";
            if (i == static_cast<size_t>(EditorInputAction::SceneToggleSelection) || i == static_cast<size_t>(EditorInputAction::SceneAddSelection))
            { result[i].context = "scene-selection-controls"; result[i].category = "Scene View/Selection"; }
            if (i == static_cast<size_t>(EditorInputAction::CanvasToggleSelection) || i == static_cast<size_t>(EditorInputAction::CanvasAddSelection))
                result[i].context = "ui-designer-selection-controls";
            if (i == static_cast<size_t>(EditorInputAction::VfxTimelineZoom))
            { result[i].context = "vfx-timeline-controls"; result[i].category = "VFX/Timeline"; }
            if (i == static_cast<size_t>(EditorInputAction::SceneCancel)) result[i].context = "scene-input-gesture";
        }
        return result;
    }();
    return settings;
}
std::span<const ShortcutDescriptor> EditorShortcutAdapter::NavigationSettings()
{
    static const auto settings = []
    {
        std::array<ShortcutDescriptor, 6> result;
        const char* directions[] = {"forward", "backward", "left", "right", "up", "down"};
        const char* names[] = {"Fly Mode Forward", "Fly Mode Backward", "Fly Mode Left", "Fly Mode Right", "Fly Mode Up", "Fly Mode Down"};
        for (size_t i = 0; i < result.size(); ++i)
        {
            result[i].id = std::string("scene-view.fly-") + directions[i];
            result[i].displayName = names[i];
            result[i].category = "Scene View/Fly Mode";
            result[i].context = ShortcutContexts::SceneViewFlyMode;
        }
        return result;
    }();
    return settings;
}
const std::vector<Input::BindingId>& EditorShortcutAdapter::GetBindingSlots(const std::string& id) const
{
    static const std::vector<Input::BindingId> empty;
    const auto found = m_bindingSlots.find(id);
    return found == m_bindingSlots.end() ? empty : found->second;
}
Input::ActionId EditorShortcutAdapter::ActionForCommand(const std::string& command) { return Guid::NewDeterministic("editor.action." + command); }
Input::ActionId EditorShortcutAdapter::NavigationAction()
{
    static const auto id = ActionForCommand("editor.navigation.move");
    return id;
}
Input::InputValue EditorShortcutAdapter::NavigationContribution(const std::string& id)
{
    if (id == "scene-view.fly-forward") return {0, 0, 1};
    if (id == "scene-view.fly-backward") return {0, 0, -1};
    if (id == "scene-view.fly-left") return {-1, 0, 0};
    if (id == "scene-view.fly-right") return {1, 0, 0};
    if (id == "scene-view.fly-up") return {0, 1, 0};
    if (id == "scene-view.fly-down") return {0, -1, 0};
    return {1};
}
Input::BindingId EditorShortcutAdapter::BindingForCommand(const std::string& command, size_t slot)
{ return Guid::NewDeterministic("editor.binding." + command + "." + (slot == 0 ? std::string("primary") : "alternate-" + std::to_string(slot))); }
Input::ContextId EditorShortcutAdapter::ContextForScope(const std::string& scope) { return Guid::NewDeterministic("editor.context." + scope); }
bool EditorShortcutAdapter::IsSessionControl(const std::string& id)
{
    return id == "editor.play" || id == "editor.stop" || id == "editor.pause" || id == "editor.step" || id == "editor.release-game-input" || id.starts_with("debug.renderdoc");
}
Input::Chord EditorShortcutAdapter::ConvertChord(const ShortcutBinding& binding)
{
    Input::Chord chord;
    chord.matchModifiers = true;
    chord.modifierMatch = Input::ModifierMatch::Exact;
    auto add = [&](EShortcutModifier source, Input::Modifier target)
    { if (HasModifier(binding.modifiers, source)) chord.modifiers = chord.modifiers | target; };
    add(EShortcutModifier::Shift, Input::Modifier::Shift); add(EShortcutModifier::Ctrl, Input::Modifier::Control);
    add(EShortcutModifier::Alt, Input::Modifier::Alt); add(EShortcutModifier::Super, Input::Modifier::Super);
    add(EShortcutModifier::Primary, Input::Modifier::Primary);
    return chord;
}
EditorShortcutAdapter::EditorShortcutAdapter(Input::InputSystem& system, EditorShortcutRegistry& commands)
    : m_system(system), m_commands(commands), m_domain(system, "Editor"), m_user(m_domain.CreateUser()), m_navigationUser(m_domain.CreateUser())
{ m_user->SetFixedBufferEnabled(false); m_navigationUser->SetFixedBufferEnabled(false); m_gamepads.reserve(16); }

Input::Chord EditorShortcutAdapter::NavigationChord(const ShortcutBinding& binding)
{
    auto chord = ConvertChord(binding);
    chord.modifierMatch = Input::ModifierMatch::RequiredSubset;
    auto required = static_cast<uint8_t>(chord.modifiers);
#ifdef __APPLE__
    if (required & 16u) required |= 8u;
#else
    if (required & 16u) required |= 2u;
#endif
    chord.forbiddenModifiers = static_cast<Input::Modifier>((2u | 4u | 8u) & ~required);
    return chord;
}

void EditorShortcutAdapter::Rebuild(const std::vector<ShortcutContext>& contexts)
{
    m_subscriptions.clear();
    Input::InputDefinition data;
    data.id = Guid::NewDeterministic("nullus.editor.default-input");
    m_scopes.clear();
    m_bindingSlots.clear();
    auto addContext = [&](const std::string& scope, int priority)
    {
        if (std::find(m_scopes.begin(), m_scopes.end(), scope) != m_scopes.end()) return;
        Input::ContextDefinition context;
        context.id = ContextForScope(scope); context.name = scope; context.priority = priority;
        if (m_defaults)
            for (const auto& source : m_defaults->GetDefinition().contexts)
                if (source.id == context.id) { context = source; context.bindings.clear(); break; }
        // The current UI grants focus after the pump, so new events in that
        // pump must be evaluated. Already-held controls are latched separately.
        if (!m_defaults || std::none_of(m_defaults->GetDefinition().contexts.begin(), m_defaults->GetDefinition().contexts.end(),
            [&](const auto& source) { return source.id == context.id; }))
            context.reacquire = scope == ShortcutContexts::SceneViewFlyMode ? Input::Reacquire::ResumeImmediately : Input::Reacquire::RequireRelease;
        data.contexts.push_back(context); m_scopes.push_back(scope);
    };
    addContext(ShortcutContexts::Global, 0); addContext(ControlScope, 200);
    addContext(GameControlScope, 210);
    for (const auto& context : contexts) if (context.id != ShortcutContexts::TextInput) addContext(context.id, context.priority);
    // Continuous operations retain their resource Action and independent slots;
    // they expose settings without registering empty business Commands.
    auto addContinuous = [&](const ShortcutDescriptor& setting, Input::ActionId id, bool direction)
    {
        if (!m_defaults) return;
        const auto& definition = m_defaults->GetDefinition();
        const auto action = std::find_if(definition.actions.begin(), definition.actions.end(),
            [&](const auto& value) { return value.id == id; });
        if (action == definition.actions.end()) return;
        if (std::none_of(data.actions.begin(), data.actions.end(), [&](const auto& value) { return value.id == id; }))
            data.actions.push_back(*action);
        addContext(setting.context, direction ? 40 : 30);
        auto& context = *std::find_if(data.contexts.begin(), data.contexts.end(),
            [&](const auto& value) { return value.id == ContextForScope(setting.context); });
        for (const auto& source : definition.contexts)
            if (source.id == context.id)
                for (const auto& binding : source.bindings)
                    if (binding.action == id && (!direction || binding.id == BindingForCommand(setting.id, 0)))
                    { context.bindings.push_back(binding); m_bindingSlots[setting.id].push_back(binding.id); }
    };
    for (const auto& setting : NavigationSettings()) addContinuous(setting, NavigationAction(), true);
    for (const auto& setting : ContinuousSettings()) addContinuous(setting, ActionForCommand(setting.id), false);
    for (const auto& command : m_commands.GetCommands())
    {
        const bool control = IsSessionControl(command.id) || command.allowDuringTextInput;
        const auto scope = command.id == "editor.release-game-input" ? GameControlScope : control ? ControlScope : command.context;
        auto scopeIndex = std::find(m_scopes.begin(), m_scopes.end(), scope);
        if (scopeIndex == m_scopes.end()) { addContext(scope, 20); scopeIndex = std::prev(m_scopes.end()); }
        const size_t context = static_cast<size_t>(scopeIndex - m_scopes.begin());
        Input::ActionDefinition action;
        action.id = ActionForCommand(command.id); action.name = command.displayName;
        if (m_defaults)
            for (const auto& source : m_defaults->GetDefinition().actions)
                if (source.id == action.id) { action = source; break; }
        if (std::none_of(data.actions.begin(), data.actions.end(), [&](const auto& value) { return value.id == action.id; }))
            data.actions.push_back(action);
        auto& slots = m_bindingSlots[command.id];
        if (m_defaults)
            for (const auto& source : m_defaults->GetDefinition().contexts)
                if (source.id == data.contexts[context].id)
                    for (const auto& binding : source.bindings)
                        if (binding.action == action.id)
                        {
                            auto& bindings = data.contexts[context].bindings;
                            if (std::none_of(bindings.begin(), bindings.end(), [&](const auto& value) { return value.id == binding.id; })) bindings.push_back(binding);
                            if (std::find(slots.begin(), slots.end(), binding.id) == slots.end()) slots.push_back(binding.id);
                        }
        // Dynamic commands without an asset mapping still expose one unbound
        // slot, so a user's stable-ID override can bind a script command.
        if (slots.empty())
        {
            Input::BindingDefinition binding;
            binding.id = BindingForCommand(command.id, 0); binding.action = action.id;
            data.contexts[context].bindings.push_back(binding); slots.push_back(binding.id);
        }
        const std::string id = command.id;
        const auto registration = m_commands.GetCommandGeneration(id);
        m_subscriptions.push_back(m_user->BindAction(action.id, Input::ActionPhase::Performed, [this, id, control, registration](const Input::ActionEvent& event)
        {
            if (m_commands.GetCommandGeneration(id) != registration) return;
            const auto* command = m_commands.FindCommand(id);
            if (!command || (control ? !m_router.AllowsControlCommands() : !m_router.AllowsScope(command->context))) return;
            if (m_router.GetRoute().widgetKeyboardActivation &&
                (event.control == 32 || event.control == 257 || event.control == 335))
            {
                const auto* device = m_system.FindDevice(event.device);
                if (device && device->descriptor.kind == Input::DeviceKind::Keyboard) return;
            }
            if (!control && command->context != ShortcutContexts::Global)
            {
                const auto context = std::find_if(m_contexts.begin(), m_contexts.end(), [&](const auto& item) { return item.id == command->context; });
                if (context == m_contexts.end() || !context->IsActive()) return;
            }
            if (m_router.GetRoute().textEditing)
            {
                // Rebinding a Play control to a printable letter does not grant
                // that letter a global text-editing exception.
                const auto* device = m_system.FindDevice(event.device);
                if (device && device->descriptor.kind == Input::DeviceKind::Keyboard &&
                    event.control >= 32 && event.control < 256 && (event.modifiers & 15u) == 0) return;
            }
            InvocationContext invocation;
            invocation.source = CommandSource::Shortcut; invocation.contentPanel = command->context;
            invocation.sourceSequence = event.sourceSequence;
            m_commands.TryExecuteCommand(id, invocation);
        }));
    }
    std::vector<std::string> diagnostics;
    auto compiled = Input::CompiledInput::Compile(std::move(data), diagnostics);
    if (!compiled) throw std::logic_error("Invalid editor input defaults: " + diagnostics.front());
    m_user->SetDefinition(compiled);
    m_navigationUser->SetDefinition(compiled);
    for (size_t i = 0; i < m_continuousHandles.size(); ++i)
        m_continuousHandles[i] = m_navigationUser->FindAction(ActionForCommand(std::string(EditorInputActionIds[i])));
    m_continuousStates = {};
    m_continuousFrame = m_continuousSequence = UINT64_MAX;
    m_preparedFrame = UINT64_MAX;
    m_navigationProfileRevision = 0;
    m_generation = m_commands.GetGeneration();
}

void EditorShortcutAdapter::Prepare(const std::vector<ShortcutContext>& contexts, EditorInputRoute route)
{
    std::erase_if(m_gamepads, [this](auto id)
    {
        const auto* device = m_system.FindDevice(id);
        if (device && device->connected) return false;
        m_user->UnpairDevice(id); m_navigationUser->UnpairDevice(id); return true;
    });
    for (const auto& device : m_system.GetDevices())
        if (device.connected && device.descriptor.kind == Input::DeviceKind::Gamepad && std::find(m_gamepads.begin(), m_gamepads.end(), device.id) == m_gamepads.end())
        { m_user->PairDevice(device.id); m_navigationUser->PairDevice(device.id); m_gamepads.push_back(device.id); }
    m_contexts = contexts;
    m_router.SetRoute(route);
    if (m_generation != m_commands.GetGeneration()) Rebuild(contexts);
    bool routingChanged = false;
    for (const auto& scope : m_scopes)
    {
        const bool continuous = scope == ShortcutContexts::SceneViewFlyMode ||
            std::any_of(ContinuousSettings().begin(), ContinuousSettings().end(), [&](const auto& setting) { return setting.context == scope; });
        // These passive snapshots never execute Commands. The viewport/canvas
        // authorizes capture using current UI ownership, including a press that
        // itself moves focus away from a text widget later in this UI frame.
        const bool active = continuous && route.windowFocused && !route.gameOwnsInput;
        routingChanged |= m_navigationUser->IsContextEnabled(ContextForScope(scope)) != active;
        if (active) m_navigationUser->EnableContext(ContextForScope(scope)); else m_navigationUser->DisableContext(ContextForScope(scope));
    }
    if (routingChanged) { m_continuousStates = {}; m_continuousFrame = UINT64_MAX; }
    m_preparedFrame = m_system.GetFrameIndex();
    RefreshContinuousInput(true, routingChanged);
}
void EditorShortcutAdapter::RefreshContinuousInput(bool force, bool checkRestoredPress)
{
    if (m_navigationProfileRevision != m_user->GetProfileRevision())
    {
        m_navigationUser->SetProfile(m_user->GetProfile());
        m_navigationProfileRevision = m_user->GetProfileRevision();
        m_continuousStates = {}; m_continuousFrame = UINT64_MAX;
        force = true;
    }
    const auto stream = m_system.GetEvents();
    const auto sequence = stream.empty() ? 0 : stream.back().sequence;
    const auto frame = m_system.GetFrameIndex();
    if (!force && frame == m_continuousFrame && sequence == m_continuousSequence) return;
    m_navigationUser->Update(m_system.Now());
    const auto definition = m_navigationUser->GetDefinition();
    for (size_t i = 0; i < m_continuousStates.size(); ++i)
    {
        auto state = m_navigationUser->GetActionState(m_continuousHandles[i]);
        if (checkRestoredPress && state.pressed && definition && m_continuousHandles[i])
        {
            const auto id = definition->GetDefinition().actions[m_continuousHandles[i].index].id;
            // ResumeImmediately may restore a held value when UI ownership
            // changes. Only a new source event can initiate a pointer capture.
            // Original presses remain cached for the late-UI focus handoff.
            const auto after = m_continuousSequence == UINT64_MAX ? 0 : m_continuousSequence;
            state.pressed = std::any_of(m_navigationUser->GetActionEvents().begin(), m_navigationUser->GetActionEvents().end(),
                [&](const auto& event)
                {
                    if (event.action != id || event.phase != Input::ActionPhase::Pressed || event.sourceSequence <= after) return false;
                    const auto origin = std::lower_bound(stream.begin(), stream.end(), event.sourceSequence,
                        [](const auto& raw, uint64_t sequence) { return raw.sequence < sequence; });
                    return origin != stream.end() && origin->sequence == event.sourceSequence;
                });
        }
        if (frame == m_continuousFrame && !state.canceled)
        {
            const auto& previous = m_continuousStates[i];
            state.pressed |= previous.pressed; state.released |= previous.released;
            state.performed |= previous.performed;
            if (definition && m_continuousHandles[i] &&
                definition->GetDefinition().actions[m_continuousHandles[i].index].nature == Input::ValueNature::Delta)
                state.value.vector += previous.value.vector;
        }
        m_continuousStates[i] = state;
    }
    m_continuousFrame = frame; m_continuousSequence = sequence;
}
Input::ActionSnapshot EditorShortcutAdapter::GetInputState(EditorInputAction action)
{
    const auto index = static_cast<size_t>(action);
    if (index >= m_continuousStates.size()) return {};
    // GetInputRoute runs before Prepare. A focus/ownership query must not
    // consume this frame's events through the previous frame's disabled route.
    if (m_preparedFrame != m_system.GetFrameIndex()) return {};
    RefreshContinuousInput();
    return m_continuousStates[index];
}
void EditorShortcutAdapter::Dispatch(EditorInputRoute route)
{
    m_router.SetRoute(route);
    m_user->SetDeviceBlocked(Input::DeviceKind::Mouse, route.widgetOwnsPointer);
    // The complete command stream is evaluated once after panels have reported
    // this frame's focus. Early navigation never consumes command edges.
    const auto& contexts = m_contexts;
    for (const auto& scope : m_scopes)
    {
        bool active = (scope == ControlScope || scope == GameControlScope) ? m_router.AllowsControlCommands() : m_router.AllowsScope(scope);
        if (scope == ShortcutContexts::SceneViewFlyMode)
            active = route.windowFocused && !route.gameOwnsInput && !route.textEditing && !route.modal && !route.menuActive;
        if (scope == GameControlScope) active &= route.gameOwnsInput;
        if (scope != ShortcutContexts::Global && scope != ControlScope && scope != GameControlScope)
        {
            const auto it = std::find_if(contexts.begin(), contexts.end(), [&](const auto& context) { return context.id == scope; });
            active &= it != contexts.end() && it->IsActive();
        }
        if (active) m_user->EnableContext(ContextForScope(scope)); else m_user->DisableContext(ContextForScope(scope));
    }
    m_user->Update(m_system.Now());
}
void EditorShortcutAdapter::Update(const std::vector<ShortcutContext>& contexts, EditorInputRoute route)
{ Prepare(contexts, route); Dispatch(route); }
}
