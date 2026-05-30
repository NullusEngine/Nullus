# Feature Specification: Scene View Camera State Machine

**Feature Branch**: `040-scene-view-camera-state-machine`  
**Created**: 2026-05-30  
**Status**: Draft  
**Input**: User description: "Refactor SceneView camera control to use an explicit state machine so cursor transitions are event-driven instead of set every frame, and pan/orbit/fly/text-input blocking behaviors are modeled as camera interaction states."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Stable Text Entry In Scene View (Priority: P1)

As an editor user, I can click into numeric or text inputs that appear within or alongside the Scene View and complete text entry without the scene camera logic reclaiming cursor control or interrupting editing.

**Why this priority**: This is the directly reported regression. If text entry is unstable, routine transform and property editing becomes frustrating and error-prone.

**Independent Test**: Can be fully tested by focusing a Scene View-adjacent input, typing and selecting text, and verifying that camera interaction stays inactive and the text-edit cursor remains stable throughout the edit session.

**Acceptance Scenarios**:

1. **Given** a text-editable control is active and requesting text input, **When** the pointer remains over the Scene View area, **Then** the Scene View camera interaction model does not switch into any navigation mode or replace the text-edit cursor.
2. **Given** a Scene View camera interaction was previously active, **When** text entry starts, **Then** the camera interaction is cancelled or suspended and cursor ownership returns to the text-editing control.

---

### User Story 2 - Predictable Navigation Mode Transitions (Priority: P2)

As an editor user, I can still pan, orbit, and fly the Scene View camera with the existing mouse controls, but cursor changes occur only when the navigation mode changes rather than every frame.

**Why this priority**: The interaction regression is tied to cursor ownership, but the editor must preserve current camera navigation behavior while making cursor handling stable and intentional.

**Independent Test**: Can be fully tested by entering and leaving each navigation gesture independently and verifying that the camera responds correctly, the expected cursor appears for that mode, and the cursor returns to its neutral state after the mode ends.

**Acceptance Scenarios**:

1. **Given** the Scene View is active and no UI text control is editing, **When** the user starts the fly-camera gesture, **Then** the camera enters the fly navigation mode and applies the fly cursor for the duration of that mode only.
2. **Given** the Scene View is active and no UI text control is editing, **When** the user starts middle-mouse pan or orbit with the existing modifier rules, **Then** the correct navigation mode is entered and the corresponding cursor is shown until the mode exits.
3. **Given** a navigation mode ends because mouse buttons are released, focus is lost, or input is blocked, **When** the interaction model returns to neutral, **Then** cursor ownership is released exactly once and no navigation cursor remains active.
4. **Given** the cursor had a non-default shape before a Scene View navigation gesture began, **When** the gesture ends, **Then** the cursor returns to that captured pre-navigation shape rather than always falling back to an arrow.

---

### User Story 3 - Centralized Interaction Model For Contributors (Priority: P3)

As an editor contributor, I can reason about Scene View camera behavior through one centralized interaction model instead of scattered per-frame cursor updates and mode-specific side effects.

**Why this priority**: The long-term value of this change is maintainability. Cursor ownership, input capture, and mode transitions need one source of truth so future editor interactions do not reintroduce this class of bug.

**Independent Test**: Can be fully tested by reviewing the interaction map and targeted tests for each camera mode and block condition, confirming that contributors can add or adjust a mode without touching unrelated per-frame cursor branches.

**Acceptance Scenarios**:

1. **Given** a contributor adds or changes a Scene View camera interaction mode, **When** they update the interaction model, **Then** the mode's entry conditions, exit conditions, cursor ownership, and cleanup behavior are defined in one place.
2. **Given** a new blocking condition such as active text entry or editor modal state is introduced, **When** the contributor updates the interaction model, **Then** the camera can be prevented from activating without editing multiple unrelated cursor-setting paths.

---

### Edge Cases

- Text input starts while a pan, orbit, or fly interaction is already active.
- The Scene View loses focus or a blocking window opens while a navigation mode owns the cursor.
- The user changes modifiers during a middle-mouse interaction, such as switching between pan and orbit expectations.
- Mouse button release is missed by the interaction initiator but the underlying button state is no longer down.
- A Scene View overlay, gizmo, or embedded UI control is active while the pointer still remains inside Scene View bounds.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The Scene View camera interaction system MUST model camera navigation through explicit interaction states rather than relying on unconditional per-frame cursor reassignment.
- **FR-002**: The system MUST block Scene View camera interaction whenever text entry is active, including when the active text-editing control is inside Scene View bounds.
- **FR-003**: The system MUST preserve the current user-facing camera gestures for fly, pan, and orbit, including the existing modifier expectations for each gesture.
- **FR-004**: The system MUST assign camera navigation cursors when entering a navigation state and release them when leaving that state, without reapplying the same cursor continuously while the state remains unchanged.
- **FR-005**: The system MUST provide a neutral camera state that restores the cursor shape captured before camera navigation, disables camera-specific cursor capture, and clears any transient mouse interaction flags when navigation is not active.
- **FR-006**: The system MUST define deterministic exit behavior for every camera interaction state when focus is lost, a blocking editor context opens, text entry begins, or mouse button ownership no longer matches the active state.
- **FR-007**: The system MUST keep Scene View camera motion behavior and camera focus updates consistent with the active interaction state so that blocked or neutral states do not continue to mutate the camera.
- **FR-008**: The system MUST coordinate Scene View camera cursor ownership with UI-managed cursor ownership so that text-editing, resize, and other editor cursors are not overridden by camera state updates.
- **FR-009**: The system MUST expose the camera interaction rules in a form that supports targeted automated tests for state entry, state exit, blocking conditions, and cursor ownership transitions.

### Key Entities *(include if feature involves data)*

- **Camera Interaction State**: A named Scene View camera mode such as neutral, fly, pan, orbit, or blocked, including its cursor ownership, movement behavior, and cleanup requirements.
- **Interaction Trigger**: A user or editor condition that can enter, exit, or block a camera interaction state, such as mouse button changes, modifier keys, focus loss, text entry, or shortcut modal windows.
- **Cursor Ownership Session**: The period during which either the Scene View camera or a UI control is responsible for the visible cursor shape and any associated cursor capture behavior.
- **Input Block Condition**: A rule that prevents or cancels camera interaction, such as active text input, shortcut settings windows, or other editor-controlled modal states.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: During manual validation of Scene View transform or property editing, the text-edit cursor remains stable for the full duration of an active text-entry session and does not alternate with camera or default cursors.
- **SC-002**: Entering and exiting each supported Scene View navigation mode results in one visible cursor transition on entry and one visible cursor transition on exit, with no repeated cursor churn while the mode remains active.
- **SC-003**: Existing Scene View fly, pan, and orbit gestures continue to produce the same camera movement results in targeted regression tests and focused manual validation.
- **SC-004**: Automated tests cover the neutral, blocked, fly, pan, and orbit interaction paths, including at least one text-entry blocking scenario and one forced-reset scenario.

## Assumptions

- The current Scene View mouse gestures and keyboard shortcuts remain the intended user behavior for this iteration; this change is about interaction control flow, not new navigation features.
- This change is limited to editor-side Scene View camera interaction and cursor ownership; gameplay camera systems and unrelated editor panels are out of scope.
- Existing UI cursor ownership pathways, including text input and resize cursors, remain the authoritative source whenever the camera interaction system is blocked.
- The first implementation phase may centralize state rules inside the current camera controller and Scene View collaboration boundary before considering broader editor-wide interaction orchestration.
