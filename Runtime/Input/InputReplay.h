#pragma once
#include "InputDomain.h"

namespace NLS::Input
{
// A typed journal for automation/debug replay. Record from a new Play epoch,
// including initial definitions, profiles and topology; it does not snapshot
// arbitrary in-progress interaction state. Native handles are never replayed.
enum class ReplayOperation : uint8_t
{
    BeginDomain, StopDomain, AddDevice, RemoveDevice, AddWindow, RemoveWindow,
    Event, Definition, Profile, PushContext, PopContext, EnableContext,
    DisableContext, PairDevice, UnpairDevice, BlockDevice, SuspendUser,
    EnableUser, CancelUser, SuspendDomain, AdvanceTime, Update, FixedTick, EndFrame,
    FinishEventPump
};

struct InputReplayRecord
{
    ReplayOperation operation = ReplayOperation::Event;
    uint64_t user = 1; // Journal-local user identity, independent of host IDs.
    uint64_t object = 0; // Recorded device/window identity, or Fixed tick.
    double time = 0; // Monotonic RealTime; FixedTick uses this as deltaTime.
    int priority = 0;
    bool enabled = true;
    DeviceKind deviceKind = DeviceKind::Keyboard;
    CancelReason reason = CancelReason::Pause;
    Reacquire reacquire = Reacquire::RequireRelease;
    ContextId context;
    DeviceDescriptor device;
    InputEvent event;
    std::shared_ptr<const CompiledInput> definition;
    InputProfile profile;
};

struct InputReplayBatch
{
    uint64_t user = 0;
    uint64_t inputFrame = 0;
    UpdateMode mode = UpdateMode::Dynamic;
    FixedInputFrame frame; // Update: tick=0; Fixed: actual tick/deltaTime.
};

// Uses the production evaluator and command buffer, without platform devices or
// gameplay callbacks. Source sequence numbers are restored from the journal.
// Failure leaves result unchanged. Callers can persist the typed records in an
// upper adapter; the input kernel has no serialization/asset dependency.
NLS_INPUT_API bool ReplayInput(std::span<const InputReplayRecord> records,
    std::vector<InputReplayBatch>& result, std::vector<std::string>& diagnostics);
}
