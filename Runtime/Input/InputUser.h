#pragma once
#include "InputAction.h"
#include "InputSystem.h"
#include <functional>
#include <memory>

namespace NLS::Input
{
class NLS_INPUT_API InputSubscription final
{
public:
    InputSubscription() = default;
    ~InputSubscription();
    InputSubscription(InputSubscription&&) noexcept;
    InputSubscription& operator=(InputSubscription&&) noexcept;
    InputSubscription(const InputSubscription&) = delete;
    InputSubscription& operator=(const InputSubscription&) = delete;
    void Reset();
private:
    friend class InputUser;
    struct Slot;
    explicit InputSubscription(std::shared_ptr<Slot> slot);
    std::shared_ptr<Slot> m_slot;
};

struct FixedInputFrame
{
    uint64_t tick = 0;
    uint64_t epoch = 0;
    double deltaTime = 0;
    std::vector<ActionEvent> events;
    std::vector<ActionSnapshot> actions;
};

class NLS_INPUT_API InputUser final
{
public:
    explicit InputUser(InputSystem& system, uint64_t id);
    ~InputUser();
    InputUser(const InputUser&) = delete;
    InputUser& operator=(const InputUser&) = delete;
    uint64_t GetId() const;
    void SetDefinition(std::shared_ptr<const CompiledInput> definition);
    std::shared_ptr<const CompiledInput> GetDefinition() const;
    ActionHandle FindAction(ActionId id) const;
    ActionHandle FindAction(std::string_view nameOrPath) const;
    ActionSnapshot GetActionState(ActionHandle action) const;
    InputValue GetActionValue(ActionHandle action) const;
    bool WasActionPressed(ActionHandle action) const;
    bool WasActionReleased(ActionHandle action) const;
    std::span<const ActionEvent> GetActionEvents() const;
    InputSubscription BindAction(ActionId action, ActionPhase phase, std::function<void(const ActionEvent&)> callback,
        UpdateMode mode = UpdateMode::Dynamic);
    InputSubscription BindAction(std::string_view nameOrPath, ActionPhase phase, std::function<void(const ActionEvent&)> callback,
        UpdateMode mode = UpdateMode::Dynamic);

    bool PushContext(ContextId context, int priorityOffset = 0);
    bool PopContext(ContextId context);
    bool EnableContext(ContextId context);
    bool DisableContext(ContextId context);
    bool IsContextEnabled(ContextId context) const;
    void PairDevice(DeviceId device);
    void UnpairDevice(DeviceId device);
    bool IsDevicePaired(DeviceId device) const;
    // Domain routing masks Action sources, preserving host device state.
    void SetDeviceBlocked(DeviceKind kind, bool blocked);
    void SetPrimaryModifier(Modifier modifier);
    void SetProfile(InputProfile profile);
    InputProfile GetProfile() const;
    uint64_t GetProfileRevision() const;
    void SetBindingOverride(BindingOverride value);
    void RestoreBindingDefault(BindingId binding);
    void ClearBinding(BindingId binding);
    std::span<const ControlPart> GetEffectiveBinding(BindingId binding) const;
    Chord GetEffectiveChord(BindingId binding) const;
    std::vector<BindingId> GetUnappliedOverrides() const;

    // Called once after event pump and routing. Sequence checkpoints make a
    // second call with the same stream harmless; timers still advance.
    void Update(double realTime, bool dispatchDynamic = true);
    void DispatchDynamicEvents();
    void Cancel(CancelReason reason, Reacquire reacquire = Reacquire::RequireRelease);
    void SetSuspended(bool suspended, CancelReason reason = CancelReason::Pause);
    bool IsSuspended() const;
    void SetEnabled(bool enabled);
    bool IsEnabled() const;
    void SetFixedBufferEnabled(bool enabled);
    uint64_t GetEpoch() const;

    // Edges are assigned to the next unexecuted tick. The returned storage lives
    // until the next call. Reusing a completed tick is rejected, never replayed.
    const FixedInputFrame& BeginFixedTick(uint64_t tick, double deltaTime);
    const FixedInputFrame& GetFixedFrame() const;
    void SetNextFixedTick(uint64_t tick);
    size_t GetPendingFixedEventCount() const;
private:
    friend class RebindingSession;
    void SuspendForRebinding(bool acquire, uint64_t suppressThrough = 0);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
