# Feature Specification: FrameInfo Render View Snapshot

**Feature Branch**: `fix-frameinfo-render-view-snapshot`  
**Created**: 2026-05-26  
**Status**: Implemented  
**Input**: User description: "FrameInfo opens as an independent dockable panel, but it must show only render viewport frame information and must not freeze editor UI/rendering."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Open FrameInfo Without Freezing The Editor (Priority: P1)

An editor user can open the FrameInfo panel while Scene View, Game View, or Asset View is active, and the editor continues rendering and updating instead of stalling on live telemetry collection.

**Why this priority**: This is the reported regression and blocks normal editor use.

**Independent Test**: Open or refresh FrameInfo while threaded lifecycle telemetry is busy; the refresh returns from cached view-owned data without blocking.

**Acceptance Scenarios**:

1. **Given** a render view has a last successful frame snapshot, **When** FrameInfo refreshes, **Then** it displays that snapshot without querying the renderer live.
2. **Given** threaded lifecycle telemetry is locked or unavailable, **When** FrameInfo refreshes, **Then** the panel refresh returns and keeps showing the last safe view snapshot.
3. **Given** threaded offscreen render submissions are active, **When** the editor UI needs a standalone swapchain frame for FrameInfo, **Then** background RHI workers yield until the UI frame can acquire the frame context and present.

---

### User Story 2 - Show Render View Metrics Only (Priority: P2)

An editor user sees metrics for the selected or focused render viewport, not global editor UI draw timings or unrelated panel statistics.

**Why this priority**: The panel is useful only if its scope is clear and tied to the render view being inspected.

**Independent Test**: Refresh FrameInfo with Scene/Game/Asset view snapshots and assert render frame counters are present while editor panel draw metrics are absent.

**Acceptance Scenarios**:

1. **Given** a render view has produced a frame, **When** FrameInfo targets that view, **Then** it shows the target view name and renderer frame counters.
2. **Given** FrameInfo is open, **When** other editor UI panels render, **Then** FrameInfo does not include editor UI panel draw duration metrics.

---

### User Story 3 - Keep RHI/Threaded Rendering Failures Fail-Closed (Priority: P3)

When frame fences fail or telemetry cannot be read immediately, the renderer and editor avoid unsafe resource reuse and avoid blocking UI paths longer than necessary.

**Why this priority**: The FrameInfo symptom exposed nearby RHI synchronization hazards that could cause freezes or resource lifetime bugs.

**Independent Test**: Contract tests simulate standalone, standalone UI, and threaded RHI fence wait failures and verify frame resources are not reset or reused.

**Acceptance Scenarios**:

1. **Given** a standalone frame fence wait fails, **When** the renderer begins a frame, **Then** command/fence/semaphore resources are not reset and no submit/present happens.
2. **Given** a threaded RHI reusable frame fence wait fails, **When** threaded drain runs, **Then** the frame context is not reset, acquired, submitted, or presented.

### Edge Cases

- No target view is open: FrameInfo shows an empty "None" snapshot without touching renderer telemetry.
- A view has not rendered yet: FrameInfo shows a zeroed render snapshot for that view.
- Threaded telemetry mutex is busy: FrameInfo and AView render paths use try-read behavior and retain the last safe snapshot.
- A resize or immediate readback requires a drain but drain cannot complete: the view keeps the pending size/snapshot instead of releasing or resizing resources.
- Fence wait failure happens before resource reuse: standalone, UI, and threaded RHI paths fail closed.
- UI rendering fails or is skipped before producing a fence value: present ignores the invalid UI signal instead of waiting on a DX12 fence value of zero.
- Offscreen threaded RHI work briefly owns the shared frame-context lock: UI standalone frame preparation keeps its pending request alive across a bounded wait timeout so the next UI frame is not starved by another worker submission.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: FrameInfo MUST remain an independent dockable panel registered in the editor window menu.
- **FR-002**: FrameInfo MUST display only the selected render view's frame snapshot and MUST NOT display editor UI panel draw metrics.
- **FR-003**: FrameInfo refresh MUST read `AView`'s last rendered snapshot and MUST NOT query live renderer telemetry during panel drawing.
- **FR-004**: `AView` MUST publish a frame-info snapshot after successful render completion and retain the last successful snapshot when a later render is skipped or invalid.
- **FR-005**: Threaded frame telemetry reads used by editor view presentation MUST be non-blocking where they run on editor/UI paths.
- **FR-006**: Resize and immediate readback drains MUST preserve existing view resources when drain cannot complete.
- **FR-007**: RHI standalone, standalone UI, and threaded frame begin paths MUST NOT reset or reuse frame resources after a frame fence wait failure.
- **FR-008**: UI composition wait semaphore setup MUST clear stale waits when the current render view has no scene-to-UI semaphore.
- **FR-009**: UI-to-present synchronization MUST NOT publish or wait on a UI completion fence value of zero.
- **FR-010**: DX12 semaphore reset MUST NOT reset the underlying D3D12 fence or its monotonic signal counter to zero; only the per-frame pending wait value may be cleared.
- **FR-011**: Threaded RHI workers MUST yield while a standalone editor UI frame is pending so the editor swapchain present path is not starved by continuous offscreen render-view submissions.

### Key Entities

- **FrameInfo Snapshot**: A value copy of renderer frame counters and threaded lifecycle state owned by an `AView`.
- **Target Render View**: The Scene View, Game View, or Asset View currently selected for FrameInfo display.
- **Threaded Frame Telemetry**: Nonblocking lifecycle counters used to annotate snapshots and overlay presentation state.
- **RHI Frame Context**: Per-frame resources that require fence completion before reset or reuse.
- **DX12 Semaphore Fence Value**: The monotonically increasing D3D12 fence value used to order queue waits/signals across frames.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: `PanelWindowHookTests.*` passes, covering FrameInfo refresh, snapshot retention, busy telemetry, and post-render drain telemetry.
- **SC-002**: Renderer/RHI contract tests pass for standalone, UI, and threaded fence failure fail-closed behavior.
- **SC-003**: `git diff --check` reports no whitespace errors.
- **SC-004**: No files under `Runtime/*/Gen/` or `Project/*/Gen/` are modified by hand.
- **SC-005**: DX12 editor smoke validation with `--editor-validation-open-frame-info` produces continuous UI submits and presents without `DXGI_ERROR_DEVICE_REMOVED`, zero-fence waits, texture creation failures, or submit/present failures.

## Assumptions

- The immediate fix can be validated with focused editor/rendering unit and contract tests because the reported freeze is caused by editor/runtime synchronization paths with stable test hooks.
- Manual editor runtime verification and RenderDoc capture remain useful follow-up evidence, but were not required to prove the nonblocking panel-refresh regression tests.
- Existing generated reflection output may be regenerated by normal build tooling, but generated files are not hand-edited.
