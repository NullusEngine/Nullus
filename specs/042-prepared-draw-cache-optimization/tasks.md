# Tasks: Prepared Draw Cache Optimization

## Phase 1: Setup

- [x] T001 Create feature documentation under `specs/042-prepared-draw-cache-optimization/`.

## Phase 2: Frame-Local GBuffer Material Resolve Cache

- [x] T002 [P] Add failing unit test for repeated same-frame GBuffer material resolve in `Tests/Unit/DeferredSceneRendererMaterialCacheTests.cpp`.
- [x] T003 Run the focused test and verify it fails before production changes.
- [x] T004 Add frame-local resolve cache declarations and test accessors in `Runtime/Engine/Rendering/DeferredSceneRenderer.h`.
- [x] T005 Implement frame-local resolve cache in `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`.
- [x] T006 Replace threaded GBuffer capture loop material resolution with frame-local resolve.
- [x] T007 Run focused deferred material cache tests and fix regressions.

## Phase 3: Prepared Recorded Draw Static-Base Cache

- [x] T008 Add a prepared draw cache test proving pipeline/material binding/mesh reuse while per-object binding stays per draw.
- [x] T009 Add per-frame static-base cache declarations in `ABaseRenderer`.
- [x] T010 Implement static-base cache key freshness for material revisions, binding revision, shader instance/generation, mesh instance/revision, pass binding, pipeline state, and overrides.
- [x] T011 Refactor recorded draw preparation to reuse cached static base and repopulate per-draw submit fields.
- [x] T012 Add invalidation coverage for shader generation and sampler/binding changes.

## Phase 4: Shader Support Query Cache

- [x] T013 Add per-frame indexed-object-data shader support cache in `EngineFrameObjectBindingProvider`.
- [x] T014 Add guarded test-hook coverage for cache hits and shader-generation misses.

## Phase 5: Telemetry

- [x] T015 Add renderer stats and FrameInfo fields for GBuffer resolve hit/miss counters.
- [x] T016 Add renderer stats and FrameInfo fields for prepared static-base hit/miss counters.
- [x] T017 Update editor FrameInfo formatting tests.

## Phase 5.5: Existing DX12 Child Recording Correctness Guards

- [x] T017a Invalidate parent descriptor/root-table binding cache after DX12 `ExecuteBundle()`.
- [x] T017b Treat descriptor heap instability inside a child bundle range as unsafe for child recording and route it to safe serial full-pass fallback before the parent render pass opens.
- [x] T017c Add focused DX12/threaded lifecycle coverage for child descriptor state and fallback behavior.

## Phase 5.6: Build Ownership Cleanup

- [x] T017d Move rendering-dependent resource-manager implementations under `Runtime/Rendering/ResourceManagement/`.
- [x] T017e Remove hardcoded cross-module source injection from `Runtime/Rendering/CMakeLists.txt`.
- [x] T017f Split project executables into project static libraries plus thin entry targets so tests link project code without hardcoded source inventories.

## Phase 5.7: Trace Reliability, Persistent Cache, UI Isolation, And Instancing Follow-Up

- [x] T017g Fix TimelineProfiler event-name ownership so trace export no longer reads recycled allocator memory.
- [x] T017h Add source/test coverage proving profiler event names are read through owned `ProfilerEvent` storage.
- [x] T017i Convert prepared recorded draw static-base cache from frame-local to persistent across frames, with dirty revision replacement.
- [x] T017j Add cross-frame prepared static-base cache hit and material-dirty miss coverage.
- [x] T017k Add trace export filtering for editor UI/profiler/ImGui/UI bridge events while preserving scene draw-preparation events.
- [x] T017l Add trace-scale dynamic instancing coverage proving 259 compatible static opaque objects reduce to one submitted draw.
- [x] T017m Make persistent prepared static-base cache device/backend-aware and bounded so device recreation or resource churn cannot retain stale RHI objects indefinitely.
- [x] T017n Make material explicit binding/layout caches and shader explicit module caches device-aware to prevent same-backend device replacement reuse.
- [x] T017o Add regression coverage for explicit device replacement, bounded prepared static-base churn, and fully-qualified UI bridge trace filtering.
- [x] T017p Replace prepared static-base cache hit bookkeeping with a stable-resource index so repeated hits cannot grow eviction metadata and dirty revisions replace old entries without full-cache scans.
- [x] T017q Re-key prepared static-base cache insertion after material binding refresh so first-time binding creation cannot create a stale-revision cache entry.
- [x] T017r Add regression coverage for repeated-hit stable index size, mesh content-revision replacement, pipeline/depth/primitive override separation, and explicit uniform layout device replacement.
- [x] T017s Allow in-render-pass child recording descriptor-heap instability to fall back to serial pass recording before the parent render pass opens, instead of dropping the frame.
- [x] T017t Add monotonic RHI device cache identity and use it for prepared static-base, explicit uniform layout, material explicit binding/layout, and shader explicit module caches.
- [x] T017u Fix `MaterialPipelineStateOverrides` equality/hash consistency for explicit empty color-format overrides.
- [x] T017v Clear reusable child command resources and retained threaded submit resources when the explicit device is replaced.
- [x] T017w Avoid full prepared static-base cache scans on hot misses by replacing dirty resource tuples through the stable-resource index and limiting age sweeps to frame boundaries.
- [x] T017x Replace prepared static-base overflow eviction with a bounded LRU list so unique-material churn does not scan the full cache on every overflow miss.
- [x] T017y Make indexed object-data buffers/layouts/binding sets device-aware so explicit device replacement cannot reuse old descriptor-backed resources.
- [x] T017z Route explicit device replacement through frame-resource release/drain and reset stale swapchain/UI synchronization resources.
- [x] T017aa Synchronize explicit device replacement with threaded RHI submission before releasing old frame/swapchain resources.
- [x] T017ab Make Engine frame/object binding-set caches and DX12 binding-set descriptor heap allocator ownership safe across explicit device replacement.
- [x] T017ac Prevent stale external Scene View output frames from entering RHI after a newer overlapping external-output frame has started submission.
- [x] T017ad Retire stale published external-output frames before render-scene build so moving-camera Scene View bursts skip older overlapping output work earlier.
- [x] T017ae Remove the synchronous threaded-submit retirement fence wait and rely on deferred frame-scoped retirement until frame-context reuse.
- [x] T017af Apply the same deferred frame-scoped retirement contract to standalone explicit/UI submissions after successful fence-signaled submit.
- [x] T017ag Quarantine submitted GPU work that lacks a reliable retirement fence and release retained frame resources during device-lost teardown.
- [x] T017ah Skip blocking reusable frame-fence waits when the fence is already signaled and split the profiler scope so future traces separate fence polling from real GPU stalls.
- [x] T017ai Treat frame retirement fences as reliable only when `frameFenceSignalQueued` is reported, mark DX12 semaphore-only signal attempts as queued GPU work before the signal call, and fail closed after quarantine/device-lost before reusing threaded frame contexts.
- [x] T017aj Split unsafe queued-GPU-work quarantine from true device-lost, preserve quarantined RHI resources through driver destruction, and mark DX12 queue semaphore waits as queued GPU work before `ID3D12CommandQueue::Wait`.
- [x] T017ak Close review-found queued-work lifetime gaps: validate DX12 submit command lists before queue waits, classify DX12 present waits as queued GPU work, signal swapchain/UI retirement fences after present waits, make quarantine/device-lost gates atomic, and expose unsafe quarantine telemetry separately from true device loss.
- [x] T017al Close second-round RHI fence review gaps: validate all DX12 submit/present semaphore wait values before queueing any waits, treat `IDXGISwapChain::Present` itself as queued GPU-visible work before the call, move standalone present telemetry recording to caller merge points, and add staged queue failure tests for wait-after-submit and present-after-call failures without retirement fences.

## Phase 6: Validation

- [x] T018 Analyze baseline trace `App/Win64_Release_Runtime_Shared/trace.json`.
- [ ] T019 Capture and compare a fresh post-change high draw-count Scene View trace.
- [x] T020 Run focused deferred material cache tests.
- [x] T021 Run focused renderer object binding tests for prepared static-base and shader-support caches.
- [x] T022 Run focused renderer stats and FrameInfo panel tests.
- [x] T023 Run `cmake --build Build/windows --config Release --target NullusUnitTests -- /m:1` after `/m` hit a transient object-file permission lock.
- [x] T024 Run plan-review quality gate until no P0/P1 findings remain.

## Trace Evidence

- Baseline capture: `App/Win64_Release_Runtime_Shared/trace.json`, last written 2026-05-31 20:46.
- High draw-count Scene View path still spends substantial CPU before RHI submission: `NLS::Engine::Rendering::DeferredSceneRenderer::BeginFrame` totals about 1221 ms across 95 samples, and `NLS::Engine::Rendering::BaseSceneRenderer::CaptureThreadedPreparedDraw` totals about 2231 ms across 38512 samples.
- Repeated GBuffer material preparation is visible in the same capture: `DeferredSceneRenderer::GetOrCreateGBufferMaterial` appears both as long nested samples and as about 38198 short samples totaling about 90 ms in the fully qualified scope.
- Trace reliability follow-up found corrupted event names such as `rawData`, `l`, and `nel`, consistent with exported events reading profiler event names from recycled allocator-backed storage.
- Slow frames also show editor UI/profiler overhead (`Editor::RenderEditorUI`, `Canvas::DrawPanels`, `DX12UIBridge::RenderDrawData`), so trace export now supports isolating editor UI cost during draw-call analysis.
- Persistent prepared static-base cache follow-up fixed review-found lifecycle risks: cache keys now include device/backend identity, cached entries are bounded by capacity and frame age, and related material/shader explicit RHI caches are device-aware.
- Explicit device replacement now drains threaded RHI submission before releasing old resources, invalidates Engine frame/object binding-set caches by device identity, and keeps DX12 binding-set descriptor heap allocators alive through retained binding sets.
- In-render-pass child recording now treats descriptor-heap instability as a child-recording failure and falls back to serial full-pass recording before opening the parent pass, preserving frame output instead of silently reusing unsafe child command state.
- Fresh trace from 2026-06-01 13:40 shows `CPU Frame` at 179 samples while threaded RHI submissions reach 1127 samples, which exposed a correctness risk for external Scene View output: an older camera snapshot could finish render-scene preparation after a newer one and overwrite the same offscreen output texture later. The lifecycle now tracks the newest external-output frame per external output resource identity set (color/depth texture or view fallback) and retires older overlapping external-output `RenderReady` frames before they can write stale camera output, without dropping independent editor/offscreen outputs.
- Latest-wins external output retirement now also runs at `Published -> RenderScenePreparing` worker claim time, so older overlapping Scene View snapshots are discarded before expensive render-scene preparation when a newer camera frame is already queued. Targeted harness `TryBeginRenderScene(slot)` keeps its existing semantics for race tests; RHI-stage retirement remains the package contract for frames that have already started building.
- RHI fence-wait optimization removes the submit-tail `ThreadedRhiFrame::WaitFrameFence` block from threaded submissions. Submitted frame-scoped resources now stay retained and telemetry marks `deferredFrameScopedRetirement` until `BeginThreadedRhiFrame` reuses that frame context and waits/releases safely. This should move the dominant trace wait from every submit to ring-buffer reuse only.
- Standalone explicit and threaded UI standalone submissions now follow the same no-submit-tail-wait resource lifetime contract: successful fence-signaled submits retain command/descriptor/upload resources and release them only after the reusable frame fence confirms completion. DX12 queue success results also report whether command lists may have queued GPU work, so defer decisions no longer rely only on failure-path metadata.
- Post-submit failures that may have queued GPU work but did not queue the frame retirement fence no longer enter normal reusable-frame retirement. Those paths retain submitted resources and enter a separate unsafe GPU-work quarantine, reject further frame-context reuse, and preserve CPU/COM references through driver destruction unless a true device-lost path is reported. True device-lost remains the only path that can abandon fence waits and release retained references.
- Latest trace `App/Win64_Release_Runtime_Shared/trace.json` from 2026-06-01 14:55 still shows the old `ThreadedRhiFrame::WaitFrameFence` submit-tail scope dominating about 31.3 s total, while the current source has removed that scope. Treat that trace as stale relative to this work until the runtime is rebuilt and recaptured. Reusable fence waits now first poll `IsSignaled()` and only enter `RhiThreadCoordinator::WaitFrameFenceWithDriverPolicy` for actual blocking waits.
- Review-found GPU lifetime risks are now locked down: successful submits are not assumed to have queued a retirement fence unless the backend reports `frameFenceSignalQueued`, DX12 semaphore wait/signal attempts set `mayHaveQueuedGpuWork` before the queue call, unsafe quarantine is no longer reported as true device loss, and quarantined/device-lost threaded frame contexts are rejected before reuse.
- Follow-up review found one P0 and several P1 gaps in the RHI queued-work contract. Current source now validates DX12 command buffers before any submit queue wait, carries queued-work classification through DX12 present/UI waits, queues the reusable frame retirement fence after swapchain/UI present waits instead of before them, reads quarantine/device-lost state through acquire/release atomics, preserves quarantined resources only once, and surfaces unsafe quarantine as independent frame telemetry.
- Second-round review closed the remaining RHI queued-work risks: DX12 now validates every submit/present semaphore wait value before queueing the first wait, marks `IDXGISwapChain::Present` as `mayHaveQueuedGpuWork` before calling into DXGI, and tests staged queue side effects for submit-wait and present-call failures that never signal the retirement fence. Multi-agent review reported no remaining P0/P1 after this fix.
- This supports the implemented optimization direction: cache repeated frame-local GBuffer material resolves, persist static prepared draw base state across frames, reduce repeated shader-support reflection scans, isolate editor UI trace noise, and preserve dynamic instancing at the observed 259-draw scale.
- A fresh post-change trace is still required before claiming measured FPS improvement or broader UE-style draw-command parity.

## Dependencies

- T002 must complete before T003.
- T004-T006 must complete after the RED failure in T003.
- T008-T012 depend on the prepared draw capture path.
- T020-T024 run after implementation fixes are complete.
