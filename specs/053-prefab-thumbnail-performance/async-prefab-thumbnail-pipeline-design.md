# Async Prefab Thumbnail Pipeline Design

**Date**: 2026-07-11
**Status**: Approved for implementation
**Scope**: Visible prefab thumbnails in the Asset Browser

## Problem

Large prefab thumbnails can remain pending for minutes. Current runtime evidence shows that a
resource-pending prefab is retried every 2 seconds while each retry inspects only four mesh
dependencies. A prefab with 405 unique meshes therefore needs at least 102 scheduling turns,
or about 202 seconds, before resource discovery alone can complete. Multiple visible prefabs
rotate through the same queue and extend the delay further.

The same pump calls `MeshManager::PumpAsyncLoadsForPaths` from the editor UI thread. Completed
artifact loads are promoted by constructing runtime GPU meshes in that call, so a single pump
can block the UI for hundreds of milliseconds. Increasing the existing batch size alone would
reduce latency by increasing UI stalls and is not an acceptable fix.

## Goals

- Show the first visible prefab thumbnail promptly, including for large prefabs.
- Keep thumbnail work from producing long editor UI frames.
- Preserve the complete-preview rule: do not render or cache a prefab thumbnail until every
  required mesh is ready and bounded resource discovery is complete.
- Preserve visible, inspector, prefetch, and background request priority semantics.
- Keep resource and rendering work bounded during shutdown, cancellation, and scope changes.

## Non-Goals

- Publishing partial geometry as a successful thumbnail.
- Adding a CPU silhouette or structure-diagram fallback.
- Claiming backend correctness from DX12 evidence alone.
- Replacing the existing thumbnail disk cache or prefab snapshot cache.

## Design

### 1. Separate Discovery From Runtime Promotion

The prefab preview preparation job continues to build the immutable renderable snapshot and
full dependency plan off the UI thread. Resource discovery then requests missing mesh and
material artifacts in larger bounded batches. Starting an artifact request must not construct
runtime GPU resources or wait for artifact completion.

The resource pump reports separate progress for:

- dependencies discovered;
- artifact reads pending or ready;
- runtime resources pending promotion;
- resources ready for preview rendering.

This distinction prevents the scheduler from applying the current long resource-pending delay
to cheap request-discovery work.

### 2. Move Expensive Promotion Off The UI Thread

Ready mesh artifact data is promoted to runtime GPU resources through the existing threaded
rendering lifecycle. The render-side work owns GPU buffer creation and upload submission. The
UI thread only polls completion and publishes the resulting resource into the manager/cache.

Promotion uses explicit per-frame limits for item count and upload bytes. A single oversized
mesh may span scheduling turns or consume one render-side turn, but it must not make the UI
thread wait for its GPU work. Materials retain a small bounded promotion budget; any material
operation that creates GPU state follows the same render-thread boundary.

### 3. Prioritize Time To First Thumbnail

The scheduler keeps one visible prefab as the active heavy-preview request while it is making
progress. Other visible requests remain queued and may continue background artifact reads, but
they do not repeatedly displace the active prefab's render-resource promotion. Pending GPU
readback still has precedence so submitted work can retire promptly.

The active request yields when it is cancelled, superseded, fails, stops making progress, or
finishes. This produces the first usable thumbnail sooner without starving the queue.

### 4. Keep UI Work Time-Bounded

Asset Browser thumbnail work keeps an approximately 2 ms UI-thread budget per frame for queue
maintenance, cache lookup, completion polling, and texture publication. The UI path never waits
for mesh decoding, GPU buffer upload, preview rendering, fences, readback, encoding, or disk
writes.

When the budget is exhausted, remaining work stays queued with its existing priority and active
request identity. Budget exhaustion is not recorded as failure and does not reset completed
discovery work.

### 5. Completion And Failure

The preview renderer submits only when the complete snapshot and every required mesh are ready.
Readback remains non-blocking. Successful pixels are encoded and written through the existing
background cache-write path, then published through the existing asynchronous UI texture upload
path.

Cancellation releases preview interests and render-side keep-alive state after outstanding GPU
work retires. Device loss, artifact failure, and terminal resource failure use the existing
diagnostic and finite retry policies.

## Validation

Automated tests must cover:

- a 405-mesh dependency plan can be fully discovered without a 2-second delay per four meshes;
- ready mesh promotion is not executed on the editor UI thread;
- one visible prefab remains active until completion or loss of progress;
- a second visible prefab is not starved after the first completes;
- cancellation and scope supersession preserve bounded cleanup;
- mixed mesh readiness and truncated discovery never publish a partial thumbnail;
- existing cache, priority, readback, and renderer-unavailable behavior remains intact.

Runtime validation uses the existing prefab thumbnail telemetry. The acceptance evidence is:

- no `ThumbnailUiPostDrawPumpStartHeavyGpu` frame dominated by mesh runtime creation;
- resource discovery advances without the fixed 2-second-per-small-batch cadence;
- a representative large prefab reaches a rendered and UI-sampled thumbnail;
- DX12 RenderDoc evidence confirms the complete Forward preview and UI sampling path.

DX12 runtime evidence does not establish Vulkan, Metal, or other backend correctness; those
backends require their own targeted verification when available.
