# Profiling and Graphics Debugging

Performance work happens in two layers: first locate the stage with CPU/GPU time, then inspect resources and state with a capture tool. This page covers Nullus's marker API, capture workflows, and reading order.

## Shared profiling markers

Engine and editor code mark work through a unified facade rather than tool-specific calls at each site.

| Macro | Purpose |
| --- | --- |
| `NLS_PROFILE_SCOPE()` | Uses the calling function name as the event name |
| `NLS_PROFILE_NAMED_SCOPE("name")` | Uses a custom display name |
| `NLS_PROFILE_REGISTER_THREAD("name")` | Registers a thread name at the entry point |
| `NLS_GPU_PROFILE_SCOPE(nativeCommandBuffer)` | GPU command recording scope |
| `NLS_GPU_PROFILE_NAMED_SCOPE(nativeCommandBuffer, "name")` | Named GPU scope |

```cpp
void Editor::Core::Editor::Update(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    // work to measure
}
```

Thread registration writes the name into shared scope events and forwards it to Tracy, so the track is already named before a capture connects. An unregistered thread gets a stable `Thread <id>` fallback.

RHI code should prefer `RHICommandBuffer::BeginGpuProfileScope()` / `EndGpuProfileScope()` and let the backend decide how to bridge them.

## Build switches

| Option | Default | Effect |
| --- | --- | --- |
| `NLS_ENABLE_PROFILING` | `ON` | Shared profiling marker behavior |
| `NLS_ENABLE_TRACY` | `OFF` | Tracy destination |
| `NLS_ENABLE_TIMELINE_PROFILER` | `OFF` | Built-in editor Timeline Profiler |
| `NLS_BUILD_TRACY_PROFILER` | Follows `NLS_ENABLE_TRACY` | Builds the Tracy GUI tool |

With all three off, instrumented code still builds and runs; there is simply no destination.

## Tracy

Tracy provides CPU and GPU timelines. When enabled on Windows, the DX12 command buffer bridge carries the native command queue: one context per profileable direct and compute queue, plus copy queues only when the device reports copy-queue timestamp support. A queue calibration failure leaves only that queue unprofiled; it never disables CPU scopes or other queues.

Editor builds deploy three tool executables to `App/<platform>_<config>_Runtime_*/Tracy/<version>` next to `Editor.exe`, so the packaged directory carries a protocol-matched profiler. The same tools are exposed through `Tools > Tracy Profiler`, which shows the resolved paths, launches the GUI, and opens the capture directory.

The editor emits one CPU frame mark per editor frame, and DX12 command-list submission batches advance the matching GPU context, so one capture shows CPU scopes and GPU pass times on the same timeline.

### Headless capture

```powershell
tracy-capture -o Build\Tracy\capture.tracy -f -s 30
App\Win64_RelWithDebInfo_Runtime_Shared\Editor.exe --backend dx12 path\to\Project.nullus
tracy-csvexport --truncated_mean=95 Build\Tracy\capture.tracy > Build\Tracy\cpu-total.csv
tracy-csvexport -e --truncated_mean=95 Build\Tracy\capture.tracy > Build\Tracy\cpu-self.csv
tracy-csvexport -g Build\Tracy\capture.tracy > Build\Tracy\gpu-events.csv
```

Start the capture before launching the editor, trigger the workload (including Play mode) while it runs, and only compare captures with matching build, project, scene, backend, and frame count.

### Reading order

1. Rank CPU total/self and GPU pass times using the CSV exports.
2. Use the Tracy GUI when call stacks or frame correlation are needed.
3. After Tracy identifies a pass or queue, switch to RenderDoc for draws, shaders, and resources.
4. When you want the in-editor DX12 GPU timeline without connecting Tracy, use Timeline Profiler.

For steady-state editor or Play measurement, attach Tracy after startup and asset restore complete. Capture from the beginning only when startup is the target.

## Timeline Profiler

The editor registers a dockable `Profiler` panel. Without a destination enabled, the panel reports availability and explains why live data is missing. With `NLS_ENABLE_TIMELINE_PROFILER`, the panel draws the Timeline Profiler HUD.

Timeline Profiler GPU support requires the native DX12 device and graphics queue to be initialized. CPU scopes remain valid when GPU data is unavailable.

## GPU scopes and queues

- Only destinations reporting `ProfilerCapability_GPUScopes` receive GPU events.
- The DX12 backend bridges GPU scopes to Timeline Profiler's D3D12 timestamp profiler (with `NLS_ENABLE_TIMELINE_PROFILER=ON`) or Tracy's DX12 contexts (with `NLS_ENABLE_TRACY=ON`).
- Other backends keep CPU profiling and report GPU scopes as unavailable.

## RenderDoc

RenderDoc is the first choice for rendering problems. The runtime probes these locations: `RENDERDOC_PATH`, `%ProgramFiles%\RenderDoc`, `%ProgramFiles(x86)%\RenderDoc`, and `PATH`. Without it, the application runs normally and capture actions are simply unavailable.

### In-application actions

| Action | Location |
| --- | --- |
| Queue the next frame | `Debugging > RenderDoc > Capture Next Frame` (F11) |
| Open the latest capture | `Debugging > RenderDoc > Open Latest Capture` (Ctrl+F11) |
| Open the capture folder | `Debugging > RenderDoc > Open Capture Folder` |
| Auto-open the Replay UI | `Debugging > RenderDoc > Auto Open Replay UI` |

### Command line

```powershell
Editor.exe --renderdoc MyProject.nullus
Editor.exe --capture-after-frames 60 MyProject.nullus
Game.exe --renderdoc
Game.exe --capture-after-frames 120
Editor.exe --backend dx12 --renderdoc MyProject.nullus
```

### Environment variables

| Variable | Effect |
| --- | --- |
| `NLS_RENDERDOC_ENABLE=1` | Enable RenderDoc |
| `NLS_RENDERDOC_CAPTURE=1` | Queue a startup capture |
| `NLS_RENDERDOC_CAPTURE_AFTER_FRAMES=N` | Capture after N frames |
| `NLS_RENDERDOC_CAPTURE_DIR=<path>` | Custom capture directory |
| `NLS_RENDERDOC_CAPTURE_LABEL=<name>` | Capture file label |
| `NLS_RENDERDOC_AUTO_OPEN=1` | Auto-open the Replay UI after capture |

Command-line flags take precedence over environment variables, which take precedence over defaults.

### Capture output

| Target | Directory |
| --- | --- |
| Editor | `<project>/Logs/RenderDoc/Editor/` |
| Game | `<project>/Logs/RenderDoc/Game/` |

The repository runner adds a backend directory below that, for example `TestProject/Logs/RenderDoc/Editor/dx12/`. Output directory priority is `--capture-dir`, `NLS_RENDERDOC_CAPTURE_DIR`, then the project default.

### Runner

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12 --capture --open-capture-ui
```

The runner defaults to `launch-mode=auto`, can use `renderdoccmd capture` on DX12, locates `Editor.exe` or `Game.exe` under `App`, defaults the Editor project to `TestProject/TestProject.nullus`, writes captures under `<project>/Logs/RenderDoc/<target>/<backend>`, and reports the exact `latest_capture` path after capture.

### Capture analysis

```powershell
py -3 Tools/RenderDoc/rdc_analyze.py TestProject/Logs/RenderDoc/Editor/dx12/editor_dx12_DX12_frame2.rdc
py -3 Tools/RenderDoc/rdc_analyze.py path\to\capture.rdc --focus-eid 66
py -3 Tools/RenderDoc/rdc_analyze.py path\to\capture.rdc --json-out Build\RenderDocAnalysis\capture.json
py -3 Tools/RenderDoc/rdc_analyze.py path\to\capture.rdc --skip-doctor
```

The analyzer runs `rdc doctor` (skippable with `--skip-doctor`), opens the capture, collects `info`, `stats`, `passes`, `draws`, and `events`, picks a focus event automatically, loads pipeline, binding, and shader metadata, and prints a Markdown analysis skeleton. If `rdc --session` fails, it falls back to RenderDoc Python replay.

## Debugging policy

- Use RenderDoc first for rendering problems.
- Fall back to screenshots only for desktop or interface problems RenderDoc cannot cover.
- Record both the requested and the actual backend whenever a fallback happens.
- Performance conclusions require repeated measurements on matching builds and workloads.

## HZB and large-scene validation

Large-scene occlusion validation must run on the active DX12 runtime path and archive the capture alongside the validation record:

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12 --capture --open-capture-ui
```

What to record is listed in [Visibility and Occlusion](../large-scenes/visibility-occlusion.md). Unit tests cannot replace real DX12 capture evidence.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| `TimelineProfiler: Disabled` | The build switch is off; configure with `NLS_ENABLE_TIMELINE_PROFILER=ON` |
| `TimelineProfiler: Unavailable` | The destination is enabled but cannot initialize in this environment |
| Tracy destination unavailable | `NLS_ENABLE_TRACY` is off, or the third-party source is missing |
| Tracy connects but has no GPU data | Requires Windows/DX12, a queue with timestamp calibration support, and `--backend dx12` |
| Timeline has no GPU data | Requires the native DX12 device and graphics queue; CPU data remains valid |

## Next steps

- [Debugging and Testing](debugging.md) covers behavior regressions and CI coverage.
- [Graphics Backends and Large-Scene Features](../rendering/backends.md) covers capability queries and conservative fallback.
