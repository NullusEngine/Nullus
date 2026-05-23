# Feature Specification: Editor Settings Trim

**Feature Branch**: `024-editor-settings-trim`
**Created**: 2026-05-21
**Status**: Draft
**Input**: User description: "移除诊断/自动化开关 和无用项，剩下的能合并成一个就合并成一个，只留下真正对用户有用的"

## User Scenarios & Testing

### User Story 1 - Focus Project Settings On User Preferences (Priority: P1)

As an editor user, I want the Project Settings panel to show only settings that change normal editor behavior, so the panel stays understandable and does not expose validation-only or no-op controls.

**Why this priority**: This is the direct user-facing cleanup. It removes confusing controls and keeps persistent settings meaningful.

**Independent Test**: Open or inspect reflected editor settings and verify that persistent runtime/debug-draw settings no longer include diagnostic, automation, or no-op visualizer fields.

**Acceptance Scenarios**:

1. **Given** reflected editor settings are registered, **When** Project Settings enumerates runtime fields, **Then** launch validation, render diagnostic logging, pass-disable, and one-off profiling fields are absent.
2. **Given** reflected debug-draw settings are registered, **When** Project Settings enumerates debug-draw fields, **Then** only real debug draw toggles and light billboard scale remain.

---

### User Story 2 - Preserve One-Shot Diagnostics For Automation (Priority: P2)

As a developer running validation or rendering diagnostics, I want one-shot launch arguments to keep working without becoming persistent user settings, so automated checks remain possible without polluting project settings.

**Why this priority**: Existing validation workflows depend on CLI diagnostics. The cleanup must not remove those workflows.

**Independent Test**: Parse existing editor validation and logging CLI arguments and verify they still populate `EngineDiagnosticsSettings`.

**Acceptance Scenarios**:

1. **Given** the editor is launched with validation view, object selection, readback, or scene camera arguments, **When** launch args are parsed, **Then** the diagnostics override contains those values.
2. **Given** the editor is launched with render draw-path diagnostics, **When** launch args are parsed, **Then** draw-path and DX12 frame-flow logging are enabled for that run only.

### Edge Cases

- Existing `editor-settings.json` files may still contain removed fields; loading must ignore them rather than failing.
- Runtime diagnostic structures may keep fields for CLI and test harness use even when those fields are no longer persisted.

## Requirements

### Functional Requirements

- **FR-001**: Persistent `EditorRuntimeSettingsObject` MUST keep only user-facing runtime preferences: threaded rendering and RenderDoc startup preferences.
- **FR-002**: Persistent `EditorDebugDrawSettingsObject` MUST keep only controls that have runtime-visible behavior: enabled, grid, bounds, cameras, lights, and light billboard scale.
- **FR-003**: Project Settings MUST NOT expose launch validation fields, render diagnostic logging fields, pass-disable fields, grid sub-part disable fields, or no-op frustum visualizer fields.
- **FR-004**: Existing editor validation CLI arguments MUST continue to populate `EngineDiagnosticsSettings`.
- **FR-005**: Existing render draw-path CLI diagnostics MUST continue to populate `EngineDiagnosticsSettings`.
- **FR-006**: Loading settings files containing removed fields MUST remain non-fatal and ignore those fields.
- **FR-007**: Reflection generated outputs MUST be regenerated through the MetaParser build flow.

### Key Entities

- **Editor Runtime Settings**: Persisted user-facing runtime preferences for the editor.
- **Editor Debug Draw Settings**: Persisted user-facing debug visualization preferences.
- **Engine Diagnostics Settings**: Non-persisted runtime carrier for CLI/test diagnostics and validation automation.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Reflected Project Settings runtime fields contain zero removed diagnostic/automation fields.
- **SC-002**: Reflected Project Settings debug-draw fields contain zero no-op visualizer fields.
- **SC-003**: Existing editor CLI diagnostics tests continue to pass.
- **SC-004**: Settings persistence tests continue to pass when loading files with unknown or invalid fields.

## Assumptions

- Diagnostic and automation settings are still useful as one-shot CLI overrides, but not as persistent user preferences.
- Removed persisted fields do not require migration beyond being ignored during load.
- RenderDoc startup preferences remain user-facing enough to persist.
