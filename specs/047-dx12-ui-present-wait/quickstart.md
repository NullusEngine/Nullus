# Quickstart: DX12 UI Present Wait Validation

## Automated Validation

1. Build `NullusUnitTests` in Debug.
2. Run targeted tests:

   ```powershell
   .\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=DX12UIFrameFenceTrackerTests.*:ProfilerDestinationTest.DX12UiBridgeUsesAllocatorReuseWaitScope --gtest_break_on_failure=1
   ```

3. Run threaded rendering plus UI/DX12 relevant subsets if available:

   ```powershell
   .\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.*:DX12UIFrameFenceTrackerTests.* --gtest_break_on_failure=1
   ```

4. Run full `NullusUnitTests` or record why a narrower command was used.

## Runtime Trace Validation

1. Launch the editor on Windows DX12 with TimelineProfiler recording enabled.
2. Capture an idle editor window similar to the provided screenshot.
3. Inspect main-thread UI render scopes:
   - `DX12UIBridge::WaitForBackbufferReuse` should not appear every frame.
   - If a wait remains, it should identify allocator reuse/exhaustion or scene wait specifically.
   - Worker-thread `RhiThread::WaitForWake` and `RenderThread::WaitForWake` may remain because they represent idle worker sleep.

## Evidence To Record

- Build command and result.
- Targeted test command and result.
- Full or relevant suite command and result.
- Runtime trace filename and whether the repeated main-thread backbuffer wait disappeared.
- Any backend/platform not validated.
