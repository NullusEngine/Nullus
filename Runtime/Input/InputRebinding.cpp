#include "InputRebinding.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace NLS::Input
{
RebindingSession::RebindingSession(InputSystem& system, InputUser& user, BindingId binding, RebindingOptions options)
    : m_system(system), m_user(user), m_options(options), m_checkpoint(system.GetLastSequence()), m_started(system.Now())
{
    if (!user.GetDefinition() || !user.GetDefinition()->HasBinding(binding)) throw std::invalid_argument("Unknown rebind slot");
    if (!std::isfinite(options.axisThreshold) || !std::isfinite(options.axisNeutralThreshold) || !std::isfinite(options.axisSettleTime) || !std::isfinite(options.timeout) ||
        options.axisThreshold <= options.axisNeutralThreshold || options.axisNeutralThreshold < 0 || options.axisSettleTime < 0 || options.timeout <= 0)
        throw std::invalid_argument("Invalid rebinding thresholds");
    m_result.binding = binding;
    m_definition = user.GetDefinition(); m_profileRevision = user.GetProfileRevision();
    m_result.state = OverrideState::Override;
    const auto effectiveParts = user.GetEffectiveBinding(binding);
    m_result.parts.assign(effectiveParts.begin(), effectiveParts.end());
    if (m_result.parts.empty())
        for (const auto& context : user.GetDefinition()->GetDefinition().contexts)
            for (const auto& candidate : context.bindings) if (candidate.id == binding) m_result.parts = candidate.parts;
    if (m_result.parts.empty()) m_result.parts.push_back({{}, {1}});
    if (options.partIndex >= m_result.parts.size()) throw std::invalid_argument("Unknown composite part");
    m_result.chord = user.GetEffectiveChord(binding);
    for (const auto& context : user.GetDefinition()->GetDefinition().contexts)
        for (const auto& candidate : context.bindings) if (candidate.id == binding) m_nature = candidate.nature;
    for (const auto& device : system.GetDevices())
        if (user.IsDevicePaired(device.id))
        {
            for (uint16_t i = 0; i < device.buttons.size(); ++i)
                if (device.buttons[i]) m_startControls.emplace_back(device.id, i);
            if (device.descriptor.kind == DeviceKind::Gamepad)
                for (uint16_t i = 0; i < device.axes.size(); ++i)
                    if (std::abs(device.axes[i]) > options.axisNeutralThreshold) m_initialAxes.push_back({DeviceKind::Gamepad, ControlKind::Axis, i, device.id});
        }
    if (m_startControls.empty()) m_status = RebindingStatus::Listening;
    user.SuspendForRebinding(true);
}
RebindingSession::~RebindingSession() { m_user.SuspendForRebinding(false, m_checkpoint); }
void RebindingSession::Complete(ControlPath path, uint32_t modifiers)
{
    // Profiles address the player's paired device type, never a session-local handle.
    path.deviceId = {};
    m_result.parts[m_options.partIndex].path = path;
    Chord chord = m_result.chord.value_or(Chord{});
    chord.matchModifiers = path.device == DeviceKind::Keyboard;
    chord.modifiers = static_cast<Modifier>(modifiers & 15u);
    m_result.chord = chord;
    m_status = RebindingStatus::Completed;
}
RebindingStatus RebindingSession::Update()
{
    if (!m_applied && m_status != RebindingStatus::Canceled && m_status != RebindingStatus::TimedOut &&
        (m_user.GetDefinition() != m_definition || m_user.GetProfileRevision() != m_profileRevision)) m_status = RebindingStatus::Canceled;
    if (m_status == RebindingStatus::Completed || m_status == RebindingStatus::Canceled || m_status == RebindingStatus::TimedOut) return m_status;
    if (m_system.Now() - m_started >= m_options.timeout) return m_status = RebindingStatus::TimedOut;
    for (const auto& event : m_system.GetEvents())
    {
        if (event.sequence <= m_checkpoint) continue;
        m_checkpoint = event.sequence;
        if (event.type == InputEventType::FocusLost || event.type == InputEventType::DeviceReset) { Cancel(); break; }
        if (m_status == RebindingStatus::WaitingForRelease)
        {
            if (event.type == InputEventType::ButtonUp) std::erase(m_startControls, std::pair{event.device, event.control});
            if (m_startControls.empty()) m_status = RebindingStatus::Listening;
            continue;
        }
        if (!m_user.IsDevicePaired(event.device)) continue;
        const auto* device = m_system.FindDevice(event.device);
        if (!device || !device->connected) continue;
        if (device->descriptor.kind == DeviceKind::Keyboard && event.type == InputEventType::ButtonDown && event.control == m_options.cancelKey)
        { Cancel(); break; }
        const auto kind = device->descriptor.kind;
        if ((kind == DeviceKind::Keyboard && !m_options.keyboard) || (kind == DeviceKind::Mouse && !m_options.mouse) || (kind == DeviceKind::Gamepad && !m_options.gamepad)) continue;
        if (m_nature == ValueNature::Delta)
        {
            if (event.type == InputEventType::PointerDelta && (std::abs(event.x) + std::abs(event.y)) >= 2)
            { Complete({kind, ControlKind::PointerDelta, 0}, event.modifiers); break; }
            if (event.type == InputEventType::Wheel && (event.x != 0 || event.y != 0))
            { Complete({kind, ControlKind::Wheel, 0}, event.modifiers); break; }
            continue;
        }
        if (event.type == InputEventType::ButtonDown)
        {
            if (kind == DeviceKind::Keyboard && event.control >= 340 && event.control <= 347) continue;
            ControlPath path{kind, ControlKind::Button, event.control};
            const auto identity = m_result.parts[m_options.partIndex].path.kind;
            if (kind == DeviceKind::Keyboard && (identity == ControlKind::ScanCode || identity == ControlKind::LogicalKey))
            {
                path.kind = identity; path.control = 0;
                path.code = identity == ControlKind::ScanCode ? static_cast<uint32_t>(event.scanCode) : event.logicalKey;
                if (!path.code) continue;
            }
            Complete(path, event.modifiers);
            break;
        }
        if (event.type == InputEventType::Axis && kind == DeviceKind::Gamepad && m_options.analogAxes)
        {
            const ControlPath candidate{kind, ControlKind::Axis, event.control, event.device};
            const auto initial = std::find(m_initialAxes.begin(), m_initialAxes.end(), candidate);
            if (initial != m_initialAxes.end())
            {
                if (std::abs(event.x) <= m_options.axisNeutralThreshold) m_initialAxes.erase(initial);
                continue;
            }
            if (std::abs(event.x) < m_options.axisNeutralThreshold && m_hasCandidate && m_candidate == candidate) m_hasCandidate = false;
            if (std::abs(event.x) >= m_options.axisThreshold && (!m_hasCandidate || !(m_candidate == candidate)))
            { m_candidate = candidate; m_candidateTime = m_system.Now(); m_hasCandidate = true; }
        }
    }
    if (m_hasCandidate && m_system.Now() - m_candidateTime >= m_options.axisSettleTime)
    {
        const auto* device = m_system.FindDevice(m_candidate.deviceId);
        if (device && device->connected && std::abs(device->axes[m_candidate.control]) >= m_options.axisThreshold) Complete(m_candidate, 0);
        else m_hasCandidate = false;
    }
    return m_status;
}
void RebindingSession::Cancel() { if (!m_applied && m_status != RebindingStatus::TimedOut) m_status = RebindingStatus::Canceled; }
bool RebindingSession::Apply()
{
    if (m_status != RebindingStatus::Completed || m_applied) return false;
    if (m_user.GetDefinition() != m_definition || m_user.GetProfileRevision() != m_profileRevision) { Cancel(); return false; }
    m_user.SetBindingOverride(m_result);
    m_applied = true;
    return true;
}
std::vector<BindingConflict> FindBindingConflicts(const InputUser& user, BindingId target, std::span<const ControlPart> parts,
    const Chord& chord, const std::function<bool(ContextId, ContextId)>& mutuallyExclusive)
{
    std::vector<BindingConflict> conflicts;
    if (!user.GetDefinition()) return conflicts;
    const auto& data = user.GetDefinition()->GetDefinition();
    ContextId targetContext;
    ActionId targetAction;
    for (const auto& context : data.contexts) for (const auto& binding : context.bindings)
        if (binding.id == target) { targetContext = context.id; targetAction = binding.action; }
    auto modifiers = [](Modifier modifiers)
    {
        auto bits = static_cast<uint8_t>(modifiers);
#ifdef __APPLE__
        return (bits & ~16u) | ((bits & 16u) ? 8u : 0u);
#else
        return (bits & ~16u) | ((bits & 16u) ? 2u : 0u);
#endif
    };
    for (const auto& context : data.contexts)
    {
        if (mutuallyExclusive && mutuallyExclusive(targetContext, context.id)) continue;
        for (const auto& binding : context.bindings)
        {
            if (binding.id == target) continue;
            auto otherChord = user.GetEffectiveChord(binding.id);
            // Four physical modifier bits make exhaustive overlap testing cheap
            // and keep Exact, RequiredSubset and forbidden modifiers consistent.
            const auto accepts = [&](const Chord& value, unsigned mask)
            {
                if (!value.matchModifiers) return true;
                const auto required = modifiers(value.modifiers);
                if ((mask & modifiers(value.forbiddenModifiers)) != 0) return false;
                return value.modifierMatch == ModifierMatch::Exact ? mask == required : (mask & required) == required;
            };
            bool chordOverlap = false;
            for (unsigned mask = 0; mask < 16; ++mask) chordOverlap |= accepts(chord, mask) && accepts(otherChord, mask);
            if (!chordOverlap) continue;
            bool overlaps = false;
            for (const auto& a : parts) for (const auto& b : user.GetEffectiveBinding(binding.id))
                if (a.path.device == b.path.device && a.path.kind == b.path.kind && a.path.control == b.path.control && a.path.code == b.path.code &&
                    (!a.path.deviceId || !b.path.deviceId || a.path.deviceId == b.path.deviceId) &&
                    (binding.action != targetAction || !(a.contribution == b.contribution))) overlaps = true;
            if (overlaps) conflicts.push_back({binding.id, context.id, binding.action});
        }
    }
    return conflicts;
}
}
