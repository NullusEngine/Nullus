# Nullus Profiling

Nullus exposes a shared profiler facade in `Runtime/Base/Profiling`. Engine and editor code should add one shared marker instead of placing tool-specific Tracy or TimelineProfiler calls at each call site.

## Scoped Markers

Use `NLS_PROFILE_SCOPE()` for the common case. The event name is captured from the calling function name.

```cpp
void Editor::Core::Editor::Update(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    // Work to measure...
}
```

Use `NLS_PROFILE_NAMED_SCOPE("Display Name")` when a clearer domain label is better than the function name.

```cpp
void RenderSceneView()
{
    NLS_PROFILE_NAMED_SCOPE("Render Scene View");
}
```

Both markers route through `NLS::Base::Profiling::Profiler` and can feed every enabled destination.

## Thread Names

Use `NLS_PROFILE_REGISTER_THREAD("Name")` once near a worker thread entry point. Registration records the name on shared scope events and forwards it to the Tracy destination (`tracy::SetThreadName`) so the track name exists before a capture connects; registering an empty name, or the same name twice on one thread, is ignored. A thread that emits a scope without registering receives a stable `"Thread <id>"` fallback.

Startup names the Game Thread, `"Render Thread"`, the dedicated `"RHI Thread"`, `"TaskGraph Worker <n>"`, `"Background Job Worker <n>"`, and the Editor's metadata/import, startup-cache, prefab-preparation, shader compile/reflect/output, file-watcher, managed-script-build, debug-endpoint, progress-dialog and trace-writer workers. `Tasks` RHI work keeps the physical TaskGraph worker name instead of claiming a dedicated RHI thread.

## GPU Markers

Use `NLS_GPU_PROFILE_SCOPE(nativeCommandBuffer)` or `NLS_GPU_PROFILE_NAMED_SCOPE(nativeCommandBuffer, "Display Name")` only around GPU command recording work. The macro routes through the same profiler facade, but only destinations that report `ProfilerCapability_GPUScopes` receive the event.

RHI code should prefer the backend-neutral `RHICommandBuffer::BeginGpuProfileScope()` and `EndGpuProfileScope()` hooks. The DX12 backend bridges those hooks to TimelineProfiler's D3D12 timestamp profiler when `NLS_ENABLE_TIMELINE_PROFILER=ON` and the native DX12 device/queues have initialized the GPU profiler. Other backends keep CPU profiling available and report GPU scope support as unavailable/unsupported.

When Tracy is enabled on Windows, the DX12 command-buffer bridge also carries the native command queue. Nullus creates one Tracy D3D12 context for each profileable direct/compute queue and for copy queues only when the device reports copy-queue timestamp support. Queue calibration failures leave that queue unprofiled without disabling CPU scopes or other queues.

## Tracy Capture

`NLS_ENABLE_TRACY=ON` builds the Tracy client destination. Prefer `NLS_BUILD_TRACY_PROFILER=OFF` for normal Editor builds and obtain the matching Tracy tools separately. The vendored release tools are `tracy-profiler`, `tracy-capture`, and `tracy-csvexport`; if the exact vendored patch has no official binary release, build those tools from the vendored `ThirdParty/Tracy` source so the client/server protocol versions match.

Editor builds deploy the three tool executables from `Tools/Tracy/<version>` into `App/<platform>_<config>_Runtime_*/Tracy/<version>` next to `Editor.exe`, so a packaged output directory carries a protocol-matched profiler. The same tools are exposed in the editor through `Tools > Tracy Profiler`, which shows the resolved paths, launches the GUI profiler, and opens the capture directory.

The Editor sends one Tracy CPU frame mark per editor frame. DX12 command-list submission batches also advance the corresponding Tracy GPU context, so one capture contains CPU scopes and GPU pass timings on the same timeline. The shared `NLS_PROFILE_*` facade remains the instrumentation contract; do not add Tracy-specific calls at engine call sites.

Example headless capture:

```powershell
tracy-capture -o Build\Tracy\capture.tracy -f -s 30
App\Win64_RelWithDebInfo_Runtime_Shared\Editor.exe --backend dx12 path\to\Project.nullus
tracy-csvexport --truncated_mean=95 Build\Tracy\capture.tracy > Build\Tracy\cpu-total.csv
tracy-csvexport -e --truncated_mean=95 Build\Tracy\capture.tracy > Build\Tracy\cpu-self.csv
tracy-csvexport -g Build\Tracy\capture.tracy > Build\Tracy\gpu-events.csv
```

Start the capture before launching the Editor, trigger the workload (including Play mode) while the capture window is open, and compare only captures with matching build, project, scene, backend, and frame count. Use the CSV exports to rank CPU total/self and GPU pass time first; use the Tracy GUI when call-stack or frame correlation is needed. Use RenderDoc for draw/shader/resource detail after Tracy identifies a pass or queue, and use TimelineProfiler for the in-editor DX12 GPU timeline when Tracy is not connected.

For steady-state Editor or Play-mode work, attach Tracy after Editor startup and asset restore have completed; otherwise the capture can be dominated by startup work and can end before steady rendering begins. Keep the capture-first order only when startup itself is the target.

### Startup Resource Prewarm

Editor startup uses the asynchronous scene-resource pipeline by default. Pass
`--editor-startup-resource-pipeline legacy` to restore the previous synchronous
prefab prewarm path for A/B comparison or rollback. Scene mesh artifact jobs are
high priority; material and texture decoding stay normal priority so queue
fairness advances geometry without starving textures.

Asset preimport and texture compression can be prepared before a timing run:

```powershell
Tools\Performance\Warm-NullusProjectAssets.ps1 -ProjectPath .\TestProject\TestProject.nullus
```

The script runs `Editor.exe --editor-prewarm-assets <project>` without opening
the editor, forces a complete startup-source import, and verifies available disk
space before writing artifacts. Record the one-time migration duration
separately; startup acceptance timing uses the following cache-hit launches.

Before measuring render performance, confirm `Editor/Rendering > Rendering > Scene View Static Cache` is disabled. It defaults to off so an unchanged Scene View cannot skip renderer submission and inflate the captured frame rate.

## Destination Switches

Profiling is controlled through CMake options:

- `NLS_ENABLE_PROFILING`: enables shared profiling instrumentation behavior.
- `NLS_ENABLE_TRACY`: enables the Tracy destination when `ThirdParty/Tracy` is available.
- `NLS_ENABLE_TIMELINE_PROFILER`: enables the in-editor TimelineProfiler destination built into `NLS_UI` as a Dear ImGui extension.

The default configuration keeps all three options off. Instrumented code still builds and runs when profiler tools are unavailable.

## Editor Panel

The editor registers a dockable `Profiler` panel. In the current fallback state, the panel reports TimelineProfiler availability and explains why live timeline data is unavailable. When `NLS_ENABLE_TIMELINE_PROFILER` is enabled, the panel draws the TimelineProfiler HUD from `Runtime/UI/ImGuiExtensions/TimelineProfiler`.

## Dear ImGui Extensions

TimelineProfiler is source-integrated into the UI module instead of being a standalone third-party target. New Dear ImGui extensions should follow the same pattern:

1. Add source files under `Runtime/UI/ImGuiExtensions/<Name>/`.
2. Preserve upstream license and provenance in `LICENSE` and `UPSTREAM.md`.
3. Register the extension in `Runtime/UI/ImGuiExtensions/CMakeLists.txt` with `nls_register_imgui_extension`.
4. Keep local compatibility shims inside the extension folder.
5. Consume the extension through `NLS_UI`; do not add per-extension targets to `ThirdParty/CMakeLists.txt`.

## Troubleshooting

- `TimelineProfiler: Disabled`: the build option is off. Reconfigure with `NLS_ENABLE_TIMELINE_PROFILER=ON`.
- `TimelineProfiler: Unavailable`: the destination was enabled but cannot be initialized or used in the current environment.
- `Tracy destination is not enabled or ThirdParty/Tracy is unavailable`: build with `NLS_ENABLE_TRACY=ON` and make sure `ThirdParty/Tracy` is present.
- Tracy connects but GPU scopes are missing: verify the build is Windows/DX12, the queue has timestamp calibration support, and the Editor was launched with `--backend dx12`. CPU scopes remain valid when GPU support is unavailable.
- TimelineProfiler GPU data unavailable: CPU scopes remain valid. TimelineProfiler GPU support requires the native DX12 device plus graphics queue to be initialized; Tracy GPU capture uses its own DX12 queue contexts when `NLS_ENABLE_TRACY=ON`.
