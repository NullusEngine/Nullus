# Quickstart: TimelineProfiler GPU Fast Path Validation

## Automated Validation

1. Configure or use an existing Windows build with TimelineProfiler enabled.
2. Run the targeted lifecycle tests:

   ```powershell
   ctest --test-dir build --output-on-failure -R NullusUnitTests
   ```

   If the local build tree uses a different directory name, run the equivalent `NullusUnitTests` target from that build tree.

3. Confirm the new tests in `TimelineProfilerGpuLifecycleTests` cover:

   - empty GPU frames are eligible for no-op completion,
   - pending command-list queries prevent empty-frame skipping,
   - open queue events prevent empty-frame skipping,
   - frames with recorded GPU query work still require readback submission.

## Runtime Trace Validation

1. Launch the editor with TimelineProfiler recording enabled and reproduce a short idle capture similar to `TestProject/Logs/trace.json`.
2. Export the trace.
3. Inspect the main-thread `CPU Frame` events:

   - The first visible child may be `TimelineProfiler::TickFrame`; this is the intentional frame-maintenance marker.
   - `Application::TickFrame` should follow after `TimelineProfiler::TickFrame` with no multi-millisecond unlabelled gap.
   - Record the `TimelineProfiler::TickFrame` maintenance duration; the optimization target is under 0.25 ms on a Windows DX12 idle capture with no GPU queue events, but this must be proven from an after-change trace.
   - GPU queue tracks may be empty for no-GPU-scope captures.

4. Capture a frame with at least one GPU timeline scope if available and confirm GPU events still publish after readback latency.

## Manual Evidence To Record

- Build configuration and backend used.
- Targeted test command and result.
- Trace filename after optimization.
- Before/after start-gap summary for CPU frames with no GPU queue events.
- Any backend/platform not validated must be explicitly marked as not validated.

## Evidence Recorded 2026-06-12

- Build: `cmake --build Build --target NullusUnitTests --config Debug` completed successfully after the TimelineProfiler changes.
- Targeted profiler tests: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=TimelineProfilerGpuLifecycleTests.*:PanelWindowHookTests.TimelineProfiler*:ProfilerDestinationTest.TimelineSink*:ProfilerDestinationTest.ClearingGpuContextOnlyRemovesMatchingNativeDevice --gtest_break_on_failure=1` passed 27/27 tests, including a real DX12 empty-frame check that submitted GPU readback count remains zero after repeated empty TimelineProfiler ticks.
- Stale GPU context regression sequence: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.*:TimelineProfilerGpuLifecycleTests.* --gtest_break_on_failure=1` passed 184/184 tests.
- Render framework isolation after one noisy full-suite failure: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RenderFrameworkContractTests.ThreadedTelemetryDiagnosticMergeKeepsSubstringDistinctMessages --gtest_break_on_failure=1` passed 1/1, and `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RenderFrameworkContractTests.* --gtest_break_on_failure=1` passed 33/33.
- Full unit suite: `Build\bin\Debug\NullusUnitTests.exe --gtest_brief=1 --gtest_break_on_failure=1` ran 2224 tests: 2223 passed and 1 skipped (`AssetDatabaseFacadeTests.ImportedAssimpModelManifestRecordsParserTextureDependencies`).
- Diff hygiene: `git diff --check` reported no whitespace errors, and `git diff --name-only -- 'Runtime/*/Gen/*' 'Project/*/Gen/*'` reported no generated-file edits. Only repository CRLF conversion warnings were printed.
- Baseline trace: `TestProject/Logs/trace.json` contained 305 `CPU Frame` events. Every analyzed frame had `Application::TickFrame` as the first child; start gap before that child averaged 3056.384 us, with min 38.000 us and max 5400.000 us.
- Runtime after-trace: not captured in this pass. No headless trace-export entrypoint was identified, so the optimized DX12 editor trace still needs an interactive capture before claiming the under-0.25 ms runtime target.
- Backend scope: automated evidence is Windows Debug/unit-test coverage plus pre-change trace analysis. No Vulkan, Linux, or macOS GPU timeline behavior was validated.
