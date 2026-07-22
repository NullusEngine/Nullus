# Editor Scene View Camera Performance Design

## Status

Approved for specification on 2026-07-22. Implementation remains subject to
review of this written specification.

## Problem

Moving or rotating the Scene View camera forces a render every editor frame.
On the DX12 threaded-rendering path, this exposes two independent sources of
latency:

- Release builds frequently exhaust the prepared-frame slot ring. A frame that
  cannot reserve a slot may spend close to two separate 8 ms waits before it
  skips publication, producing visible long-frame spikes.
- Debug builds spend most of their main-thread time rebuilding visible queues
  and repeating per-draw preparation for scene commands whose mesh, material,
  transform, and render state did not change.

The existing TimelineProfiler export is useful for root-cause analysis but
changes the measured workload. Its periodic JSON export work adds 15-28 ms to
some Release frames, so it cannot be the source of the final before/after
numbers.

## Baseline

The reproducible baseline uses:

- Windows DX12;
- VSync disabled;
- `TestProject` and `New Scene.scene`;
- only Scene View open;
- 300 fixed 0.1-unit forward camera steps;
- the first 30 frames excluded as warm-up.

The initial trace-derived measurements are:

| Configuration | Application::TickFrame mean | Mean FPS | P99 |
| --- | ---: | ---: | ---: |
| Debug | 51.736 ms | 19.33 | 114.742 ms |
| Release | 11.868 ms | 84.26 | 29.802 ms |

The trace also identified these hotspots after warm-up:

| Configuration | Scope | Mean | P99 |
| --- | --- | ---: | ---: |
| Debug | `DeferredSceneRenderer::BeginFrame::ThreadedCapture` | 13.599 ms | 46.636 ms |
| Debug | `DeferredSceneRenderer::BeginFrame::ParseScene` | 16.189 ms | 49.016 ms |
| Debug | `RenderScene::GatherVisibleCommands` | 8.603 ms | 28.668 ms |
| Release | `DeferredSceneRenderer::BeginFrame::ThreadedCapture` | 7.176 ms | 23.846 ms |
| Release | `DeferredSceneRenderer::BeginFrame::ParseScene` | 1.829 ms | 4.696 ms |

In Release, 57 of 271 measured frames entered `ThreadedCapture` but never
entered a draw-capture child scope. Those frames averaged 15.188 ms in
`ThreadedCapture`, which matches two sequential waits bounded by the editor's
8 ms retirement timeout. Frames that captured draws averaged 5.042 ms in the
same parent scope. This makes slot backpressure the primary Release long-tail
cause rather than draw preparation alone.

## Goals

- Reduce visible hitches while moving or rotating the Scene View camera.
- Improve both Debug and Release builds on the same deterministic workload.
- Prevent stacked frame-slot and fence waits from consuming more than one
  editor retirement-wait budget.
- Avoid rebuilding static draw preparation data when its revision contract is
  unchanged.
- Avoid matrix comparisons and uploads for static object data already valid in
  the selected frame slot.
- Avoid recomputing opaque sort keys from resource state on every camera frame.
- Produce low-overhead, machine-readable before/after measurements without
  TimelineProfiler export enabled.
- Preserve rendering correctness, descriptor lifetime, picking/readback
  ordering, dynamic instancing, LOD selection, and material/mesh invalidation.

## Non-Goals

- Replacing threaded rendering or the frame graph.
- Changing VSync, editor resolution, camera speed, or scene content to improve
  the reported result.
- Skipping per-frame frustum, LOD, HLOD, occlusion, or streaming decisions that
  legitimately depend on the camera.
- Caching transparent distance ordering across camera movement.
- Dropping swapchain, readback, picking, capture, or other exact-frame work.
- A persistent GPU-driven scene, indirect-draw architecture, or general render
  pipeline rewrite.
- Treating TimelineProfiler trace timings as the final acceptance numbers.

## Chosen Approach

The implementation is a focused combination of four changes: a low-overhead
benchmark mode, a single frame-resource wait deadline, a revision-validated
static draw fast path, and cached opaque queue metadata. Each change has a
conservative fallback to the current path.

### 1. Low-Overhead Editor Benchmark

Add an editor validation argument that writes a summary file for the existing
fixed-step Scene View camera motion. The benchmark records wall-clock time
around `Application::TickFrame` without enabling the profiler or emitting a
per-frame log line.

The summary contains:

- configuration label, backend, VSync state, requested frame count, and warm-up
  count;
- measured frame count and individual measured frame durations;
- mean frame time, mean FPS computed as `1000 / mean_ms`, P95, P99, and maximum;
- cumulative threaded publication, blocked publication, reserved-slot wait,
  reserved-slot timeout, total wait time, latest published frame, and latest
  retired frame deltas;
- prepared static-base cache hits and misses, static fast-path hits and misses,
  object-data revision hits and fallbacks, opaque sort-key hits and rebuilds;
- the number of camera steps for which a new threaded frame was published.

The first 30 fixed-step frames are warm-up. The next 300 fixed-step frames are
measured. The editor writes one JSON summary after the last measured frame and
then closes. Summary generation occurs after timing collection, so JSON
serialization is outside the sample window.

The benchmark does not open the Profiler or Frame Info panels and does not use
`--editor-validation-trace-frames`. Timeline traces remain a separate optional
root-cause tool.

### 2. One Retirement-Wait Deadline

The editor continues to use an 8 ms retirement-wait budget, but one prepared
publication attempt receives one absolute deadline. All waits involved in
reserving the lifecycle slot and validating the slot's deferred GPU fence use
only the remaining duration.

The API boundary changes from repeated duration-based waits to an internal
deadline-aware reservation operation. `ThreadedRenderingLifecycle` records the
actual time spent waiting and preserves timeout telemetry. The driver must not
restart the full timeout after the lifecycle wait completes.

If the deadline expires or no safe slot becomes available:

- the renderer skips publication as it does today;
- the previously completed Scene View texture remains displayed;
- no in-flight slot or fence-protected resource is reused early;
- the frame is counted as a benchmark publication miss and a wait timeout.

The lifecycle already retires stale published or render-ready frames for the
same external output. That latest-output policy remains limited to replaceable
offscreen external outputs such as Scene View. Swapchain frames and work with
explicit readback, picking, RenderDoc capture, or synchronous-consumption
requirements retain exact ordering and are never made replaceable by this
change.

### 3. Revision-Validated Static Draw Preparation

Extend the scene drawable descriptor with an optional stable draw identity and
the revisions required to validate reusable CPU preparation:

- scene primitive handle identity and cached-command build serial;
- mesh instance and content revision;
- effective material instance, parameter, render-state, and binding revision;
- effective shader instance and generation;
- transform render revision;
- object-data grouping identity for dynamically merged opaque draws.

The values originate in `RenderScene`, which already owns stable primitive
handles, cached command build serials, transform/material input stamps, and
camera-independent cached draw commands. A merged opaque group derives its
identity from the ordered stable identities of all members; it cannot claim the
single-object fast path.

`ABaseRenderer` adds a stable prepared-draw lookup keyed by the scene identity,
pass/light mode, pipeline overrides, pass binding set, and explicit device.
The cached entry stores the complete revision stamp and prepared static base.
On a hit with an equal stamp, the renderer populates the draw directly and
updates a cheap last-used frame value. It does not rebuild the full general
cache key, query every resource revision again, hash the full key, or splice an
LRU node.

A missing identity, unequal revision, changed device, changed pass binding,
changed pipeline state, or changed merged-group membership falls back to the
existing general cache path. A successful fallback refreshes the stable entry.
The existing full cache remains the authority for dynamic, synthetic,
fullscreen, decal, transparent, and otherwise untrusted drawables.

Stable entries use the existing bounded size and frame-age policy. LRU order is
updated only on insertion, revision refresh, and the bounded age sweep; every
hit updates `lastUsedFrame` without list mutation. Device cache invalidation
clears both lookup structures.

### 4. Revision-Aware Object Data and Opaque Keys

Each object-data frame slot stores the stable object identity and transform
revision that produced every valid matrix range. When a non-merged single
object reaches the same slot with matching identity, transform revision,
object index, and object count, `TryPrepareIndexedObjectData` reuses the range
without scanning validity bytes or calling `memcmp`.

Merged instance groups and descriptors without a trustworthy revision continue
to use the existing source-shadow comparison. A revision mismatch performs the
normal transpose and upload, then refreshes the slot metadata. Slot reset,
capacity rebuild, device change, and object-index reassignment invalidate the
metadata together with the matrix shadow.

`RenderCachedDrawCommand` also stores its camera-independent opaque sort token.
It is rebuilt only when the cached command is rebuilt. `FinalizeOpaqueQueue`
uses this token plus the stable primitive identity, while transparent and decal
queues continue using camera distance. Dynamic instancing behavior and object
index assignment remain unchanged.

## Data Flow

For a measured camera frame:

1. `Application::TickFrame` starts the low-overhead wall-clock sample.
2. The validation step moves the Scene View camera and forces a render.
3. `RenderScene::Synchronize` refreshes only genuinely changed scene commands
   and their revision stamps.
4. `GatherVisibleCommands` performs camera-dependent visibility and LOD work,
   then emits descriptors carrying stable draw metadata and cached opaque keys.
5. Deferred capture reserves a prepared frame slot using the single absolute
   retirement deadline.
6. Each visible draw first attempts the stable prepared-draw lookup. A valid hit
   reuses pipeline, material binding, pass binding, and mesh preparation.
7. Indexed object data first attempts the slot-local identity/revision check. A
   valid hit reuses the matrix range; otherwise the current comparison/upload
   path runs.
8. The frame is published or conservatively skipped when no safe slot is
   available.
9. After the frame, the benchmark samples cumulative renderer telemetry and
   stops the wall-clock sample before any summary serialization.
10. After 30 warm-up plus 300 measured frames, the editor writes the JSON
    summary and exits.

## Correctness And Failure Handling

- Stable metadata is an optimization hint, never the only validity source. Any
  missing or inconsistent field uses the current path.
- Material parameter, render state, binding, shader generation, mesh content,
  transform, pass binding, pipeline, device, LOD mesh, or merged-group changes
  invalidate the affected fast path.
- Slot metadata is scoped to the actual reserved frame context. It is never
  reused across an unsafe or unretired slot.
- A timed-out reservation does not publish a partial snapshot and releases any
  reservation acquired during the failed attempt.
- Device loss and unsafe GPU-work quarantine continue to return no slot without
  waiting or touching protected resources.
- Benchmark file-open or write failure logs one error and exits nonzero after
  the requested samples; it does not silently claim a valid comparison.
- An invalid benchmark frame count or output path is rejected by launch-argument
  parsing.

## Test Strategy

### Unit Tests

- A shared retirement deadline cannot produce two full timeout waits.
- A reusable slot returned near the deadline gives the fence wait only the
  remaining budget.
- Timeout releases reservations and leaves all non-retired resources untouched.
- External-output stale-frame retirement remains isolated by output identity.
- Swapchain and exact-consumption frames are not treated as replaceable.
- A stable prepared-draw entry hits when every revision is equal.
- Each material, shader, mesh, pass, pipeline, device, command-build, and group
  revision change produces a miss and refresh.
- Repeated stable hits do not mutate the LRU list and age eviction remains
  bounded and correct.
- Object-data revision hits skip comparison/upload only for the same slot and
  range; transform changes, merged groups, slot reset, and index changes fall
  back correctly.
- Cached opaque sort tokens preserve the existing stable order and dynamic
  instancing result.
- Benchmark percentile and FPS calculations use deterministic sample vectors,
  exclude warm-up, and serialize all required fields.

### Build And Regression Tests

Build and run the focused renderer, lifecycle, editor launch-argument, Scene
View lifecycle, draw-cache, object-data, visibility, LOD, dynamic-instancing,
picking, and readback test filters in both Debug and Release. Then build the
Editor executable in both configurations.

### Performance Validation

Run the low-overhead benchmark at least three times per configuration using the
same machine and project. Use the median run for the comparison table and keep
all raw summaries as build artifacts. Do not run Debug and Release
simultaneously.

The report includes, for both configurations:

- before and after mean frame time and mean FPS;
- before and after P95, P99, and maximum frame time;
- absolute and percentage change;
- publication ratio and slot wait/timeout deltas;
- stable cache and object-data fast-path hit rates.

One post-change TimelineProfiler trace per configuration is allowed only to
confirm hotspot movement. It is reported separately from the acceptance
numbers.

## Acceptance Criteria

- Debug mean frame time improves by at least 15 percent against the new
  low-overhead baseline.
- Release P99 frame time improves by at least 30 percent against the new
  low-overhead baseline.
- A prepared publication attempt waits no longer than one 8 ms retirement
  budget, excluding scheduler measurement tolerance.
- The Scene View frame publication ratio does not regress from its low-overhead
  baseline. If latest-output retirement raises it, the report shows the gain.
- No benchmark run reports descriptor allocation failure, device loss, unsafe
  GPU quarantine, or object-data overflow.
- Scene color/depth readback, picking, resize, LOD transitions, dynamic
  instancing, material edits, mesh reloads, and transform edits pass their
  focused regressions.
- Debug and Release comparisons are generated with identical scene, camera
  path, frame counts, viewport, backend, and VSync settings.

## Alternatives Considered

### Wait Budget Only

Merging the two waits is small and directly addresses Release spikes, but it
does little for Debug, where scene parsing and repeated draw work dominate.

### Persistent Render Package

Keeping a persistent, patchable render package could remove more per-frame CPU
work. It crosses scene ownership, frame-graph resources, descriptor lifetime,
visibility, and asynchronous retirement boundaries, making it too broad for
this targeted performance repair.

### More Frame Slots

Increasing the ring size may hide backpressure temporarily, but increases
latency and resource retention without eliminating repeated CPU work or stacked
waits. The slot count remains unchanged.
