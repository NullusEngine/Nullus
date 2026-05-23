# Feature Specification: Reflected Editor Settings Panel

**Feature Branch**: `017-reflected-settings-panel`  
**Created**: 2026-05-05  
**Status**: Draft  
**Input**: User description: "Implement a Unity-like Project Settings panel for the editor where settings pages are backed by reflected SettingObject instances. The settings panel lists settings categories on the left, shows selected settings on the right, supports search, renders and edits properties using the same reflection-based property drawing pipeline shared with Inspector, and persists edited settings."

## User Scenarios & Testing *(mandatory)*

<!--
  IMPORTANT: User stories should be PRIORITIZED as user journeys ordered by importance.
  Each user story/journey must be INDEPENDENTLY TESTABLE - meaning if you implement just ONE of them,
  you should still have a viable MVP (Minimum Viable Product) that delivers value.
  
  Assign priorities (P1, P2, P3, etc.) to each story, where P1 is the most critical.
  Think of each story as a standalone slice of functionality that can be:
  - Developed independently
  - Tested independently
  - Deployed independently
  - Demonstrated to users independently
-->

### User Story 1 - Edit Reflected Editor Settings (Priority: P1)

An editor user opens the Settings window from the editor menu, selects a settings category, and edits the visible fields without needing a custom hand-written panel for that settings page.

**Why this priority**: This is the core workflow: settings must be reachable, readable, editable, and visually aligned with the rest of the editor before additional settings pages matter.

**Independent Test**: Open Settings, select an initial settings page, edit supported field types, close and reopen the window, and confirm the edited values are still shown.

**Acceptance Scenarios**:

1. **Given** the editor is running, **When** the user chooses the Settings command from the Edit menu, **Then** a modal Settings window opens using the editor's Unity-aligned styling.
2. **Given** the Settings window is open, **When** the user selects a category on the left, **Then** the right side shows the selected settings object's reflected properties.
3. **Given** a reflected property is editable, **When** the user changes its value, **Then** the visible value updates immediately and is marked for persistence.

---

### User Story 2 - Share Property Drawing With Inspector (Priority: P2)

An editor developer adds or changes reflected properties on a settings object and expects the Settings panel to render those properties with the same field behavior as the Inspector.

**Why this priority**: The user explicitly requested Settings and Inspector to share one reflected-property logic path, which prevents duplicate UI behavior and keeps future reflected editors consistent.

**Independent Test**: Add a representative reflected settings object and verify the same supported property types are drawn consistently in both the Settings panel and Inspector-backed reflected drawing tests.

**Acceptance Scenarios**:

1. **Given** a reflected object with supported scalar, vector, boolean, and enum fields, **When** the object is shown in Settings, **Then** each supported field uses the shared reflected-property drawing rules.
2. **Given** a reflected property type is not supported yet, **When** Settings renders the object, **Then** the field is shown as read-only or gracefully skipped with a clear non-crashing fallback.
3. **Given** Inspector and Settings both draw a supported reflected property, **When** the property value changes, **Then** both surfaces use equivalent editing behavior and display labels.

---

### User Story 3 - Discover Settings Quickly (Priority: P3)

An editor user searches for a setting by partial text, sees matching categories and fields, and navigates directly to the relevant settings page.

**Why this priority**: Unity-style Settings windows rely on search for scale. Search becomes increasingly important once more SettingObjects are registered.

**Independent Test**: Register multiple settings categories, type partial search text, and confirm the left category list and selected detail area surface relevant matches.

**Acceptance Scenarios**:

1. **Given** several settings categories exist, **When** the user types a partial category name, **Then** only matching categories remain prominent in the navigation list.
2. **Given** a property label matches the search query, **When** the user searches for that label, **Then** the owning settings page remains discoverable.
3. **Given** the search query has no matches, **When** the user views the Settings window, **Then** the UI shows an empty result state without losing registered settings.

---

### User Story 4 - Extend Settings Without Rewriting the Window (Priority: P4)

An editor developer creates a new SettingObject, registers it with an id, display name, category path, and persistence scope, and gets a Settings page automatically.

**Why this priority**: Extensibility is required, but it builds on the working Settings window and shared reflected property drawer.

**Independent Test**: Add a new settings object and verify it appears in the correct category, draws reflected properties, detects duplicate identities, and participates in save/load.

**Acceptance Scenarios**:

1. **Given** a settings object is registered with a unique id and category, **When** the Settings window opens, **Then** that object appears in the navigation tree.
2. **Given** two settings objects attempt to register the same id, **When** registration occurs, **Then** the duplicate is rejected or reported before it creates ambiguous UI.
3. **Given** a settings object declares persisted fields, **When** values are saved and the editor restarts, **Then** the object loads the saved values.

### Edge Cases

- A settings object has no reflected editable fields.
- A persisted settings file is missing, empty, or contains invalid values.
- A registered category path is duplicated by multiple settings objects.
- A property type is reflected but has no editor widget yet.
- Search matches a category but not any field, or matches a field but not the category name.
- The Settings modal is open while Scene view would normally consume mouse input.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The editor MUST expose a Settings command from the Edit menu.
- **FR-002**: The Settings UI MUST open as a modal popup/window rather than a dockable editor panel.
- **FR-003**: The Settings UI MUST use a Unity-aligned layout with a search field at the top, category navigation on the left, and selected setting details on the right.
- **FR-004**: The Settings UI MUST pause Scene view mouse interaction while the modal is open.
- **FR-005**: Each settings page MUST be backed by a SettingObject registered with a stable id, display name, category path, and persistence scope.
- **FR-006**: SettingObject fields MUST be rendered from reflection metadata instead of hand-written per-page property rows.
- **FR-007**: Settings and Inspector MUST share one reflected-property drawing path for supported reflected field types.
- **FR-008**: The shared reflected-property drawing path MUST support the field types already expected by the Inspector for common editor workflows, including booleans, numeric scalars, strings, vectors, colors where available, and enums.
- **FR-009**: Unsupported reflected field types MUST not crash the Settings UI and MUST have a deterministic fallback.
- **FR-010**: The Settings system MUST persist edited values and reload them in later editor sessions.
- **FR-011**: The persistence format MUST include enough identity information to associate saved values with the correct SettingObject and fields after editor restart.
- **FR-012**: The Settings system MUST detect duplicate SettingObject ids and prevent ambiguous registration.
- **FR-013**: Search MUST match category names, setting display names, and visible property labels using partial case-insensitive text.
- **FR-014**: The Settings UI MUST preserve a sensible selected category when search text changes, including when the current selection is filtered out.
- **FR-015**: The feature MUST include at least one real editor settings object to demonstrate registration, reflected drawing, and persistence.

### Key Entities *(include if feature involves data)*

- **SettingObject**: A reflected editor settings object with stable identity, category placement, editable fields, current values, and persistence scope.
- **Settings Registry**: The editor-owned collection of registered SettingObjects, responsible for uniqueness and lookup.
- **Settings Window State**: The transient UI state for search text, selected category, selected SettingObject, dirty state, and modal visibility.
- **Persisted Settings Data**: Saved values keyed by SettingObject identity and reflected field identity.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can open Settings, locate a registered page, edit a value, and close the window in under 30 seconds.
- **SC-002**: Edited settings values survive a close-and-reopen cycle with 100% fidelity for supported field types covered by tests.
- **SC-003**: Adding a new basic SettingObject requires no Settings-window-specific drawing code.
- **SC-004**: Duplicate settings identities are detected before the Settings window can show ambiguous pages.
- **SC-005**: Searching by partial text filters or surfaces matching settings in under one frame for the initial set of editor settings pages.

## Assumptions

- The feature is editor-only and does not affect runtime game builds.
- The first implementation can focus on project/editor settings already meaningful to Nullus rather than reproducing every Unity Project Settings category.
- Existing reflection metadata is the source of truth for reflected fields.
- Existing Inspector behavior should be preserved while extracting shared property drawing.
- Persistence can use the repository's existing lightweight file-based settings conventions unless planning finds a stronger local pattern.
- Unity reference behavior is used for layout and workflow alignment: Edit menu entry, Settings modal, left navigation, right detail view, and top search.
