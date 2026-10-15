# Project Layout and Command Line

This is a lookup page: what each file in a project does, and which launch options the executables accept.

## Project layout

| Path | Committed | Content |
| --- | --- | --- |
| `<Project>.nullus` | Yes | Project file holding startup options |
| `Assets/` | Yes | Source assets and scripts |
| `ProjectSettings/` | Yes | Project-level settings such as tags and layers |
| `UserSettings/` | No | Machine-local user settings |
| `Library/` | No | Import results, artifacts, thumbnail cache, and IDE workspace |
| `Logs/` | No | Runtime logs and RenderDoc captures |

`Library/` can be deleted and regenerated. Only the project file, `Assets/`, and `ProjectSettings/` need to be preserved long term.

### Templates

Project templates live under `App/Assets/Templates/<template id>/`:

| File | Notes |
| --- | --- |
| `template.json` | Template name, description, category, and sort order |
| `content/` | Initial content copied into a new project |
| Preview image | Optional, shown when listing templates |

The built-in Empty Project template's `content/` contains `Assets/Scripts/ExampleBehaviour.cs` and `ExampleBehaviour.lua`.

## Project file

`<Project>.nullus` uses a `key=value` text format written by the Launcher and read by the editor and game.

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

| Key | Notes |
| --- | --- |
| `gravity` | Global gravity acceleration; currently a configuration value not yet consumed by a solver |
| `executable_name` | Game executable name |
| `x_resolution` / `y_resolution` | Startup resolution |
| `fullscreen` | Fullscreen mode |
| `start_scene` | Startup scene; falls back to `last_opened_scene` when empty |
| `vsync` | Vertical synchronization shared by the editor and game |
| `multi_sampling` / `samples` | Multisample toggle and sample count |
| `graphics_backend` | Preferred graphics backend |
| `dev_build` | Whether to run as a development build |
| `last_opened_scene` | Scene the editor opened last |
| `csharp_debugging` | Enables C# debugging support |
| `lua_panda_debugging` / `lua_panda_port` | LuaPanda toggle and port |
| `stop_on_entry` | Whether to pause at entry |
| `enable_threaded_rendering` | Threaded rendering toggle |
| `renderdoc_startup_capture_frames` | Frame number for an automatic startup capture; `0` disables it |

Missing keys keep their defaults. When a project file has no `rhi_thread_mode`, each backend uses its own default mode; writing `rhi_thread_mode` makes the value explicit.

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

## Editor command line

```text
Editor [options] [project_path]
```

`project_path` is a `.nullus` project file or a project directory, and is required.

### General options

| Option | Notes |
| --- | --- |
| `--backend <name>` / `-b <name>` | Select the graphics backend for this run |
| `--renderdoc` / `--no-renderdoc` | Toggle RenderDoc integration |
| `--capture-after-frames <N>` | Auto-capture after N presents |
| `--help` / `-h` | Show help |
| `--editor-prewarm-assets` | Import project assets and exit without opening the editor |
| `--editor-dx12-debug-layer` | Enable DX12 validation in Debug builds (slower startup) |
| `--editor-startup-resource-pipeline <async\|legacy\|pipelined>` | Startup resource configuration; Windows defaults to `pipelined`, other platforms to `async` |

### Startup validation options

| Option | Notes |
| --- | --- |
| `--editor-validation-focus-view <scene\|game>` | Focus a view during startup validation |
| `--editor-validation-exclusive-view <scene\|game>` | Close the other view during startup validation |
| `--editor-validation-open-frame-info` | Open Frame Info |
| `--editor-validation-open-profiler` | Open Profiler |
| `--editor-validation-trace-frames <N>` | Export a Timeline Profiler trace for N frames |
| `--editor-validation-select-gameobject <name>` | Select a named GameObject |
| `--editor-validation-camera-forward-frames <N>` | Step the Scene View camera forward for N fixed-step frames |
| `--editor-validation-create-asset <path>` | Create an asset instance |
| `--editor-validation-asset-browser-folder <path>` | Select an Asset Browser folder |
| `--editor-validation-disable-hzb-occlusion` | Disable HZB occlusion for A/B comparison |
| `--editor-validation-occlusion-stack <N>` | Create N overlapping cubes for occlusion validation |
| `--editor-validation-scene-readback-output <path>` | Write a Scene View render target PNG |
| `--editor-validation-scene-readback-summary <path>` | Write a Scene View readback summary |
| `--editor-validation-prefab-drag-proxy-summary <path>` | Write a Prefab drag proxy summary |
| `--editor-validation-scene-camera <pos;rot>` | Force the Scene View camera, for example `1,2,3;10,20,30` |
| `--editor-log-render-draw-path` | Log renderer draw and package diagnostics |
| `--editor-log-scene-camera-input` | Log Scene View input and camera movement diagnostics |

### Camera benchmark options

| Option | Notes |
| --- | --- |
| `--editor-camera-performance-output <path>` | Write a low-overhead Scene View camera benchmark JSON |
| `--editor-camera-performance-warmup-frames <N>` | Warm-up frames, default 30 |
| `--editor-camera-performance-frames <N>` | Measured frames, default 300 |
| `--editor-camera-performance-settle-frames <N>` | Static frames captured after camera motion, default 0 |

### Thumbnail and telemetry options

| Option | Notes |
| --- | --- |
| `--editor-thumbnail-telemetry-summary <path>` | Write a thumbnail telemetry summary on editor shutdown |
| `--editor-thumbnail-resident <0\|1>` | Enable the resident Prefab thumbnail source |
| `--editor-thumbnail-proxy-pool <0\|1>` | Enable the preview proxy pool |
| `--editor-thumbnail-atlas <0\|1>` | Enable the decoded thumbnail atlas |
| `--editor-thumbnail-readback-ring <0\|1>` | Enable the GPU readback ring |
| `--editor-thumbnail-adaptive-budget <0\|1>` | Enable the adaptive thumbnail budget |
| `--editor-thumbnail-lanes <0\|1>` | Enable explicit thumbnail lanes |
| `--editor-thumbnail-cache-root <path>` | Override the thumbnail cache root |

### Examples

```powershell
Editor.exe MyProject.nullus
Editor.exe --renderdoc MyProject.nullus
Editor.exe --capture-after-frames 60 MyProject.nullus
Editor.exe --backend dx12 --editor-dx12-debug-layer MyProject.nullus
```

## Game command line

See [Game Runtime and Asset Manifests](../platform-build/runtime.md#game-command-line).

## Environment variables

| Variable | Effect |
| --- | --- |
| `NLS_BUILD_JOBS` | Overrides build parallelism |
| `NLS_MSVC_MP_JOBS` | Limits target-internal MSVC `/MP` processes |
| `NLS_BUILD_TARGETS` | Restricts the build to named targets |
| `NLS_CMAKE_OPTIONS` | Passes extra CMake configuration arguments |
| `NLS_RENDERDOC_ENABLE` / `NLS_RENDERDOC_CAPTURE` / `NLS_RENDERDOC_CAPTURE_AFTER_FRAMES` | RenderDoc startup capture |
| `NLS_RENDERDOC_CAPTURE_DIR` / `NLS_RENDERDOC_CAPTURE_LABEL` / `NLS_RENDERDOC_AUTO_OPEN` | RenderDoc capture output and automation |

## Next steps

- [Build Options](build-options.md) for compile-time switches.
- [Editor and Project Settings](../editor/settings.md) for the meaning of each setting.
