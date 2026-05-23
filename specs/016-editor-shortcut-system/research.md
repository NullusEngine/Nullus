# Research: Editor Shortcut System

## Decision: Use a Unity-style command, context, and profile model

**Rationale**: Unity's Shortcuts Manager separates command identity from key binding, scopes bindings by context, and persists user profiles separately from built-in defaults. That maps well to Nullus because editor commands already exist in `EditorActions`, panels expose focus, and hard-coded shortcuts are currently split between `Editor` and `MenuBar`.

**Alternatives considered**:

- Keep panel-local shortcut checks: rejected because conflicts remain invisible and persistence becomes scattered.
- Build a generic Runtime input action system: rejected because the requested scope and Unity reference are editor-only.

## Decision: Keep the system Editor-only

**Rationale**: Unity's shortcut APIs live in editor namespaces and are not gameplay input. Nullus should mirror that boundary: editor commands consume input state, while Runtime/Game input remains a separate concern.

**Alternatives considered**:

- Shared editor/game action mapper: rejected because it would expand scope and risk changing gameplay input behavior.
- Runtime-level shortcut registry: rejected because shortcut commands depend on editor concepts such as panels, focused views, scenes, and editor tools.

## Decision: Persist user profile overrides as JSON under project `UserSettings`

**Rationale**: Existing editor layout settings already live under project `UserSettings`. A JSON profile can represent command IDs, key names, modifiers, unassigned commands, and future profile metadata without forcing this into reflection serialization.

**Alternatives considered**:

- INI persistence: rejected because nested shortcut data, profiles, and stale command handling become clumsy.
- Reflection serializer: rejected because shortcut profile data is editor preference data, not reflected scene/resource data.
- Global application settings only: deferred because project-local persistence matches the existing editor layout behavior and is simpler for initial validation.

## Decision: Resolve conflicts by active context compatibility

**Rationale**: The system must reject duplicate global bindings and duplicate bindings within the same context. It may allow the same binding in mutually exclusive contexts because only one can be active. Global/context collisions should be blocked unless a command explicitly allows a context override.

**Alternatives considered**:

- Reject every duplicate binding globally: rejected because it blocks useful Unity-style reuse between mutually exclusive tools or panels.
- Always let context bindings override globals: rejected because users would lose predictable global shortcuts without an explicit rule.

## Decision: Treat text entry as a high-priority capture context

**Rationale**: Shortcuts must not fire while the user is typing in search, rename, or inspector fields. Commands that truly need to work during text entry can opt in explicitly.

**Alternatives considered**:

- Always process shortcuts first: rejected because it breaks normal text entry.
- Fully disable shortcuts whenever any ImGui item is active: rejected because some non-text controls may still allow safe global shortcuts.

## Decision: Use `imgui_keyboard` only as an optional visual/recording UI control

**Rationale**: The linked library can render a keyboard, highlight assigned keys, and support recording, but it does not provide command registration, context resolution, conflict detection, or persistence. It belongs in the shortcut settings panel, not the service core.

**Alternatives considered**:

- Make `imgui_keyboard` a core dependency: rejected because it would couple shortcut semantics to a UI widget.
- Skip visual keyboard permanently: deferred; the core service should not depend on it, but the UI may benefit from it later.
