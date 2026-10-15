# Your First Project

This page takes the shortest path to a running result: create a project, build a visible cube scene, attach a first script, and enter Play. Manage the project and scene in the editor and edit script contents in an external code editor.

## Prerequisites

`Editor` and `Launcher` are already built, as described in [Environment and Build](build.md).

## 1. Create a project

Start the Launcher, choose to create a project, and provide a name, a disk location, and a template. The creation wizard:

1. Creates the project directory plus `Assets/`, `ProjectSettings/`, and `UserSettings/`.
2. Copies the selected template's `content/` into the project. The built-in Empty Project template lives in `App/Assets/Templates/EmptyProject` and ships `Assets/Scripts/ExampleBehaviour.cs` and `ExampleBehaviour.lua`.
3. Writes the `<Project>.nullus` project file.

`Library/` is generated the first time the editor opens the project.

`<Project>.nullus` records startup-time options such as resolution, graphics backend, start scene, and whether threaded rendering is enabled. These are read while the editor and game start and cannot change at runtime. The full key list is in the [Project Layout and Command Line](../reference/layout-cli.md#project-file) reference.

## 2. Open the editor

Open the new project from the Launcher. Confirm that the title and project path point to the directory you just created. The first open requires asset initialization and script compilation; an asset still being imported is not necessarily missing.

## 3. Build a scene

1. Choose `File > New Scene` (`Ctrl+N`). The current editor creates a base scene with `Main Camera`, a directional light, ambient light, and a skybox.
2. Choose `GameObjects > Primitives > Cube`. Select Cube in the Hierarchy and set its Transform position to `(0, 0, 5)`, rotation to `(0, 0, 0)`, and scale to `(1, 1, 1)` in the Inspector.
3. Select `Main Camera` and confirm that both position and rotation are `(0, 0, 0)`. Forward is `+Z`, so the camera faces the cube. Keep the camera and cube at different positions.
4. Keep the default directional and ambient lights. If your scene has none, use `GameObjects > Others > Camera` and `GameObjects > Lights > Light` to create separate objects, then configure the light type in the Inspector.
5. Choose `File > Save Scene As...` (`Ctrl+Shift+S`) and save to `Assets/Scenes/FirstScene.scene` inside the project. Use `Ctrl+S` for subsequent saves.

Expected result: the Hierarchy contains Cube, a camera, and lights; the Game View shows the cube in Play mode. Scene View uses a separate editor camera, so its composition does not establish what the game camera sees.

For external geometry, follow [Models, Meshes, and Textures](../assets/models-textures.md). Scene and Prefab file formats are described in [Reflection and Serialization](../architecture/reflection-serialization.md).

## 4. Write your first C# script

Open `Assets/Scripts` in the Asset Browser, creating the directory with `Create > Folder` if needed. Right-click empty space, choose `Create > C# Script`, and name it `Rotator`. Open `Rotator.cs` in a code editor and replace its contents with:

```csharp
using Nullus;

public sealed class Rotator : Behaviour
{
    private void Awake()
    {
        Debug.Log("Rotator awake");
    }

    private void Update()
    {
        transform.Rotate(Vector3.up * (30.0f * Time.deltaTime));
    }
}
```

Saving triggers an incremental build through the asset watcher. On success the editor hot reloads at a frame boundary and preserves fields; on failure the previous working assembly keeps running and the error appears in the Console.

After compilation succeeds, drag `Rotator.cs` from the Asset Browser onto the **Cube object row** in the Hierarchy, rather than empty space. Select Cube, confirm that the Inspector shows the Rotator script component, and save the scene again.

Press Play on the toolbar. The cube should rotate in the Game View and the Console should show `Rotator awake`. Press Stop to return to edit mode; the runtime rotation is not written back to the edit scene.

## 5. Write your first Lua script

This is an optional alternative to C#. Create a script with `Create > Lua Script`, edit it, and drag it onto another test object. Lua shares the native object model and API manifest. A script returns a module table and the lifecycle functions are members of that table:

```lua
local Module = {}

function Module:Update()
    local position = self.transform.localPosition
    position.x = position.x + Nullus.Time.deltaTime
    self.transform.localPosition = position
end

return Module
```

`Nullus` is the global API table exposed by the engine, for example `Nullus.Time`. Saving a `.lua` file needs no build step; it takes effect on the next run or reload. See [C# versus Lua](../scripting/overview.md#c-versus-lua) for the differences.

## 6. Run and observe

- Press Play. The Game View shows the runtime output while the Scene View keeps the editing camera.
- The Console collects script logs and errors.
- The Profiler panel shows Timeline or Tracy data when the matching build switches are on; see [Profiling and Graphics Debugging](../quality/profiling.md).

## Troubleshooting and limits

| Symptom | Check |
| --- | --- |
| No cube in the Game View | Check that the camera is enabled, Cube is along `+Z`, lights remain, and model/material import succeeded |
| A script cannot attach | Check Console compilation errors, inheritance from `Behaviour`, and that the drop targets an object row |
| Double-click does not open an IDE | Configure the external editor or open the file from disk; see [Script Debugging and Hot Reload](../scripting/debugging.md) |
| Rotation resets after Stop | This is expected Play sandbox behavior; change and save the initial Transform in edit mode |

The menus and APIs on this page have been checked against source. Example compilation, runtime unit tests, and a full desktop walkthrough are separate validation levels. Current acceptance scope is recorded in [Debugging and Testing](../quality/debugging.md).

## What appeared on disk

| Path | Contents |
| --- | --- |
| `Assets/` | Source assets and scripts |
| `ProjectSettings/` | Project-level settings |
| `UserSettings/` | Machine-local settings such as `editor-settings.json` |
| `Library/` | Import results, artifacts, thumbnail cache, and IDE workspace |
| `Library/ScriptApi/ScriptApi.json` | Merged scripting API manifest |

`Library/` is rebuildable intermediate data and should not be committed.

## Next steps

- [Editor Workspace](../editor/workspace.md) makes the interface familiar.
- [Scenes, GameObjects, and Components](../scene/objects.md) explains the object model.
- [Scripting API Reference](../scripting/api.md) lists the available types and members.
