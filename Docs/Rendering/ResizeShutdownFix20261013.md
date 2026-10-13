# Resize and shutdown repair, 2026-10-13

## Shutdown lifetime

The reported access violation occurs when `SceneRenderStore` releases a
`ResourceHandle<Material>` through a destroyed `ResourceLifetimeRegistry`.
`SceneRenderStoreRegistry` owned entries strongly even though its documentation
called it a weak registry. `RenderScene` attempted to erase the entry only when
its store reference count was at most two. Cached or in-flight owners could
prevent erasure and retain resource leases until static teardown.

Scene destruction now releases its registry entry. Releasing an entry also
clears the store through an ordered Render Command, even when cached owners
retain the store itself. Removed proxy resources continue to retire behind GPU
fences. `ReleaseAll()` clears remaining scene entries before Context shuts down
the Driver and resource managers. Editor panels and preview renderers are
destroyed while the Render/RHI lanes are still available.

Driver shutdown now joins a main-pipe marker before the final GPU flush. Frame
command counters alone cannot observe general scene/resource release commands;
the marker covers those commands, including queues longer than the eight-command
render tick slice.

## Resize behavior

- Pending swapchain resize pauses all producers sharing the allocator ring,
  including offscreen Scene View frames, so new frames cannot indefinitely
  postpone the drain. Requests coalesce to the latest dimensions.
- Texture-backed views resize without waiting for every in-flight CPU frame.
  Captured target snapshots keep the old framebuffer views alive. Views needing
  immediate readback retain their synchronization requirement.
- A queued view size no longer overrides a newer content-region size.
- Native size callbacks execute one Editor frame. A Win32 timer lets rendering
  progress while the OS modal sizing loop blocks the application's normal loop,
  including when the user stops moving the cursor while holding the border.
- Same-size refresh messages are coalesced against the completion of the last
  Editor frame, so a normally progressing main loop does not also run timer
  frames. Repeated requests for the same swapchain dimensions avoid another
  synchronous Render Command. The Render/RHI paths still retry pending resize.
- The timer is removed when native sizing ends or the window procedure is
  restored. Sizing exit explicitly refreshes the final dimensions.

## Verification

Git HEAD: `73a584ad5f45a6a355ca5dece7b1799d42155533`, with preexisting migration
and Editor changes preserved. No staging or commit was performed.

Final RelWithDebInfo Editor/NullusUnitTests, Debug Editor and Release Editor/Game
builds passed through `Tools/BuildCleanEnvironment.cmd` with `/m:1`.

The final regression contains 473 passing tests across 13 suites, with zero
failures, disabled cases or errors. It covers SceneRenderStore ownership and
shutdown, scene proxy updates, Driver resize, view resize policies, target
snapshot ownership, RHI executor/frame resource retirement, RenderScene caches,
Picking readback and the resource lifetime registry. New cases cover retained
store owners with mesh/material leases, 32 queued release commands at shutdown,
offscreen producers during pending resize, latest-size recovery and native
refresh throttling. ShaderCache tests require access to the normal local cache;
the earlier sandbox-only permission failures are excluded from the final result.

Native smoke checks use an isolated Forward project, DX12 Tasks, static frame
cache and idle pacing disabled. Final Debug and Release Editor checks perform
24 native size changes and normal close under cdb with DX12 Debug Layer enabled.
The cdb script traps access violations, heap corruption and stack-buffer failures.
Both final runs exited zero; their logs contain no captured native memory
exception and no D3D12 ERROR/CORRUPTION message.
The final Vulkan Tasks Editor check also performs 24 size changes and exits zero;
it is an application smoke check, not a Vulkan Validation acceptance run.

The actual border-drag trial verifies a `HTBOTTOMRIGHT` hit, changes the window
size six times after 24 scripted resizes, then holds the cursor stationary for
two seconds inside the OS sizing loop. The final trace records a 4,536.6 ms
`Editor::PollEvents` interval containing 122 resize frames and 119 native DXGI
Presents. Its final two seconds contain 56 Presents. This verifies continued
submission during modal sizing; the largest Present gap was 92.3 ms. The paused
window screenshot was inspected for visible UI and selection overlays; it does
not establish shader/material correctness or total image recovery latency.

## Matched resize sample

Baseline `before2` and final `after-progress-final` use the same RelWithDebInfo
configuration, isolated project, DX12 Tasks mode, disabled VSync/static cache,
six-second warmup, 18-second Tracy capture and 24 `SetWindowPos` changes between
1280x800, 1600x1000 and 1440x900, 250 ms apart. No builds or unit tests ran during
either capture. Tracy tools and client are version 0.13.3.

| Native resize call, ms | Before | Final |
| --- | ---: | ---: |
| Median | 63.52 | 47.37 |
| P95, nearest rank | 99.96 | 82.35 |
| Mean | 71.21 | 66.59 |
| Maximum | 136.57 | 359.63 |

Immediate follow-up Editor frame counts fell from 25 to 2. Median native resize
call duration fell approximately 25%, but one first-cycle long-tail sample
remains. These are native call/CPU measurements from one small-project pair,
not mouse-to-restored-image latency, repeated statistical acceptance, or a
performance result for the user's large scene. Intermediate timer implementations
that repeated Render Thread synchronization were replaced and are not the final
result. Earlier test-overlapped and incorrectly identified startup-window trials
are also excluded.

## Limits and artifacts

DX12 Debug Layer still emits clear-value mismatch warnings (#820) and live-object
reports during device teardown. These checks do not claim warning-free GPU
validation or proven absence of leaks. Metal validation remains skipped at the
user's request. This repair does not complete the broader threading migration's
10,000-frame/30-minute stress and performance acceptance requirements.

Evidence directory: `Build/validation/resize-shutdown-20261013`.

- `regression-final-build.log` and XML: final 473-test result.
- `build-progress-gate.log`, `build-debug-progress.log`,
  `build-release-progress.log`: final build logs.
- `before2.tracy`, `after-progress-final.tracy`, corresponding resize CSV and
  result JSON: matched sample and binary hashes.
- `after-progress-modal2.tracy`, individual frame/Present/PollEvents CSV,
  `after-progress-modal2-modal-paused.png`: actual modal sizing evidence.
- `after-debug-progress-cdb-*`, `after-release-progress-cdb-*`:
  final Debug/Release native diagnostics and process results.
- `after-vulkan-final-*`: Vulkan smoke logs and result.
- `RunResizeTrial.ps1`: isolated-process test harness. Only trial-owned processes
  are closed; the script verifies the target HWND owner before sending messages.
