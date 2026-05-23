# Data Model: Editor Shortcut System

## ShortcutCommand

Represents one editor command that can be invoked by shortcut.

**Fields**

- `id`: stable unique command identifier, such as `file.save-scene`.
- `displayName`: user-facing command name.
- `category`: grouping shown in the shortcut UI, such as `File`, `Edit`, `Scene View`, `Debugging`.
- `context`: context where the command may execute.
- `defaultBinding`: optional built-in shortcut binding.
- `requiredBinding`: whether the command may be unassigned.
- `allowDuringTextInput`: whether text-entry capture may be bypassed.
- `allowContextOverride`: whether a context binding may intentionally override a global binding.
- `availability`: current editor-state predicate used to decide whether the command can run.
- `execute`: action invoked when the shortcut resolves.

**Validation Rules**

- `id` must be non-empty and unique.
- `displayName` and `category` must be non-empty.
- A command without a default binding is allowed only when `requiredBinding` is false.
- Disabled commands remain registered but do not execute.

## ShortcutBinding

Represents a key combination.

**Fields**

- `primaryKey`: one non-modifier key.
- `modifiers`: zero or more modifiers: Ctrl, Shift, Alt, Super.
- `platform`: optional platform qualifier for platform-specific bindings.
- `assigned`: false when the command is intentionally unassigned.

**Validation Rules**

- A binding must not consist only of modifiers.
- Unsupported or unknown keys are invalid.
- Display order is normalized as `Ctrl + Shift + Alt + Super + Key`.
- Left/right modifier distinction may be normalized for shortcut matching unless a future requirement needs side-specific bindings.

## ShortcutContext

Represents an editor area or state that controls shortcut eligibility.

**Fields**

- `id`: stable context identifier.
- `displayName`: user-facing context label.
- `priority`: ordering used when more than one context is eligible.
- `exclusiveGroup`: optional group that marks contexts as mutually exclusive.
- `isActive`: predicate evaluated during shortcut resolution.

**Validation Rules**

- `Global` is always eligible but has lower priority than active explicit contexts.
- `TextInput` has high priority and suppresses normal shortcuts unless a command opts in.
- Two contexts in the same exclusive group must not both be considered active after final resolution.

## ShortcutProfile

Represents saved user overrides relative to built-in defaults.

**Fields**

- `version`: profile schema version.
- `profileName`: user-visible profile name; initial version may use `Default`.
- `bindings`: map from command ID to binding override.
- `unassignedCommands`: set of command IDs intentionally cleared by the user.
- `updatedAt`: optional timestamp for diagnostics.

**Validation Rules**

- Unknown command IDs are ignored and preserved only if future implementation chooses to round-trip stale data.
- Invalid bindings are ignored with diagnostics and fall back to default bindings.
- Built-in defaults are never overwritten by profile loading.

## ShortcutConflict

Represents a binding collision.

**Fields**

- `binding`: conflicting shortcut binding.
- `commands`: commands participating in the conflict.
- `conflictType`: global duplicate, same-context duplicate, global-context collision, invalid binding, reserved binding.
- `severity`: blocking or warning.
- `message`: user-facing explanation.

**Validation Rules**

- Blocking conflicts prevent the binding from becoming active.
- Mutually exclusive contexts may reuse a binding without producing a blocking conflict.
- Explicit context override may downgrade a global-context collision according to command metadata.

## State Transitions

```text
Built-in defaults loaded
  -> user profile loaded
  -> commands registered
  -> active profile resolved
  -> shortcuts evaluated each frame
  -> user edits binding
  -> conflict validation
  -> active profile updated
  -> profile saved
```
