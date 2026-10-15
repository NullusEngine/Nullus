# Editor and Project Settings

Settings live in three layers: the project file (startup options), project settings (version controlled with the project), and user settings (machine-local preferences).

## The three layers

| Layer | Location | Typical content | Committed |
| --- | --- | --- | --- |
| Project file | `<Project>/<Name>.nullus` | Resolution, graphics backend, thread mode, debug switches | Yes |
| Project settings | `<Project>/ProjectSettings/` | Tags and layers, project-level editor settings | Yes |
| User settings | `<Project>/UserSettings/editor-settings.json` | Viewport caches, panel layout, local debug switches | No |

`Edit > Settings...` opens the project settings panel. Settings are registered per object; group IDs include `editor.large-scene`, `editor.rendering`, `editor.runtime`, `editor.view`, `editor.debug-draw`, and `editor.scene-tools`.

## Project file

`<Name>.nullus` uses a simple `key=value` text format written by the Launcher and read by the editor and game. Startup options cannot change while running.

```ini
gravity=-9.810000
executable_name=Game
x_resolution=1280
y_resolution=720
fullscreen=false
start_scene=
vsync=false
multi_sampling=true
samples=4
graphics_backend=DX12
dev_build=true
last_opened_scene=New Scene.scene
csharp_debugging=true
lua_panda_debugging=true
lua_panda_port=8818
stop_on_entry=false
enable_threaded_rendering=true
renderdoc_startup_capture_frames=0
```

| Key | Meaning |
| --- | --- |
| `gravity` | Global gravity acceleration |
| `executable_name` | Game executable name |
| `x_resolution` / `y_resolution` | Startup resolution |
| `fullscreen` | Fullscreen mode |
| `start_scene` | Startup scene; falls back to `last_opened_scene` when empty |
| `vsync` | Vertical synchronization, shared with the game runtime |
| `multi_sampling` / `samples` | Multisample toggle and sample count |
| `graphics_backend` | Preferred graphics backend |
| `dev_build` | Whether to run as a development build |
| `last_opened_scene` | Scene the editor opened last |
| `csharp_debugging` | Enables C# debugging support |
| `lua_panda_debugging` / `lua_panda_port` | LuaPanda toggle and port |
| `stop_on_entry` | Whether to pause at entry |
| `enable_threaded_rendering` | Threaded rendering toggle |
| `renderdoc_startup_capture_frames` | Frame number for an automatic startup capture; `0` disables it |

Missing keys keep their defaults. Code distinguishes an explicitly written value from an unmigrated legacy project through `has*` flags, so adding keys does not break existing projects.

## Project settings

Project settings live under `ProjectSettings/` and are committed with the project. They currently cover:

- **Tags and layers**: the value source for a GameObject's `tag` and `layer`.
- **Project-level editor settings**: edit behavior that should match across a team.

Writes go through a serialized backend, and the setting objects are themselves serializable objects described by reflection.

## User settings

`UserSettings/editor-settings.json` stores machine-local preferences grouped by setting object:

```json
{
  "objects": {
    "editor.large-scene": {
      "fields": {
        "enableHZBOcclusion": true
      }
    }
  }
}
```

The editor writes this file back on exit after a toggle or layout change. This layer should not be committed.

## Rendering-related settings

- The HZB occlusion switch under `editor.large-scene` supports large-scene A/B validation; see [Visibility and Occlusion](../large-scenes/visibility-occlusion.md).
- The Scene View static cache is off by default so a benchmark cannot skip submission and inflate the frame rate when nothing changed.
- Candidate render paths and visibility strategies are exposed as options in project settings, with conservative defaults.

## Shortcut settings

Shortcut configuration is also stored per user as a versioned profile:

```json
{
  "version": 1,
  "profileName": "Default",
  "bindings": {},
  "unassigned": []
}
```

`bindings` holds overridden commands and `unassigned` records commands whose binding was explicitly cleared. Everything else keeps the built-in default.

## Next steps

- [Project Layout and Command Line](../reference/layout-cli.md#project-file) for the full key list and directory conventions.
- [Build Options](../reference/build-options.md) for compile-time switches.
