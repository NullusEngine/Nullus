# Implementation Plan: Large Model Scene Performance

**Branch**: `028-large-model-scene-performance` | **Date**: 2026-05-18 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/028-large-model-scene-performance/spec.md`

## Summary

Imported models were entering the scene with culling disabled and with per-frame mesh RHI adapter churn. The fix aligns generated model prefabs, editor deferred mesh binding, editor viewport cameras, and default scene cameras around normal frustum culling, makes mesh RHI adapter access stable, removes node-allocated `std::multimap` insertion from the per-frame scene drawable collection path, keeps default material fallback lookup non-loading inside `ParseScene()`, removes avoidable work from the editor UI draw path for large hierarchies, makes generated-model resource resolution cache-only on the UI frame, and keeps Scene View from draining threaded rendering every ordinary presentation frame. The next architecture step formally introduces UE-style persistent `RenderScene` / `RenderPrimitive` / `CachedDrawCommand` state, then stages bitset/parallel visibility, per-frame object buffers, command sort/merge, and dynamic instancing on top. The model-scene importer version is bumped so stale cached generated prefabs refresh.

## Technical Context

**Language/Version**: C++17 project code  
**Primary Dependencies**: Nullus editor/runtime/rendering components, GoogleTest-based unit tests  
**Storage**: `Library/ArtifactDB` and `Library/Artifacts` cache manifests for imported assets  
**Testing**: `NullusUnitTests` focused test filters plus Release build  
**Target Platform**: Windows editor/runtime development build  
**Project Type**: Desktop editor and engine runtime  
**Performance Goals**: Large imported model scenes remain interactive by avoiding unnecessary visible draw work and hot-path allocations  
**Constraints**: Do not hand-edit generated files under `Runtime/*/Gen/`; preserve existing asset cache semantics except for intentional model-scene importer invalidation  
**Scale/Scope**: Imported FBX/glTF/OBJ model prefabs and editor scene rendering paths

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first scope**: This change touches `Runtime/`, `Project/Editor/`, rendering resources, and tests, so a spec bundle is required.
- **Validation matched to subsystem**: Use targeted editor/render/resource unit tests and build validation. No cross-backend rendering correctness claim is made.
- **Generated code boundary**: No files under `Runtime/*/Gen/` are edited by hand.
- **Incremental delivery**: The fix is split into culling defaults, RHI adapter reuse, cache invalidation, and focused tests.
- **Product runtime preservation**: Editor and Game default cameras keep normal rendering behavior with geometry culling enabled.

## Project Structure

### Documentation

```text
specs/028-large-model-scene-performance/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Project/Editor/Core/EditorActions.cpp
Project/Editor/Panels/AViewControllable.cpp
Runtime/Core/Assets/AssetMeta.cpp
Runtime/Engine/Assets/ModelPrefabBuilder.cpp
Runtime/Engine/SceneSystem/SceneManager.cpp
Runtime/Engine/Rendering/BaseSceneRenderer.h
Runtime/Engine/Rendering/BaseSceneRenderer.cpp
Runtime/Engine/Rendering/DeferredSceneRenderer.cpp
Runtime/Engine/Rendering/ForwardSceneRenderer.cpp
Runtime/Engine/Rendering/RenderScene.h
Runtime/Engine/Rendering/RenderScene.cpp
Runtime/Rendering/Buffers/IndexBuffer.h
Runtime/Rendering/Buffers/IndexBuffer.cpp
Runtime/Rendering/Buffers/VertexBuffer.h
Runtime/Rendering/Buffers/VertexBuffer.inl
Runtime/Rendering/Resources/Mesh.h
Runtime/Rendering/Resources/Mesh.cpp
Runtime/UI/ImGuiExtensions/TimelineProfiler/ProfilerTraceCursor.h
Runtime/UI/ImGuiExtensions/TimelineProfiler/ProfilerWindow.cpp
Runtime/UI/Profiling/TimelineProfilerLimits.h
Runtime/UI/Profiling/TimelineProfilerSink.cpp
Runtime/UI/Internal/WidgetContainer.h
Runtime/UI/Internal/WidgetContainer.cpp
Runtime/UI/Panels/APanel.h
Runtime/UI/Panels/APanel.cpp
Runtime/UI/Panels/PanelWindow.h
Runtime/UI/Panels/PanelWindow.cpp
Runtime/UI/Widgets/AWidget.h
Runtime/UI/Widgets/AWidget.cpp
Runtime/UI/Widgets/Layout/TreeNode.h
Runtime/UI/Widgets/Layout/TreeNode.cpp
Tests/Unit/AssetImportPipelineTests.cpp
Tests/Unit/AssetPrefabPipelineTests.cpp
Tests/Unit/EditorRenderPathContractTests.cpp
Tests/Unit/RenderFrameworkContractTests.cpp
Tests/Unit/DebugSceneLifecycleTests.cpp
Tests/Unit/MeshBoundingSphereTests.cpp
Tests/Unit/PanelWindowHookTests.cpp
Tests/Unit/ProfilerDestinationTests.cpp
Tests/Unit/RenderSceneCacheTests.cpp
```

**Structure Decision**: Keep changes in the existing editor/runtime/resource modules that own the affected behavior. The retained-scene foundation lives beside `BaseSceneRenderer` because it bridges engine scene components to renderer-visible command queues before later RHI-level command submission work. Tests live in the existing unit test target and assert the public contracts around import, culling, mesh resource stability, retained primitive lifetime, and cached command invalidation.

## Research

### Decision: Imported Mesh Renderers Use Model Culling

**Rationale**: Imported model nodes should participate in the same visibility system as ordinary mesh renderers. Disabling frustum behavior makes every imported mesh eligible for rendering work even when outside the view, which scales poorly for large scenes.

**Alternatives considered**: Keeping culling disabled during deferred asset resolution was rejected because the fallback visibility convenience becomes a persistent performance hazard once real mesh bounds are available.

### Decision: Editor Cameras Enable Geometry Frustum Culling By Default

**Rationale**: Unity-like editor interaction depends on keeping off-camera geometry out of per-frame render preparation. Editor view cameras and default scene cameras should opt into geometry culling unless a specific tool overrides it.

**Alternatives considered**: Leaving camera culling opt-in was rejected because newly created scenes and editor viewports are the common path for model inspection.

### Decision: Cache The Mesh RHI Adapter Per Mesh

**Rationale**: Draw preparation asks each visible mesh for an RHI mesh repeatedly. Returning a newly allocated adapter on every call creates avoidable allocation and shared pointer churn in the hot path.

**Alternatives considered**: Pooling adapters globally was rejected as more complex than storing the stable adapter on the owning mesh.

### Decision: Bump Model Scene Importer Version

**Rationale**: Cached generated prefabs can preserve the old disabled-culling setting. Changing the importer version forces stale manifests through the existing cache invalidation path.

**Alternatives considered**: Runtime patching old prefabs was rejected because it would hide stale artifact semantics and add special cases to scene instantiation.

### Decision: Collect Scene Drawables In Contiguous Arrays

**Rationale**: `ParseScene()` is on the per-frame scene render preparation path. Node-allocated `std::multimap` insertion performs one allocation per drawable and pays tree insertion cost while the final renderer only needs ordered iteration. Collecting into `std::vector<std::pair<float, Drawable>>` and sorting once preserves current order semantics while reducing allocation churn for large imported models.

**Alternatives considered**: A full UE-style cached mesh draw command scene was rejected for this increment because it requires broader invalidation, lifetime, and shader binding architecture. Opaque material/PSO bucket sorting is also deferred because it changes draw ordering semantics and needs separate validation.

### Decision: Keep Default Material Fallback Non-Loading In ParseScene

**Rationale**: Missing material bindings are common while imported model resources are still resolving. `ParseScene()` must not trigger synchronous material file IO or shader/texture resolution while preparing a frame, so fallback material lookup uses `GetResource(path, false)` and skips the draw if the fallback has not already been prewarmed.

**Alternatives considered**: Loading `:Materials\Default.mat` on demand in `ParseScene()` was rejected because it can create a first-visible-frame stall. Prewarming remains the responsibility of startup/import resolution paths.

### Decision: Attribute UI Draw Cost With Cached Panel Scopes

**Rationale**: The current trace groups most editor cost under generic `Canvas::Draw` and `APanel::DrawPanel` scopes, which makes the next optimization target ambiguous. Panel scopes are built once per name/ID change and reused so profiling attribution does not itself add per-frame string work.

**Alternatives considered**: Adding ad-hoc profiling around only the Hierarchy panel was rejected because the trace also shows other editor UI costs; per-panel naming provides reusable evidence for future panel-specific optimization.

### Decision: Cache TreeNode ImGui Labels

**Rationale**: Large model scenes create thousands of hierarchy nodes. Building `name + "###" + m_widgetID` during every `TreeNode::_Draw_Impl()` performs avoidable allocation/copy work on an already hot UI path. Caching the label until the name or widget ID changes preserves ImGui identity semantics while reducing repeated string work.

**Alternatives considered**: Replacing the entire Hierarchy with a virtualized tree is the likely larger win, but it changes selection, drag/drop, and expansion behavior. Label caching is a narrower first step with low behavioral risk.

### Decision: Dirty-Gate WidgetContainer Garbage Collection

**Rationale**: `DrawWidgets()` calls `CollectGarbages()` for every container, and every `TreeNode` is also a `WidgetContainer`. Scanning child vectors on every clean frame multiplies with hierarchy size. A container-local dirty flag keeps normal draw O(1) for garbage collection and scans only after a child calls `Destroy()`.

**Alternatives considered**: Removing the automatic collection calls was rejected because it would change widget lifetime semantics. A global UI garbage queue is deferred because parent-local dirty flags fit the existing ownership model with less coupling.

### Decision: Keep Generated-Model Material Resolution Cache-Only On The UI Frame

**Rationale**: The updated Sponza trace and log showed the editor frame dominated by `RenderEditorUI` / `UIManager::DrawCanvas`, while generated-model renderer resource resolution also spent many frames on material slots that were not resident. The resource-resolution step now applies material path hints and visible fallback material, probes `MaterialManager::GetResource(path, false)`, records unresolved slots, and continues scanning a bounded slot batch without calling `PrewarmArtifact()` or material-manager loading from the editor frame.

**Alternatives considered**: Synchronously prewarming each cold material during deferred binding was rejected because it converts cache misses into repeated frame stalls. Fully correct async material and texture streaming is the long-term fix, but it requires dependency tracking and retry/rebind semantics beyond this CPU-stall mitigation.

### Decision: Treat Invalid Cached Materials As Unresolved During Binding

**Rationale**: Cache-only material probing can return a material object whose shader dependency was not resident during prewarm. Binding that object would replace the visible fallback with an invalid material and defer the failure into renderer draw preparation. `BindDeferredMaterialPaths()` now checks both null and `!IsValid()` before binding, keeps the fallback slot intact, and records the slot as unresolved for a later cache-resident retry.

**Alternatives considered**: Registering invalid prewarmed materials was rejected because it poisons later cache-only probes with objects that cannot render. Forcing shader loading during the bind step was rejected because that is the UI-frame stall this phase is removing.

### Decision: Make Material Artifact Prewarm Dependency-Cache-Only

**Rationale**: The updated trace pointed at `ShaderLoader` during material/artifact flows. A material cache probe that exists only to keep renderer resolution non-blocking must not turn a cold shader reference into synchronous shader loading or compilation. `MaterialLoader::LoadOptions` now separates missing texture and missing shader loading, normal `Create()` keeps legacy dependency loading, and `MaterialManager::PrewarmArtifact()` calls `CreateResource(path, {false, false})` so prewarm only registers material objects whose dependencies are already resident. Invalid prewarm results are destroyed immediately and are not registered.

**Alternatives considered**: A single `loadMissingTextures=false` option was rejected because shader resolution remained a hidden synchronous dependency path. Forcing all material loading to cache-only was rejected because ordinary user-requested material loads still expect dependencies to resolve.

### Decision: Index Prefab Asset References And Rebuild Live Object Lookup Per Step

**Rationale**: Large generated prefabs can contain thousands of mesh/material references and resolved sub-assets. `ResolvePrefabAssetPath()` previously scanned `prefab.resolvedAssets` for each reference, and delayed binding could linearly scan source-object mappings per task. The collector now builds `PrefabResolvedAssetIndex` once per prefab artifact, while each delayed `RunRendererResourceResolutionStep()` rebuilds a live source-object index once from the currently validated prefab instance. That keeps task processing O(1) per lookup without storing long-lived `GameObject*` maps across delayed frames.

**Alternatives considered**: Caching the live object map in `RendererResourceResolutionState` was rejected because scene objects can be deleted or re-instantiated while delayed resource-resolution tasks remain queued; rebuilding once per step is O(n) per frame instead of O(n) per task and avoids stale pointer state.

### Decision: Complete Async Drag/Drop Imports On Worker Exceptions

**Rationale**: The bounded worker catches task exceptions only for logging, which is too late for async drag/drop because the completion callback lives inside the scheduled task. The import lambda now catches import exceptions before the worker boundary and invokes completion with `pendingImport=false`, `importSucceeded=false`, rejected status, and `dragdrop-background-import-failed` diagnostics. The background lambda no longer captures an external `ImportProgressTracker*`; this prevents a thrown import path or stale progress object from leaving the editor waiting on a pending import forever.

**Alternatives considered**: Relying on the generic worker catch was rejected because it has no knowledge of the captured completion callback or drag/drop result contract.

### Decision: Keep The Profiler Timeline Visible While Recording Trace

**Rationale**: Trace export still advances with `LastExportedFrame`, but `BeginTrace()` seeds the cursor to the latest completed profiler frame instead of frame 0 so clicking Begin does not backfill the whole timeline history. Recording no longer pauses the profiler timeline UI, and the Begin/End controls stay on the original inline button path. `ProfilerPanel::DrawTimeline` keeps a dedicated scope so any profiler self-cost remains visible instead of hidden.

**Alternatives considered**: Pausing the timeline during recording would reduce profiler self-cost, but it removes the live timeline the user wants to see while capturing. Disabling all profiler panel UI while recording was rejected because the trace controls and timeline should both remain accessible.

### Decision: Do Not Drain Scene View Threaded Rendering For Ordinary Presentation

**Rationale**: The 2026-05-19 trace shows the largest recurring main-thread stall under `Panel::Draw:Scene View` is `AView::DrainThreadedRendering`, commonly blocking 18-20 ms. The drain was caused by Scene View defaulting to synchronized retired-frame presentation even though immediate picking readback is disabled by default. Scene View should publish new frames and present the latest retired texture asynchronously during normal camera motion, gizmo interaction, and pending delayed picking, while retaining explicit drains for resize-safe resource lifetime and immediate readback/validation paths.

**Alternatives considered**: Keeping strict per-frame presentation synchronization was rejected because it trades away editor interactivity for a one-frame-freshness guarantee that is not required for ordinary inspection. Disabling retired-frame consumption entirely was rejected because Scene View still needs safe texture lifetime and readback behavior for resize and validation flows.

### Decision: Build Editor Generated-Model Fallback From Loaded Editor Shader State

**Rationale**: The editor drag/drop fallback path must keep imported geometry visible while material artifacts stream or miss cache, but it must not load `:Materials\Default.mat` on the UI frame. The fallback material is constructed from the already startup-loaded `DebugLitColor` shader via `EditorResources::GetLoadedShader()`.

**Alternatives considered**: Reusing `MaterialManager::operator[](":Materials\\Default.mat")` was rejected because the operator can synchronously load the default material and its dependencies during resource resolution.

### Decision: Keep Cold Mesh Artifact IO Background-Only And Bind RHI Resources On The Editor/RHI Path

**Rationale**: The current DX12 `GPUOnly` initial data path performs an upload-context copy and waits on a fence before returning. For editor-created generated-model mesh artifacts bound from drag/drop resource resolution, the background task now resolves the artifact path, loads `.nmesh` CPU data, and uses the artifact's precomputed bounding sphere so the editor/RHI bind step does not scan all vertices again. `RunRendererResourceResolutionStep()` also shares duplicate pending loads by model path. When a pending load completes, `BindDeferredMeshPath()` only probes `ModelManager::GetResource(path, false)` so an already resident GPU-local model can be reused without turning the editor frame back into synchronous package IO/RHI construction. Cold paths create one shared transient `CPUToGPU` fallback model per pending load state and do not register it in the global `ModelManager`. Cold binds remain limited to one mesh task per frame so large imports do not dump all RHI creation into one UI tick.

**Alternatives considered**: Calling `ModelManager::operator[]` during the bind step was rejected because a `.nmesh` path can synchronously load the owning model package and build GPU-only resources for many meshes on the UI/editor frame. Constructing `Mesh`/`Model` in a generic editor background thread was rejected because `Mesh` immediately creates RHI vertex/index buffers through the global driver/device and Nullus does not define cross-backend thread-safe resource creation. A dedicated async GPU-only upload queue with completion tokens remains the preferred renderer architecture because it keeps long-lived mesh buffers in GPU-local memory without blocking the editor frame. That is deferred because it touches broader RHI lifetime and synchronization contracts.

### Decision: Treat The Main Generated-Model Package Probe As Optional Warmup

**Rationale**: Generated model prefabs can reference both the owning `.nmodel` package and per-mesh `.nmesh` artifacts. The `.nmodel` package probe exists only to reuse an already-warm package alias cache; a cold miss must not fail the delayed renderer-resource job before the individual mesh artifact tasks can load CPU data and bind transient fallback models. `ProbeMainModelPackageCache()` therefore remains cache-only and records a warmed package when present, but always completes the warmup task so mesh and material tasks continue.

**Alternatives considered**: Retrying or failing the whole job until `ModelManager::GetResource(package, false)` succeeds was rejected because it makes a cache miss look like an import failure and leaves freshly dropped models invisible. Calling a loading `ModelManager` path was rejected because it reintroduces the synchronous package IO/RHI construction stalls this phase removed.

### Decision: Keep `.nmesh` Header And Bounds Field-Wise, With Native Vertex Payloads

**Rationale**: The `.nmesh` cache needs stable metadata so editor resource resolution can reuse precomputed bounds without re-scanning vertices. Version 2 writes the header and bounding sphere field-by-field in little-endian byte order. The vertex and index payloads remain raw native cache data tied to the current `Geometry::Vertex` layout and importer version; this phase does not promote `.nmesh` into a cross-ABI interchange format.

**Alternatives considered**: Serializing every vertex/index field into a portable stream was rejected for this performance pass because the artifact is an editor cache and changing the full mesh payload schema requires a broader cache/versioning migration. If Nullus later needs cross-platform artifact reuse, that migration should add a vertex-layout hash or fully field-wise payload schema.

### Decision: Use A Bounded Editor Background Worker Queue

**Rationale**: Mesh artifact loading and asset preimport can schedule many short-lived background tasks. Creating one `std::thread` per request risks IO/CPU oversubscription and thread-handle growth across long editor sessions. `TrackBackgroundTask()` now pushes callable work into an editor-owned bounded worker queue, reuses a small fixed set of workers, catches/logs task exceptions, stops launching not-yet-started queue work during editor shutdown, and reports queue rejection back to async drag/drop completion callbacks so pending imports do not hang silently. Async model-drop completion stores only a parent address token and revalidates it by walking the live current scene before instantiation, so a parent deleted while import is pending degrades to root insertion instead of dereferencing a stale `GameObject*`.

**Alternatives considered**: Keeping the API as `TrackBackgroundTask(std::thread)` was rejected because it leaves each caller responsible for launch policy and makes global concurrency impossible. A full engine-wide job system remains a future architecture option.

### Decision: Keep DX12 Upload-Heap Buffers In GenericRead

**Rationale**: DX12 upload heaps are created in `D3D12_RESOURCE_STATE_GENERIC_READ` and readback heaps in `D3D12_RESOURCE_STATE_COPY_DEST`; neither may be transitioned away from those fixed states, and texture resources are not valid on upload/readback heaps. The RHI `GetState()` and `GetDesc().memoryUsage` values are consumed by generic barrier/state-tracker/upload paths, so usage-specific states such as `VertexBuffer` or `ShaderRead`, or a default `GPUOnly` description on a physically upload-heap uniform buffer, would be mistaken for a transitionable resource and could generate illegal upload-heap transitions. Upload-heap buffers now report `ResourceState::GenericRead` and CPU-to-GPU effective memory usage, legal fixed-state CPU-visible barriers are skipped only when both before and after states are legal fixed-state values, illegal CPU-visible buffer barrier/copy requests are rejected/logged instead of silently swallowed, backend upload context rejects CPU-visible upload destinations, CPU-to-GPU storage/UAV buffers are rejected, readback buffers are restricted to `CopyDst`, and DX12 textures reject CPU-visible memory usage.

**Alternatives considered**: Separating public logical usage from backend physical state would be cleaner long-term, but it requires a wider RHI contract change. Keeping a single `GenericRead` state is the narrower safe fix because existing `GetState()` call sites already mean "barrier source state".

### Decision: Add Render-Prep Counters Before The Persistent Scene Rewrite

**Rationale**: The latest 2026-05-19 trace (`App/Win64_Debug_Runtime_Shared/trace.json`, modified 22:57:42, 10.32 MB) shows `AView::DrainThreadedRendering` no longer appears, but `DeferredSceneRenderer::GetOrCreateGBufferMaterial` totals about 25.27 s across 2,789 events, `BaseSceneRenderer::CaptureThreadedPreparedDraw` remains visible, `Panel::Draw:Scene View` averages about 192 ms across 27 draws, and `AView::RendererBeginFrame` averages about 84 ms across 28 calls. FrameInfo now surfaces renderer-owned counters for `ParseScene()`, drawable counts, GBuffer material syncs, binding-set creations, snapshot-buffer creations, and target-panel draw time so the next trace can distinguish visibility work, material sync churn, descriptor churn, and editor panel cost without reading profiler JSON by hand.

**Alternatives considered**: Jumping directly to a persistent `RenderScene` / cached draw-command architecture was rejected for this increment because invalidation, lifetime, object data, material descriptor, and cross-thread ownership contracts are not yet isolated enough. Counters are cheap, testable, and provide the acceptance evidence for the larger rewrite.

### Decision: Sync Deferred GBuffer Materials Only On Source Material Revisions

**Rationale**: `GetOrCreateGBufferMaterial()` was a top trace hotspot. The deferred renderer already caches pass-variant materials, but syncing source parameters into the GBuffer material every visible frame defeats that cache. Each cache entry now stores a source material sync stamp made from stable material instance identity plus parameter and render-state revisions. Unchanged frames for the same source material skip `SyncGBufferMaterial()`, while switching to a different runtime material instance with the same pass variant uses a separate cached GBuffer material instead of reusing stale parameters. `Material::Set<T>()`, shader changes, and render-state setters advance revisions and invalidate explicit binding caches. Public material uniform access is now const-only, and editor material mutation/deletion paths go through revision-aware setters so cached GBuffer materials cannot silently miss in-place source changes.

**Alternatives considered**: Hashing every parameter block per draw was rejected because it moves work into the hot path. Revision-only invalidation was rejected because two runtime material instances can share the same pass variant and revision numbers while holding different parameter values. Pointer-only invalidation was rejected because material instances are edited in place. Rebuilding all GBuffer materials after any material edit was rejected because it scales poorly with large scenes.

### Decision: Introduce Persistent RenderScene Before Parallel Visibility Or Instancing

**Rationale**: UE4.27's retained renderer keeps primitive state and cached mesh draw commands across frames, then visible frames operate on bitsets and visible command queues. Nullus should follow that order. The first retained-scene increment creates `RenderScene`, `RenderPrimitive`, and `CachedDrawCommand` in `Runtime/Engine/Rendering`, synchronizes them from `Scene::FastAccessComponents`, and lets the existing forward/deferred renderers consume an `AllDrawables` queue built from cached command inputs. This avoids a risky whole-renderer rewrite while establishing the ownership and invalidation points required for the later phases.

**Alternatives considered**: Jumping directly to parallel frustum culling or dynamic instancing was rejected because the current renderer still rebuilds drawable command inputs from components every frame; parallelizing or merging that transient work would cement the wrong ownership model. Building a fully event-driven scene proxy layer in the first increment was rejected because scene add/remove/material/transform mutation events are not yet comprehensive. The initial retained scene may scan fast-access components each frame, but persistent primitive and command caches must survive unchanged frames.

### Decision: Invalidate Cached Commands From Stable Input Stamps

**Rationale**: `CachedDrawCommand` rebuilds should be driven by the inputs that affect mesh/material draw submission: model pointer, mesh pointer, material pointer, material instance id, material parameter revision, material render-state revision, primitive mode, material state mask, and renderer override material identity. Transform and user matrix are object data, so they update visible-frame descriptors but do not invalidate command caches.

**Alternatives considered**: Hashing full material parameter values per draw was rejected because it moves work back into the hot path. Pointer-only invalidation was rejected because material instances mutate in place. Treating transforms as command invalidation was rejected because the next object-buffer phase needs command reuse with per-frame object indices.

### Decision: Stage UE-Style Visibility And Command Submission

**Rationale**: After persistent command caches exist, visibility can be converted from per-model vector filtering to a bitset-backed pass over `RenderPrimitive` ranges. Serial bitset visibility lands first, followed by parallel range partitioning once the serial and parallel queues can be compared in tests. Per-frame object buffers then replace per-draw object binding snapshots. Opaque sort/merge and dynamic instancing are later queue finalization phases and must preserve transparent back-to-front semantics.

**Alternatives considered**: Keeping distance-sorted `AllDrawables` as the final architecture was rejected because it cannot express state-bucket sorting, instance merging, or object-index submission cleanly. Introducing instancing before object indices was rejected because per-instance data would still require binding churn or duplicated draw commands.

### UE 4.27 Comparison For Retained Render Prep

UE 4.27's render path keeps persistent render-thread primitive state rather than rebuilding draw submission from Actor/Component state every frame. Relevant local reference points are `FPrimitiveSceneInfo::StaticMeshCommandInfos`, `CacheMeshDrawCommands()`, `RemoveCachedMeshDrawCommands()`, `AddStaticMeshes()`, and `UpdateStaticMeshes()` in `F:/Epic Games/UE_4.27/Engine/Source/Runtime/Renderer/Public/PrimitiveSceneInfo.h`; the `FMeshDrawCommand` comments in `MeshPassProcessor.h` state that mesh draw commands are cached at primitive `AddToScene` time, can use `RHIUpdateUniformBuffer` for per-frame data without changing bindings, and expose dynamic-instancing match/hash helpers; `SceneVisibility.cpp` uses `PrimitiveVisibilityMap` bit arrays plus `ParallelFor` frustum culling; and `MeshDrawCommands.cpp` sorts visible commands, performs dynamic instancing/merge, and submits ranges through `SubmitMeshDrawCommandsRange()`.

This Nullus increment intentionally implements the safe bridge, not the full UE architecture: `ParseScene()` is still per-frame, but its output counts are visible; material pass variants are still local to the deferred renderer, but source sync is revision-gated; binding/snapshot churn is counted; material mutation APIs are tightened so invalidation is reliable. The next architectural phase should introduce persistent `RenderScene` / `RenderPrimitive` / `CachedDrawCommand` objects, per-frame object data buffers with object indices, bitset/parallel visibility, command sort/merge, and later dynamic instancing, LOD/HLOD, occlusion, and spatial indexing.

### UE 4.27 Comparison For This Increment

UE 4.27 treats long editor/runtime asset loads as handle-driven work through `FStreamableManager::RequestAsyncLoad()` and AssetManager APIs that return `FStreamableHandle`; synchronous loads still exist and are explicit stall-prone branches, such as `FStreamableManager::RequestSyncLoad()` and AssetManager paths that select sync loading. Static-mesh render resources and streaming updates are separated from editor UI interaction: reference points include `FStaticMeshLODResources::InitResources()` and `FStaticMeshRenderData::InitResources()` in `Engine/Source/Runtime/Engine/Private/StaticMesh.cpp`, async buffer creation helpers in `Engine/Source/Runtime/Engine/Private/Streaming/StaticMeshUpdate.cpp`, asset streaming in `FStreamableManager::StreamInternal()` / `RequestSyncLoad()` under `Engine/Source/Runtime/Engine/Private/StreamableManager.cpp`, and asset-manager loading paths in `Engine/Source/Runtime/Engine/Private/AssetManager.cpp`. This Nullus increment intentionally does not claim that full architecture: cache-only material probing plus visible fallback avoids UI-frame material IO, background cold mesh artifact work stays CPU-only and deduped, precomputed mesh bounds avoid one UI-frame vertex scan, bounded editor/RHI-owned binds avoid generic worker-thread RHI creation, and completed mesh paths only reuse `ModelManager` entries that are already resident. Any `CPUToGPU` mesh model is an editor cold-bind fallback, not UE-class static mesh residency or streaming architecture. The remaining UE-class work is async material/texture streaming with retry/rebind semantics and an async GPU-only upload queue that returns completion tokens without blocking the editor frame.

## Constitution Check Re-evaluation

- Spec scope remains focused on editor/imported-model performance.
- No generated files are edited.
- Validation is covered by build and focused unit tests.
- No unsupported backend/platform claims are made.

## Complexity Tracking

No constitution violations or additional complexity exceptions.
