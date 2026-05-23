# Contract: Editor Shortcut Service

This contract defines the behavior expected by editor code, panels, and tests. It is an internal editor subsystem contract, not a Runtime/Game input contract.

## Command Registration

**Consumer**: Editor bootstrap, editor panels, editor tools.

**Requirements**

- A command registration includes stable ID, display name, category, context, optional default binding, and execution action.
- Registering a duplicate command ID fails and leaves the previous command unchanged.
- Registered commands can be queried by ID, category, context, and display text.
- Registered commands expose current binding display text for menus and shortcut lists.

## Shortcut Resolution

**Consumer**: Editor frame update.

**Requirements**

- Each frame, the service accepts current input state and active editor context state.
- A shortcut press resolves to zero or one executable command.
- Disabled commands do not execute, even when their binding matches.
- Normal shortcuts do not execute while text input is capturing keys.
- Commands marked as allowed during text input may execute if their binding matches.

## Conflict Validation

**Consumer**: Shortcut settings UI, tests, future editor tool registration.

**Requirements**

- A proposed binding can be validated before saving.
- Duplicate global bindings are blocking conflicts.
- Duplicate same-context bindings are blocking conflicts.
- Global/context collisions are blocking unless explicitly allowed by command metadata.
- Bindings in mutually exclusive contexts may be accepted.
- Invalid bindings return a blocking validation result with a user-facing message.

## Profile Persistence

**Consumer**: Editor startup/shutdown, shortcut settings UI.

**Requirements**

- The service loads user overrides after built-in defaults are available.
- Missing profile files are treated as default profile state.
- Malformed profile files fall back to defaults and report diagnostics.
- Saving writes only user overrides and intentional unassigned states, not a copy of every built-in command.
- Loading a profile must not change built-in default bindings.

## Settings UI

**Consumer**: Shortcut settings panel.

**Requirements**

- The UI can list categories and commands.
- The UI can request command search by name, category, context, or shortcut display text.
- The UI can assign, clear, and reset a command binding through the service.
- The UI cannot commit a blocking conflict.
- Optional visual keyboard recording uses the same validation path as direct key capture.
