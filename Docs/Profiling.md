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

Use `NLS_PROFILE_REGISTER_THREAD("Name")` once near a worker thread entry point. Thread names are carried on shared scope events so TimelineProfiler and Tracy destinations can show render and RHI work on recognizable tracks.

The threaded renderer registers `"Render Thread"` and `"RHI Thread"` when those workers start.

## GPU Markers

Use `NLS_GPU_PROFILE_SCOPE(nativeCommandBuffer)` or `NLS_GPU_PROFILE_NAMED_SCOPE(nativeCommandBuffer, "Display Name")` only around GPU command recording work. The macro routes through the same profiler facade, but only destinations that report `ProfilerCapability_GPUScopes` receive the event.

RHI code should prefer the backend-neutral `RHICommandBuffer::BeginGpuProfileScope()` and `EndGpuProfileScope()` hooks. The DX12 backend bridges those hooks to TimelineProfiler's D3D12 timestamp profiler when `NLS_ENABLE_TIMELINE_PROFILER=ON` and the native DX12 device/queues have initialized the GPU profiler. Other backends keep CPU profiling available and report GPU scope support as unavailable/unsupported.

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
- GPU timeline data unavailable: CPU scopes remain valid. GPU timeline support is currently DX12 TimelineProfiler-specific and requires the native DX12 device plus graphics queue to be initialized.
