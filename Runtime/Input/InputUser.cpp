#include "InputUser.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace NLS::Input
{
struct InputSubscription::Slot
{
    ActionId action;
    ActionPhase phase;
    UpdateMode mode;
    std::function<void(const ActionEvent&)> callback;
    bool active = true;
};
InputSubscription::InputSubscription(std::shared_ptr<Slot> slot) : m_slot(std::move(slot)) {}
InputSubscription::~InputSubscription() { Reset(); }
InputSubscription::InputSubscription(InputSubscription&& other) noexcept = default;
InputSubscription& InputSubscription::operator=(InputSubscription&& other) noexcept
{
    if (this != &other) { Reset(); m_slot = std::move(other.m_slot); }
    return *this;
}
void InputSubscription::Reset() { if (m_slot) m_slot->active = false; m_slot.reset(); }

namespace
{
constexpr double Infinity = std::numeric_limits<double>::infinity();
bool HasFlag(InputEventFlags value, InputEventFlags flag)
{
    return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
}
struct InteractionState
{
    bool down = false;
    bool qualified = false;
    bool failed = false;
    uint32_t taps = 0;
    double started = 0;
    double lastRelease = -Infinity;
    double deadline = Infinity;
};
struct Transition
{
    bool started = false;
    bool released = false;
    bool performed = false;
    bool canceled = false;
    bool pulse = false;
};
Transition AdvanceInteraction(InteractionState& state, const Interaction& config, bool down, double now)
{
    Transition result;
    const bool entered = down && !state.down;
    const bool exited = !down && state.down;
    if (entered)
    {
        if (now - state.lastRelease > config.interval) state.taps = 0;
        state.started = now;
        state.qualified = false;
        state.failed = false;
        result.started = true;
        state.deadline = (config.kind == InteractionKind::Hold || config.kind == InteractionKind::Tap || config.kind == InteractionKind::MultiTap || config.kind == InteractionKind::Repeat)
            ? now + std::max(0.000001, config.duration) : Infinity;
        if (config.kind == InteractionKind::Press || config.kind == InteractionKind::Repeat) result.performed = state.qualified = true;
    }
    if (down && now >= state.deadline)
    {
        if (config.kind == InteractionKind::Hold) { result.performed = state.qualified = true; state.deadline = Infinity; }
        else if (config.kind == InteractionKind::Repeat) { result.performed = true; state.deadline += std::max(0.000001, config.interval); }
        else { result.canceled = state.failed = true; state.taps = 0; state.deadline = Infinity; }
    }
    if (exited)
    {
        result.released = true;
        if (config.kind == InteractionKind::Release) result.performed = result.pulse = true;
        if ((config.kind == InteractionKind::Tap || config.kind == InteractionKind::MultiTap) && !state.failed && now - state.started <= config.duration)
        {
            ++state.taps;
            if (config.kind == InteractionKind::Tap || state.taps >= config.count)
            {
                result.performed = result.pulse = true;
                state.taps = 0;
            }
        }
        else if (config.kind == InteractionKind::Hold && !state.qualified) result.canceled = true;
        state.lastRelease = now;
        state.qualified = false;
        state.deadline = Infinity;
    }
    state.down = down;
    return result;
}

bool MatchesPath(const ControlPath& a, const ControlPath& b)
{
    return a.device == b.device && a.kind == b.kind && a.control == b.control && a.code == b.code &&
        (!a.deviceId || !b.deviceId || a.deviceId == b.deviceId);
}
}

struct InputUser::Impl
{
    InputSystem& system;
    uint64_t id;
    std::shared_ptr<const CompiledInput> definition;
    uint32_t generation = 0;
    struct ContextState { bool pushed = false; bool enabled = true; int offset = 0; uint64_t order = 0; };
    struct BindingState
    {
        const BindingDefinition* definition = nullptr;
        size_t context = 0;
        size_t action = 0;
        std::vector<ControlPart> parts;
        Chord chord;
        InteractionState interaction;
        InputValue value;
        InputValue lastActuatedValue;
        InputEvent activation;
        bool requireRelease = false;
        bool pulse = false;
        bool consumed = false;
        bool blocked = false;
        bool actuatedSource = false;
    };
    struct ActionState { ActionSnapshot snapshot; InteractionState interaction; size_t context = SIZE_MAX; InputEvent activation; };
    std::vector<DeviceId> pairs;
    std::array<bool, 3> blockedDevices{};
    std::vector<DeviceState> devices;
    std::vector<ContextState> contexts;
    std::vector<BindingState> bindings;
    std::vector<ActionState> actions;
    std::vector<size_t> contextOrder;
    std::vector<size_t> winners;
    std::vector<std::vector<size_t>> contextBindings;
    std::vector<std::vector<size_t>> contextActions;
    std::vector<std::vector<size_t>> actionBindings;
    std::vector<ControlPath> consumed;
    std::vector<ActionEvent> events;
    std::vector<ActionEvent> pending;
    std::vector<ActionEvent> dynamicPending;
    FixedInputFrame fixed;
    std::vector<std::shared_ptr<InputSubscription::Slot>> subscriptions;
    std::vector<std::function<void()>> mutations;
    InputProfile profile;
    uint64_t profileRevision = 1;
    InputEvent source;
    uint64_t checkpoint = 0;
    uint64_t eventSequence = 0;
    uint64_t epoch = 1;
    uint64_t nextTick = 1;
    uint64_t completedTick = 0;
    uint64_t contextSequence = 0;
    size_t deliveredEvents = 0;
    double realTime = 0;
    double activeTime = 0;
    double evaluatedTime = 0;
    double realTimeOffset = 0;
    bool suspended = false;
    uint32_t rebindingLocks = 0;
    bool Suspended() const { return suspended || rebindingLocks != 0; }
    bool enabled = true;
    bool bufferFixed = true;
    bool updating = false;
    bool dispatching = false;
    bool advancingTimers = false;
    Modifier primary =
#ifdef __APPLE__
        Modifier::Super;
#else
        Modifier::Control;
#endif

    Impl(InputSystem& system, uint64_t id) : system(system), id(id)
    {
        pairs = {system.GetKeyboard(), system.GetMouse()};
        devices.assign(system.GetDevices().begin(), system.GetDevices().end());
        checkpoint = system.GetLastSequence();
        realTime = system.Now();
        events.reserve(128); pending.reserve(256); dynamicPending.reserve(256); consumed.reserve(64); subscriptions.reserve(32); mutations.reserve(8);
    }
    const InputDefinition& Data() const { return definition->GetDefinition(); }
    bool Paired(DeviceId device) const { return std::find(pairs.begin(), pairs.end(), device) != pairs.end(); }
    void Mutate(std::function<void()> mutation)
    {
        if (updating || dispatching) mutations.push_back(std::move(mutation));
        else mutation();
    }
    void ApplyMutations()
    {
        if (mutations.empty()) return;
        auto changes = std::move(mutations);
        mutations.clear();
        struct RecycleCapacity
        {
            decltype(mutations)& pending;
            decltype(mutations)& executing;
            ~RecycleCapacity()
            {
                executing.clear();
                if (pending.empty() && pending.capacity() < executing.capacity()) pending.swap(executing);
            }
        } recycle{mutations, changes};
        for (auto& change : changes) change();
    }
    InputValue Read(const ControlPath& path) const
    {
        InputValue result;
        for (const auto& device : devices)
        {
            if (!device.connected || !Paired(device.id) || device.descriptor.kind != path.device || (path.deviceId && path.deviceId != device.id)) continue;
            InputValue value;
            switch (path.kind)
            {
            case ControlKind::Button: if (path.control < device.buttons.size()) value.vector.x = device.buttons[path.control] ? 1.0f : 0.0f; break;
            case ControlKind::Axis:
                if (path.control < device.axes.size()) value.vector.x = device.axes[path.control] * (device.descriptor.kind == DeviceKind::Gamepad ? profile.gamepadSensitivity : 1.0f);
                break;
            case ControlKind::PointerPosition: value = {device.position.x, device.position.y}; break;
            case ControlKind::PointerDelta: value = {device.delta.x * profile.pointerSensitivity, device.delta.y * profile.pointerSensitivity}; break;
            case ControlKind::Wheel: value = {device.wheel.x, device.wheel.y}; break;
            case ControlKind::ScanCode:
            case ControlKind::LogicalKey:
                // Identity tables are populated from normalized key records.
                for (const auto& key : keyIdentities)
                    if (key.device == device.id && key.down && (path.kind == ControlKind::ScanCode ? key.scan == path.code : key.logical == path.code)) value.vector.x = 1;
                break;
            }
            if (value.MagnitudeSquared() > result.MagnitudeSquared()) result = value;
        }
        return result;
    }
    struct KeyIdentity { DeviceId device; uint16_t control; uint32_t scan; uint32_t logical; bool down; };
    std::vector<KeyIdentity> keyIdentities;
    uint8_t Modifiers() const
    {
        auto held = [this](uint16_t code) { return Read({DeviceKind::Keyboard, ControlKind::Button, code}).AsButton(); };
        return ((held(340) || held(344)) ? 1u : 0u) | ((held(341) || held(345)) ? 2u : 0u) |
            ((held(342) || held(346)) ? 4u : 0u) | ((held(343) || held(347)) ? 8u : 0u);
    }
    bool ChordMatches(const Chord& chord) const
    {
        for (const auto& control : chord.controls) if (!Read(control).AsButton()) return false;
        if (!chord.matchModifiers) return true;
        auto required = static_cast<uint8_t>(chord.modifiers);
        if (required & 16u) required = (required & ~16u) | static_cast<uint8_t>(primary);
        const auto actual = Modifiers();
        auto forbidden = static_cast<uint8_t>(chord.forbiddenModifiers);
        if (forbidden & 16u) forbidden = (forbidden & ~16u) | static_cast<uint8_t>(primary);
        if ((actual & forbidden) != 0) return false;
        return chord.modifierMatch == ModifierMatch::Exact ? actual == required : (actual & required) == required;
    }
    InputValue CompositeValue(const BindingState& binding) const
    {
        InputValue value;
        if (binding.definition->composite == Composite::Direct)
        {
            if (!binding.parts.empty())
            {
                value = Read(binding.parts.front().path);
                const auto& scale = binding.parts.front().contribution.vector;
                value.vector.x *= scale.x;
                // Direct vector controls retain their other components by default.
                if (binding.parts.front().path.kind == ControlKind::PointerDelta || binding.parts.front().path.kind == ControlKind::PointerPosition || binding.parts.front().path.kind == ControlKind::Wheel)
                {
                    value.vector.y *= scale.y == 0 ? scale.x : scale.y;
                    value.vector.z *= scale.z == 0 ? scale.x : scale.z;
                }
            }
        }
        else
            for (const auto& part : binding.parts) value.vector += part.contribution.vector * Read(part.path).vector.x;
        return value;
    }
    void Emit(size_t index, ActionPhase phase, InputValue value, CancelReason reason = CancelReason::None)
    {
        auto origin = source;
        if (advancingTimers)
        {
            origin = actions[index].activation;
            for (const auto binding : actionBindings[index])
                if (bindings[binding].value.MagnitudeSquared() > 0 && bindings[binding].activation.sequence > origin.sequence)
                    origin = bindings[binding].activation;
        }
        if (phase == ActionPhase::Started) actions[index].activation = origin;
        ActionEvent event;
        event.sequence = ++eventSequence;
        event.sourceSequence = origin.sequence;
        event.epoch = epoch;
        event.tick = nextTick;
        event.timestamp = evaluatedTime;
        event.realTimestamp = evaluatedTime + realTimeOffset;
        event.action = Data().actions[index].id;
        event.phase = phase;
        event.value = value;
        event.device = origin.device;
        event.window = origin.window;
        event.control = origin.control;
        event.modifiers = origin.modifiers;
        event.reason = reason;
        events.push_back(event);
        if (bufferFixed) pending.push_back(event);
        dynamicPending.push_back(event);
    }
    void CancelAction(size_t index, CancelReason reason)
    {
        auto& action = actions[index];
        if (action.snapshot.actuated || action.interaction.down)
        {
            Emit(index, ActionPhase::Canceled, {}, reason);
            action.snapshot.canceled = true;
        }
        action.snapshot.value = {};
        action.snapshot.actuated = false;
        action.interaction = {};
        action.context = SIZE_MAX;
    }
    void CancelAll(CancelReason reason, Reacquire reacquire)
    {
        ++epoch;
        pending.clear();
        // A second boundary (reload followed by profile application, for
        // example) must not erase an undispatched lifecycle cancellation.
        std::erase_if(dynamicPending, [](const auto& event) { return event.phase != ActionPhase::Canceled; });
        for (auto& event : dynamicPending)
        {
            event.epoch = epoch;
            for (auto& record : events) if (record.sequence == event.sequence) { record.epoch = epoch; break; }
        }
        fixed.events.clear();
        for (size_t i = 0; i < actions.size(); ++i) CancelAction(i, reason);
        // Lifecycle cancellation is observable dynamically; it is never a
        // gameplay command queued into a later Play/Pause epoch.
        pending.clear();
        for (auto& binding : bindings)
        {
            binding.interaction = {};
            binding.value = {};
            binding.requireRelease = reacquire == Reacquire::RequireRelease && CompositeValue(binding).MagnitudeSquared() > 0;
        }
    }
    void Rebuild(std::shared_ptr<const CompiledInput> value)
    {
        std::vector<std::pair<ContextId, ContextState>> previousContexts;
        if (definition)
            for (size_t i = 0; i < contexts.size(); ++i) previousContexts.emplace_back(Data().contexts[i].id, contexts[i]);
        if (definition) CancelAll(CancelReason::DefinitionChanged, Reacquire::RequireRelease);
        definition = std::move(value);
        ++generation;
        contexts.clear(); bindings.clear(); actions.clear(); contextOrder.clear(); winners.clear();
        contextBindings.clear(); contextActions.clear(); actionBindings.clear();
        if (!definition) return;
        actions.resize(Data().actions.size()); winners.resize(actions.size(), SIZE_MAX); actionBindings.resize(actions.size());
        contexts.resize(Data().contexts.size());
        contextBindings.resize(contexts.size()); contextActions.resize(contexts.size());
        for (size_t c = 0; c < contexts.size(); ++c)
        {
            for (const auto& previous : previousContexts) if (previous.first == Data().contexts[c].id) contexts[c] = previous.second;
            for (const auto& binding : Data().contexts[c].bindings)
            {
                BindingState state;
                state.definition = &binding;
                state.context = c;
                state.action = definition->FindAction(binding.action);
                state.parts = binding.parts;
                contextBindings[c].push_back(bindings.size());
                actionBindings[state.action].push_back(bindings.size());
                if (std::find(contextActions[c].begin(), contextActions[c].end(), state.action) == contextActions[c].end()) contextActions[c].push_back(state.action);
                bindings.push_back(std::move(state));
            }
        }
        ApplyProfile();
    }
    void ApplyProfile()
    {
        for (auto& binding : bindings)
        {
            binding.parts = binding.definition->parts;
            binding.chord = binding.definition->chord;
            for (const auto& entry : profile.overrides)
                if (entry.binding == binding.definition->id && definition->IsOverrideCompatible(entry))
                {
                    if (entry.state == OverrideState::ExplicitlyUnbound) binding.parts.clear();
                    if (entry.state == OverrideState::Override)
                    {
                        binding.parts = entry.parts;
                        if (entry.chord) binding.chord = *entry.chord;
                    }
                    break;
                }
            binding.requireRelease = CompositeValue(binding).MagnitudeSquared() > 0;
        }
    }
    bool Active(size_t context) const { return contexts[context].pushed && contexts[context].enabled; }
    void SortContexts()
    {
        contextOrder.clear();
        for (size_t i = 0; i < contexts.size(); ++i) if (Active(i)) contextOrder.push_back(i);
        std::sort(contextOrder.begin(), contextOrder.end(), [this](size_t a, size_t b)
        {
            const int64_t pa = int64_t{Data().contexts[a].priority} + contexts[a].offset;
            const int64_t pb = int64_t{Data().contexts[b].priority} + contexts[b].offset;
            return pa != pb ? pa > pb : contexts[a].order > contexts[b].order;
        });
    }
    bool IsConsumed(const BindingState& binding) const
    {
        for (const auto& part : binding.parts)
            for (const auto& used : consumed) if (MatchesPath(part.path, used)) return true;
        return false;
    }
    void Commit(size_t index, InputValue value, size_t context, bool interrupted)
    {
        auto& state = actions[index];
        const auto& config = Data().actions[index];
        if (state.context != SIZE_MAX && state.context != context && state.snapshot.actuated) CancelAction(index, CancelReason::RoutingChanged);
        if (interrupted && state.snapshot.actuated) { CancelAction(index, CancelReason::RoutingChanged); return; }
        state.context = context;
        const auto oldValue = state.snapshot.value;
        const bool down = value.AsButton(state.snapshot.actuated ? config.releaseThreshold : config.pressThreshold);
        const auto transition = AdvanceInteraction(state.interaction, config.interaction, down, evaluatedTime);
        state.snapshot.value = value;
        state.snapshot.actuated = down;
        if (transition.started)
        {
            state.snapshot.pressed = true;
            Emit(index, ActionPhase::Started, value);
            Emit(index, ActionPhase::Pressed, value);
        }
        if (transition.released) { state.snapshot.released = true; Emit(index, ActionPhase::Released, value); }
        if (transition.performed) { state.snapshot.performed = true; Emit(index, ActionPhase::Performed, value); }
        if (transition.canceled) { state.snapshot.canceled = true; Emit(index, ActionPhase::Canceled, value); }
        if (!(value == oldValue))
        {
            auto eventValue = value;
            if (config.nature == ValueNature::Delta) eventValue.vector -= oldValue.vector;
            Emit(index, ActionPhase::ValueChanged, eventValue);
        }
    }
    void Evaluate(bool closePulses = false)
    {
        if (!definition || Suspended() || !enabled) return;
        SortContexts();
        std::fill(winners.begin(), winners.end(), SIZE_MAX);
        for (size_t c : contextOrder)
            for (const size_t index : contextBindings[c])
                if (winners[bindings[index].action] == SIZE_MAX) winners[bindings[index].action] = c;
        for (auto& binding : bindings)
            if (!Active(binding.context) || winners[binding.action] != binding.context)
            {
                binding.interaction = {};
                binding.value = {};
                binding.consumed = false;
            }
        consumed.clear();
        bool pulses = false;
        for (size_t c : contextOrder)
        {
            const auto& context = Data().contexts[c];
            const size_t before = events.size();
            for (size_t index : contextBindings[c])
            {
                auto& binding = bindings[index];
                if (winners[binding.action] != c) continue;
                const auto& config = Data().actions[binding.action];
                auto raw = CompositeValue(binding);
                binding.blocked = false;
                const bool wasActuated = binding.actuatedSource;
                binding.actuatedSource = ChordMatches(binding.chord) && !IsConsumed(binding) &&
                    ProcessValue(raw, binding.definition->processors, config.type).AsButton(config.pressThreshold);
                if (binding.actuatedSource && !wasActuated) binding.activation = source;
                if (binding.requireRelease)
                {
                    if (raw.MagnitudeSquared() == 0) binding.requireRelease = false;
                    else { binding.value = {}; binding.interaction = {}; binding.blocked = true; continue; }
                }
                raw = ProcessValue(raw, binding.definition->processors, config.type);
                if (raw.MagnitudeSquared() > 0) binding.lastActuatedValue = raw;
                bool routed = true;
                for (const auto& part : binding.parts) routed &= !blockedDevices[static_cast<size_t>(part.path.device)];
                for (const auto& control : binding.chord.controls) routed &= !blockedDevices[static_cast<size_t>(control.device)];
                const bool gate = routed && !IsConsumed(binding) && ChordMatches(binding.chord);
                if (!gate)
                {
                    binding.value = {}; binding.interaction = {}; binding.blocked = raw.MagnitudeSquared() > 0;
                    binding.requireRelease = binding.blocked && context.reacquire == Reacquire::RequireRelease;
                    continue;
                }
                binding.pulse = false;
                if (binding.definition->hasInteraction)
                {
                    auto transition = AdvanceInteraction(binding.interaction, binding.definition->interaction, raw.AsButton(config.pressThreshold), evaluatedTime);
                    const auto kind = binding.definition->interaction.kind;
                    bool qualifies = binding.interaction.qualified;
                    if (kind == InteractionKind::Press || kind == InteractionKind::Repeat) qualifies = binding.interaction.down;
                    if (kind == InteractionKind::Release || kind == InteractionKind::Tap || kind == InteractionKind::MultiTap) qualifies = false;
                    if (transition.pulse && !closePulses) { raw = binding.lastActuatedValue; qualifies = binding.pulse = pulses = true; }
                    if (!qualifies) raw = {};
                }
                binding.value = raw;
            }
            for (size_t a : contextActions[c])
            {
                if (winners[a] != c) continue;
                InputValue aggregate;
                bool blocked = false;
                for (size_t index : actionBindings[a])
                    if (const auto& binding = bindings[index]; binding.context == c)
                    {
                        blocked |= binding.blocked;
                        if (Data().actions[a].aggregation == Aggregation::Sum) aggregate.vector += binding.value.vector;
                        else if (binding.value.MagnitudeSquared() > aggregate.MagnitudeSquared()) aggregate = binding.value;
                    }
                aggregate = ProcessValue(aggregate, Data().actions[a].processors, Data().actions[a].type);
                const bool chord = ChordMatches(Data().actions[a].chord);
                const bool interrupted = aggregate.MagnitudeSquared() == 0 && blocked;
                if (!chord)
                {
                    aggregate = {};
                    if (context.reacquire == Reacquire::RequireRelease)
                        for (size_t index : actionBindings[a])
                            if (bindings[index].context == c && CompositeValue(bindings[index]).MagnitudeSquared() > 0) bindings[index].requireRelease = true;
                }
                Commit(a, aggregate, c, interrupted || (!chord && actions[a].snapshot.actuated));
            }
            if (context.consumption == Consumption::Never) continue;
            for (size_t index : contextBindings[c])
            {
                auto& binding = bindings[index];
                if (winners[binding.action] != c) continue;
                const bool actuated = binding.actuatedSource || (binding.value.MagnitudeSquared() > 0 && actions[binding.action].snapshot.actuated);
                if (!actuated) binding.consumed = false;
                bool started = false, performed = false;
                if (context.consumption == Consumption::OnStarted || context.consumption == Consumption::OnPerformed)
                    for (size_t i = before; i < events.size(); ++i)
                        if (events[i].action == binding.definition->action)
                        {
                            started |= events[i].phase == ActionPhase::Started;
                            performed |= events[i].phase == ActionPhase::Performed;
                        }
                if ((context.consumption == Consumption::OnStarted && started) || (context.consumption == Consumption::OnPerformed && performed)) binding.consumed = true;
                if ((context.consumption == Consumption::OnActuated && actuated) || (binding.consumed && context.consumption != Consumption::Never))
                    for (const auto& part : binding.parts) consumed.push_back(part.path);
            }
        }
        for (size_t a = 0; a < actions.size(); ++a)
            if (winners[a] == SIZE_MAX && actions[a].snapshot.actuated) CancelAction(a, CancelReason::ContextDisabled);
        if (pulses) Evaluate(true);
    }
    void AdvanceTimers(double time)
    {
        if (!definition || Suspended() || !enabled) { evaluatedTime = time; return; }
        advancingTimers = true;
        struct TimerScope { bool& value; ~TimerScope() { value = false; } } scope{advancingTimers};
        for (;;)
        {
            double deadline = Infinity;
            for (const auto& action : actions) deadline = std::min(deadline, action.interaction.deadline);
            for (const auto& binding : bindings) deadline = std::min(deadline, binding.interaction.deadline);
            if (deadline > time || !std::isfinite(deadline)) break;
            evaluatedTime = std::max(evaluatedTime, deadline);
            Evaluate();
        }
        evaluatedTime = time;
    }
    void ApplyEvent(const InputEvent& event)
    {
        if (!event.device || !Paired(event.device)) return;
        auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) { return device.id == event.device; });
        if (it == devices.end())
        {
            const auto* actual = system.FindDevice(event.device);
            if (!actual) return;
            devices.emplace_back();
            it = std::prev(devices.end());
            it->id = event.device;
            it->descriptor = actual->descriptor;
            it->connected = true;
        }
        auto& device = *it;
        switch (event.type)
        {
        case InputEventType::ButtonDown:
        case InputEventType::ButtonUp:
            if (HasFlag(event.flags, InputEventFlags::FocusReset)) break;
            device.buttons[event.control] = event.type == InputEventType::ButtonDown;
            if (device.descriptor.kind == DeviceKind::Keyboard)
            {
                auto key = std::find_if(keyIdentities.begin(), keyIdentities.end(), [&](const auto& value) { return value.device == event.device && value.control == event.control; });
                if (key == keyIdentities.end()) { keyIdentities.push_back({event.device, event.control}); key = std::prev(keyIdentities.end()); }
                key->scan = static_cast<uint32_t>(event.scanCode); key->logical = event.logicalKey; key->down = event.type == InputEventType::ButtonDown;
            }
            break;
        case InputEventType::Axis: device.axes[event.control] = static_cast<float>(event.x); break;
        case InputEventType::PointerDelta: device.delta += Maths::Vector2(static_cast<float>(event.x), static_cast<float>(event.y)); break;
        case InputEventType::PointerMove:
        {
            const Maths::Vector2 position(static_cast<float>(event.x), static_cast<float>(event.y));
            const Maths::Vector2 screen(static_cast<float>(event.screenX), static_cast<float>(event.screenY));
            if (device.hasPointerPosition && !HasFlag(event.flags, InputEventFlags::PointerWarp) && !HasFlag(event.flags, InputEventFlags::Resynchronize))
                device.delta += HasFlag(event.flags, InputEventFlags::ScreenPosition) ? screen - device.screenPosition : position - device.position;
            device.position = position; device.screenPosition = screen; device.hasPointerPosition = true;
            break;
        }
        case InputEventType::Wheel: device.wheel += Maths::Vector2(static_cast<float>(event.x), static_cast<float>(event.y)); break;
        case InputEventType::DeviceReset:
        case InputEventType::DeviceRemoved:
            device.buttons.fill(false); device.axes.fill(0); device.delta = {}; device.wheel = {};
            for (auto& key : keyIdentities) if (key.device == device.id) key.down = false;
            if (event.type == InputEventType::DeviceRemoved) device.connected = false;
            for (size_t a = 0; a < actions.size(); ++a)
                if (actions[a].snapshot.actuated)
                    for (const auto& binding : bindings)
                        if (binding.action == a && std::any_of(binding.parts.begin(), binding.parts.end(), [&](const auto& part) { return part.path.device == device.descriptor.kind; }))
                        { CancelAction(a, event.type == InputEventType::DeviceRemoved ? CancelReason::DeviceLost : CancelReason::FocusLost); break; }
            break;
        default: break;
        }
    }
    void Dispatch(std::span<const ActionEvent> records, UpdateMode mode)
    {
        dispatching = true;
        struct DispatchScope { bool& value; ~DispatchScope() { value = false; } } scope{dispatching};
        const size_t count = subscriptions.size();
        for (const auto& event : records)
            for (size_t i = 0; i < count; ++i)
            {
                const auto* candidate = subscriptions[i].get();
                if (candidate->active && candidate->mode == mode && candidate->action == event.action && candidate->phase == event.phase)
                {
                    // Only the invoked callback needs a retained owner. Most
                    // records do not match this slot; avoid atomic refcounts
                    // for every nonmatching event/subscription pair.
                    auto slot = subscriptions[i];
                    slot->callback(event);
                }
            }
        dispatching = false;
        std::erase_if(subscriptions, [](const auto& slot) { return !slot->active; });
    }
};

InputUser::InputUser(InputSystem& system, uint64_t id) : m_impl(std::make_unique<Impl>(system, id)) {}
InputUser::~InputUser() { for (const auto& slot : m_impl->subscriptions) slot->active = false; }
uint64_t InputUser::GetId() const { return m_impl->id; }
void InputUser::SetDefinition(std::shared_ptr<const CompiledInput> definition)
{
    m_impl->Mutate([this, definition = std::move(definition)] { m_impl->Rebuild(definition); });
}
std::shared_ptr<const CompiledInput> InputUser::GetDefinition() const { return m_impl->definition; }
ActionHandle InputUser::FindAction(std::string_view nameOrPath) const
{
    const auto definition = GetDefinition();
    const auto index = definition ? definition->FindAction(nameOrPath) : SIZE_MAX;
    return index == SIZE_MAX ? ActionHandle{} : FindAction(definition->GetDefinition().actions[index].id);
}
ActionHandle InputUser::FindAction(ActionId id) const
{
    auto& impl = *m_impl;
    if (!impl.definition) return {};
    const auto index = impl.definition->FindAction(id);
    return index == SIZE_MAX ? ActionHandle{} : ActionHandle{static_cast<uint32_t>(index), impl.generation};
}
ActionSnapshot InputUser::GetActionState(ActionHandle handle) const
{
    const auto& impl = *m_impl;
    return handle.generation == impl.generation && handle.index < impl.actions.size() ? impl.actions[handle.index].snapshot : ActionSnapshot{};
}
InputValue InputUser::GetActionValue(ActionHandle action) const { return GetActionState(action).value; }
bool InputUser::WasActionPressed(ActionHandle action) const { return GetActionState(action).pressed; }
bool InputUser::WasActionReleased(ActionHandle action) const { return GetActionState(action).released; }
std::span<const ActionEvent> InputUser::GetActionEvents() const { return m_impl->events; }
InputSubscription InputUser::BindAction(std::string_view nameOrPath, ActionPhase phase, std::function<void(const ActionEvent&)> callback, UpdateMode mode)
{
    const auto handle = FindAction(nameOrPath);
    if (!handle) return {};
    return BindAction(m_impl->Data().actions[handle.index].id, phase, std::move(callback), mode);
}
InputSubscription InputUser::BindAction(ActionId action, ActionPhase phase, std::function<void(const ActionEvent&)> callback, UpdateMode mode)
{
    if (!callback) return {};
    auto slot = std::make_shared<InputSubscription::Slot>();
    slot->action = action; slot->phase = phase; slot->mode = mode; slot->callback = std::move(callback);
    m_impl->subscriptions.push_back(slot);
    return InputSubscription(std::move(slot));
}
bool InputUser::PushContext(ContextId id, int offset)
{
    auto& impl = *m_impl;
    const auto index = impl.definition ? impl.definition->FindContext(id) : SIZE_MAX;
    if (index == SIZE_MAX) return false;
    impl.Mutate([this, id, offset]
    {
        auto& p = *m_impl;
        const size_t index = p.definition->FindContext(id);
        if (index == SIZE_MAX) return;
        auto& context = p.contexts[index];
        context.pushed = context.enabled = true; context.offset = offset; context.order = ++p.contextSequence;
        for (auto& binding : p.bindings) if (binding.context == index)
            binding.requireRelease = p.Data().contexts[index].reacquire == Reacquire::RequireRelease && p.CompositeValue(binding).MagnitudeSquared() > 0;
    });
    return true;
}
bool InputUser::PopContext(ContextId id)
{
    if (!DisableContext(id)) return false;
    m_impl->Mutate([this, id] { auto& p = *m_impl; const auto index = p.definition->FindContext(id); if (index != SIZE_MAX) p.contexts[index].pushed = false; });
    return true;
}
bool InputUser::EnableContext(ContextId id)
{
    auto& p = *m_impl;
    const auto index = p.definition ? p.definition->FindContext(id) : SIZE_MAX;
    if (index == SIZE_MAX) return false;
    if (p.Active(index)) return true;
    return PushContext(id, p.contexts[index].offset);
}
bool InputUser::DisableContext(ContextId id)
{
    auto& p = *m_impl;
    if (!p.definition || p.definition->FindContext(id) == SIZE_MAX) return false;
    p.Mutate([this, id]
    {
        auto& p = *m_impl;
        const auto index = p.definition->FindContext(id);
        if (index == SIZE_MAX) return;
        p.contexts[index].enabled = false;
        for (size_t a = 0; a < p.actions.size(); ++a) if (p.actions[a].context == index) p.CancelAction(a, CancelReason::ContextDisabled);
        for (auto& b : p.bindings) if (b.context == index) { b.interaction = {}; b.value = {}; }
    });
    return true;
}
bool InputUser::IsContextEnabled(ContextId id) const
{
    auto& p = *m_impl; const auto index = p.definition ? p.definition->FindContext(id) : SIZE_MAX;
    return index != SIZE_MAX && p.Active(index);
}
void InputUser::PairDevice(DeviceId device)
{
    m_impl->Mutate([this, device]
    {
        auto& p = *m_impl;
        if (p.Paired(device) || !p.system.FindDevice(device)) return;
        p.pairs.push_back(device);
        const auto* state = p.system.FindDevice(device);
        auto mirror = std::find_if(p.devices.begin(), p.devices.end(), [device](const auto& value) { return value.id == device; });
        if (mirror == p.devices.end()) p.devices.push_back(*state);
        else *mirror = *state;
        for (auto& b : p.bindings) b.requireRelease = p.CompositeValue(b).MagnitudeSquared() > 0;
    });
}
void InputUser::UnpairDevice(DeviceId device)
{
    m_impl->Mutate([this, device]
    {
        auto& p = *m_impl;
        if (!p.Paired(device)) return;
        p.CancelAll(CancelReason::DeviceLost, Reacquire::RequireRelease);
        std::erase(p.pairs, device);
        std::erase_if(p.devices, [device](const auto& value) { return value.id == device; });
        std::erase_if(p.keyIdentities, [device](const auto& value) { return value.device == device; });
    });
}
bool InputUser::IsDevicePaired(DeviceId device) const { return m_impl->Paired(device); }
void InputUser::SetDeviceBlocked(DeviceKind kind, bool blocked)
{
    if (static_cast<size_t>(kind) >= 3) throw std::invalid_argument("Unknown input device kind");
    m_impl->Mutate([this, kind, blocked] { m_impl->blockedDevices[static_cast<size_t>(kind)] = blocked; });
}
void InputUser::SetPrimaryModifier(Modifier modifier)
{
    if (modifier != Modifier::Control && modifier != Modifier::Super) throw std::invalid_argument("Primary must resolve to Control or Super");
    m_impl->Mutate([this, modifier] { m_impl->primary = modifier; });
}
void InputUser::SetProfile(InputProfile profile)
{
    if (profile.version != 1 || !std::isfinite(profile.pointerSensitivity) || profile.pointerSensitivity <= 0 || !std::isfinite(profile.gamepadSensitivity) || profile.gamepadSensitivity <= 0)
        throw std::invalid_argument("Invalid input profile");
    m_impl->Mutate([this, profile = std::move(profile)]
    { auto& p = *m_impl; p.CancelAll(CancelReason::Rebinding, Reacquire::RequireRelease); p.profile = profile; ++p.profileRevision; p.ApplyProfile(); });
}
InputProfile InputUser::GetProfile() const { return m_impl->profile; }
uint64_t InputUser::GetProfileRevision() const { return m_impl->profileRevision; }
void InputUser::SetBindingOverride(BindingOverride value)
{
    m_impl->Mutate([this, value = std::move(value)]
    {
        auto& p = *m_impl;
        p.CancelAll(CancelReason::Rebinding, Reacquire::RequireRelease);
        std::erase_if(p.profile.unresolved, [&](const auto& item) { return item.binding == value.binding; });
        auto it = std::find_if(p.profile.overrides.begin(), p.profile.overrides.end(), [&](const auto& item) { return item.binding == value.binding; });
        if (it == p.profile.overrides.end()) p.profile.overrides.push_back(value); else *it = value;
        ++p.profileRevision;
        p.ApplyProfile();
    });
}
void InputUser::RestoreBindingDefault(BindingId binding) { SetBindingOverride({binding, OverrideState::Default}); }
void InputUser::ClearBinding(BindingId binding) { SetBindingOverride({binding, OverrideState::ExplicitlyUnbound}); }
std::span<const ControlPart> InputUser::GetEffectiveBinding(BindingId id) const
{
    for (const auto& binding : m_impl->bindings) if (binding.definition->id == id) return binding.parts;
    return {};
}
std::vector<BindingId> InputUser::GetUnappliedOverrides() const
{
    std::vector<BindingId> result;
    for (const auto& entry : m_impl->profile.overrides)
        if (!m_impl->definition || !m_impl->definition->IsOverrideCompatible(entry)) result.push_back(entry.binding);
    for (const auto& entry : m_impl->profile.unresolved) result.push_back(entry.binding);
    return result;
}
Chord InputUser::GetEffectiveChord(BindingId id) const
{
    for (const auto& binding : m_impl->bindings) if (binding.definition->id == id) return binding.chord;
    return {};
}
void InputUser::Update(double realTime, bool dispatchDynamic)
{
    auto& p = *m_impl;
    if (p.updating || p.dispatching) throw std::logic_error("Input update cannot be reentered");
    if (!std::isfinite(realTime) || realTime < p.realTime) throw std::invalid_argument("Input time must be finite and monotonic");
    if (p.deliveredEvents)
    {
        p.events.erase(p.events.begin(), p.events.begin() + static_cast<std::ptrdiff_t>(p.deliveredEvents));
        p.deliveredEvents = 0;
    }
    p.ApplyMutations();
    for (size_t i = 0; i < p.actions.size(); ++i)
    {
        auto& action = p.actions[i];
        action.snapshot.pressed = action.snapshot.released = action.snapshot.performed = action.snapshot.canceled = false;
        if (p.Data().actions[i].nature == ValueNature::Delta) action.snapshot.value = {};
    }
    for (const auto& event : p.events)
        if (p.definition)
        {
            const auto index = p.definition->FindAction(event.action);
            if (index < p.actions.size()) p.actions[index].snapshot.canceled |= event.phase == ActionPhase::Canceled;
        }
    p.updating = true;
    const double oldActive = p.activeTime;
    p.realTimeOffset = p.realTime - oldActive;
    if (!p.Suspended() && p.enabled) p.activeTime += realTime - p.realTime;
    for (auto& device : p.devices) { device.delta = {}; device.wheel = {}; }
    p.evaluatedTime = oldActive;
    // Transient controls return to neutral before this update's ordered events.
    // A RequireRelease latch from the previous update must not swallow fresh
    // movement, and the previous Delta cannot remain actuated indefinitely.
    p.Evaluate();
    for (const auto& event : p.system.GetEvents())
    {
        if (event.sequence <= p.checkpoint) continue;
        p.checkpoint = event.sequence;
        p.AdvanceTimers((p.Suspended() || !p.enabled) ? oldActive : std::clamp(oldActive + event.timestamp - p.realTime, oldActive, p.activeTime));
        p.source = event;
        p.ApplyEvent(event);
        p.Evaluate();
    }
    p.AdvanceTimers(p.activeTime);
    p.Evaluate();
    p.realTime = realTime;
    p.updating = false;
    p.deliveredEvents = p.events.size();
    if (dispatchDynamic) DispatchDynamicEvents();
    // Events remain queryable until the next Update, independently of EndFrame.
    // Move them to the start-of-update cleanup below rather than clearing here.
    p.deliveredEvents = p.events.size();
}
void InputUser::DispatchDynamicEvents()
{
    auto& p = *m_impl;
    if (p.dispatching || p.updating) throw std::logic_error("Input callback dispatch cannot be reentered");
    do
    {
        try { p.Dispatch(p.dynamicPending, UpdateMode::Dynamic); }
        catch (...) { p.dynamicPending.clear(); throw; }
        p.dynamicPending.clear();
        p.ApplyMutations();
    } while (!p.dynamicPending.empty());
}
void InputUser::Cancel(CancelReason reason, Reacquire reacquire)
{
    m_impl->Mutate([this, reason, reacquire] { m_impl->CancelAll(reason, reacquire); });
}
void InputUser::SetSuspended(bool suspended, CancelReason reason)
{
    m_impl->Mutate([this, suspended, reason]
    {
        auto& p = *m_impl;
        if (p.suspended == suspended) return;
        p.CancelAll(reason, Reacquire::RequireRelease);
        p.suspended = suspended;
        p.realTime = p.system.Now();
    });
}
void InputUser::SuspendForRebinding(bool acquire, uint64_t suppressThrough)
{
    m_impl->Mutate([this, acquire, suppressThrough]
    {
        auto& p = *m_impl;
        // A rebind can finish before the user's next Update. Advance its local
        // device mirror through the captured event without evaluating actions;
        // later releases and independent presses in this same pump stay queued.
        if (!acquire)
            for (const auto& event : p.system.GetEvents())
                if (event.sequence > p.checkpoint && event.sequence <= suppressThrough)
                { p.checkpoint = event.sequence; p.source = event; p.ApplyEvent(event); }
        const bool wasSuspended = p.Suspended();
        if (acquire) ++p.rebindingLocks;
        else if (p.rebindingLocks) --p.rebindingLocks;
        if (wasSuspended != p.Suspended())
        { p.CancelAll(CancelReason::Rebinding, Reacquire::RequireRelease); p.realTime = p.system.Now(); }
    });
}
bool InputUser::IsSuspended() const { return m_impl->Suspended(); }
void InputUser::SetEnabled(bool enabled)
{
    m_impl->Mutate([this, enabled]
    {
        auto& p = *m_impl;
        if (p.enabled == enabled) return;
        p.CancelAll(CancelReason::ContextDisabled, Reacquire::RequireRelease);
        p.enabled = enabled;
    });
}
bool InputUser::IsEnabled() const { return m_impl->enabled; }
void InputUser::SetFixedBufferEnabled(bool enabled)
{
    m_impl->Mutate([this, enabled] { m_impl->bufferFixed = enabled; if (!enabled) m_impl->pending.clear(); });
}
uint64_t InputUser::GetEpoch() const { return m_impl->epoch; }
const FixedInputFrame& InputUser::BeginFixedTick(uint64_t tick, double deltaTime)
{
    auto& p = *m_impl;
    if (tick <= p.completedTick || tick != p.nextTick || !std::isfinite(deltaTime) || deltaTime <= 0) throw std::invalid_argument("Fixed input tick must advance exactly once");
    p.fixed.events.clear();
    p.fixed.tick = tick; p.fixed.epoch = p.epoch; p.fixed.deltaTime = deltaTime;
    p.fixed.actions.resize(p.actions.size());
    for (size_t i = 0; i < p.actions.size(); ++i)
    {
        auto snapshot = p.actions[i].snapshot;
        snapshot.pressed = snapshot.released = snapshot.performed = snapshot.canceled = false;
        if (p.Data().actions[i].nature == ValueNature::Delta) { snapshot.value = {}; snapshot.actuated = false; }
        p.fixed.actions[i] = snapshot;
    }
    for (const auto& event : p.pending)
        if (event.epoch == p.epoch && event.tick == tick)
        {
            p.fixed.events.push_back(event);
            const size_t action = p.definition->FindAction(event.action);
            if (action == SIZE_MAX) continue;
            auto& snapshot = p.fixed.actions[action];
            snapshot.pressed |= event.phase == ActionPhase::Pressed;
            snapshot.released |= event.phase == ActionPhase::Released;
            snapshot.performed |= event.phase == ActionPhase::Performed;
            snapshot.canceled |= event.phase == ActionPhase::Canceled;
            if (p.Data().actions[action].nature == ValueNature::Delta && event.phase == ActionPhase::ValueChanged) snapshot.value.vector += event.value.vector;
        }
    std::erase_if(p.pending, [tick](const auto& event) { return event.tick <= tick; });
    for (size_t i = 0; i < p.fixed.actions.size(); ++i)
        if (p.Data().actions[i].nature == ValueNature::Delta) p.fixed.actions[i].actuated = p.fixed.actions[i].value.AsButton(p.Data().actions[i].pressThreshold);
    p.completedTick = tick;
    p.nextTick = tick + 1;
    p.Dispatch(p.fixed.events, UpdateMode::Fixed);
    p.ApplyMutations();
    return p.fixed;
}
void InputUser::SetNextFixedTick(uint64_t tick)
{
    auto& p = *m_impl;
    if (tick <= p.completedTick || !p.pending.empty()) throw std::invalid_argument("Cannot retarget pending fixed input");
    p.nextTick = tick;
}
size_t InputUser::GetPendingFixedEventCount() const { return m_impl->pending.size(); }
const FixedInputFrame& InputUser::GetFixedFrame() const { return m_impl->fixed; }
}
