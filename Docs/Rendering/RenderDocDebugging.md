# RenderDoc Debugging Workflow

RenderDoc is the standard graphics debugger for Nullus render investigations.

For rendering bugs, prefer RenderDoc captures over desktop screenshots.

## Install

- Install RenderDoc for Windows.
- The runtime probes these locations:
  - `RENDERDOC_PATH`
  - `%ProgramFiles%\\RenderDoc`
  - `%ProgramFiles(x86)%\\RenderDoc`
  - `PATH`

If RenderDoc is not installed, the app still runs normally. Capture actions simply become unavailable.

## App Workflow

The runtime exposes RenderDoc capture support through `Driver`.

### Keyboard Shortcuts

- `F11`: capture the next presented frame
- `Ctrl + F11`: open the latest `.rdc`

### Editor Menu

Editor menu: `Settings -> Debugging -> RenderDoc`

- **Enabled**: toggle RenderDoc on/off
- **Capture Next Frame**: trigger a single-frame capture (same as F11)
- **Open Latest Capture**: open the most recent `.rdc` file (same as Ctrl+F11)
- **Open Capture Folder**: open the capture directory in Explorer
- **Auto Open Replay UI**: automatically open `qrenderdoc.exe` after each capture

## Command-Line Options

Both `Editor.exe` and `Game.exe` support these RenderDoc flags:

| Flag | Description |
|------|-------------|
| `--renderdoc` | Enable RenderDoc debugging |
| `--no-renderdoc` | Disable RenderDoc debugging |
| `--capture-after-frames <N>` | Automatically capture after N presents (enables RenderDoc) |

### Examples

```powershell
# Enable RenderDoc and open project
Editor.exe --renderdoc MyProject.nullus

# Auto-capture after 60 frames
Editor.exe --capture-after-frames 60 MyProject.nullus

# Game with RenderDoc enabled
Game.exe --renderdoc

# Auto-capture game after 120 frames
Game.exe --capture-after-frames 120
```

### Backend Selection

Use `--backend <name>` to select graphics backend (dx12, vulkan, opengl, dx11):

```powershell
Editor.exe --backend vulkan --renderdoc MyProject.nullus
```

## Environment Variables

Startup capture can also be requested through environment variables:

- `NLS_RENDERDOC_ENABLE=1` - enable RenderDoc
- `NLS_RENDERDOC_CAPTURE=1` - queue startup capture
- `NLS_RENDERDOC_CAPTURE_AFTER_FRAMES=N` - capture after N frames
- `NLS_RENDERDOC_CAPTURE_DIR=<path>` - custom capture directory
- `NLS_RENDERDOC_CAPTURE_LABEL=<name>` - capture file label
- `NLS_RENDERDOC_AUTO_OPEN=1` - auto-open qrenderdoc after capture

**Note:** Command-line flags take precedence over environment variables. Environment variables override defaults.

## Capture Output

Capture files are saved to:

- Editor: `Build/RenderDocCaptures/Editor/`
- Game: `Build/RenderDocCaptures/Game/`

## Runner Script

Use the repo runner to launch a capture-ready session:

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend vulkan --capture --open-capture-ui
```

Common examples:

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend vulkan --capture --capture-after-frames 180
py -3 Tools/RenderDoc/renderdoc_runner.py --target game --backend opengl --capture
```

Runner behavior:

- uses `launch-mode=auto` by default
- in `auto`, OpenGL launches use direct startup with early RenderDoc preload because that path is currently the most reliable in Nullus
- in `auto`, Vulkan and DX12 can still use `renderdoccmd capture` when available
- locates `Editor.exe` or `Game.exe` under `App`
- defaults Editor project to `TestProject/TestProject.nullus`
- writes captures under `Build/RenderDocCaptures/<target>/<backend>`
- can open `qrenderdoc.exe` automatically after capture
- uses a capture discovery step that detects newly written `.rdc` files and reports the exact `latest_capture` path

## rdc-cli Analysis

Use the repo analysis helper to turn an existing `.rdc` into a first-pass structured summary:

```powershell
py -3 Tools/RenderDoc/rdc_analyze.py Build/RenderDocCaptures/game/opengl/game_opengl_OpenGL_frame2.rdc
```

What it does:

- runs `rdc doctor` unless you opt out
- opens the capture
- collects `info`, `stats`, `passes`, `draws`, and `events`
- picks a focus EID automatically from the first sampled draw unless you provide `--focus-eid`
- loads `pipeline`, `bindings`, and shader metadata for that focus event
- closes the session and prints a Markdown analysis skeleton
- if `rdc --session` fails (daemon crash or missing CLI), it falls back to RenderDoc Python replay to keep analysis working

Useful variants:

```powershell
py -3 Tools/RenderDoc/rdc_analyze.py path\to\capture.rdc --focus-eid 66
py -3 Tools/RenderDoc/rdc_analyze.py path\to\capture.rdc --json-out Build\RenderDocAnalysis\capture.json
py -3 Tools/RenderDoc/rdc_analyze.py path\to\capture.rdc --skip-doctor
```

## Debugging Policy

When debugging rendering issues in Nullus:

- use RenderDoc first
- only fall back to screenshots for desktop/UI problems that RenderDoc cannot help with
- always record the requested backend and the actual backend when a fallback occurs
