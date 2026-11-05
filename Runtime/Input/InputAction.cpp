#include "InputAction.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace NLS::Input
{
namespace
{
bool ValidInteraction(const Interaction& value)
{
    return static_cast<uint8_t>(value.kind) <= static_cast<uint8_t>(InteractionKind::Repeat) && std::isfinite(value.duration) && std::isfinite(value.interval) &&
        value.duration >= 0.000001 && value.interval >= 0.001 && value.count > 0;
}
bool ValidProcessors(std::span<const Processor> values)
{
    for (const auto& value : values)
        if (static_cast<uint8_t>(value.kind) > static_cast<uint8_t>(ProcessorKind::Clamp) ||
            !std::isfinite(value.minimum) || !std::isfinite(value.maximum) || !std::isfinite(value.scale.x) || !std::isfinite(value.scale.y) || !std::isfinite(value.scale.z) ||
            value.minimum > value.maximum || (value.kind == ProcessorKind::Deadzone && (value.minimum < 0 || value.minimum >= value.maximum))) return false;
    return true;
}
bool ValidPath(const ControlPath& path)
{
    return static_cast<uint8_t>(path.device) <= static_cast<uint8_t>(DeviceKind::Gamepad) &&
        static_cast<uint8_t>(path.kind) <= static_cast<uint8_t>(ControlKind::LogicalKey) &&
        (path.kind != ControlKind::Button || path.control < 512) && (path.kind != ControlKind::Axis || path.control < 16);
}
bool ValidChord(const Chord& chord)
{
    return static_cast<uint8_t>(chord.modifiers) < 32 && static_cast<uint8_t>(chord.forbiddenModifiers) < 32 &&
        static_cast<uint8_t>(chord.modifierMatch) <= 1 && std::all_of(chord.controls.begin(), chord.controls.end(), ValidPath);
}
}
bool InputValue::AsButton(float threshold) const { return MagnitudeSquared() >= threshold * threshold; }
float InputValue::MagnitudeSquared() const { return vector.x * vector.x + vector.y * vector.y + vector.z * vector.z; }

InputValue ProcessValue(InputValue value, std::span<const Processor> processors, ValueType type)
{
    auto& v = value.vector;
    for (const auto& processor : processors)
    {
        const float length = std::sqrt(value.MagnitudeSquared());
        switch (processor.kind)
        {
        case ProcessorKind::Scale: v.x *= processor.scale.x; v.y *= processor.scale.y; v.z *= processor.scale.z; break;
        case ProcessorKind::Invert: v.x = -v.x; v.y = -v.y; v.z = -v.z; break;
        case ProcessorKind::Deadzone:
            if (length <= processor.minimum) v = {};
            else if (length > 0)
                v *= std::clamp((length - processor.minimum) / std::max(0.00001f, processor.maximum - processor.minimum), 0.0f, 1.0f) / length;
            break;
        case ProcessorKind::Normalize: if (length > 0) v *= 1.0f / length; break;
        case ProcessorKind::Clamp:
            v.x = std::clamp(v.x, processor.minimum, processor.maximum);
            v.y = std::clamp(v.y, processor.minimum, processor.maximum);
            v.z = std::clamp(v.z, processor.minimum, processor.maximum);
            break;
        }
    }
    if (type == ValueType::Button || type == ValueType::Axis) { v.y = 0; v.z = 0; }
    if (type == ValueType::Vector2) v.z = 0;
    return value;
}

std::shared_ptr<const CompiledInput> CompiledInput::Compile(InputDefinition definition, std::vector<std::string>& diagnostics)
{
    const size_t errors = diagnostics.size();
    auto result = std::make_shared<CompiledInput>();
    if (definition.version != 1) diagnostics.emplace_back("Unsupported input definition version");
    if (!definition.id.IsValid()) diagnostics.emplace_back("Invalid input definition ID");
    for (size_t i = 0; i < definition.actions.size(); ++i)
    {
        const auto& action = definition.actions[i];
        if (static_cast<uint8_t>(action.type) > 3 || static_cast<uint8_t>(action.nature) > 2 || static_cast<uint8_t>(action.aggregation) > 1 ||
            !ValidInteraction(action.interaction) || !ValidProcessors(action.processors) || !ValidChord(action.chord)) diagnostics.emplace_back("Invalid action processing configuration");
        if (!action.id.IsValid() || !result->m_actions.emplace(action.id, i).second) diagnostics.emplace_back("Invalid or duplicate ActionId");
        if (!std::isfinite(action.pressThreshold) || !std::isfinite(action.releaseThreshold) || action.pressThreshold <= 0 ||
            action.releaseThreshold < 0 || action.releaseThreshold >= action.pressThreshold) diagnostics.emplace_back("Invalid action thresholds");
    }
    for (size_t i = 0; i < definition.contexts.size(); ++i)
    {
        const auto& context = definition.contexts[i];
        if (static_cast<uint8_t>(context.consumption) > 3 || static_cast<uint8_t>(context.reacquire) > 1) diagnostics.emplace_back("Invalid context policy");
        if (!context.id.IsValid() || !result->m_contexts.emplace(context.id, i).second) diagnostics.emplace_back("Invalid or duplicate ContextId");
        for (const auto& binding : context.bindings)
        {
            if (!binding.id.IsValid() || !result->m_bindings.emplace(binding.id, true).second) diagnostics.emplace_back("Invalid or duplicate BindingId");
            const auto action = result->m_actions.find(binding.action);
            if (static_cast<uint8_t>(binding.composite) > 3 || !ValidProcessors(binding.processors) || !ValidChord(binding.chord) ||
                (binding.hasInteraction && !ValidInteraction(binding.interaction))) diagnostics.emplace_back("Invalid binding processing configuration");
            for (const auto& part : binding.parts)
            {
                if (!ValidPath(part.path) || !std::isfinite(part.contribution.vector.x) || !std::isfinite(part.contribution.vector.y) || !std::isfinite(part.contribution.vector.z)) diagnostics.emplace_back("Invalid binding control");
                const bool delta = part.path.kind == ControlKind::PointerDelta || part.path.kind == ControlKind::Wheel;
                if (delta != (binding.nature == ValueNature::Delta)) diagnostics.emplace_back("Delta controls require a Delta binding; State/Rate sources need an explicit separate action");
            }
            if (action == result->m_actions.end()) { diagnostics.emplace_back("Binding references a missing ActionId"); continue; }
            if (binding.nature != definition.actions[action->second].nature) diagnostics.emplace_back("State, Delta and Rate sources require separate actions");
            // An empty default slot is intentional and remains rebindable by ID.
            if (binding.composite == Composite::Direct && binding.parts.size() > 1) diagnostics.emplace_back("Direct binding accepts one control");
        }
    }
    if (definition.defaultContext.IsValid() && !result->m_contexts.contains(definition.defaultContext)) diagnostics.emplace_back("Default context does not exist");
    if (diagnostics.size() != errors) return {};
    result->m_definition = std::move(definition);
    return result;
}
size_t CompiledInput::FindAction(ActionId id) const { const auto it = m_actions.find(id); return it == m_actions.end() ? SIZE_MAX : it->second; }
size_t CompiledInput::FindAction(std::string_view nameOrPath) const
{
    const auto equal = [](std::string_view a, std::string_view b)
    {
        if (a.size() != b.size()) return false;
        const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        for (size_t i = 0; i < a.size(); ++i) if (fold(a[i]) != fold(b[i])) return false;
        return true;
    };
    if (const auto id = Guid::TryParse(nameOrPath)) return FindAction(*id);
    const auto separator = nameOrPath.find('/');
    const auto name = separator == std::string_view::npos ? nameOrPath : nameOrPath.substr(separator + 1);
    if (name.empty()) return SIZE_MAX;
    size_t found = SIZE_MAX;
    for (size_t i = 0; i < m_definition.actions.size(); ++i)
    {
        const auto& action = m_definition.actions[i];
        if (!equal(action.name, name)) continue;
        bool inContext = separator == std::string_view::npos;
        if (!inContext)
            for (const auto& context : m_definition.contexts)
                if (equal(context.name, nameOrPath.substr(0, separator)))
                    for (const auto& binding : context.bindings) inContext |= binding.action == action.id;
        if (!inContext) continue;
        if (found != SIZE_MAX && found != i) return SIZE_MAX;
        found = i;
    }
    return found;
}
size_t CompiledInput::FindContext(ContextId id) const { const auto it = m_contexts.find(id); return it == m_contexts.end() ? SIZE_MAX : it->second; }
bool CompiledInput::HasBinding(BindingId id) const { return m_bindings.contains(id); }
bool CompiledInput::IsOverrideCompatible(const BindingOverride& value) const
{
    if (static_cast<uint8_t>(value.state) > 2) return false;
    for (const auto& context : m_definition.contexts) for (const auto& binding : context.bindings)
        if (binding.id == value.binding)
        {
            if (value.state != OverrideState::Override) return true;
            if (value.parts.empty() || (binding.composite == Composite::Direct && value.parts.size() > 1) || (value.chord && !ValidChord(*value.chord))) return false;
            for (const auto& part : value.parts)
            {
                if (!ValidPath(part.path) || !std::isfinite(part.contribution.vector.x) || !std::isfinite(part.contribution.vector.y) || !std::isfinite(part.contribution.vector.z)) return false;
                const bool delta = part.path.kind == ControlKind::PointerDelta || part.path.kind == ControlKind::Wheel;
                if (delta != (binding.nature == ValueNature::Delta)) return false;
            }
            return true;
        }
    return false;
}
}
