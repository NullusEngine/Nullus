# Review Patterns

This file records project-specific review patterns that should be checked before `plan-review` on larger Nullus changes.

## JobSystem

- Callback exception boundaries: foreground, background, combine, cancel, cleanup, and continuation callbacks must be caught at the scheduler boundary and reported through `JobViolationKind::CallbackException`.
- Terminal cleanup versus cancel semantics: payload cleanup must run at most once, and successful or started-failure grouped jobs must not run per-job cancel callbacks after ownership has moved to the group terminal cleanup path.
- Cross-queue wait/help: foreground and background queues must avoid synchronous waits that can include the currently executing job or another non-terminal background job on the same background lane.
- Scoped wait helping: `Complete(backgroundHandle)` may help foreground dependencies in that background handle's dependency chain, but it must not pump foreground dependencies owned only by unrelated background jobs.
- Shutdown ordering: immediate shutdown must not report a group as terminal while a combine or completion callback is still running.
- Binding ABI: C-facing structs must keep `structSize`, `version`, plain fields, named constants, and deterministic invalid-handle/version errors.
- Generated code: files under `Runtime/*/Gen/` are generated output and must not be hand-edited.

Useful grep:

```powershell
rg -n "CallbackException|terminalCleanup|completeCallbackRunning|CompleteNoClear|NLS_JOB_BINDING_VERSION|Runtime/.*/Gen" Runtime Tests Docs specs
```

## Large-Scene Rendering

- Retained scene state must remain generation-safe: primitive handles are scene-scoped, generation-checked, tombstoned on removal, and snapshots must not retain live component pointers.
- Large-scene sync/visibility/finalization must publish touched counts. Review any hidden O(N) scan that moved from visibility into sync, candidate generation, debug overlays, or queue finalization.
- Spatial-index rebuild fallbacks must use explicit telemetry, last-good data, dirty overlays, staged rebuilds, or clear budgets; silent rebuild spikes are a review failure.
- Serial, parallel, and full-scan comparison modes must preserve equivalent visibility and sparse handle output. Parallel paths must use immutable snapshots and must fall back safely when JobSystem scheduling is unavailable.
- LOD/HLOD decisions must stay view-local. Editor-selected HLOD children must remain inspectable, and transparent/order-dependent/skinned/animated children require explicit proxy compatibility before suppression.
- HZB/occlusion must be conservative: invalid, stale, moved, representation-changed, ineligible-depth, or capability-gated history keeps primitives visible. Ordinary occlusion frames must not introduce synchronous readback, blocking maps, or CPU/GPU fence waits.
- RHI/FrameGraph HZB changes must declare texture access, exported transitions, dependency edges, and per-subresource ranges. Do not assume DX12 behavior proves Vulkan/Metal/OpenGL/DX11 correctness.
- Streaming residency must separate interest from residency, deduplicate tickets, enforce CPU/GPU memory and upload budgets, preserve frame-retirement pins, and expose request/commit/evict telemetry.
- Editor debug overlays and FrameInfo must consume renderer snapshots only; disabled overlays must not traverse live scenes, synchronize render scenes, or drain resource queues.

Useful grep:

```powershell
rg -n "ScenePrimitiveHandle|largeScene|SceneSpatialIndex|SceneVisibilityPipeline|SceneLOD|SceneHLOD|SceneOcclusion|SceneStreamingResidency|BuildCullingOverlayItems|largeSceneCullReasonSnapshot|ReadPixelsChecked|BeginReadPixels|WaitForFence|Map" Runtime Project Tests Docs specs
```
