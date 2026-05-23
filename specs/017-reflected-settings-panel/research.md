# Research: Reflected Editor Settings Panel

## Decision: Use Unity-style Settings workflow as the interaction baseline

**Rationale**: The requested behavior matches Unity's editor workflow: Settings are opened from the Edit menu, presented in a dedicated settings window, searchable, and navigated by category. This gives Nullus a familiar mental model without copying every Unity setting page.

**Alternatives considered**:
- Keep the existing dockable `ProjectSettings` panel: rejected because the user explicitly requested a popup/modal rather than a dockable panel.
- Put settings under the shortcut settings window: rejected because settings are broader than shortcuts and need their own category browser.

## Decision: Extract Inspector reflected drawing into a shared editor property drawer

**Rationale**: `Inspector.cpp` already contains field-type-specific reflected drawing for booleans, numeric scalars, strings, vectors, quaternions, enums, arrays, and special editor values. Moving this into a reusable drawer satisfies the requirement that Inspector and Settings share one logic path and avoids duplicate behavior.

**Alternatives considered**:
- Duplicate the Inspector field drawing logic inside Settings: rejected because it would diverge quickly.
- Build a generic nested-object inspector immediately: rejected for v1 because current Inspector needs mostly flat component fields and settings can start with the same supported field set.

## Decision: Model settings pages as registered SettingObject entries

**Rationale**: A registry with stable ids, display names, category paths, persistence scopes, and object access gives the Settings window a clean extension point. New settings pages can be added without editing the window layout.

**Alternatives considered**:
- Discover all reflected objects automatically: rejected because not every reflected object is a settings page and automatic discovery would need metadata conventions that do not yet exist.
- Hard-code categories in the panel: rejected because it recreates the existing hand-written Settings problem.

## Decision: Persist supported fields using file-based JSON keyed by object id and field name

**Rationale**: The shortcut system already established JSON persistence patterns in the editor, and json11 is available in the project. A JSON object keyed by stable settings id and reflected field identity is simple to test and resilient to category display name changes.

**Alternatives considered**:
- Continue using INI keys only: rejected because reflected SettingObjects need nested object grouping and type-aware values.
- Persist every reflected field as untyped strings: rejected because numeric/bool/enum round trips should be type-safe and testable.

## Decision: Register an initial real editor settings object

**Rationale**: The feature needs a real page to prove the extension path. Existing editor settings such as debug draw and transform snapping are editor-only, visible, and meaningful for the first page.

**Alternatives considered**:
- Create only a fake sample object: rejected because it would not validate real editor workflow.
- Migrate all existing project settings at once: rejected because it increases scope and risk beyond the first reflected settings system.
