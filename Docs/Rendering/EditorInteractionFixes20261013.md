# Editor Interaction Fixes (2026-10-13)

This follow-up addresses selection outline movement, picking that stops working
after slot reuse, the Profiler panel freeze, and the native-window resize crash.
It supplements `SceneInteractionRegression20261013.md`. Metal validation remains
waived by the user. Existing unrelated working-tree changes are preserved.

## Outline and Object Bindings

Game-thread helpers recorded constants before the render-thread scene capture.
The provider did not adopt the Driver's reserved resource slot at BeginFrame.
Initializing that slot later reset its allocation records, allowing scene draws
to overwrite helper matrices from the same frame.

The new regression recorded a helper translation of (2, 3, 5) followed by a scene
translation of (20, 30, 50). Both the legacy uniform ring and indexed-object path
overwrote the helper before the fix. BeginFrame now initializes and resolves the
reserved slot, and render capture continues its allocations without resetting
the producing frame's records. The regression now passes for both paths.

## Picking and Helper Caches

A binding-set reference does not make its buffer contents immutable. Picking
cached commands that referenced a frame-slot uniform ring; those bytes are reused
after the slot's GPU fence retires. Draw preparation now carries an explicit
`objectBindingImmutable` property. Only a dedicated object snapshot qualifies;
indexed data, ring blocks, and a failed snapshot's mutable fallback do not.

Picking, Grid, and Debug Lights use the same cache-eligibility check. Unsafe
commands are captured again for their current frame. Frame constants are still
refreshed when a valid cached command is reused.

Repeated hover requests no longer replace a matching in-flight readback before
it completes. A failed frame publication cancels exactly its pending picking
serial. Scene View recognizes the discarded request and retries a pending click
instead of waiting indefinitely for a frame that was never submitted.

## Profiler and Native Callbacks

The original Editor freeze was captured with CDB: the Game Thread held the
Timeline GPU profiler mutex while waiting for a Submit Lane operation, and a
Translate worker needed that mutex to finish EndGpuScope before submission.

Timeline initialization, Tick, and shutdown now enter the ordered Submit Lane
before taking the mutex. Recording and GPU-availability state use atomics.
Driver clears the profiling context while its Submit Lane is still alive, after
draining frame work; Profiler query resources are released before RHI shutdown.
Lifecycle and resolved-GPU-event regressions cover None, DedicatedThread, and
Tasks, including the earlier-submit versus profiler-Tick lock dependency.

The screenshot's 0xC000041D arose when a failed ordered render command unwound a
C++ exception through GLFW/Win32 resize callbacks. Those listeners now capture
the original exception and stop further resize callbacks. The application
rethrows on its main loop, where the Editor entrypoint logs the diagnostic and
returns failure. The underlying profiler deadlock is repaired separately.

## Verification

Artifacts are under `build/validation/editor-interaction-20261013/`.

| Check | Result | Evidence |
| --- | --- | --- |
| RelWithDebInfo unit-test and Editor build | Passed | `build-helper-final.log` |
| Release Editor build | Passed | `build-release.log` |
| Main affected suites | 595 passed, no failures or skips | `final-regression.xml` |
| Final helper-cache and binding regressions | 31 passed, no failures or skips | `helper-cache-regression.xml` |
| Release / DX12 / Tasks, Profiler open and object selected | 3,000 camera-motion frames and 24 native resizes, exit 0 | `release-final-editor.log`, `release-final-stacks.log` |
| Deployed EXE and Render / Engine / UI DLLs | Hashes match build outputs in both configurations | `final-binary-manifest.json` |

The runtime smoke uses `TestProject/TestProject.nullus`, camera
`-10,3,10;0,135,0`, and `columns_1stfloor_03 Primitive 0`. DX12 Debug Layer is
enabled; GPU-based validation is disabled. No rendering error, device loss,
submission failure, or wait timeout was logged. Native resize includes
WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE, and the test restores the original docking
layout. CDB attachment pauses the process, so these runs are stability evidence
and must not be used for a performance comparison.

The 31-test run overlaps the main suite and adds one new cache regression;
together they cover 596 distinct tests, not 626 distinct tests. Several tests
exercise policy or mock-device behavior; GPU profiler tests use the real DX12
device.

## Remaining Interactive Boundary

The tool execution environment reports foreground HWND 0 and GetCursorPos
failure. Physical mouse clicking, long-duration repeated picking, and visual
confirmation that the outline remains aligned during camera movement are not
verified here. Automated preselection and camera movement do not substitute for
those checks. This repair does not claim the full UE threading stress or
five-percent performance acceptance criteria have been completed.
