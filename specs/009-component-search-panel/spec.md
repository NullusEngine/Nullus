# Feature Specification: Reflection-Driven Component Search Dropdown

**Feature Branch**: `[009-component-search-panel]`
**Created**: 2026-05-02
**Status**: Draft
**Input**: User description: "点击AddComponent按钮应该用反射遍历所有组件，制作一个类似Unity的组件添加面板，支持搜索"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Add Any Eligible Component From One Dropdown (Priority: P1)

As a level designer using the Inspector, I want the `Add Component` action to open a Unity-style dropdown under the button so I can browse and add reflected component types without waiting for the editor to add new hardcoded menu entries.

**Why this priority**: This is the core value of the feature. Without reflection-driven discovery, the panel still has the current maintenance bottleneck and cannot scale with new components.

**Independent Test**: Can be fully tested by selecting an actor, opening the dropdown from `Add Component`, confirming that every eligible reflected component appears under metadata-driven categories, and adding one that was not previously hardcoded.

**Acceptance Scenarios**:

1. **Given** an actor is selected in the Inspector, **When** the user clicks `Add Component`, **Then** the editor opens a dropdown-style component picker anchored under the button instead of a fixed four-item list.
2. **Given** the picker search is empty, **When** the dropdown appears, **Then** it shows metadata-driven component categories for browsing rather than one flat list.
3. **Given** a reflected component type is eligible to be added and is not already present on the actor, **When** the user selects it from the dropdown, **Then** the component is attached to the actor and becomes visible in the Inspector immediately.
4. **Given** a new reflected component type is available in the project, **When** the editor opens the dropdown, **Then** that type appears without requiring a new hardcoded Inspector menu entry.

---

### User Story 2 - Find Components Quickly Through Search (Priority: P2)

As a designer working with many components, I want to search the component list by name so I can find the target component quickly instead of scanning a long menu.

**Why this priority**: Search is the main usability requirement once the list is reflection-driven and no longer small enough for a fixed drop-down.

**Independent Test**: Can be fully tested by opening the dropdown, entering partial search terms, and verifying that the visible UI switches from category browsing to a flat matching result list while non-matching entries are hidden.

**Acceptance Scenarios**:

1. **Given** the component dropdown is open, **When** the user types part of a component name, **Then** the dropdown switches to a flat matching result list in the same interaction.
2. **Given** the search input is cleared, **When** the filter text becomes empty, **Then** the dropdown returns to metadata-driven category browsing.
3. **Given** the search text matches no component, **When** the dropdown updates, **Then** it shows a clear empty-state message instead of a blank or broken list.

---

### User Story 3 - Prevent Invalid Add Actions (Priority: P3)

As a user editing an actor, I want the panel to avoid offering components that cannot be added so the UI stays trustworthy and I do not spend time clicking entries that will fail.

**Why this priority**: A reflection-driven list must still respect component rules and actor state, otherwise the experience regresses from the current limited but safe menu.

**Independent Test**: Can be fully tested by selecting actors that already contain common single-instance components and verifying that duplicate or non-addable component types are excluded or clearly unavailable in the panel.

**Acceptance Scenarios**:

1. **Given** an actor already has a component that should only exist once on that actor, **When** the user opens the panel, **Then** that component is not presented as an available add action or is clearly shown as unavailable.
2. **Given** a reflected type is abstract, foundational, or otherwise not intended for direct addition, **When** the list is built, **Then** that type is excluded from the addable results.
3. **Given** a component add attempt cannot be completed, **When** the user triggers the action, **Then** the editor leaves the actor unchanged and presents a clear failure indication.

### Edge Cases

- What happens when the project contains reflected component base classes, helper types, or other types that derive from the component hierarchy but should never appear in the add dropdown?
- How does the panel behave when a search term only differs by spacing or case from the displayed component name?
- What happens when the selected actor changes or becomes invalid while the component dropdown is open?
- How does the panel behave when all reflected component types are already present or otherwise unavailable for the selected actor?
- What happens when a component declares no explicit component-menu metadata and must fall back to a default category?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The Inspector `Add Component` action MUST open a dropdown-style component picker anchored under the button instead of relying on the current fixed component dropdown.
- **FR-002**: The component picker MUST build its available entries by traversing the reflected type system for types that belong to the component hierarchy.
- **FR-003**: The panel MUST present each available component entry using a readable display name derived consistently from the component type.
- **FR-004**: The picker MUST allow the user to add a selected component to the currently inspected actor directly from the dropdown.
- **FR-005**: After a component is added successfully, the Inspector MUST refresh so the new component is visible without requiring the user to reselect the actor.
- **FR-006**: The picker MUST provide a search input that filters component entries by component name while the dropdown is open.
- **FR-007**: Search matching MUST be case-insensitive.
- **FR-008**: When the search input is empty, the picker MUST show component categories derived from reflection metadata rather than a flat result list.
- **FR-009**: When no component matches the current search text, the picker MUST show an explicit no-results state.
- **FR-010**: The panel MUST exclude component types that are not valid for direct user addition, including non-instantiable component types and types reserved as editor or runtime foundations.
- **FR-011**: The panel MUST prevent the user from adding a component that the selected actor already contains when duplicate addition is not allowed by existing actor/component rules.
- **FR-012**: The panel MUST use the same add-component rules as the underlying actor/component system so the UI availability matches actual addability.
- **FR-013**: If the selected actor is unavailable when the user confirms an add action, the panel MUST not perform any addition and MUST fail gracefully.
- **FR-014**: The component category structure shown by the picker MUST come from per-component reflection metadata rather than from a separately maintained hardcoded Inspector registry.
- **FR-015**: The interaction model of the picker MUST support quick repeated use in a manner consistent with a Unity-style component picker: open, search or browse, choose, and return to normal Inspector editing.
- **FR-016**: Reflected component types intended for direct addition SHOULD be able to declare a component-menu path through type-level reflection metadata.
- **FR-017**: When a component type has no explicit component-menu metadata, the picker MUST place it under a `Miscellaneous` fallback category.
- **FR-018**: When the search input is non-empty, the picker MUST switch from category browsing to a flat list of matching component results.

### Key Entities *(include if feature involves data)*

- **Component Entry**: A user-selectable representation of one reflected component type, including its display name, metadata-derived menu path, search text, and current addability state for the selected actor.
- **Component Menu Path**: A reflection metadata string declared on a component type that defines its category hierarchy and final picker label, such as `Rendering/Light`.
- **Component Search Query**: The text the user enters to filter the available component entries during one panel interaction.
- **Selected Actor Context**: The current Inspector target whose existing components and validity determine which component entries can be added.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a project build where additional reflected component types exist beyond the current hardcoded menu, users can add those components from the Inspector without any new manual Inspector menu wiring.
- **SC-002**: In a validation pass with at least ten eligible components, a user can narrow the visible list to the intended component using partial name search in a single dropdown interaction.
- **SC-003**: In targeted editor verification, duplicate or non-addable component types are never applied to an actor through the component panel.
- **SC-004**: After a successful add action, the newly attached component appears in the Inspector immediately during the same editing flow.

## Assumptions

- The existing reflection database already exposes enough runtime type information to enumerate component-derived types in the editor.
- The current actor/component system remains the source of truth for whether a specific component type can be instantiated or added to an actor.
- The Unity-like expectation refers to an anchored dropdown interaction with metadata-driven category browsing and search-driven flat results, not to a requirement for pixel-perfect visual parity with Unity's exact styling.
- Reorganizing existing component categories beyond metadata-declared menu paths, or introducing favorites and keyboard shortcut workflows, is out of scope for this first version.
