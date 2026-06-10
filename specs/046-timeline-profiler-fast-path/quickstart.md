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

   - The first child should still be `Application::TickFrame`.
   - The start gap before `Application::TickFrame` should be under 0.25 ms on the validated Windows DX12 capture when no GPU queue events are present.
   - GPU queue tracks may be empty for no-GPU-scope captures.

4. Capture a frame with at least one GPU timeline scope if available and confirm GPU events still publish after readback latency.

## Manual Evidence To Record

- Build configuration and backend used.
- Targeted test command and result.
- Trace filename after optimization.
- Before/after start-gap summary for CPU frames with no GPU queue events.
- Any backend/platform not validated must be explicitly marked as not validated.
