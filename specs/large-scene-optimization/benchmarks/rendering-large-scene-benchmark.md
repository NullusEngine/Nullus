# Benchmark Draft: Rendering Large-Scene Visibility And Residency

This is the large-scene benchmark entry promoted into the `/plan-review` benchmark registry for functional implementation review. Measured performance or industry benchmark sign-off still requires concrete local benchmark evidence with a fixed hardware, scene fixture, camera path, backend, build configuration, feature-gate set, fallback log, and before/after per-stage deltas.

## Rendering Large-Scene Visibility And Residency

**Nullus target implementation**:

- `Runtime/Engine/Rendering/RenderScene.*` - retained primitive slots, explicit source-sync sweep telemetry, dirty downstream spatial-index maintenance, immutable snapshots, sparse queue finalization, draw-command cache, dynamic instancing, and object-index allocation.
- `Runtime/Engine/Rendering/SceneSpatialIndex.*` - static/dynamic spatial candidate generation, last-good index plus dirty overlay, staged rebuilds, and dynamic-heavy query telemetry.
- `Runtime/Engine/Rendering/SceneVisibilityPipeline.*` - view-local visibility, LOD/HLOD representation selection, conservative occlusion decisions, sparse visible command eligibility, and cull reasons.
- `Runtime/Engine/Rendering/SceneStreamingResidency.*` - modeled dependency closure, residency tickets, budgeted request/commit/evict, and in-flight resource pinning. Current validation covers state-machine and budget contracts; real loader/uploader backend integration is a later slice.
- `Runtime/Rendering/FrameGraph/*` and `Runtime/Rendering/RHI/*` - HZB mip0 resource access for the current shader path, backend capability gates, and non-blocking history updates.

**Industry references**:

| Project | Strategy | Key files/modules |
|---------|----------|-------------------|
| Unreal Engine 4.27 | Retained primitive state, view visibility, HLOD state, HZB/query occlusion, cached mesh draw commands, and world-composition streaming state separation | `Renderer/Private/SceneVisibility.cpp`, `Renderer/Private/SceneOcclusion.cpp`, `Renderer/Private/MeshDrawCommands.cpp`, `Renderer/Public/PrimitiveSceneInfo.h`, `Engine/Private/WorldComposition.cpp` |
| Unity 2018.4 | Scriptable culling setup, dynamic culling job ranges, LOD mask calculation, render-node queues, batching/instancing, and Umbra/static occlusion hooks where available | `Runtime/Graphics/ScriptableRenderLoop/ScriptableCulling.cpp`, `Runtime/Camera/SceneCulling.cpp`, `Runtime/Graphics/LOD/LODGroupManager.cpp`, `Runtime/Camera/RenderNodeQueue.h`, `Runtime/GfxDevice/InstancingBatcher.cpp`, `Runtime/Interfaces/IUmbra.h` |
| D3D12 / modern explicit APIs | Resource state correctness, texture subresource transitions, no CPU/GPU synchronization for ordinary visibility decisions | `ID3D12GraphicsCommandList::ResourceBarrier`, D3D12 resource states, command queues, fence synchronization, readback heap, and copy/readback guidance |

**Key comparison points**:

- **Retained state**: Nullus extends `RenderScene` rather than rebuilding draw inputs from components every frame.
- **Sync scalability**: Nullus currently reports live source-scene sweep cost explicitly and keeps downstream spatial-index maintenance, visibility, and queue finalization dirty/sparse; fully O(changed) source synchronization remains dependent on source dirty events rather than an unreported full sweep.
- **Candidate generation**: Nullus's spatial-first candidate stage is a local measured design choice. UE and Unity justify layered visibility/rendering, but do not prove every path starts from a spatial candidate query.
- **Snapshot safety**: Parallel visibility consumes immutable snapshots and generation-checked handles, not mutable component pointers.
- **Finalization scalability**: Queue finalization must consume sparse visible handles or cached command-offset ranges so O(N) work does not move after culling.
- **Representation**: LOD/HLOD decisions are view-local and transparent/order-dependent children require explicit proxy compatibility.
- **Occlusion**: HZB/history is conservative, uses qualified opaque depth, invalidates on view/depth/primitive/representation/material eligibility changes, and does not block on ordinary readback/fence/map waits.
- **Residency**: Visibility emits interest; dependency closure and residency tickets own request, coalesce, budget, cancel, commit, pin, and evict decisions.

**"Industrial grade" means**:

- Stable-frame source synchronization fallback, downstream spatial-index maintenance, visibility, queue finalization, and streaming commit all publish touched counts and timings, not just final visible/draw counts.
- Candidate and touched counts are bounded for both static partitioned scenes and dynamic-heavy localized scenes.
- Static index rebuilds use last-good data plus dirty overlays, staged rebuilds, or explicit budgets; rebuild fallback spikes are reported.
- Stable handles are generation-checked, scene-scoped, and safe across async snapshots.
- Queue finalization preserves prior draw-call optimization semantics, including transparent order, dynamic instancing, cached-command invalidation, and object-index allocation.
- HZB occlusion has backend capability gates, mip0 texture dependencies matching the current shader, qualified opaque depth source, conservative invalidation, and no ordinary CPU/GPU synchronization.
- Streaming has dependency closure, deduplicated residency tickets, budget exhaustion telemetry, priority aging, cancellation, and frame-retirement pinning.
- Functional sign-off evidence names backend, build, feature gates, fallback reasons, tests, and RenderDoc/RHI captures where required.
- Performance or industry benchmark sign-off evidence additionally names hardware, scene, camera path, and comparable before/after per-stage baseline deltas. Without those numbers, claims must be limited to functional/contract correctness and telemetry readiness.
