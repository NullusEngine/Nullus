# Feature Specification: DX12 UI Present Wait Reduction

**Feature Branch**: `047-dx12-ui-present-wait`  
**Created**: 2026-06-12  
**Status**: Draft  
**Input**: User description: "Optimize the visible main-thread `DX12UIBridge::WaitForBackbufferReuse` wait shown in TimelineProfiler captures without breaking DX12 UI synchronization."

## User Scenarios & Testing

### User Story 1 - UI Rendering Avoids Per-Backbuffer CPU Stalls (Priority: P1)

An editor user running with threaded rendering wants UI submission to avoid repeatedly blocking the main thread while waiting for the same swapchain backbuffer to become reusable.

**Why this priority**: The provided trace shows `DX12UIBridge::WaitFor...` inside the main-thread UI render path on nearly every frame. This hides useful editor work behind a CPU wait and makes frame time look worse than necessary.

**Independent Test**: Simulate multiple frames reusing swapchain backbuffers while UI command allocators have independent fence state; verify the bridge can select a completed allocator without requiring a backbuffer fence wait.

**Acceptance Scenarios**:

1. **Given** a swapchain backbuffer is reused before its previous UI fence is complete, **When** another completed UI command allocator is available, **Then** UI rendering records with that allocator and does not block the main thread for backbuffer reuse.
2. **Given** all UI command allocators are still in flight, **When** UI rendering needs to submit a new frame, **Then** the bridge waits for the oldest required allocator fence rather than unsafely resetting an in-flight allocator.

---

### User Story 2 - DX12 UI Synchronization Remains Correct (Priority: P2)

A rendering engineer wants the optimization to preserve command allocator lifetime, queue ordering, texture handle retirement, and UI composition fence behavior.

**Why this priority**: Removing the wait naively can reset command allocators while the GPU still uses them or release UI texture descriptors too early.

**Independent Test**: Unit tests cover allocator selection, exhausted-pool wait decisions, fence recording, and reset behavior across swapchain resize.

**Acceptance Scenarios**:

1. **Given** a UI command allocator was submitted with a fence value, **When** the fence is incomplete, **Then** that allocator is not selected for reset.
2. **Given** swapchain resources are recreated, **When** the bridge resets UI frame state, **Then** allocator/fence tracking is cleared and starts from a safe empty state.
3. **Given** UI work is submitted, **When** texture handles are retained or retired, **Then** they continue to use submitted fence values that represent real GPU completion.

---

### User Story 3 - Trace Waits Are Attributable (Priority: P3)

An engineer inspecting future TimelineProfiler captures wants any remaining UI wait to indicate whether it is allocator exhaustion, scene dependency, or another synchronization point.

**Why this priority**: The current shortened label in captures makes it easy to confuse idle worker sleeps with a real main-thread stall.

**Independent Test**: Source-level tests confirm the old per-backbuffer wait scope is removed or renamed and any remaining wait scopes are specific.

**Acceptance Scenarios**:

1. **Given** the UI bridge must wait because all allocator slots are in flight, **When** a trace is inspected, **Then** the wait is labelled as allocator reuse rather than generic backbuffer reuse.
2. **Given** worker threads are sleeping in `WaitForWake`, **When** a trace is inspected, **Then** they remain recognizable as idle worker waits and are not treated as the main-thread bottleneck.

### Edge Cases

- Swapchain buffer count may be 2 or more; allocator pool capacity must not assume a fixed double-buffer setup.
- GPU may lag multiple frames behind; the bridge must wait only when no safe allocator is available.
- Swapchain resize and bridge shutdown must still drain submitted UI work before releasing resources.
- Device lost or fence signal failure must preserve the existing quarantine behavior.
- Empty or invalid ImGui draw data must avoid submitting UI work and avoid advancing allocator fences.

## Requirements

### Functional Requirements

- **FR-001**: DX12 UI rendering MUST stop using swapchain backbuffer index as the sole command allocator reuse key.
- **FR-002**: DX12 UI rendering MUST select a command allocator whose recorded fence value is complete before resetting it.
- **FR-003**: DX12 UI rendering MUST wait only when every allocator in the UI allocator pool is still in flight.
- **FR-004**: The optimization MUST preserve scene wait semaphore ordering before UI command execution.
- **FR-005**: The optimization MUST preserve UI composition fence signalling and texture handle retirement semantics.
- **FR-006**: Swapchain resource reset MUST clear UI allocator tracking without leaking or reusing stale in-flight state.
- **FR-007**: Remaining main-thread waits MUST be labelled specifically enough to distinguish allocator exhaustion from scene waits.
- **FR-008**: Non-DX12 backends and no-UI-backend paths MUST retain their existing behavior.

### Key Entities

- **UI Allocator Slot**: A reusable DX12 command allocator with an associated submitted fence value.
- **Allocator Pool**: A collection of UI allocator slots sized to provide slack beyond swapchain backbuffer reuse.
- **Backbuffer Resource**: The swapchain image selected for the current frame; no longer the owner of allocator lifetime.
- **UI Fence Value**: The queue signal used to determine when UI command work and retained UI descriptors are safe to reuse or release.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Automated tests prove allocator selection avoids waiting when at least one allocator slot is complete, even if the current backbuffer's previous fence is incomplete.
- **SC-002**: Automated tests prove allocator selection requests a wait when all allocator slots are incomplete.
- **SC-003**: A DX12 editor trace after the change no longer shows `DX12UIBridge::WaitForBackbufferReuse` on every frame during the provided idle UI scenario.
- **SC-004**: Targeted DX12 UI bridge tests and the relevant threaded rendering/unit test subset pass after the change.

## Assumptions

- The main-thread wait in the provided capture is the `DX12UIBridge::WaitForBackbufferReuse` scope, not the worker-thread `WaitForWake` idle scopes.
- DX12 queue ordering already preserves scene/UI command order once command lists are submitted on the graphics queue.
- Command allocator reuse, not backbuffer ownership, is the CPU-side resource that requires fence-based protection before reset.
- Runtime validation is limited to Windows DX12 unless additional backend evidence is gathered.
