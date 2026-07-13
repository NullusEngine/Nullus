# Launcher Editor Selection Design

## Problem

Launcher currently treats any existing `.exe` recorded in a project's
`last_editor_executable` setting as a usable Editor installation. A stale Editor binary can
therefore remain preferred after it has been removed from Launcher's installation list. The
Launcher exits after creating that process, even if the stale Editor immediately crashes.

The observed project was bound to an old `Win64_Debug_Runtime_Static/Editor.exe`. Windows
reported that process crashing with exception `0xc000041d`, while the current default Release
Editor opened the same project successfully.

## Selection Rule

When opening an existing project, Launcher selects the Editor in this order:

1. Use the project-bound Editor only when its path is registered in the current Launcher
   installation list and the executable is valid.
2. Otherwise use the valid default Editor installation.
3. If neither exists, preserve the current same-directory `Editor` discovery fallback in the
   Launcher entry point.

Path membership must use the same platform-aware comparison as the rest of
`LauncherSettings`, including case-insensitive comparison on Windows.

After a successful process creation, the existing metadata write records the selected Editor as
the project's new `last_editor_executable` value.

## Scope

Add an explicit `LauncherSettings` query for whether an executable is a registered, valid
installation, and use it in `LauncherPanel::OpenProject`. Do not add process monitoring,
automatic retry after Editor crashes, project-format migration, or changes to Editor startup.

## Tests

Add focused unit coverage proving that:

- a registered valid Editor is recognized;
- an existing but unregistered Editor is rejected;
- platform path comparison continues to follow the existing Launcher settings rules;
- Launcher source selection requires a registered project binding before preferring it over the
  default Editor.

Run the focused Launcher settings/source guard tests, the existing UI overlay and ShaderManager
tests, a Release Launcher build, and `git diff --check`.

