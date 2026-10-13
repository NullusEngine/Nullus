# Scene Interaction Regression (2026-10-13)

## Scope

Repair the DX12 crash after selecting an object and the visible Scene View stalls
following the UE 5.7.4 threading migration. Existing unrelated work remains in
the working tree. Metal validation remains waived by the user's decision.

## Selection Crash

Parallel Translate workers can request the same read-only depth/stencil view for
the first time. The old lazy initializer wrote directly into the shared COM
pointer and published the heap before the descriptor handle was ready.

The new concurrent-first-access regression reproduced the original null-pointer
access violation under CDB before the repair. Each texture view now serializes
this initialization, builds the descriptor heap and DSV locally, then publishes
the initialized heap and handle together. Writable DSV access is unchanged.

The regression covers Depth24Stencil8 and Depth32F, 32 fresh views per format,
eight simultaneous workers per view, and 64 repeated reads per worker. Ten CDB
iterations passed after the repair without the original access violation.

## Scene Presentation

The original CPU benchmark did not establish visible frame cadence. In a clean
Release/DX12 run, 300 camera-update ticks produced three native swapchain Presents
and a 1,163 ms maximum presentation gap. Vsync was disabled and the moving Scene
View measured 1,281 x 858 pixels.

Two admission problems compounded the delay:

- Scene producers could occupy every resource slot while UI publication retried
  without waiting, starving the command that presents the Scene output.
- Editor resource headroom also increased the CPU queue budget to 16 frames.
  Reserving one physical UI slot alone still left long queues of Scene work in
  front of Present; one such diagnostic run produced only 37 Presents per 300
  camera ticks.

The Driver now reserves a tail resource slot for swapchain consumers whenever the
device supports the UI overlay graph. Scene producers leave this slot available.
They acquire a CPU token only after finding a reusable GPU resource slot.

`cpuFrameLatencyLimit` controls CPU queue depth separately from resource capacity.
Editor sets it to three before expanding the GPU resource pool. Scene producers
leave one CPU token for presentation and share a two-frame producer limit. UI
consumers already using that reservation do not reduce producer capacity twice.
All submissions still use the same FIFO Render Command and ordered RHI executor.

Admission polls completed CPU fences independently of GPU-slot availability, so
failed Present fences stop admission and expose their diagnostic even while the
GPU retirement fence remains pending. Recovery still requires explicitly resetting
the executor and latency controller.

## Final Matched Presentation Measurements

All six runs use Release, DX12, `TestProject/TestProject.nullus`, `New Scene.scene`,
the same camera start (`-10,3,10;0,135,0`), a 0.1-unit forward step per tick,
60 warmup ticks and 300 measured ticks. Scene View is 1,281 x 858, no object is
selected, and vsync is disabled. Tracy 0.13.3 records native
`IDXGISwapChain::Present` calls within the camera measurement window. The
frame-bounded windows last 1.15-1.99 seconds; these are short interaction trials,
not long-duration stability or display scanout measurements.

| Capture label | Native Presents / 300 ticks | Presents / second | Longest Present gap (ms) | CPU mean (ms) | CPU P95 (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| before-final-unselected-trace1 | 3 | 2.37 | 1163.15 | 4.232 | 7.107 |
| before-verified-trace1 | 9 | 5.62 | 721.73 | 5.372 | 8.981 |
| before-verified-trace2 | 1 | 0.87 | 668.57 | 3.845 | 6.082 |
| after-verified-trace1 | 212 | 106.56 | 25.38 | 6.640 | 12.071 |
| after-verified-trace2 | 217 | 125.86 | 18.43 | 5.757 | 10.403 |
| after-verified-trace3 | 204 | 102.69 | 21.30 | 6.655 | 11.679 |

The final interleaved order was after-verified-trace1, before-verified-trace1,
after-verified-trace2, before-verified-trace2, after-verified-trace3. The frozen
original baseline also includes before-final-unselected-trace1 at the same
dimensions. Every run reports zero device-lost and unsafe-quarantine events.
The final build consistently removes the long gaps between native Presents.

CPU tick timings increased while substantially more UI work reached Present and
admission began waiting for bounded rendering progress. These samples do not
establish a five-percent CPU-performance acceptance result. Present calls also
do not measure physical input-to-display latency or guarantee a distinct visible
Scene image on every call.

## Evidence

Artifacts are stored under `build/validation/scene-input-20261013/`:

- `dsv-before-cdb.log`: original native access violation reproduced by the new test.
- `dsv-after-console.log`, `dsv-after-cdb.log`: ten successful concurrent-access runs.
- `before-final-unselected-trace1*`: frozen original Release binaries and native
  Present cadence under the same project, camera path, dimensions and sample count.
- `after-admission-diagnostic*`: physical-slot reservation alone leaves CPU queues
  too deep; `UIOnlyFrame::AdmissionUnavailable` dominates unchanged-content skips.
- `after-latency-trace1*`: initial separate-budget check, 299 native Presents per
  300 ticks, approximately 63 Presents/second.
- `after-verified-trace1..3*`, `before-verified-trace1..2*`: final matched native
  Present measurements in the table above, with isolated Tracy clients and the
  restored viewport dimensions.
- `after-selected-debuglayer*`: selected-object camera motion for 1,200 measured
  frames, with DX12 Debug Layer enabled and normal process exit; collected before
  the final mixed UI/Scene budget refinement.
- `after-verified-selected-debuglayer*`: final Release build, selected
  `columns_1stfloor_03 Primitive 0`, 1,200 measured camera-motion frames at
  1,281 x 858, DX12 Debug Layer explicitly enabled (GPU-based validation disabled)
  and exit code zero. Device-lost, unsafe-quarantine, descriptor-allocation failure
  and resource-wait timeout counters are all zero. CPU mean is 6.290 ms and
  P95 is 10.328 ms; this selected workload is not pooled with the unselected
  presentation trials.
- `selected-resize.log`: selected-object run with 18 native window size changes
  and normal shutdown.

`verified-regression.xml` records 880 passed tests, zero failures and zero errors,
including the mixed UI/Scene admission regression and readbacks in all RHI modes.
Release and RelWithDebInfo Editor, Game and Launcher builds succeeded. Deployed
EXE and runtime DLL hashes match this build's archived outputs, recorded in
`verified-binary-manifest.json`.

Intermediate captures that overlap builds/tests are diagnostic only. In
particular, `after-latency-trace2` overlapped the test runner and its Tracy
collector did not complete; it is excluded from performance evidence. The native
resize smoke also changed automatically saved docking ratios. The original
1,281-pixel Scene viewport was restored before the final matched captures; the
temporary 502-pixel runs are not compared against the original larger viewport.

## Validation Boundary

The current execution environment returns no desktop foreground window. Attempts
to focus the owned test Editor failed even outside the filesystem sandbox. Actual
physical mouse clicking therefore remains an interactive validation gap. The
concurrent native DSV repro, automated selection/outline path, picking regressions
and application camera runs are separate evidence for the implemented repair.

Earlier CPU-only threading performance gates must not be reused as evidence for
this interaction fix: the original high tick rate coexisted with presentation
starvation. CPU tick timings after bounded admission include intentional waiting
for submitted rendering work. Native Present cadence and gaps are reported
separately; this document does not claim the old five-percent CPU gate passes.
