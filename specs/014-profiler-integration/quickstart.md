# Quickstart: Unified Profiler Integration

## Configure And Build

From the repository root on Windows:

```powershell
cmake -S . -B Build -G "Visual Studio 17 2022" -A x64
cmake --build Build --config Debug --target NullusUnitTests -- /m:1
cmake --build Build --config Debug --target Editor -- /m:1
```

Expected result:

- `NullusUnitTests` builds successfully.
- `Editor` builds successfully without requiring a Tracy viewer or an open profiler window.

## Run Automated Tests

```powershell
ctest --test-dir Build -C Debug --output-on-failure
```

Expected result:

- Profiler facade tests pass for disabled profiling, default function-name labels, explicit labels, nested scopes, and destination independence.
- Existing unit tests continue to pass.

Latest focused validation:

- 2026-05-05: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ProfilerScopeTest.*:ProfilerDestinationTest.*:PanelWindowHookTests.ProfilerPanelReportsTimelineDisabledByDefault` passed 15/15 focused profiler tests.
- 2026-05-05: `cmake --build Build --config Debug --target Editor -- /m:1` completed successfully after the `Profiler` panel was registered.
- 2026-05-05: `BuildProfiler\bin\Debug\NullusUnitTests.exe --gtest_filter=ProfilerScopeTest.*:ProfilerDestinationTest.*:PanelWindowHookTests.ProfilerPanelReportsTimelineDisabledByDefault` passed 16/16 focused profiler tests with `NLS_ENABLE_PROFILING=ON`, `NLS_ENABLE_TRACY=ON`, `NLS_ENABLE_TIMELINE_PROFILER=ON`, and `TRACY_ENABLE=ON`.
- 2026-05-05: `cmake --build BuildProfiler --config Debug --target Editor -- /m:1` completed successfully with Tracy and TimelineProfiler enabled.
- 2026-05-05: `ctest --test-dir Build -C Debug --output-on-failure` passed 1/1 tests for the default disabled-profiler build.
- 2026-05-05: `ctest --test-dir BuildProfiler -C Debug --output-on-failure` passed 1/1 tests for the enabled-profiler build before the temporary `BuildProfiler` directory was removed.
- 2026-05-05: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ProfilerScopeTest.*:ProfilerDestinationTest.*:PanelWindowHookTests.ProfilerPanelReportsTimelineDisabledByDefault` passed 15 tests and skipped the enabled-only TimelineProfiler test in the default build.
- 2026-05-05: `cmake --build Build --config Debug --target NullusUnitTests -- /m:1` completed after moving ImGuizmo and TimelineProfiler into `Runtime/UI/ImGuiExtensions`.
- 2026-05-05: `ctest --test-dir Build -C Debug --output-on-failure` passed after moving ImGuizmo and TimelineProfiler into `NLS_UI`.
- 2026-05-05: `cmake --build Build --config Debug --target Editor -- /m:1` passed after removing the standalone `ImGuizmo` target.
- 2026-05-05: Enabled-profiler focused validation passed 16/16 with TimelineProfiler built as a UI ImGui extension.
- 2026-05-05: Default-build focused validation passed 15/16 and skipped the enabled-only TimelineProfiler recording test after changing the off-by-default panel state to `TimelineProfiler: Disabled`.
- 2026-05-05: Enabled-profiler focused validation passed 17/17 after preloading TimelineProfiler HUD fonts before ImGui frames, covering the `FontAtlas locked` crash path.
- 2026-05-05: `ctest --test-dir Build -C Debug --output-on-failure` passed with profiling, Tracy, and TimelineProfiler enabled.
- 2026-05-05: `cmake --build Build --config Debug --target Editor -- /m:1` rebuilt `App\Win64_Debug_Runtime_Static\Editor.exe` with TracyClient and TimelineProfiler enabled.
- 2026-05-05: Enabled-profiler focused validation passed 18/18 after moving TimelineProfiler frame ticking out of Profiler panel drawing, covering the active-scope frame-advance crash path.
- 2026-05-05: `ctest --test-dir Build -C Debug --output-on-failure` passed after the TimelineProfiler tick-order fix.
- 2026-05-05: `cmake --build Build --config Debug --target Editor -- /m:1` rebuilt `App\Win64_Debug_Runtime_Static\Editor.exe` after the TimelineProfiler tick-order fix.
- 2026-05-05: Enabled-profiler focused validation passed 19/19 after fixing TimelineProfiler `Tick()` to release its thread-data lock before starting the next `CPU Frame` event.
- 2026-05-05: `ctest --test-dir Build -C Debug --output-on-failure` passed after the TimelineProfiler first-tick deadlock fix.
- 2026-05-05: `cmake --build Build --config Debug --target Editor -- /m:1` rebuilt `App\Win64_Debug_Runtime_Static\Editor.exe` after the TimelineProfiler first-tick deadlock fix.
- 2026-05-05: `cmake --build Build --config Debug --target NullusUnitTests -- /m:1` completed after adding thread naming, GPU scope facade, render/RHI CPU scopes, and DX12 TimelineProfiler GPU bridge.
- 2026-05-05: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ProfilerDestinationTest.*:ProfilerScopeTest.*:PanelWindowHookTests.ProfilerPanelReportsTimelineDisabledByDefault:PanelWindowHookTests.ProfilerPanelDrawDoesNotAdvanceTimelineFrameInsideActiveScope` passed 25/25 focused profiler tests, including thread-name, GPU-scope routing, and late GPU-context replay coverage.
- 2026-05-05: `ctest --test-dir Build -C Debug --output-on-failure` passed 1/1 tests after render/RHI/GPU profiler instrumentation.
- 2026-05-05: `cmake --build Build --config Debug --target Editor -- /m:1` rebuilt `App\Win64_Debug_Runtime_Static\Editor.exe` after render/RHI/GPU profiler instrumentation.
- 2026-05-05: Added regression coverage for TimelineProfiler CPU scopes that begin before a frame tick and end after the tick on main/worker threads. This covers the render-thread `std::vector<ProfilerEvent>::operator[]` crash caused by ending a scope against the wrong frame slot.
- 2026-05-05: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ProfilerDestinationTest.TimelineSinkCanEndCpuScopeAfterFrameTickAdvances:ProfilerDestinationTest.TimelineSinkCanEndCpuScopeOnWorkerAfterFrameTickAdvances:ProfilerDestinationTest.*:ProfilerScopeTest.*` passed 25/25 after storing CPU event stack entries as `{frameIndex, eventIndex}`.
- 2026-05-05: `ctest --test-dir Build -C Debug --output-on-failure` passed 1/1 and `cmake --build Build --config Debug --target Editor -- /m:1` rebuilt `Editor.exe` after the TimelineProfiler cross-frame scope fix.

## Manual Editor Verification

1. Start the editor with the normal project launcher or direct editor script used by the current workflow.
2. Open the `Profiler` window from the existing editor window/menu path.
3. Exercise a known profiled path, such as editor update, scene view update, or UI rendering.
4. Confirm the Profiler window shows live timeline data or a clear empty/unavailable state.
5. Close and reopen the Profiler window.
6. Confirm the editor remains stable and the profiler session state is still coherent.

Expected result:

- The Profiler window is dockable.
- New CPU scope events appear while the window is open.
- Closing the window does not break external profiler output.

Status: Not yet manually executed in this implementation pass.

## Manual Tracy Verification

1. Enable the Tracy destination using the selected build/runtime switch.
2. Start the editor or game.
3. Connect the Tracy viewer.
4. Exercise a profiled path that also appears in the editor timeline.

Expected result:

- The same shared scope label appears in Tracy and the editor timeline when both destinations are enabled.
- If the editor timeline destination is disabled, Tracy still receives events.

Status: Tracy is vendored under `ThirdParty/Tracy` and builds in `BuildProfiler`; viewer connection has not yet been manually executed in this implementation pass.

## GPU/Backend Availability Verification

1. Run with `NLS_ENABLE_TIMELINE_PROFILER=ON` on DX12.
2. Open the editor `Profiler` panel and exercise scene/game rendering.
3. Confirm CPU tracks include editor, renderer, render-thread, and RHI-thread scopes where those paths are active.
4. Confirm GPU tracks appear only after the DX12 device and command queues initialize the TimelineProfiler GPU bridge.
5. Run on a backend where TimelineProfiler GPU data is not supported.

Expected result:

- CPU profiling remains usable.
- Unsupported GPU data is shown as unsupported, not as a silent failure or false live view.

Status: Automated routing and build validation completed. Manual DX12 visual verification is still pending.
