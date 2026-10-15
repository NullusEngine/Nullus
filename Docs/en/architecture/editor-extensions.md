# Editor Script Extensions

The editor is not closed: a project can ship editor scripts under its own `Assets` directory to register custom Inspectors, tool windows, and menu items. This page covers the boundaries and entry points.

## Assembly split

| Source location | Output | Loaded by |
| --- | --- | --- |
| `Assets/**/Editor/**/*.cs` | `GameScriptsEditor.dll` | Editor process only |
| `Assets/**/*.cs` (everything else) | `GameScripts.dll` | Editor and player builds |
| `Managed/Nullus.Editor` | Editor scripting API | Editor |

Editor scripts may reference `Nullus.Editor`; runtime project code may not.

## Extension points

| Extension point | Base type or attribute |
| --- | --- |
| Custom Inspector | `ComponentEditor` with `[CustomEditor]` and `[CanEditMultipleObjects]` |
| Custom tool window | `EditorWindow` with `[EditorMenuItem]` |
| Menu item | `[EditorMenuItem]`, top level restricted to `Window` or `Tools` |

Menu items end up next to the built-in panels in the editor's top-level menus.

## Editor-side data channel

Scripts never write component objects directly. Reads and writes go through:

```text
serializedObject -> SerializedProperty -> ApplyModifiedProperties()
```

That path is what makes script editors share the following with built-in editors:

- The undo stack
- Prefab override recording
- Multi-object editing semantics
- Field type conversion and validation

Bypassing it breaks undo and Prefab semantics and is not a supported usage.

## Target types

| Target | How to declare it |
| --- | --- |
| Reflected engine component | Use the generated type carrying `NativeTypeName` |
| Project script component | Use a `Nullus.Behaviour` subclass |

Script components share one native reflected type, so matching is by the script's own full C# name. One script's editor never affects another, and base-class matching for subclasses is not supported.

## Priority and conflicts

- A script editor defaults to a higher priority than the built-in editor, which is how it can override one.
- A duplicate combination of target type, inheritance rule, and priority fails registration with an explicit error rather than being silently deduplicated.

## Error isolation

Exceptions thrown by a script or editor are caught:

- That Inspector falls back to generic drawing and shows the error.
- After enough consecutive failures the script editor is disabled.
- Other panels are unaffected.
- Recompiling scripts rescans the registry and rebuilds open editors and windows.

## Controlled control set

`EditorGUILayout` provides `Label`, `Space`, `Toggle`, `IntField`, `FloatField`, `Slider`, `TextField`, `Button`, `HelpBox`, and `PropertyField`, plus `BeginHorizontal` / `EndHorizontal` and `BeginVertical` / `EndVertical`.

## Current boundaries

- Script `CustomPropertyDrawer` registration is not exposed.
- There is no arbitrary ImGui passthrough.
- Extension points are limited to Inspectors, tool windows, and menu items.

## Relationship to native editor code

Built-in panels (Hierarchy, Inspector, Asset Browser, Console, Profiler, Project Settings, and others) are implemented in the C++ editor and register through the same panel and command system. Script extensions are peers of built-in panels for menus and panel management, but scripts only receive the controlled API.

Panels open and close on demand. The quickest way to check whether a panel registered is to open the `Window` menu and look for it.

## Advice for extension authors

1. Start with a minimal Inspector under `Assets/**/Editor/` to verify the registration path.
2. Access fields through `serializedObject`; never cache component objects.
3. For multi-object editing use `[CanEditMultipleObjects]` and avoid assuming a single target.
4. When you need a new control type, extend the controlled API first and then update this page.

## Next steps

- [UI Designer and Editor Extensions](../ui/designer.md) has the full sample code.
- [Module Structure](modules.md) shows where assemblies and tools live.
