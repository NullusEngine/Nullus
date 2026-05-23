# Feature Specification: Editor Frame Sync Performance

**Feature Branch**: `011-editor-frame-sync-performance`  
**Created**: 2026-05-03  
**Status**: Draft  
**Input**: User description: "Editor FPS is only around 20; investigate the performance bottleneck and start fixing it."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Remove Per-Frame DX12 UI CPU Drain (Priority: P1)

As an editor user running the DX12 backend, I want the editor UI pass to avoid blocking the CPU every frame after submitting ImGui commands, so the editor can recover interactive frame rate while still presenting only after UI rendering is complete.

**Why this priority**: Investigation showed the dominant stall moves into `DX12UIBridge::RenderDrawData()` when SceneView drains are removed. The bridge currently signals a fence and immediately waits on the CPU every frame.

**Independent Test**: Unit tests validate per-backbuffer fence reuse and present wait value propagation; a DX12 editor runtime check verifies the UI pass no longer performs an unconditional queue drain.

**Acceptance Scenarios**:

1. **Given** a DX12 editor frame with a newly available backbuffer, **When** UI commands are submitted, **Then** the CPU does not wait immediately for the just-submitted UI work.
2. **Given** a DX12 backbuffer whose previous UI command allocator is still in use, **When** the same backbuffer is reused, **Then** the CPU waits only for that backbuffer's recorded fence value before resetting reusable resources.
3. **Given** the UI pass submits work with fence value N, **When** the swapchain is presented, **Then** the present queue waits on UI fence value N rather than a hardcoded value.

---

### User Story 2 - Preserve Resize And Shutdown Safety (Priority: P2)

As a developer maintaining renderer lifetime correctness, I want resize and shutdown paths to keep draining outstanding UI work before releasing resources, so performance changes do not introduce use-after-free or allocator reuse hazards.

**Why this priority**: Removing the per-frame wait is only safe if resource retirement and command allocator reuse are still synchronized at lifecycle boundaries.

**Independent Test**: Unit tests cover tracker reset behavior and build/runtime validation exercises swapchain resize or shutdown without DX12 validation errors.

**Acceptance Scenarios**:

1. **Given** swapchain UI resources are about to be released, **When** the release path runs, **Then** outstanding UI work is drained before backbuffers, allocators, descriptor heaps, or command lists are destroyed.
2. **Given** the bridge is recreated after resize, **When** new swapchain resources are allocated, **Then** old per-backbuffer fence state is not reused for the new image set.

---

### User Story 3 - Isolate Remaining SceneView Sync Stalls (Priority: P3)

As an editor user working in SceneView, I want the offscreen SceneView composition path to avoid unnecessary full-thread drains where safe, so the remaining frame-time bottleneck can be addressed without regressing texture lifetime safety.

**Why this priority**: Investigation showed SceneView's retirement-aware drain costs roughly 19-22 ms, and follow-up probes after the UI bridge fix showed UI composition still synchronously drained offscreen render/RHI work, holding the editor near 30 FPS.

**Independent Test**: Existing resize lifecycle tests plus policy and picking lifecycle tests validate when SceneView can consume retired textures without blocking, and when picking readback must wait for a readable submitted frame.

**Acceptance Scenarios**:

1. **Given** an already retired SceneView texture is available, **When** the UI draws SceneView, **Then** it can use the available texture without draining unrelated in-flight work.
2. **Given** resize or resource retirement requires exclusive ownership, **When** SceneView changes framebuffer lifetime, **Then** the safe drain/deferral policy remains in effect.
3. **Given** threaded picking submits an offscreen picking frame, **When** readback data is not yet available, **Then** picking returns no result instead of forcing a same-frame drain.
4. **Given** a previously submitted threaded picking frame becomes readable, **When** the user picks inside that frame's bounds, **Then** picking decodes from the readable frame metadata.
5. **Given** only offscreen threaded frames are in flight, **When** the UI starts its standalone explicit frame for composition, **Then** the UI path can proceed without synchronously draining offscreen render/RHI queues.
6. **Given** the threaded RHI worker currently owns the shared frame context, **When** the UI attempts to start a standalone explicit frame, **Then** the UI skips that submission without blocking the window thread or resetting the worker-owned frame context.

---

### User Story 4 - Profile Empty Scene Without DX12 Validation Overhead (Priority: P3)

As an editor user investigating the remaining empty-scene FPS after synchronization fixes, I want an explicit performance launch mode that disables expensive DX12 debug validation diagnostics, so I can distinguish renderer/editor cost from debug tooling cost without losing safe validation defaults for normal debug runs.

**Why this priority**: Follow-up investigation after the 20-30 FPS synchronization fixes showed an empty scene around 80 FPS while the Debug editor still enabled DX12 Debug Layer, GPU-based validation, and DRED diagnostics by default.

**Independent Test**: Editor launch-argument tests cover default validation safety and performance-mode opt-out behavior; a DX12 editor smoke run verifies performance mode starts responsively and logs no DRED/GPU-based validation enablement.

**Acceptance Scenarios**:

1. **Given** the editor is launched normally, **When** DX12 device resources are created, **Then** debug validation diagnostics remain enabled for developer safety.
2. **Given** the editor is launched with `--editor-performance-mode`, **When** DX12 device resources are created, **Then** expensive RHI debug validation diagnostics are disabled for profiling.
3. **Given** the editor is launched with `--no-rhi-debug-validation`, **When** DX12 device resources are created, **Then** RHI debug validation is disabled without requiring other performance-mode semantics.

### Edge Cases

- First use of a backbuffer has no prior fence value and must not wait.
- Fence value `0` means no submitted UI work is available for present to wait on.
- Backbuffer index outside the allocated UI resource range must still abort safely.
- Swapchain resource release and bridge shutdown may block intentionally to protect resource lifetime.
- Performance mode is an explicit profiling mode; normal editor debug launches keep validation diagnostics enabled.
- This fix is validated for DX12 on Windows only; other graphics backends are not claimed by this change.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: DX12 UI rendering MUST remove the unconditional CPU wait immediately after every UI command submission.
- **FR-002**: DX12 UI rendering MUST track submitted fence values per swapchain backbuffer before command allocator reuse.
- **FR-003**: DX12 UI rendering MUST wait before resetting a command allocator only when that backbuffer has an incomplete previously submitted fence value.
- **FR-004**: The UI bridge MUST expose both the native UI synchronization handle and the latest submitted UI fence value.
- **FR-005**: Present MUST wait on the UI fence value supplied by the current UI submission and MUST NOT use a hardcoded DX12 fence value.
- **FR-006**: Swapchain resize and shutdown paths MUST continue draining outstanding UI work before releasing DX12 UI resources.
- **FR-007**: The editor MUST remain runnable with the DX12 backend during and after the change.
- **FR-008**: Tests MUST cover the new synchronization policy before production code depends on it.
- **FR-009**: View rendering MUST only drain immediately after offscreen submission for consumers that require same-frame readback.
- **FR-010**: Threaded picking readback MUST use a submitted/readable frame lifecycle rather than forcing same-frame retirement.
- **FR-011**: UI composition MUST NOT synchronously drain offscreen-only threaded work before rendering UI; it MUST only reject composition while a threaded swapchain-targeting frame owns the swapchain frame context.
- **FR-012**: Standalone UI composition MUST NOT wait on or reset shared frame-context resources while the threaded RHI submission path owns them; it MUST fail fast for that UI submit attempt.
- **FR-013**: Editor launch arguments MUST expose an explicit performance mode that disables RHI debug validation diagnostics while preserving the existing default validation behavior.
- **FR-014**: DX12 DRED diagnostics MUST follow the same `DriverSettings::debugMode` gate as the DX12 Debug Layer and GPU-based validation.

### Key Entities

- **UI Frame Fence Tracker**: Per-backbuffer record of submitted UI fence values used to decide when CPU waits are required before allocator/resource reuse.
- **UI Signal Sync Handle**: Native backend handle plus submitted value used by present to sequence swapchain presentation after UI rendering.
- **Standalone UI Frame**: The explicit RHI frame used by UI rendering when the threaded renderer is not presenting a normal swapchain frame.
- **Picking Readback Frame**: Metadata for the submitted picking scene and render dimensions that becomes readable only after the renderer exposes an active readback source.
- **Editor Performance Mode**: Command-line launch mode that disables expensive RHI debug validation diagnostics for FPS profiling while leaving normal debug launches validation-first.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Unit tests fail before implementation and pass after implementation for per-backbuffer fence reuse and UI present value propagation.
- **SC-002**: `NullusUnitTests` passes after synchronization changes.
- **SC-003**: `Editor` builds successfully after synchronization changes.
- **SC-004**: DX12 editor runtime verification shows the UI bridge no longer contains the measured per-frame CPU fence drain that held editor FPS near 20.
- **SC-005**: DX12 editor runtime verification after removing the UI composition offscreen drain shows frame time no longer dominated by synchronous offscreen render/RHI queue draining at the UI boundary.
- **SC-006**: DX12 editor runtime verification shows startup remains responsive after the offscreen-drain removal; the editor window must continue responding during a 15-second DX12 launch sample.
- **SC-007**: DX12 editor performance-mode runtime verification shows no DRED, Debug Layer, or GPU-based validation enablement logs while the window remains responsive during a 15-second launch sample.

## Assumptions

- The initial fix targets Windows DX12 because the measured stall and implementation path are DX12-specific.
- Debug/GPU validation mode is intentionally preserved by default, but follow-up FPS profiling should use `--editor-performance-mode` to measure renderer/editor cost without validation overhead.
- SceneView/offscreen drains are a follow-up within the same spec unless the DX12 UI bridge fix fully resolves the observed issue.
- Generated files under `Runtime/*/Gen/` are out of scope and must not be hand-edited.
