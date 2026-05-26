# Feature Specification: DebugDrawPass Line Batching

**Feature Branch**: `033-debugdraw-batching`
**Created**: 2026-05-24
**Status**: Draft
**Input**: User description: "DebugDrawPass 需要合批"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Select Objects Without Debug Draw Stalls (Priority: P1)

As an editor user, I can select an object that displays bounds or helper lines without the debug overlay causing severe frame-rate drops.

**Why this priority**: The current selected-object workflow becomes unusable because line-heavy debug helpers are submitted as many independent draw operations.

**Independent Test**: Submit multiple visible debug lines with the same visual state and verify the debug pass presents them as one grouped line submission while preserving the same visible geometry.

**Acceptance Scenarios**:

1. **Given** several consecutive visible debug lines with the same color, depth mode, and line width, **When** the debug draw pass executes, **Then** those lines are rendered through one grouped line submission.
2. **Given** selected-object bounds that emit many line segments, **When** the editor draws the debug overlay, **Then** the overlay draw overhead scales with the number of distinct line states rather than the number of line segments.

---

### User Story 2 - Preserve Visual State Boundaries (Priority: P2)

As a developer using debug drawing, I can mix different debug line styles and still get correct visual results.

**Why this priority**: Batching must not merge lines that require different render state or shader constants.

**Independent Test**: Submit lines with different colors, depth modes, or line widths and verify they are split into separate grouped submissions.

**Acceptance Scenarios**:

1. **Given** debug lines with different colors, **When** the debug draw pass executes, **Then** lines with distinct colors are not merged into the same grouped submission.
2. **Given** debug lines with different depth modes or line widths, **When** the debug draw pass executes, **Then** each incompatible render state is rendered separately.

---

### User Story 3 - Keep Existing Debug Primitive Behavior (Priority: P3)

As a developer using existing debug points and triangles, I can rely on their current behavior while line batching is introduced.

**Why this priority**: The immediate performance issue is line-heavy helpers; unrelated primitive behavior should remain stable.

**Independent Test**: Submit point, line, and triangle primitives and verify points and triangles still flow through the existing per-primitive path while compatible lines are grouped.

**Acceptance Scenarios**:

1. **Given** visible point, line, and triangle primitives, **When** the debug draw pass executes, **Then** the point and triangle are rendered once each through the existing primitive path.
2. **Given** hidden debug primitives from a disabled category, **When** the debug draw pass executes, **Then** hidden primitives are not included in grouped or per-primitive rendering.

### Edge Cases

- If no debug draw service exists, the pass returns without rendering.
- If no visible debug lines exist, no line batch is submitted.
- If a line batch contains one line, it still renders correctly.
- If the debug shader is unavailable, the pass skips debug primitive rendering as it does today.
- One-frame debug helpers must still be cleared after frame end and must not be duplicated by batching.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The debug draw pass MUST group consecutive visible line primitives that share the same color, depth mode, and line width into a single line submission per run.
- **FR-002**: The debug draw pass MUST NOT group line primitives with different color, depth mode, or line width into the same submission.
- **FR-003**: The grouped line submission MUST preserve every line segment's start and end positions.
- **FR-004**: Point and triangle debug primitives MUST keep the existing per-primitive rendering behavior in this change.
- **FR-005**: Category visibility filtering, one-frame lifetime behavior, and relative ordering around non-line or incompatible line primitives MUST remain unchanged.
- **FR-006**: The debug primitive shader MUST support both existing per-primitive rendering and grouped line rendering.
- **FR-007**: The change MUST be covered by automated tests for grouping, state splitting, and existing primitive behavior.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Submitting 72 consecutive compatible bounds-sphere line segments results in 1 grouped line submission instead of 72 individual line submissions.
- **SC-002**: Lines that differ by color, depth mode, or line width produce separate grouped submissions in automated tests.
- **SC-003**: Existing DebugDrawPass and DebugDrawGeometry unit tests pass after the change.
- **SC-004**: No generated files are hand-edited.

## Assumptions

- The immediate performance problem is caused by line-heavy debug helpers, especially selected-object bounds, rather than point or triangle debug primitives.
- The first implementation can use transient mesh construction per line batch; persistent buffer reuse is a follow-up optimization if profiling shows upload allocation cost remains significant.
- This change validates behavior with the existing unit-test backend and does not claim full cross-backend performance parity without later runtime or RenderDoc evidence.
