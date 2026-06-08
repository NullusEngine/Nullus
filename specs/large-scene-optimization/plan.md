# Implementation Plan: Large Scene Optimization

**Branch**: `large-scene-optimization` | **Date**: 2026-06-03 | **Spec**: [spec.md](spec.md)
**Input**: Complete large-scene optimization design requested from an isolated worktree.

## Summary

Nullus already has the beginning of a UE-style retained renderer: `RenderScene` keeps persistent primitive state, caches draw commands, evaluates bitset visibility, can partition visibility through the JobSystem, sorts opaque commands for state locality, assigns object indices, and dynamically instances compatible opaque objects. The complete large-scene optimization plan extends that foundation into a full pipeline: instrument first, move primitive ownership to generation-safe slot handles, query a static/dynamic spatial index with layer/distance/active metadata, select LOD and HLOD representations, conservatively apply HZB/history occlusion, keep residency under streaming and memory budgets, and surface cull reasons and budgets through FrameInfo and editor debug tools.

The plan intentionally avoids a single risky rewrite. It defines complete architecture and acceptance gates for each layer so implementation can land in stable increments without leaving the design incomplete. UE 4.27 and Unity 2018.4 are used as concrete references, but Nullus adapts the ideas to the existing RenderScene, JobSystem, RHI, FrameGraph, editor asset path, and draw-call optimization work.

## Technical Context

**Language/Version**: C++20 for current Nullus runtime/editor code.
**Primary Dependencies**: Nullus Rendering, Engine SceneSystem, Runtime/Base/Jobs, RHI/FrameGraph, GoogleTest-based unit tests, RenderDoc workflow for GPU evidence.
**Storage**: Runtime scene state, imported asset metadata, streaming residency state, optional HLOD artifacts.
**Testing**: Focused `NullusUnitTests`, synthetic runtime scenes, editor Scene View traces, and RenderDoc/RHI validation for GPU occlusion paths.
**Target Platform**: Windows DX12 first; other backends and platforms require direct evidence before claims.
**Project Type**: Desktop engine runtime and editor.
**Performance Goals**: Avoid full-scene per-frame work in large scenes, keep stable frames from rebuilding cached draw state, reduce distant and hidden draw preparation, and keep streaming commits bounded.
**Constraints**: Do not hand-edit generated files under `Runtime/*/Gen/`; preserve current Editor/Game render entrypoints; do not assume one backend proves another; prefer RenderDoc evidence for GPU correctness.
**Scale/Scope**: Imported large environments, authored scenes, repeated prop fields, additive scenes, editor Scene View and Game View rendering.

## Constitution Check

- **Spec-first major change**: PASS. Rendering pipeline, RHI, scene, asset, and editor behavior are covered by this spec bundle before implementation.
- **Generated-file boundary**: PASS. No generated file edits are part of this design.
- **Rendering validation**: PASS. CPU-only stages use unit/perf tests; GPU occlusion and resource-state changes require RenderDoc or RHI-event evidence.
- **Cross-backend caution**: PASS. DX12 is the first target, and backend claims stay evidence-limited.
- **Incremental validation**: PASS. Each subsystem has an independent acceptance gate and fallback path.
- **Existing workflow preservation**: PASS. The plan reuses existing build/test ownership, JobSystem, RenderScene, FrameInfo, and editor panel patterns.

## Architecture Overview

### Layer 1: Instrumented Retained Scene Foundation

`Runtime/Engine/Rendering/RenderScene.*` remains the owner of render-side primitive state. The existing raw `MeshRenderer*` index is replaced by a slot-map/free-list/tombstone handle model with a separate component-to-handle lookup. Handles are stable only when index and generation match; dense iteration arrays are derived per snapshot and never define handle identity. Records cache world/model bounds, feature flags, layer/distance settings, resource validity, last sync serial, and last visible serial. `RendererStats` and `FrameInfo` gain large-scene counters so every later layer can be verified without scraping profiler text.

The first implementation must keep the current gather path functional when large-scene features are disabled. This gives a safe fallback and a baseline for every performance claim.

### Layer 2: Static/Dynamic Spatial Index

Nullus should add `SceneSpatialIndex` beside `RenderScene`, not inside `SceneSystem::Scene`. Static primitives use a cache-friendly tree, binned BVH, or equivalent measured index that can be refit when bounds change and rebuilt when topology changes exceed a threshold. Dynamic primitives use a loose grid, dynamic tree, or equivalent measured dynamic spatial index that updates every frame without touching static nodes. The index also stores coarse active, layer, and distance metadata so localized visibility does not first scan every primitive just to reject inactive or masked objects.

The output is a compact candidate list or bitset range for each view. This stage prevents the existing bitset visibility evaluator from receiving every primitive in large partitioned scenes. Telemetry must distinguish registered primitive count, spatial candidate count, primitive records touched, and visibility-tested primitive count so the candidate stage cannot hide a remaining O(N) scan.

### Layer 3: Unified Visibility Pipeline

Visibility becomes a named pipeline instead of scattered checks:

1. Snapshot live primitive records and per-view inputs.
2. Query static and dynamic spatial/layer/distance candidate data.
3. Revalidate activity, layer, distance, and editor visibility masks for candidates.
4. Run frustum tests on candidates.
5. Select per-object LOD and HLOD proxy or child representation.
6. Apply conservative occlusion history when valid.
7. Emit visible masks, representation decisions, cull reasons, and command eligibility.
8. Let `RenderScene` finalize visible queues, preserve transparent ordering, sort/merge opaque commands, assign object indices, and publish telemetry.

Serial and parallel results must be identical. The parallel path may use `Runtime/Base/Jobs`, but failed scheduling must fall back to serial evaluation.

### Layer 4: LOD And HLOD

Per-object LOD lands before HLOD because it is easier to validate and feeds both Unity-style LODGroup semantics and HLOD cluster decisions. LOD selection uses screen-relative size, view distance, LOD bias, hysteresis, and optional fade metadata. HLOD clusters then replace many child primitives with one proxy primitive for distant views, while editor inspection can disable child suppression for selected objects.

Imported large models can generate HLOD candidates from hierarchy, material compatibility, and bounds. Authored scenes can provide explicit clusters later. Proxy residency is part of streaming, not an excuse to block visibility.

### Layer 5: Conservative HZB Occlusion

Nullus should start with HZB/history occlusion rather than Umbra-style baked occlusion. HZB fits the current runtime renderer and can work for dynamic scenes, but it must be conservative: invalid history means visible. GPU data must not be read back synchronously for current-frame culling. View changes, depth-resource changes, camera cuts, backend reset, viewport changes, primitive bounds/transform changes, representation changes, and material depth-write eligibility changes invalidate history. HZB depth comes only from qualified opaque depth-writing geometry.

DX12 validation must show pass order, resource states, subresource ranges, dependency edges, and no ordinary-frame CPU/GPU readback wait: scene opaque depth writes, HZB build, optional occlusion dispatch/test, and next-frame consumption. Other backends remain capability-gated.

### Layer 6: Streaming And Residency Budgets

Visibility and LOD/HLOD produce interest. Streaming decides what can become resident under CPU, IO, GPU upload, CPU memory, and GPU memory budgets. This layer handles scene cells, HLOD proxies, mesh LODs, textures, and imported asset artifacts. It also shares budgets with editor background import/drop work so large-scene streaming does not compete with unbounded editor work.

Streaming states are explicit: `Unrequested`, `Requested`, `LoadingCpu`, `PendingGpuUpload`, `Resident`, `VisibleResident`, `EvictPending`, and `Evicted`. In-flight render packages pin resources until frame retirement.

### Layer 7: Editor Observability

FrameInfo and optional Scene View overlays consume renderer snapshots. Debug output reports why a primitive did not draw: inactive, layer, distance, spatial miss, frustum, LOD child suppressed, HLOD child suppressed, occluded, not resident, missing mesh, invalid material, or backend fallback. Disabled overlays must add no scene traversal to ordinary navigation frames.

## Data Flow

```text
Scene components and imported assets
  -> RenderScene::Synchronize retained primitive records
  -> SceneSpatialIndex static/dynamic candidate query
  -> VisibilityPipeline per-view snapshot
  -> LODGroup and HLOD cluster selection
  -> OcclusionHistory/HZB conservative culling
  -> Visible cached command queues
  -> Draw-call grouping/object-index assignment
  -> FrameGraph/RHI execution and streaming interest
  -> RendererStats/FrameInfo/editor overlays
```

## Project Structure

### Documentation

```text
specs/large-scene-optimization/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── benchmarks/
│   └── rendering-large-scene-benchmark.md
├── contracts/
│   └── large-scene-optimization-contract.md
├── checklists/
│   └── requirements.md
├── review.md
└── tasks.md
```

### Source Code Scope

```text
Runtime/Engine/Rendering/
├── RenderScene.h
├── RenderScene.cpp
├── BaseSceneRenderer.h
├── BaseSceneRenderer.cpp
├── SceneSpatialIndex.h
├── SceneSpatialIndex.cpp
├── SceneVisibilityPipeline.h
├── SceneVisibilityPipeline.cpp
├── SceneLOD.h
├── SceneLOD.cpp
├── SceneHLOD.h
├── SceneHLOD.cpp
├── SceneStreamingResidency.h
└── SceneStreamingResidency.cpp

Runtime/Engine/Components/
├── CameraComponent.h
└── CameraComponent.cpp

Runtime/Rendering/Entities/
├── Camera.h
└── Camera.cpp

Runtime/Rendering/
├── Core/RendererStats.*
├── Data/FrameInfo.h
├── FrameGraph/*
└── RHI/*

Runtime/Base/Jobs/*
Project/Editor/Panels/*
Project/Editor/Rendering/*
Project/Editor/Assets/*
Tests/Unit/*
Docs/Rendering/*
```

**Structure Decision**: Keep large-scene runtime ownership beside `RenderScene` because it already bridges `SceneSystem::Scene` into renderer-visible draw state. Keep FrameGraph/RHI changes isolated to HZB and backend capability gates. Keep editor overlays and FrameInfo as consumers of renderer snapshots. Runtime and unit-test source files are currently collected by the existing glob-based targets in `Runtime/Engine/CMakeLists.txt` and `Tests/Unit/CMakeLists.txt`; implementation tasks should verify those globs still include new files instead of adding imaginary per-directory source lists.

## Implementation Phases

The phase numbers in this section describe the architecture rollout layers. They are intentionally
different from `tasks.md` execution phases: for example, `tasks.md` Phase 3 is the US1 execution
phase that implements the architecture Phase 0 observability/baseline counters.

### Phase 0: Baseline And Counters

Add large-scene telemetry and stress fixtures before changing visibility behavior. Capture current full-scan costs, sync touched counts, visibility touched counts, queue-finalization touched counts, draw-count behavior, and streaming/resource stalls. This phase changes observability, not culling decisions.

### Phase 1: Primitive Handles And Spatial Index

Add stable primitive handles, generation checks, immutable `ScenePrimitiveSnapshot` data, cached bounds, dirty propagation for downstream spatial-index maintenance, static/dynamic spatial index queries, last-good static rebuild fallback, and sparse queue-finalization inputs. The current live source-scene sweep remains explicitly reported through telemetry until source dirty events land; prove candidate queries reduce work in partitioned and dynamic-heavy scenes and preserve existing visible results when the spatial index is disabled.

### Phase 2: Unified Visibility Pipeline

Move activity, layer, distance, frustum, cull reason, and serial/parallel equivalence into one pipeline. Existing `RenderScene::GatherVisibleCommands` can delegate to the new pipeline while still outputting the current visible queue format.

### Phase 3: LOD And HLOD

Add per-object LOD records first, then HLOD clusters and proxies. Validate editor selection behavior and fade/hysteresis. HLOD proxy assets may begin as generated or manually assigned proxies; auto-generation quality can improve later without changing visibility contracts.

### Phase 4: HZB Occlusion

Add HZB resources, build pass, conservative history, invalidation rules, and backend capability gates. This phase must include DX12 RenderDoc/RHI validation before claiming GPU occlusion correctness.

### Phase 5: Streaming Budgets

Add scene-cell and asset residency states, interest generation from visibility/LOD/HLOD, budgeted commits, and safe eviction. Integrate with editor asset/import budgets and frame retirement resource pinning.

### Phase 6: Editor Debug And Runtime Evidence

Add cull-reason overlays, FrameInfo fields, validation logs, and comparable Scene View traces. Re-run focused unit tests, runtime stress scenes, and RenderDoc checks for the enabled feature set.

## Key Design Decisions

- **Extend RenderScene instead of replacing it**: Existing retained draw-command and draw-call work is valuable and already tested.
- **Static BVH/binned BVH or equivalent plus dynamic spatial index**: Static and dynamic update patterns differ enough that one structure would either rebuild too often or scan too much; the exact static algorithm remains measured, but both static and dynamic paths must prove bounded candidates and touched counts.
- **HZB before baked occlusion**: Nullus lacks a baked Umbra-equivalent pipeline. HZB/history is incremental and backend-gateable.
- **View-local HLOD suppression**: Editor/Game views may need different representation decisions for the same retained scene.
- **HLOD needs a minimal residency snapshot before full streaming**: Proxy selection may read current resource readiness and fallback state, but only the later budgeted streaming layer may issue or commit loads.
- **Streaming interest is not residency**: Visibility can request assets, but only the budgeted residency system can commit them.
- **Snapshot debug**: Editor tools read renderer snapshots so debugging does not add the traversal or sync cost being investigated.

## Risks And Mitigations

- **Risk: Raw component lifetime races in parallel visibility**. Mitigation: visibility jobs use immutable primitive snapshots and generation-checked handles.
- **Risk: Spatial index complexity hides bugs**. Mitigation: serial full-scan comparison mode remains available in tests and debug builds.
- **Risk: Candidate queries still hide a full primitive scan**. Mitigation: add `primitiveRecordsTouched` and `visibilityTestedPrimitiveCount` telemetry plus tests that assert both stay bounded in partitioned scenes.
- **Risk: RenderScene source sync remains the hidden O(N) bottleneck**. Mitigation: expose the live source-scene sweep cost through telemetry, keep downstream spatial-index maintenance and queue finalization dirty/sparse, and track full source dirty-list/event-driven synchronization as a separate future closure item instead of hiding it inside visibility claims.
- **Risk: Queue finalization moves the O(N) scan after visibility**. Mitigation: visibility emits sparse primitive/command handles and snapshots carry command-offset tables; finalization touched counters must stay bounded.
- **Risk: Spatial rebuild fallback causes periodic hitches**. Mitigation: use last-good index data, dirty overlays, staged rebuilds, or explicit rebuild budgets with fallback counters.
- **Risk: Dynamic-heavy scenes still scan all dynamic records**. Mitigation: dynamic index path has separate candidate/touched telemetry and high-dynamic-count regression tests.
- **Risk: Layer and distance filters lack an engine-level source of truth**. Mitigation: implementation must bridge existing `GameObject::GetLayer()` and `LayerMask` with explicit camera/editor view masks, and must store named min/max draw distances in primitive visibility settings before enabling distance culling.
- **Risk: Stable handles alias after dense vector erase**. Mitigation: primitive storage uses slots, generations, free lists, and tombstones; compact iteration arrays are snapshot-only and do not define handle identity.
- **Risk: HLOD assumes a streaming system that has not landed yet**. Mitigation: implement a minimal read-only representation residency snapshot before HLOD proxy selection, then replace its data source with the full budgeted streaming state in the streaming phase.
- **Risk: Memory budgets are specified but not enforceable**. Mitigation: residency data records CPU/GPU byte sizes and telemetry records resident/requested/evicted bytes.
- **Risk: HLOD hides objects during editor selection**. Mitigation: selection and inspection modes can force child visibility per view.
- **Risk: GPU occlusion creates missing objects**. Mitigation: invalid or missing history is visible, and history invalidation covers view compatibility, depth convention, primitive bounds/transform generation, representation id, material depth-write eligibility, and backend reset.
- **Risk: HZB pass ordering is backend-sensitive**. Mitigation: HZB capability gates must flow through `RHIDevice::GetCapabilities()` and texture-format capabilities; DX12 RenderDoc/RHI validation is required; other backends are capability-gated.
- **Risk: HZB compute hazards bypass FrameGraph dependency edges**. Mitigation: prepared compute dispatches must support explicit texture resource accesses, visibility transitions, exported transitions, producer-consumer dependency edges, narrow subresource ranges for depth SRV and current mip0 HZB UAV/SRV, plus explicit buffer resource access for HZB primitive result resources. Future mip-chain generation must add matching subresource edges when additional mips are actually written.
- **Risk: Occlusion history readback becomes a hidden stall**. Mitigation: ordinary render frames must not call synchronous `ReadPixelsChecked` fallback, wait for `BeginReadPixels` completion, wait on GPU fences, or block on readback-buffer maps; async readback, GPU-side history, or previous-frame GPU buffers must be proven by tests.
- **Risk: Streaming evicts in-flight resources**. Mitigation: frame retirement pins resources until prepared packages are retired.
- **Risk: Streaming cells become flat artifact lists instead of dependency closure**. Mitigation: add deterministic `StreamingResourceDependency` closure, deduplicated `ResidencyTicket` records, priority aging, cancellation, coalescing, and dependency-aware eviction tests.
- **Risk: Debug overlays become the bottleneck**. Mitigation: overlays consume snapshots and stay disabled-cost-free.

## Phase 0 Research

See [research.md](research.md). The key conclusion is that UE and Unity both separate scene state, visibility, representation choice, occlusion, draw extraction, and residency. Nullus should mirror the layering while reusing its own retained `RenderScene`, JobSystem, and RHI.

## Phase 1 Design

See [data-model.md](data-model.md), [contracts/large-scene-optimization-contract.md](contracts/large-scene-optimization-contract.md), and [quickstart.md](quickstart.md).

## Post-Design Constitution Check

- **Spec scope**: PASS. The design covers a single large-scene optimization program under one bundle.
- **Generated-file boundary**: PASS. Generated files remain excluded.
- **Validation fit**: PASS. CPU stages have unit/stress tests; GPU stages require RenderDoc/RHI evidence.
- **Backend caution**: PASS. DX12 first, no unproven cross-backend claims.
- **Implementation readiness**: PASS. Tasks are dependency-ordered and each story has an independent test gate.

## Complexity Tracking

No constitution violation is required. The complexity comes from necessary large-scene layering: retained scene state, spatial acceleration, representation selection, occlusion, residency, and debug snapshots each solve a different bottleneck and have separate fallback paths.
