# Research: Large Scene Optimization

## Decision: Build On Nullus Retained RenderScene

**Decision**: Extend `Runtime/Engine/Rendering/RenderScene.*` with large-scene layers instead of replacing it with a new renderer.

**Rationale**: The current `RenderScene` already contains persistent primitive state, cached draw commands, bitset visibility snapshots, serial/parallel visibility evaluation, opaque state sorting, object-index assignment, and dynamic instancing. Replacing it would discard recent work from `specs/028-large-model-scene-performance` and `specs/038-optimize-draw-calls`.

**Alternatives considered**:

- A new standalone visibility manager was rejected because it would duplicate primitive ownership and draw-command cache invalidation.
- Keeping the current full primitive scan was rejected because it does not scale to partitioned large scenes.

## Decision: Add A Static/Dynamic Spatial Index Before Bitset Visibility

**Decision**: Add a renderer-owned `SceneSpatialIndex` that produces candidate primitive handles before existing bitset visibility evaluation.

**Rationale**: UE 4.27 and Unity 2018.4 both layer culling, representation choice, occlusion, retained render data, and draw extraction so large scenes avoid preparing every draw. They do not prove that every visibility path begins with a spatial candidate query: UE can parallel-scan primitive visibility maps, and Unity can partition renderer ranges when Umbra is absent. Nullus's spatial candidate stage is therefore a deliberate local architecture choice that must be measured through `primitiveRecordsTouched` and `visibilityTestedPrimitiveCount`, not an overclaim copied from UE or Unity.

**Alternatives considered**:

- Only parallelizing the current scan was rejected because it uses more cores but still does O(total primitives) work.
- A single dynamic tree for all primitives was rejected because static environment geometry and per-frame dynamic props have different update costs.

## Decision: Use LOD Before HLOD, Then HLOD Proxy Suppression

**Decision**: Implement per-object LOD first, then HLOD clusters that suppress children view-locally when a proxy is selected.

**Rationale**: Unity's `LODGroupManager` computes camera-specific LOD masks and fades before render-node extraction. UE's HLOD state then decides forced visible, hidden, and fading states for clustered actors. Nullus should follow the same layering because LOD is easier to test on individual primitives and HLOD depends on stable primitive membership.

**Alternatives considered**:

- Directly building HLOD without object LOD was rejected because imported assets often already contain useful mesh-level LODs.
- Treating HLOD suppression as global state was rejected because Scene View, Game View, selection, and debug views can need different representation choices.

## Decision: Start With HZB/History Occlusion, Not Umbra-Style Baked Occlusion

**Decision**: Implement conservative HZB occlusion and history invalidation first.

**Rationale**: UE 4.27 has HZB and query paths in `SceneOcclusion.cpp`. Unity 2018.4 uses Umbra static occlusion where baked data exists, then dynamic object culling. Nullus does not currently have a baked visibility pipeline or Umbra dependency. HZB can be implemented behind the existing RHI/FrameGraph and can degrade to visible when history is invalid. The Nullus path must be stricter than a naive HZB port: history reuse depends on a full view/depth/resource/primitive/representation compatibility key, and HZB depth must come only from eligible opaque depth-writing scene geometry.

**Alternatives considered**:

- Baked occlusion first was rejected because it requires authoring/import/cooking infrastructure before runtime benefit.
- CPU software occlusion as the main path was rejected because it risks CPU hot-path cost and still requires careful raster bounds handling. It may remain a test helper.

## Decision: Treat Streaming Residency As A Separate Budgeted System

**Decision**: Visibility, LOD, and HLOD generate interest; streaming controls actual resource residency under budgets.

**Rationale**: UE `WorldComposition.cpp` updates tile loading and visibility state based on distance and committed streaming state. Unity render-node extraction assumes renderer resources and batching data are prepared through asset/render systems, not synchronously loaded inside culling. These references justify separating visibility from residency or asset preparation; they do not prove Nullus's budget model. Nullus has recent work preventing synchronous material and mesh loads in render prep, so large-scene streaming must preserve that rule and add local CPU, IO, GPU upload, CPU memory, and GPU memory budgets with direct validation.

**Alternatives considered**:

- Loading missing resources from visibility was rejected because it reintroduces frame hitches.
- Evicting solely by distance was rejected because in-flight frames and HLOD/selection/debug needs can pin resources.

## Unreal Engine 4.27 Reference Findings

### Visibility And HLOD

Local source path: `F:/Epic Games/UE_4.27`.

- `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` contains `FSceneRenderer::ComputeViewVisibility`, which coordinates primitive visibility, frustum/distance culling, HLOD state, occlusion, and relevance gathering.
- `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` contains `FLODSceneTree::UpdateVisibilityStates`, which manages HLOD forced visible, hidden, and fading states.
- `Engine/Source/Runtime/Renderer/Public/PrimitiveSceneInfo.h` contains retained primitive state and `CacheMeshDrawCommands`.

**Nullus adaptation**: Keep `RenderPrimitive` retained state in `RenderScene`, add view-local HLOD decisions to visibility results, and avoid mutating child primitive state globally.

### Occlusion

- `Engine/Source/Runtime/Renderer/Private/SceneOcclusion.cpp` contains `FHZBOcclusionTester::Submit`, which submits bounds for HZB tests.
- `Engine/Source/Runtime/Renderer/Private/SceneOcclusion.cpp` contains `RenderOcclusion`, which coordinates occlusion queries, depth downsample/HZB, and fences.

**Nullus adaptation**: Use HZB resources behind FrameGraph/RHI, consume conservative history, and avoid CPU readback stalls. Missing history means visible.

### Draw Commands And Parallel Recording

- `Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp` contains `FParallelMeshDrawCommandPass::DispatchDraw`.
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` documents cached mesh draw command concepts.
- `Engine/Source/Runtime/Renderer/Public/PrimitiveSceneInfo.h` stores static mesh command info and exposes cached command maintenance.

**Nullus adaptation**: `specs/038-optimize-draw-calls` already maps this to `RenderScene` cached commands, dynamic instancing, and DX12 in-render-pass child command buffers. Large-scene work should feed better visible command queues into that existing path.

### Streaming

- `Engine/Source/Runtime/Engine/Private/WorldComposition.cpp` contains distance-based streaming state updates and tile visibility/load state commits.
- `Engine/Source/Runtime/Engine/Private/World.cpp` contains cull distance volume updates.

**Nullus adaptation**: Add scene cell and asset residency budgets without copying UE World Composition architecture wholesale. Nullus imported assets and editor drag/drop paths need their own budget integration.

## Unity 2018.4 Reference Findings

Local source path: `D:/VSProject/Unity2018.4.0f1`.

### Culling Pipeline

- `Runtime/Graphics/ScriptableRenderLoop/ScriptableCulling.cpp` contains `CullScriptable`, which sets up culling results, Umbra data, static occlusion, dynamic culling, renderer update, light/shadow culling, and scene culling.
- `Runtime/Camera/SceneCulling.cpp` contains `CullDynamicScene`, `CullDynamicObjectsJob`, and `CullObjectsWithoutUmbra`, which run dynamic culling in job ranges and combine output.
- `Runtime/Interfaces/IUmbra.h` and `Runtime/Camera/UmbraInterface.h` define the Umbra interface and tome loading path.

**Nullus adaptation**: Use the same separation of culling inputs, jobified evaluation, and output combination. Do not depend on Umbra; provide HZB/history and future baked data hooks.

### LOD

- `Runtime/Graphics/LOD/LODGroupManager.cpp` contains `CalculateLODMask`, `CalculateLODMasks`, and camera data management.
- `Runtime/Graphics/LOD/LODGroup.h` stores group registration and manager index.

**Nullus adaptation**: Add `LODGroupRecord` and per-camera LOD result arrays in renderer state. Use screen-relative metrics and fade/hysteresis.

### Render Nodes And Batching

- `Runtime/Camera/RenderNodeQueue.h` and `Runtime/Camera/SharedRendererScene.h` expose render-node queue ownership.
- `Runtime/Camera/RenderLoops/BatchRenderer.cpp` handles dynamic batching and render-loop batching.
- `Runtime/GfxDevice/InstancingBatcher.cpp` computes draw calls and renders instance batches from render-node queues.

**Nullus adaptation**: Nullus already has visible draw queues and dynamic instancing. Large-scene visibility should output stable command queues compatible with the draw-call optimization path rather than inventing another submission model.

## Industry Benchmark Mapping

This plan uses `C:/Users/Chenyang/.codex/skills/plan-review/benchmarks/rendering_layout.md` for the rendering benchmark entry **RHI In-Render-Pass Parallel Draw Recording** and `benchmarks/engine_runtime.md` for **Native JobSystem / Task Scheduler**.

**Benchmark alignment**:

- The draw-command and parallel-recording portion aligns with UE 4.27 `FParallelMeshDrawCommandPass::DispatchDraw` and DX12 bundle-backed child command buffers already described in the Nullus benchmark.
- The visibility parallelism portion aligns with Unity 2018 job range culling and Nullus `Runtime/Base/Jobs` guarantees around deterministic handles, safe waiting, and fallback.
- The large-scene additions extend the benchmark surface with spatial indexing, LOD/HLOD, HZB occlusion, and residency budgets. This bundle includes `benchmarks/rendering-large-scene-benchmark.md` as the dedicated large-scene visibility/residency benchmark mapping; final implementation sign-off still requires `/plan-review` to verify the registry entry and any measured baseline/performance claims.

## Large Scene Benchmark Addendum

**Industry references used by this design**:

| Area | UE 4.27 reference | Unity 2018.4 reference | Nullus standard |
|------|-------------------|------------------------|-----------------|
| Retained primitive and draw state | `PrimitiveSceneInfo.h`, `MeshDrawCommands.cpp` | `RenderNodeQueue.h`, `SharedRendererScene.h` | Extend `RenderScene` instead of rebuilding draw inputs from components every frame. |
| Frustum/range visibility | `SceneVisibility.cpp::ComputeViewVisibility` | `SceneCulling.cpp::CullDynamicScene` | Use maintained candidate sets plus serial/parallel equivalence tests; do not claim UE/Unity prove this exact structure. |
| HLOD/LOD | `SceneVisibility.cpp::FLODSceneTree::UpdateVisibilityStates` | `LODGroupManager.cpp::CalculateLODMasks` | Keep LOD/HLOD decisions view-local and test hysteresis/fallback behavior. |
| Occlusion | `SceneOcclusion.cpp::FHZBOcclusionTester::Submit`, `RenderOcclusion` | Umbra interfaces and `SceneCulling.cpp` static/dynamic culling | Start with conservative HZB/history, invalid history visible, explicit RHI barriers. |
| Streaming/residency | `WorldComposition.cpp::UpdateStreamingState` separates tile load/visibility state from rendering | Render-node and asset preparation pipeline around culling separates culling from synchronous asset loads | Visibility emits interest; Nullus-specific budgeted residency owns dependency closure, tickets, load/commit/evict, and must be validated directly. |

**Industrial standard for Nullus large-scene work**:

- Visibility tests must report candidate count and actual primitive records touched, not only final visible draw count.
- Stable handles must be generation-checked and immune to dense-array erase aliasing.
- Representation selection must be view-local and must not globally mutate child primitives.
- GPU occlusion must be conservative and must prove resource-state ordering with backend-specific evidence.
- Residency must enforce CPU, IO, GPU upload, CPU memory, and GPU memory budgets.
- Existing draw-call grouping and object-index semantics must remain a hard regression gate.

## Nullus Current-State Findings

- `Runtime/Engine/Rendering/RenderScene.h` defines `RenderSceneVisibilitySnapshot`, `RenderCachedDrawCommand`, retained `RenderPrimitive`, and sync/gather APIs.
- `Runtime/Engine/Rendering/RenderScene.cpp` uses bitsets, serial and parallel visibility evaluation, cached command stamps, opaque sorting, dynamic instancing, and object-index assignment.
- `Runtime/Rendering/Data/FrameInfo.h` already records parse counts, draw optimization stats, parallel command work units, fallback reason, and threaded frame telemetry.
- `Runtime/Engine/SceneSystem/Scene.h` exposes `FastAccessComponents`, which is a useful input but not a sufficient large-scene spatial index.
- `Runtime/Base/Jobs` provides the scheduler needed for parallel visibility, background work, and future streaming continuations.

## Open Implementation Choices Resolved By This Design

- **Spatial structure**: Start with separate static and dynamic structures. The static implementation can be BVH or binned BVH; the contract requires candidate reduction, refit, rebuild thresholds, and serial comparison tests rather than mandating one exact tree algorithm.
- **Occlusion data source**: Start with HZB/history; leave baked occlusion as a future data source.
- **HLOD proxy generation**: Support imported hierarchy/manual clusters first; high-quality automatic proxy mesh generation can be added later behind the same `HLODClusterRecord` contract.
- **Streaming owner**: Renderer publishes interest; residency system owns request/commit/evict and resource pinning.
