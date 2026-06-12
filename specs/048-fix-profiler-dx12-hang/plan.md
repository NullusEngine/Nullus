# Implementation Plan: Fix Profiler DX12 Hang

**Branch**: `048-fix-profiler-dx12-hang` | **Date**: 2026-06-12 | **Spec**: `specs/048-fix-profiler-dx12-hang/spec.md`
**Input**: Feature specification from `specs/048-fix-profiler-dx12-hang/spec.md`

## Summary

Opening the editor Profiler panel increases UI/profiler GPU work and exposed a regression introduced by `047-dx12-ui-present-wait` (`e9c928ee` / `5740622c`). That change kept a larger DX12 UI command allocator pool but removed the per-backbuffer reuse wait. The reported DX12 editor log shows `DEVICE_HUNG` immediately after `DX12UIBridge::RenderDrawData` executes UI command lists, followed by RHI quarantine and rejected readbacks. The fix restores the UI bridge's backbuffer reuse wait before command-list reset/resource barriers while keeping allocator-pool reuse, and hardens TimelineProfiler direct queue operations so they share native queue serialization, stable queue-lock lifetime, ordered profiler metadata publication, and valid readback resource state.

## Technical Context

**Language/Version**: C++20 in the existing Nullus CMake/MSVC build
**Primary Dependencies**: D3D12, ImGui TimelineProfiler extension, Nullus `NLS_UI` and `NLS_Render`
**Storage**: N/A
**Testing**: GoogleTest via `NullusUnitTests`; DX12 editor runtime verification when practical
**Target Platform**: Windows DX12
**Project Type**: Desktop engine/editor
**Performance Goals**: Opening the Profiler panel must not introduce device loss or quarantine; allocator-pool reuse should remain available after the backbuffer safety wait; GPU profiler event publication remains available.
**Constraints**: Do not edit generated files; do not claim other backends; preserve existing device-lost/quarantine behavior.
**Scale/Scope**: DX12 UI bridge backbuffer/allocator ordering fix, TimelineProfiler queue-synchronization hardening, editor validation hook, and focused regression coverage.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: Pass. This touches DX12 backend/profiler GPU synchronization, so `spec.md`, `plan.md`, and `tasks.md` are under one feature directory.
- **Validation matches subsystem**: Pass. Automated validation targets TimelineProfiler GPU lifecycle and DX12 queue-ordering contracts; runtime validation is scoped to Windows DX12.
- **Generated/backend boundaries**: Pass. No `Runtime/*/Gen/` edits; fix remains inside DX12 UI bridge and profiler/DX12 queue synchronization.
- **Incremental verified delivery**: Pass. Red test first, narrow implementation, then focused tests/build/runtime evidence.
- **Product runtime preservation**: Pass. The Editor must remain runnable and the Profiler panel must keep GPU timeline functionality.

## Project Structure

### Documentation (this feature)

```text
specs/048-fix-profiler-dx12-hang/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/Rendering/RHI/Backends/DX12/
├── DX12UIBridge.cpp
├── DX12UIFrameFenceTracker.h
├── DX12QueueSynchronization.h
└── DX12QueueSynchronization.cpp

Runtime/UI/ImGuiExtensions/TimelineProfiler/
├── Profiler.cpp
└── Profiler.h

Tests/Unit/
├── ProfilerDestinationTests.cpp
├── TimelineProfilerGpuLifecycleTests.cpp
├── EditorLaunchArgsTests.cpp
└── PanelWindowHookTests.cpp
```

**Structure Decision**: Keep the primary UI regression fix in `DX12UIBridge.cpp`, keep the existing allocator pool, and extend the DX12 queue synchronization helper so TimelineProfiler direct queue operations share ordering/lifetime guarantees with RHI queue submissions.

## Phase 0 Research

### Decision: Restore DX12 UI backbuffer reuse wait before allocator reuse

**Rationale**: Commit `5740622c` allowed a completed allocator to be selected even when the current swapchain backbuffer's previous UI fence was incomplete. Allocator readiness is necessary for `ID3D12CommandAllocator::Reset`, but it does not prove the same backbuffer is safe to transition and render to again. The UI bridge must therefore wait for the current backbuffer's previous UI fence first, then apply allocator-pool reuse. This matches the D3D12 frame-resource pattern documented by Microsoft for allocator reset safety and the DirectX `D3D12HelloFrameBuffering` sample's per-frame fence gating. Backbuffer rendering also still relies on explicit D3D12 resource barrier state transitions.

**Primary references**:

- Microsoft Learn: `ID3D12CommandAllocator::Reset` - https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset
- Microsoft DirectX-Graphics-Samples: `D3D12HelloFrameBuffering.cpp` - https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12HelloWorld/src/HelloFrameBuffering/D3D12HelloFrameBuffering.cpp
- Microsoft Learn: D3D12 resource barriers - https://learn.microsoft.com/en-us/windows/win32/direct3d12/resource-barriers
- Microsoft Learn: `ID3D12CommandQueue::Wait` - https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait
- Microsoft Learn: `ID3D12CommandQueue::Signal` - https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-signal

**Alternatives considered**:

- Fully revert `047-dx12-ui-present-wait`. Rejected because the allocator pool is still useful; only the missing backbuffer wait is unsafe.
- Disable GPU scopes while the Profiler panel is open. Rejected because it removes feature value and bypasses the UI bridge root cause.
- Route profiler resolve work through the RHI queue abstraction. Rejected for this fix because the profiler owns an internal command list/readback/fence path and a full abstraction migration is larger than the incident scope.

### Decision: Reuse the existing per-native-queue DX12 transaction lock

**Rationale**: Every existing Nullus path that directly touches `ID3D12CommandQueue` for waits, command-list execution, or fence signals uses `ScopedDX12QueueLock` to serialize access by native queue pointer. TimelineProfiler's internal resolve path was an outlier: `QueryHeap::Resolve()` calls `ExecuteCommandLists()` and `Signal()` directly, and `QueryHeap::DrainQueue()` signals directly. Opening the Profiler panel enables that path.

**Alternatives considered**:

- Add a separate mutex in TimelineProfiler. Rejected because a separate lock would not serialize with `NativeDX12Queue`, `DX12UIBridge`, uploads, or readbacks.

## Phase 1 Design

### DX12 UI Backbuffer/Allocator Contract

DX12 UI rendering must satisfy both conditions before recording work for the current swapchain backbuffer:

- `ResolveReuseWait(backBufferIndex, completedFence)` is checked and waited if the current backbuffer's previous submitted UI fence is incomplete.
- `ResolveAllocatorReuse(completedFence)` is checked and waited if every command allocator in the pool is still in flight.
- Backbuffer wait happens before command allocator reset and before barriers that transition the current backbuffer.

### Queue Operation Contract

TimelineProfiler GPU resolve operations on `ID3D12CommandQueue` must participate in the same lock registry as other DX12 queue users:

- `QueryHeap::Resolve()` locks the resolve queue before `ExecuteCommandLists()` and keeps the lock through the following resolve-fence `Signal()`.
- `QueryHeap::DrainQueue()` locks the passed queue before its drain-fence `Signal()`.
- Existing fence waits on private profiler fences remain unchanged.
- Queue synchronization state is process-lifetime because `ScopedDX12QueueLock` hands out raw mutex pointers and TimelineProfiler keeps raw/native queue users alive independently from `NativeDX12Queue` wrappers.
- `NativeDX12Queue::SubmitChecked()` reserves a profiler metadata sequence while holding the shared queue lock, then releases the native queue lock before waiting on that sequence and calling `Profiler::SubmitGpuCommandLists()`. This preserves native queue execution order without reintroducing a queue-lock -> TimelineProfiler-lock / TimelineProfiler-lock -> queue-lock ABBA deadlock.
- If TimelineProfiler resolve `ExecuteCommandLists()` succeeds but the following resolve-fence `Signal()` fails, the affected query heap is treated as submitted without a retirement fence. GPU profiler advancement stops, the heap is disabled for further profiling, and the resources are retained instead of being reset or released through a normal drain path.
- TimelineProfiler readback resources used as `ResolveQueryData` destinations are created as `D3D12_RESOURCE_STATE_COPY_DEST`.

### Validation Contract

- A source-level regression test proves `DX12UIBridge.cpp` keeps both `DX12UIBridge::WaitForBackbufferReuse` and `DX12UIBridge::WaitForAllocatorReuse`, with the backbuffer wait before allocator reset/backbuffer barriers.
- A source-level regression test proves `Profiler.cpp` includes the shared queue synchronization header and uses `ScopedDX12QueueLock` around profiler resolve `ExecuteCommandLists()` and drain/resolve `Signal()`.
- Source-level regressions prove the shared queue synchronization registry is exported/process-lifetime, profiler metadata submission order is reserved under the native queue lock and published outside it, and TimelineProfiler readback resources start in copy-destination state.
- Existing runtime GPU lifecycle tests continue to prove GPU events are resolved and published.
- Manual/runtime DX12 editor validation records whether opening the Profiler panel still produces `DEVICE_HUNG` or quarantine logs. This is smoke evidence for the reported scenario, not a complete proof of every possible synchronization interleaving.

## Constitution Check Post-Design

- **Spec-first major change**: Pass. Artifacts remain in one feature directory.
- **Validation matches subsystem**: Pass. Tests cover the profiler queue contract and GPU event publication; runtime evidence is DX12-specific.
- **Generated/backend boundaries**: Pass. No generated files or non-DX12 backend paths are modified.
- **Incremental verified delivery**: Pass. Implementation is split across the DX12 UI bridge, DX12 queue synchronization, TimelineProfiler queue/readback handling, editor validation hook, and focused tests.
- **Product runtime preservation**: Pass. Design keeps Profiler functionality enabled.

## Complexity Tracking

No constitution violations.
