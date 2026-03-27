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

The runtime now exposes RenderDoc capture support through `Driver`.

Supported trigger surfaces:

- `F11`: capture the next presented frame
- `Ctrl + F11`: open the latest `.rdc`
- Editor menu: `Settings -> Debugging -> RenderDoc`

Startup capture can be requested through environment variables:

- `NLS_GRAPHICS_BACKEND`
- `NLS_RENDERDOC_CAPTURE`
- `NLS_RENDERDOC_CAPTURE_AFTER_FRAMES`
- `NLS_RENDERDOC_CAPTURE_DIR`
- `NLS_RENDERDOC_CAPTURE_LABEL`
- `NLS_RENDERDOC_AUTO_OPEN`
- `NLS_LAUNCHER_GRAPHICS_BACKEND`

Notes:

- `Editor` now honors `NLS_GRAPHICS_BACKEND`, same as `Game`.
- Launching `Editor.exe <project.nullus>` now skips the launcher entirely, which keeps startup captures focused on the actual editor runtime.
- Capture files default to:
  - Editor: `<project>/Logs/RenderDoc/Editor`
  - Launcher: `<cwd>/Logs/RenderDoc/Launcher`
  - Game: `<cwd>/Logs/RenderDoc/Game`

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
