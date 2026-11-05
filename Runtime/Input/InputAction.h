#pragma once
#include "InputTypes.h"
#include <Base/Guid.h>
#include <Math/Vector3.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NLS::Input
{
using ActionId = Guid;
using ContextId = Guid;
using BindingId = Guid;
enum class ValueType : uint8_t { Button, Axis, Vector2, Vector3 };
enum class ValueNature : uint8_t { State, Delta, Rate };
enum class Aggregation : uint8_t { MaximumMagnitude, Sum };
enum class ControlKind : uint8_t { Button, Axis, PointerPosition, PointerDelta, Wheel, ScanCode, LogicalKey };
enum class Modifier : uint8_t { None = 0, Shift = 1, Control = 2, Alt = 4, Super = 8, Primary = 16 };
enum class ModifierMatch : uint8_t { Exact, RequiredSubset };
enum class Composite : uint8_t { Direct, Axis, Vector2, Vector3 };
enum class ProcessorKind : uint8_t { Scale, Invert, Deadzone, Normalize, Clamp };
enum class InteractionKind : uint8_t { Press, Release, Hold, Tap, MultiTap, Repeat };
enum class Consumption : uint8_t { Never, OnActuated, OnStarted, OnPerformed };
enum class Reacquire : uint8_t { RequireRelease, ResumeImmediately };
enum class OverrideState : uint8_t { Default, Override, ExplicitlyUnbound };
enum class ActionPhase : uint8_t { Started, Pressed, Released, Performed, Canceled, ValueChanged };
enum class CancelReason : uint8_t { None, FocusLost, DeviceLost, ContextDisabled, RoutingChanged, Rebinding, DefinitionChanged, Pause, Stop };
enum class UpdateMode : uint8_t { Dynamic, Fixed };

constexpr Modifier operator|(Modifier a, Modifier b) { return static_cast<Modifier>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }

struct NLS_INPUT_API InputValue
{
    Maths::Vector3 vector;
    InputValue(float x = 0, float y = 0, float z = 0) : vector(x, y, z) {}
    bool AsButton(float threshold = 0.5f) const;
    float AsAxis() const { return vector.x; }
    Maths::Vector2 AsVector2() const { return {vector.x, vector.y}; }
    float MagnitudeSquared() const;
    friend bool operator==(const InputValue& a, const InputValue& b) { return a.vector == b.vector; }
};

struct ControlPath
{
    DeviceKind device = DeviceKind::Keyboard;
    ControlKind kind = ControlKind::Button;
    uint16_t control = 0;
    // Optional exact device, scancode, or Unicode logical identity.
    DeviceId deviceId;
    uint32_t code = 0;
    friend bool operator==(const ControlPath&, const ControlPath&) = default;
};
struct ControlPart
{
    ControlPath path;
    // Composite contributions: W=(0,1), S=(0,-1), A=(-1,0), D=(1,0).
    InputValue contribution{1};
};
struct Processor
{
    ProcessorKind kind = ProcessorKind::Scale;
    Maths::Vector3 scale{1, 1, 1};
    float minimum = 0.125f;
    float maximum = 1.0f;
};
struct Interaction
{
    InteractionKind kind = InteractionKind::Press;
    double duration = 0.5;
    double interval = 0.2;
    uint32_t count = 2;
};
struct Chord
{
    std::vector<ControlPath> controls;
    Modifier modifiers = Modifier::None;
    Modifier forbiddenModifiers = Modifier::None;
    ModifierMatch modifierMatch = ModifierMatch::RequiredSubset;
    bool matchModifiers = false;
};
struct ActionDefinition
{
    ActionId id;
    std::string name;
    ValueType type = ValueType::Button;
    ValueNature nature = ValueNature::State;
    Aggregation aggregation = Aggregation::MaximumMagnitude;
    float pressThreshold = 0.5f;
    float releaseThreshold = 0.4f;
    std::vector<Processor> processors;
    Chord chord;
    Interaction interaction;
};
struct BindingDefinition
{
    BindingId id;
    ActionId action;
    Composite composite = Composite::Direct;
    ValueNature nature = ValueNature::State;
    std::vector<ControlPart> parts;
    std::vector<Processor> processors;
    Chord chord;
    bool hasInteraction = false;
    Interaction interaction;
};
struct ContextDefinition
{
    ContextId id;
    std::string name;
    int priority = 0;
    Consumption consumption = Consumption::OnActuated;
    Reacquire reacquire = Reacquire::RequireRelease;
    std::vector<BindingDefinition> bindings;
};
struct InputDefinition
{
    uint32_t version = 1;
    Guid id;
    ContextId defaultContext;
    std::vector<ActionDefinition> actions;
    std::vector<ContextDefinition> contexts;
};
struct BindingOverride
{
    BindingId binding;
    OverrideState state = OverrideState::Default;
    std::vector<ControlPart> parts;
    std::optional<Chord> chord;
};
struct InputProfile
{
    uint32_t version = 1;
    std::vector<BindingOverride> overrides;
    float pointerSensitivity = 1.0f;
    float gamepadSensitivity = 1.0f;
    // Retained for user resolution; never evaluated until explicitly applied.
    std::vector<BindingOverride> unresolved;
};

struct ActionHandle
{
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    explicit operator bool() const { return index != UINT32_MAX; }
};
struct ActionSnapshot
{
    InputValue value;
    bool actuated = false;
    bool pressed = false;
    bool released = false;
    bool performed = false;
    bool canceled = false;
};
struct ActionEvent
{
    uint64_t sequence = 0;
    uint64_t sourceSequence = 0;
    uint64_t epoch = 0;
    uint64_t tick = 0;
    double timestamp = 0;
    double realTimestamp = 0;
    ActionId action;
    ActionPhase phase = ActionPhase::ValueChanged;
    InputValue value;
    DeviceId device;
    WindowId window;
    uint16_t control = 0;
    uint32_t modifiers = 0;
    CancelReason reason = CancelReason::None;
};

class NLS_INPUT_API CompiledInput final
{
public:
    // Copies definition once. Runtime maps contain no mutable player state.
    static std::shared_ptr<const CompiledInput> Compile(InputDefinition definition, std::vector<std::string>& diagnostics);
    const InputDefinition& GetDefinition() const { return m_definition; }
    size_t FindAction(ActionId id) const;
    // Initialization-time lookup. Ambiguous names return SIZE_MAX; persist IDs.
    size_t FindAction(std::string_view nameOrPath) const;
    size_t FindContext(ContextId id) const;
    bool HasBinding(BindingId id) const;
    bool IsOverrideCompatible(const BindingOverride& value) const;
private:
    InputDefinition m_definition;
    std::unordered_map<ActionId, size_t> m_actions;
    std::unordered_map<ContextId, size_t> m_contexts;
    std::unordered_map<BindingId, bool> m_bindings;
};
NLS_INPUT_API InputValue ProcessValue(InputValue value, std::span<const Processor> processors, ValueType type);
}
