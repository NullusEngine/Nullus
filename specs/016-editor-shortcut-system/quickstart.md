# Quickstart: Editor Shortcut System

## Build

1. Configure or reuse an existing CMake build directory.
2. Build the editor and unit test target:

```powershell
cmake --build Build --target Editor NullusUnitTests --config Debug
```

Adjust the build directory name if the local workspace uses a different one.

## Automated Validation

Run the focused unit tests once implemented:

```powershell
ctest --test-dir Build --output-on-failure -R NullusUnitTests -C Debug
```

Expected coverage:

- Shortcut binding parsing/display normalization.
- Command registration uniqueness.
- Conflict detection for global, same-context, and mutually exclusive contexts.
- Context-based resolution executes at most one command.
- Profile save/load handles overrides, unassigned commands, missing files, malformed files, and stale command IDs.

## Manual Editor Validation

Launch the editor with a test project:

```powershell
.\run_editor_with_project.ps1 TestProject
```

Validate migrated default shortcuts:

- `Ctrl + N`: new scene.
- `Ctrl + S`: save scene.
- `Ctrl + Shift + S`: save scene as.
- `F5`: play/edit command behavior remains unchanged where currently supported.
- `F11`: capture next RenderDoc frame when available.
- `Ctrl + F11`: open latest RenderDoc capture.
- `Delete`: delete selected actor only when Scene View or Hierarchy is the relevant active context.

Validate shortcut settings behavior:

- Open the shortcut settings panel.
- Search for a command by name and shortcut.
- Change a non-conflicting binding and confirm menu/list display updates.
- Attempt a conflicting binding and confirm it is blocked.
- Restart or reload settings and confirm the custom binding persists.

## Scope Guard

Do not validate this feature by changing gameplay input. Runtime/Game input behavior should remain unchanged.
