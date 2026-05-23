# Feature Specification: Editor Shortcut System

**Feature Branch**: `016-editor-shortcut-system`  
**Created**: 2026-05-05  
**Status**: Draft  
**Input**: User description: "Design and implement an Editor-only shortcut system inspired by Unity Shortcuts Manager. It must register editor commands with categories and contexts, resolve shortcuts based on active editor context, prevent conflicting shortcut assignments, support user-editable profiles persisted to disk, replace existing hard-coded editor shortcuts, and optionally reuse imgui_keyboard only as a visual keyboard/recording control in the shortcut settings UI. Runtime/Game input is out of scope."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Execute Editor Commands Through Central Shortcuts (Priority: P1)

An editor user can trigger existing editor commands through a single shortcut system instead of each panel handling its own key combinations.

**Why this priority**: Central execution is the foundation for consistency, conflict detection, persistence, and future customization.

**Independent Test**: Can be tested by registering the initial editor commands, pressing their default shortcuts in the appropriate editor state, and verifying that the same actions occur as before.

**Acceptance Scenarios**:

1. **Given** the editor is in edit mode with no text field capturing input, **When** the user presses the default shortcut for creating a new scene, **Then** the editor creates a new scene through the registered command.
2. **Given** a scene is open, **When** the user presses the default save shortcut, **Then** the current scene save command runs once.
3. **Given** an actor is selected and a relevant editor view has focus, **When** the user presses the delete shortcut, **Then** the selected actor is destroyed through the registered command.

---

### User Story 2 - Avoid Shortcut Conflicts (Priority: P2)

An editor user or tool author can add or change shortcuts and receive clear feedback when the binding would conflict with another active command.

**Why this priority**: A shortcut system is unsafe if two active commands can respond to the same key combination without predictable rules.

**Independent Test**: Can be tested by assigning the same shortcut to global and context-specific commands and checking that conflicts are detected before the assignment becomes active.

**Acceptance Scenarios**:

1. **Given** two global commands, **When** both are assigned the same shortcut, **Then** the system reports a conflict and does not leave both active with the same binding.
2. **Given** two commands in the same editor context, **When** both are assigned the same shortcut, **Then** the system reports a conflict and does not leave both active with the same binding.
3. **Given** two commands in mutually exclusive editor contexts, **When** both use the same shortcut, **Then** the system accepts the binding and resolves by current context.

---

### User Story 3 - Persist User Shortcut Profiles (Priority: P3)

An editor user can customize shortcuts, restart the editor, and keep those custom bindings without changing built-in defaults.

**Why this priority**: Persistence turns shortcuts from hard-coded behavior into user preferences while preserving a reliable default set.

**Independent Test**: Can be tested by changing a shortcut, saving the profile, restarting or reloading settings, and verifying the customized binding remains active.

**Acceptance Scenarios**:

1. **Given** the user changes a shortcut, **When** the profile is saved and reloaded, **Then** the customized shortcut is restored.
2. **Given** the user removes a custom binding, **When** the profile is saved and reloaded, **Then** the command remains unassigned unless reset to defaults.
3. **Given** a persisted profile references a command that no longer exists, **When** the profile loads, **Then** the editor ignores the stale entry without blocking other shortcuts.

---

### User Story 4 - Browse and Edit Shortcuts by Category (Priority: P4)

An editor user can open a shortcuts management UI, browse commands by category, search commands, inspect current bindings, and edit or reset bindings.

**Why this priority**: Management UI improves usability after the central system, conflict rules, and persistence are reliable.

**Independent Test**: Can be tested by opening the UI, locating a command by category or search, changing its binding, and seeing the persisted binding reflected in menus and command execution.

**Acceptance Scenarios**:

1. **Given** the shortcut settings UI is open, **When** the user selects a category, **Then** only matching commands are shown.
2. **Given** the user edits a command shortcut, **When** the new binding is valid, **Then** the command list and menu shortcut text update.
3. **Given** a visual keyboard control is available, **When** the user records a shortcut through it, **Then** the recorded binding follows the same conflict and persistence rules as typed input.

### Edge Cases

- A text input, rename field, search box, or other typing-focused control is active while the user presses a shortcut-like key combination.
- Two editor contexts are simultaneously eligible because focus ownership is ambiguous.
- A shortcut uses only a modifier key or an unsupported key.
- A user attempts to assign a reserved operating-system shortcut or platform-unavailable key.
- A command is disabled by editor state, such as play-mode-only or edit-mode-only behavior.
- The persisted shortcut file is missing, malformed, partially corrupt, or from an older version.
- The default shortcut registry changes after a user profile already exists.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST be scoped to editor commands only and MUST NOT define gameplay/runtime input behavior.
- **FR-002**: The system MUST allow editor commands to be registered with a stable identifier, display name, category, optional default shortcut, context, and executable action.
- **FR-003**: The system MUST support global commands and editor-context commands, including Scene View, Hierarchy, Asset Browser, Inspector, Game View, menu/top bar, and text-input contexts.
- **FR-004**: The system MUST resolve shortcuts using the currently active editor context, with global shortcuts available unless a higher-priority active context explicitly owns the binding.
- **FR-005**: The system MUST execute at most one command for a single shortcut press.
- **FR-006**: The system MUST detect shortcut conflicts before accepting a binding.
- **FR-007**: The system MUST treat duplicate shortcuts in the global context as conflicts.
- **FR-008**: The system MUST treat duplicate shortcuts in the same editor context as conflicts.
- **FR-009**: The system MUST treat global shortcuts that collide with active context shortcuts as conflicts unless the context binding is explicitly allowed to override the global binding.
- **FR-010**: The system MUST allow the same shortcut to be assigned to commands in mutually exclusive contexts.
- **FR-011**: The system MUST allow commands to be unassigned when the command is not marked as requiring a shortcut.
- **FR-012**: The system MUST preserve a built-in default shortcut set separately from user modifications.
- **FR-013**: The system MUST persist user shortcut profile changes to disk and reload them on editor startup.
- **FR-014**: The system MUST recover from missing or invalid persisted data by using built-in defaults and reporting the problem in a user-visible or diagnostic channel.
- **FR-015**: The system MUST expose current shortcut display text so menus and shortcut listings can stay synchronized with user changes.
- **FR-016**: The initial command set MUST include the currently hard-coded editor shortcuts: new scene, save scene, save scene as, play, stop or pause where applicable, capture next RenderDoc frame, open latest RenderDoc capture, and delete selected actor.
- **FR-017**: The shortcut management UI MUST present commands by category and show command name, context, current shortcut, default shortcut status, and conflict state.
- **FR-018**: The shortcut management UI MUST support searching commands by display name, category, and shortcut text.
- **FR-019**: The shortcut management UI MUST allow assigning, clearing, and resetting individual command shortcuts.
- **FR-020**: The shortcut management UI MUST prevent or clearly block saving a conflicting binding.
- **FR-021**: Visual keyboard integration, if included, MUST be limited to shortcut recording and visualization in the editor shortcut settings UI.
- **FR-022**: The system MUST avoid running editor shortcuts while normal text entry controls are actively capturing keyboard input, except for shortcuts explicitly marked as allowed during text entry.
- **FR-023**: The system MUST provide tests or equivalent verification for registration, context resolution, conflict detection, persistence, and the migrated initial shortcuts.

### Key Entities *(include if feature involves data)*

- **Shortcut Command**: A stable editor command entry with identity, category, context, display metadata, default binding, execution availability, and action.
- **Shortcut Binding**: A key combination composed of one primary key plus zero or more modifiers, represented in a platform-independent way where possible.
- **Shortcut Context**: A named editor interaction area or state that determines whether a command binding is eligible to execute.
- **Shortcut Profile**: The user's saved binding overrides relative to the built-in default command registry.
- **Shortcut Conflict**: A detected collision between two or more bindings that could be active at the same time.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of the initial migrated editor shortcuts execute through the central shortcut system with no behavioral regression in their supported contexts.
- **SC-002**: 100% of attempted duplicate global or same-context bindings are detected before they become active.
- **SC-003**: A customized shortcut profile persists across editor restart or settings reload in at least 95% of normal save/load attempts, with malformed files falling back to defaults.
- **SC-004**: Users can locate and edit an existing shortcut from the management UI in under 30 seconds during manual validation.
- **SC-005**: Automated verification covers at least registration, conflict detection, context resolution, and persistence flows.

## Assumptions

- The first version targets the Nullus editor only; gameplay input mapping remains a separate future feature.
- The built-in shortcut defaults are owned by the editor and are safe to regenerate or change between versions.
- User profile persistence belongs under the project user settings area for the first version, matching existing editor layout persistence behavior.
- `imgui_keyboard` is useful as an optional visual editing control but is not a dependency for command registration, resolution, conflict detection, or persistence.
- Platform-specific key naming and modifier differences may be normalized in display text, but each platform must preserve the user's intended binding.
