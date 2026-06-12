# Feature Specification: Fix Profiler DX12 Hang

**Feature Branch**: `048-fix-profiler-dx12-hang`
**Created**: 2026-06-12
**Status**: Implemented
**Input**: User description: "Opening the Profiler window on DX12 logs `DX12UIBridge::RenderDrawData: device status after ExecuteCommandLists hr=-2005270522`, `NativeDX12Queue::Submit: device status after ExecuteCommandLists hr=-2005270522`, and subsequent picking readbacks fail because the RHI device is lost or GPU work is quarantined."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Open Profiler Without Losing DX12 Device (Priority: P1)

As an editor user running the DX12 backend, I want to open the Profiler window while the scene continues rendering, so profiling does not make the editor enter device-lost/quarantine state.

**Why this priority**: The reported failure immediately breaks the editor session: UI submission reports `DEVICE_HUNG`, standalone UI submit quarantines RHI resources, and picking readbacks are rejected afterward.

**Independent Test**: Run the focused TimelineProfiler GPU lifecycle regression and open the editor Profiler window under DX12; no device-lost or unsafe GPU quarantine diagnostics should be emitted from opening the panel.

**Acceptance Scenarios**:

1. **Given** the editor is running on DX12 and the Profiler panel is closed, **When** the user opens the Profiler panel, **Then** the editor continues submitting UI and scene frames without `DEVICE_HUNG`, device-lost, or unsafe GPU quarantine logs.
2. **Given** the DX12 UI bridge is rendering to a swapchain backbuffer, **When** allocator reuse optimization can select a completed allocator, **Then** the bridge must still wait for the same backbuffer's previous UI fence before resetting/recording work that transitions that backbuffer.
3. **Given** TimelineProfiler GPU scopes have been initialized, **When** profiler GPU readback/resolve work is submitted, **Then** it follows the same graphics queue ordering contract as normal scene/UI/upload/readback work and publishes profiler metadata in the same order as native queue execution.

---

### User Story 2 - Keep Timeline GPU Data Available (Priority: P2)

As a rendering engineer, I want the Profiler panel to keep showing CPU and supported GPU timeline data after the fix, so the safety change does not silently disable useful profiler functionality.

**Why this priority**: The Profiler window exists to inspect live render/editor costs. A fix that simply disables TimelineProfiler GPU scopes would avoid the hang but remove the feature value.

**Independent Test**: Run the existing non-empty GPU frame TimelineProfiler test and confirm a resolved GPU event is published.

**Acceptance Scenarios**:

1. **Given** a supported DX12 graphics queue and a GPU-scoped command list, **When** the command list is submitted and the profiler advances frames, **Then** the timeline records a valid GPU event.

### Edge Cases

- Empty profiler frames must not submit unnecessary GPU readback work.
- Unsupported queue types must remain filtered before GPU callbacks or query recording.
- Profiler shutdown must still drain submitted resolve work before releasing GPU resources.
- DX12 UI allocator-pool reuse must not bypass per-backbuffer reuse safety.
- Shared DX12 queue synchronization state must not be destroyed while profiler or raw queue users can still hold queue-lock objects.
- TimelineProfiler readback resources used as `ResolveQueryData` destinations must start in a valid copy-destination state.
- Device lost or fence failures must remain diagnosable and must not release in-flight resources unsafely.
- TimelineProfiler resolve work that was accepted by the GPU queue but cannot be fenced must stop GPU profiler advancement and retain affected resources instead of reusing or releasing them as normally retired work.
- The fix is DX12-specific and must not claim Vulkan, OpenGL, Linux, or macOS validation.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Opening the Profiler panel on DX12 MUST NOT trigger device-lost or unsafe GPU quarantine in ordinary editor rendering.
- **FR-002**: DX12 UI bridge MUST wait for previous submitted UI work for the current swapchain backbuffer before resetting/recording a command list that transitions that backbuffer.
- **FR-003**: TimelineProfiler-owned DX12 queue submissions and queue fence signals MUST obey the same serialized queue-operation contract used by normal RHI, UI bridge, upload, and readback work.
- **FR-004**: Supported TimelineProfiler GPU scopes MUST continue to record and publish resolved GPU events.
- **FR-005**: Unsupported or ambiguous DX12 queue contexts MUST remain filtered and MUST NOT advertise GPU scope support.
- **FR-006**: Empty profiler frames MUST continue to avoid unnecessary GPU readback submissions.
- **FR-007**: Shutdown and resource release MUST preserve existing drain/quarantine behavior for submitted profiler GPU work.
- **FR-008**: Validation MUST include focused automated regressions for the DX12 UI backbuffer/allocator ordering contract, the profiler queue-ordering contract, and the existing runtime GPU event publication path.
- **FR-009**: Shared DX12 queue synchronization state MUST have stable lifetime across `NativeDX12Queue` wrapper teardown/reinit so raw profiler queue users cannot observe a destroyed mutex.
- **FR-010**: TimelineProfiler GPU metadata publication MUST preserve native queue submission order without calling profiler callbacks while the shared native queue lock is held.
- **FR-011**: TimelineProfiler readback buffers used by `ResolveQueryData` MUST be created in `D3D12_RESOURCE_STATE_COPY_DEST`.
- **FR-012**: TimelineProfiler resolve submissions that pass `ExecuteCommandLists` but fail the subsequent resolve-fence `Signal` MUST quarantine the affected GPU profiler work and MUST NOT advance, reset, drain, or release those resources through the normal fenced path.

### Key Entities

- **TimelineProfiler GPU Resolve Work**: Internal query resolve command-list and fence work submitted by the profiler to make GPU timestamps readable.
- **DX12 UI Backbuffer Reuse Fence**: The UI bridge fence value recorded for the last submitted UI command list that touched a swapchain backbuffer.
- **DX12 UI Allocator Reuse Pool**: The pool of command allocators introduced by `047-dx12-ui-present-wait` to reduce stalls, which must not replace backbuffer safety waits.
- **DX12 Graphics Queue Transaction**: A sequence of queue waits, command-list execution, and fence signals that must be serialized with other users of the same native queue.
- **Profiler Panel Recording State**: Whether the editor Profiler panel is open and TimelineProfiler is actively receiving CPU/GPU events.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The DX12 UI bridge regression proves backbuffer reuse waiting remains present before command allocator reset and backbuffer resource barriers.
- **SC-002**: The focused TimelineProfiler GPU lifecycle test suite passes with a regression proving profiler resolve queue operations use the shared DX12 queue lock.
- **SC-003**: The existing non-empty GPU frame profiler test still publishes at least one valid GPU event.
- **SC-004**: A DX12 editor run that opens the Profiler panel no longer logs `DX12UIBridge::RenderDrawData: device status after ExecuteCommandLists hr=-2005270522`, `CreateNativeDX12Texture` failures, or follow-on RHI quarantine caused by opening the panel.
- **SC-005**: The fix changes only the DX12 UI/profiler queue synchronization surface and does not hand-edit generated files.

## Assumptions

- The observed HRESULT `-2005270522` is treated as the DX12 backend failure to prevent; this spec does not broaden acceptance to other backends.
- RenderDoc/DRED evidence may be limited if the device is already removed before capture or DRED settings are unavailable in the local environment.
- Investigation traced the primary regression to merge commit `e9c928ee52358342024c998b5d8851ee963327ac` / implementation commit `5740622c5272d2f4ca0881442b7d44d779d4c33b`, which removed the UI bridge's per-backbuffer reuse wait while adding allocator-pool reuse.
- The Profiler panel should remain functionally enabled; disabling TimelineProfiler GPU scopes is not an acceptable primary fix.
