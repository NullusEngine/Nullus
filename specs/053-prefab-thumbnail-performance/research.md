# Research: Prefab and Thumbnail Performance

## Decision: Make profiling and benchmark evidence the first deliverable

**Rationale**: The existing system already has several caches: L1 session prefab data, L2 prepared prefab JSON, L3 prewarm, thumbnail cache metadata, and resource manager caches. Adding more cache layers without timing evidence can increase invalidation risk while leaving the true stall untouched. Phase 1 must measure the real entry points and rank costs before optimization work is accepted.

**Alternatives considered**:
- Start with binary prepared prefab artifacts: rejected for P0 because thumbnail stalls may be dominated by GPU readback and full preview instantiation rather than JSON parsing.
- Start with thumbnail cache expansion: rejected because thumbnail caches must not become hidden prefab instantiation caches.

## Decision: Use a shared stage statistics collector rather than source-string checks

**Rationale**: Tests need to verify that stages are emitted and counted without grepping C++ source for macro names. A shared collector can measure wall-clock duration, call count, thread category, and domain counters in a way that unit tests and benchmark reports can inspect directly.

**Alternatives considered**:
- Only use the existing profiler destination begin/end events: useful for timeline tools, but it does not provide aggregate domain counters or easy benchmark snapshots.
- Parse log output: rejected because it is brittle and slower to test.

## Decision: Keep diagnostic stages domain-specific but report format shared

**Rationale**: Prefab and thumbnail flows have different stages and counters, but the report should rank stages consistently by total time, main-thread time, background-thread time, call count, and extra domain counters. This allows one report command/test harness to compare baseline and optimized runs.

**Alternatives considered**:
- One generic string-only metric map: rejected because typed counters such as object count, renderer count, GPU bytes, and fence wait time need stable semantics.
- Separate unrelated report types: rejected because it makes cross-system bottleneck ranking and regression gates harder.

## Decision: Treat prepared prefab cache as a prepared-data cache, not a thumbnail cache

**Rationale**: Prefab instantiation and thumbnail generation can share prepared prefab graph data, dependency manifests, and runtime resources. They must not share scene instances, script instances, physics objects, or preview texture lifetime. A thumbnail cache stores final visual output; it cannot be the source of truth for runtime prefab instantiation.

**Alternatives considered**:
- Let thumbnail cache store enough data to instantiate prefabs faster: rejected because it mixes a visual output cache with runtime object semantics and invalidation rules.
- Duplicate prepared data per system: rejected because it repeats artifact parsing and dependency work.

## Decision: Thumbnail GPU readback must be non-blocking for editor frames

**Rationale**: `RhiThreadCoordinator::ReadPixelsChecked` currently waits on the completion object after beginning readback. That is appropriate for synchronous callers but is a poor default for project browser thumbnail generation. GPU thumbnail work should record/submit in one frame and poll completion in later frames without blocking the main thread.

**Alternatives considered**:
- Keep synchronous readback and lower thumbnail rate: rejected because even a single visible item can stall editor interaction.
- Encode directly from GPU texture without disk readback: useful for L1 GPU display, but disk thumbnail cache still needs eventual readback and encode.

## Decision: Thumbnail preview should use lightweight renderable data where possible

**Rationale**: The current prefab preview path loads the prefab artifact, instantiates the prefab into a preview scene, applies materials, collects bounds, renders, then clears objects. This can execute much more of the runtime object graph path than thumbnail rendering needs. A lightweight preview snapshot or preview instantiation mode should create only transform and renderable data required for drawing.

**Alternatives considered**:
- Continue full instantiation but skip lifecycle: a useful interim step, but still pays component creation, scene registration, and hierarchy costs.
- Render only mesh artifact paths for prefab thumbnails: insufficient for prefab overrides, transforms, nested renderers, and material assignments.

## Decision: Prefab instantiation should be measured and then batched by phase

**Rationale**: The existing `ObjectGraphInstantiator::InstantiatePrefab` already has distinct conceptual loops for game object creation, component/state application, parent resolution, and scene addition. Stage measurements should establish which loops dominate. Later batching can reserve storage, create objects/components, restore properties, fix references, bind asset handles, register systems, and activate lifecycle in separate phases.

**Alternatives considered**:
- Rewrite instantiation around a new binary format immediately: rejected until measurements show JSON/object graph parse dominates.
- Only optimize `Scene::AddGameObject`: rejected because it may help large prefabs but not parse/resource/thumbnail stalls.

## Decision: Resource identity should move toward asset identity and in-flight coalescing

**Rationale**: Repeated prefab instantiation should share immutable mesh/material/texture/shader resources. Resource requests should use asset identity plus sub-asset and artifact stamp where possible, expose `Unloaded/Loading/Ready/Failed`, and coalesce concurrent requests. Existing material manager behavior already suggests this direction; mesh and other resources need measured parity.

**Alternatives considered**:
- Keep path-only cache keys: rejected long-term because path aliases can duplicate the same asset and do not encode artifact freshness.
- Force prefab instantiation to wait for all resources: rejected because graph creation can complete with pending/placeholder resources.

## Decision: Cache entries need explicit capacity and failure semantics from the start

**Rationale**: The user explicitly disallows unbounded global caches and half-built results. Every cache role needs key, version, stamp, capacity, invalidation, stats, coalescing, failed state retry policy, cleanup, and debug inspection.

**Alternatives considered**:
- Add unlimited hot maps first and cap them later: rejected because it hides leaks and invalidation bugs in performance tests.
- Cache failed results forever: rejected because transient import/resource/GPU failures should have finite retries.

## Decision: Testing will use counters, fixtures, and behavior assertions

**Rationale**: Performance behavior must be regression-tested without asserting absolute times unsupported by baseline. Tests should validate stage emission, cache hit/miss behavior, duplicate request coalescing, non-blocking fence polling, no script/physics execution in preview, shutdown cancellation, and relative before/after reports.

**Alternatives considered**:
- Hard-code global time thresholds: rejected because local hardware and CI variance make absolute thresholds brittle.
- Manual-only editor testing: rejected because core cache/scheduler/instantiation guarantees need repeatable coverage.
