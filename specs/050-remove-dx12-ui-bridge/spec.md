# Feature Specification: Remove DX12 Legacy UI Bridge

**Feature Branch**: `050-remove-dx12-ui-bridge`  
**Created**: 2026-06-14  
**Status**: Draft  
**Input**: User description: "Remove DX12 legacy UI bridge/direct-submit path"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - FrameGraph Only On DX12 (Priority: P1)

As a DX12 user, UI rendering should go through the RHI frame graph path only, with no legacy direct-submit bridge selected at runtime.

**Why this priority**: This removes the old DX12 UI execution path that can conflict with the new overlay flow and cause duplicate ownership or stalls.

**Independent Test**: Start Editor or Launcher on DX12 and confirm UI renders normally while the legacy DX12 bridge path is never selected.

**Acceptance Scenarios**:

1. **Given** DX12 is active and UI overlay frame-graph support is available, **When** the UI system initializes, **Then** it selects the frame-graph path and does not create a direct-submit DX12 bridge.
2. **Given** DX12 is active during steady-state rendering, **When** UI is drawn, **Then** no legacy DX12 UI submit, signal, or wait path is executed.

---

### User Story 2 - Unsupported Backends Fail Closed (Priority: P2)

As a user on a backend without the migrated overlay path, the system should not silently fall back to the old DX12 bridge behavior.

**Why this priority**: The feature must remove legacy DX12 behavior without reintroducing hidden fallback semantics.

**Independent Test**: Start an unsupported configuration and verify the UI path reports a clear unsupported state instead of selecting the removed bridge.

**Acceptance Scenarios**:

1. **Given** a backend without the migrated overlay capability, **When** UI bridge creation is requested, **Then** the system returns a null/unsupported bridge rather than a legacy DX12 bridge.
2. **Given** legacy DX12 bridge code is unavailable, **When** startup or runtime chooses a UI path, **Then** the system surfaces an explicit unsupported outcome instead of silent fallback.

---

### User Story 3 - Platform Backends Stay Intact (Priority: P3)

As an editor or launcher user, mouse/keyboard input and platform window handling should continue to work after the DX12 bridge removal.

**Why this priority**: The legacy renderer bridge is being removed, not the platform/input backend.

**Independent Test**: Launch Editor and Launcher and verify Win32/GLFW input handling remains functional while UI rendering still works through the frame graph.

**Acceptance Scenarios**:

1. **Given** the app is running on Windows, **When** the window receives input, **Then** the Win32 or GLFW platform backend still processes events normally.
2. **Given** the legacy DX12 bridge is removed, **When** UI is rendered in Editor or Launcher, **Then** platform backend initialization and event handling still function.

---

### Edge Cases

- What happens when DX12 overlay support is absent but the old bridge code is no longer present?
- What happens if a caller still asks for direct-submit UI rendering on DX12?
- What happens during resize or shutdown when no legacy bridge exists to drain in-flight UI work?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST not instantiate or return the legacy DX12 UI bridge when the frame-graph overlay path is available.
- **FR-002**: The system MUST remove DX12 direct-submit UI rendering from the supported runtime path.
- **FR-003**: The system MUST preserve platform backend initialization and input processing for Editor and Launcher UI flows.
- **FR-004**: The system MUST return an explicit unsupported or null UI bridge when no supported migrated UI path is available.
- **FR-005**: The system MUST not reintroduce hidden fallback behavior to the removed DX12 legacy bridge.
- **FR-006**: The system MUST keep UI rendering functional on the migrated frame-graph path after removal of the legacy bridge.

### Key Entities *(include if data involved)*

- **UI Bridge Selection**: The runtime decision that chooses between the migrated frame-graph path and the removed legacy bridge path.
- **Backend Capability State**: Whether the active render backend supports the migrated overlay path.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On DX12 migrated runs, zero frames select the legacy direct-submit UI bridge.
- **SC-002**: Editor and Launcher remain able to open, render, and process input with no regression in basic UI interaction.
- **SC-003**: Unsupported configurations produce a clear unsupported UI path instead of a legacy DX12 fallback.
- **SC-004**: Validation confirms the removed bridge path does not appear in runtime selection or execution for migrated DX12 runs.

## Assumptions

- The migrated RHI frame-graph UI overlay path is already in place and remains the active DX12 rendering path.
- Only the legacy DX12 UI bridge/direct-submit path is being removed; platform backends such as Win32 and GLFW remain in use.
- This feature does not change non-DX12 renderer behavior beyond explicit unsupported-path handling.
