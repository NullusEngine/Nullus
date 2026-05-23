# Feature Specification: LightGrid Performance Toggle

**Feature Branch**: `020-lightgrid-performance-toggle`  
**Created**: 2026-05-10  
**Status**: Draft  
**Input**: User description: "LightGrid做成可以控制的开关；修复性能卡点；开关 + 缓存优化，同时加 Editor Project Settings UI"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Disable LightGrid From Editor Settings (Priority: P1)

An editor user can open Project Settings and turn the LightGrid rendering path on or off for profiling or low-cost rendering experiments without rebuilding the engine.

**Why this priority**: The user explicitly needs a controllable switch. It also provides the fastest way to prove whether LightGrid is the current frame-time bottleneck.

**Independent Test**: Can be tested by opening Project Settings, changing the LightGrid setting, rendering a scene, and observing that the setting is persisted and affects subsequent scene rendering.

**Acceptance Scenarios**:

1. **Given** a project using the default settings, **When** the user opens Project Settings, **Then** LightGrid is shown as enabled.
2. **Given** LightGrid is enabled, **When** the user disables it in Project Settings, **Then** scene rendering skips clustered LightGrid preparation and continues to render without crashing.
3. **Given** the user disabled LightGrid and restarts the editor, **When** Project Settings loads, **Then** LightGrid remains disabled for that project.

---

### User Story 2 - Preserve Existing Rendering By Default (Priority: P1)

Existing projects keep the current LightGrid-enabled rendering behavior unless the user explicitly disables it.

**Why this priority**: Rendering changes are high risk. The default must avoid surprising visual regressions.

**Independent Test**: Can be tested by loading an existing project with no new setting value and verifying that LightGrid-dependent passes are still active.

**Acceptance Scenarios**:

1. **Given** a project without an explicit LightGrid setting, **When** the editor or game starts, **Then** LightGrid is enabled.
2. **Given** LightGrid is enabled, **When** forward or deferred scene rendering runs, **Then** LightGrid preparation remains available to the scene graph.

---

### User Story 3 - Reduce Duplicate LightGrid Preparation (Priority: P2)

When LightGrid is enabled, threaded scene rendering avoids repeated per-frame LightGrid preparation work for the same scene frame.

**Why this priority**: Tracy evidence shows LightGrid preparation on the main rendering path as the likely CPU-side frame-time hotspot.

**Independent Test**: Can be tested by rendering a threaded scene frame and verifying that LightGrid prepared inputs/context are captured once per scene frame and reused by the deferred render-scene package builder.

**Acceptance Scenarios**:

1. **Given** threaded rendering is enabled and LightGrid is enabled, **When** a scene frame is prepared, **Then** the expensive LightGrid preparation path is not run more than once for that frame.
2. **Given** LightGrid inputs change between frames, **When** the next frame is prepared, **Then** the new frame uses the updated LightGrid inputs.

### Edge Cases

- If LightGrid is disabled while a scene has lights, scene rendering must not crash and must use a no-LightGrid fallback for clustered lighting data.
- If LightGrid is re-enabled after being disabled, the next rendered frame must rebuild the needed LightGrid preparation state.
- If threaded rendering is disabled, the setting must still control LightGrid use in the synchronous frame graph path.
- If a project settings file lacks the new setting, the system must treat LightGrid as enabled.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST expose a project-level LightGrid enabled setting in Editor Project Settings.
- **FR-002**: The LightGrid setting MUST default to enabled for existing and newly created projects unless explicitly changed.
- **FR-003**: The editor MUST persist the LightGrid setting with other project/editor settings so it survives restart.
- **FR-004**: Scene rendering MUST skip LightGrid prepass preparation and LightGrid graph integration when the setting is disabled.
- **FR-005**: Scene rendering MUST continue producing a valid frame when LightGrid is disabled.
- **FR-006**: Forward and deferred scene renderers MUST observe the same LightGrid setting.
- **FR-007**: Threaded rendering MUST avoid duplicate LightGrid preparation work for the same scene frame when LightGrid is enabled.
- **FR-008**: Re-enabling LightGrid MUST restore LightGrid preparation on the next applicable frame.
- **FR-009**: Tests MUST cover the default enabled behavior, disabled rendering path, setting persistence surface, and duplicate-preparation prevention.

### Key Entities

- **LightGrid Rendering Setting**: Project-level boolean that controls whether clustered LightGrid preparation and graph integration are active.
- **LightGrid Prepared Frame State**: Per-frame data captured from scene lighting and frame descriptor inputs that can be reused while building the render scene package for that same frame.
- **Project Settings UI Entry**: Editor-visible control that lets users view and change the LightGrid rendering setting.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Existing projects render with LightGrid enabled without requiring migration steps.
- **SC-002**: Users can disable or re-enable LightGrid from Project Settings in under 10 seconds.
- **SC-003**: With LightGrid disabled, the scene frame avoids all LightGrid preparation work in both forward and deferred render paths.
- **SC-004**: With LightGrid enabled and threaded rendering active, a single scene frame performs LightGrid preparation no more than once.
- **SC-005**: Targeted rendering and editor settings tests pass for the enabled, disabled, and re-enabled paths.

## Assumptions

- The LightGrid switch is a project/editor rendering setting, not a per-camera or per-scene override in this change.
- Disabling LightGrid is acceptable as a visual-quality/performance diagnostic mode; lighting may be reduced or simplified.
- The first implementation should preserve default behavior and optimize duplicate CPU work before attempting deeper GPU-side LightGrid redesign.
- Runtime game launch should use the same stored project setting where existing project settings loading is already available.
