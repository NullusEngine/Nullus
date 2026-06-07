# Implementation Plan: Unity Prefab Parity Phase 2

**Branch**: `043-unity-prefab-parity` | **Date**: 2026-06-04 | **Spec**: `specs/043-unity-prefab-parity/spec.md`
**Input**: Feature specification from `specs/043-unity-prefab-parity/spec.md`

## Summary

Close the remaining Unity 2018.4 prefab behavior gaps by treating prefab identity, preview, loading, resource ownership, and editor presentation as one lifecycle. Scenes save connected prefab instances as document-level records with source identity, source/instance correspondence, and modifications. Scene load, Scene View drag preview, final drop, duplication, and repeated instantiation use one `UnifiedPrefabLoadRequest`, one artifact/runtime cache identity, one mesh/material/texture readiness gate, and one ResourceHandle/ResourceLifetime ownership path. Scene View drag creates a private preview-scene prefab instance that follows the mouse and never enters the active scene. Resource lifetime follows explicit owner tokens and `UnloadUnusedAssets`-style zero-owner trimming. Editor UX then surfaces connected, overridden, read-only model-prefab, missing-source, and pending-resource states.

## Technical Context

**Language/Version**: C++17/C++20 project style already used by Nullus  
**Primary Dependencies**: Nullus editor asset browser, Scene View, prefab utility facade, scene object graph serialization, runtime resource managers, ResourceLifetimeRegistry, ResourceHandle, artifact telemetry  
**Storage**: Scene object graph documents, generated/imported prefab artifacts under `Library/Artifacts`, manifest files, runtime resource caches  
**Testing**: `NullusUnitTests` focused gtest filters plus manual editor validation and RenderDoc/equivalent renderer evidence  
**Target Platform**: Windows editor first, currently validated renderer backend first  
**Project Type**: Desktop editor/runtime engine  
**Performance Goals**: Ready textured Scene View preview before release, release-time synchronous work within one 60 Hz frame budget for warm ready assets, hot cache/resource fast bind under 10 ms, scene-load object graph activation not blocked by full renderer-resource restoration  
**Constraints**: Do not hand-edit `Runtime/*/Gen/`; preserve existing legacy scene compatibility; do not claim cross-backend parity without backend-specific evidence; keep preview objects out of active scene persistence and hierarchy  
**Scale/Scope**: Large imported/generated model prefabs with hundreds of renderers and thousands of material references, repeated drag/drop, save/reload, scene switch, delete, and trim workflows

## Constitution Check

- Spec-first major change: PASS. This feature spans `Project/`, `Runtime/`, editor workflow, scene serialization, and resource lifetime, so it has a dedicated spec bundle.
- Validation matches subsystem: PASS. Automated tests cover serialization, editor prefab behavior, resource ownership, and load pipeline contracts; manual editor and renderer evidence are required for Scene View preview and material/texture binding.
- Generated code/backend boundaries: PASS. No generated files are part of the planned edit set; backend-specific validation is scoped to explicitly captured evidence.
- Incremental verified delivery: PASS. Work is split into PrefabInstance model, unified load request, preview/drop lifecycle, resource lifetime, scene streaming, UX, and validation phases.
- Product runtime preservation: PASS. Legacy scene/prefab loading remains readable and migration is additive until new save paths become authoritative.

## Project Structure

### Documentation

```text
specs/043-unity-prefab-parity/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code

```text
Runtime/Engine/Serialize/
├── ObjectGraphDocument.h
├── ObjectGraphReader.h
├── ObjectGraphWriter.h
└── ObjectGraphSerializer.h

Runtime/Engine/SceneSystem/
├── SceneManager.cpp
└── SceneManager.h

Runtime/Core/ResourceManagement/
├── ResourceLifetimeRegistry.h
├── ResourceLifetimeRegistry.cpp
├── ResourceHandle.h
└── ResourceHandle.cpp

Runtime/Rendering/Resources/
├── MeshManager.*
├── MaterialManager.*
└── TextureManager.*

Project/Editor/Assets/
├── PrefabUtilityFacade.cpp
├── PrefabUtilityFacade.h
├── EditorAssetDragDropBridge.cpp
├── EditorAssetDragDropBridge.h
├── EditorAssetDragPayload.cpp
└── EditorAssetDragPayload.h

Project/Editor/Core/
├── EditorActions.cpp
└── EditorActions.h

Project/Editor/Panels/
├── AssetBrowser.cpp
├── SceneView.cpp
├── Hierarchy.cpp
└── InspectorPanel.cpp

Project/Editor/Rendering/
└── DebugSceneRenderer.cpp

Tests/Unit/
├── PrefabUtilityFacadeTests.cpp
├── SceneObjectGraphSerializationTests.cpp
├── EditorAssetDragDropTests.cpp
├── GameObjectAssetImportTests.cpp
├── ResourceLifetimeRegistryTests.cpp
└── RenderSceneCacheTests.cpp
```

**Structure Decision**: Keep document persistence in runtime serialization, editor prefab semantics in `PrefabUtilityFacade`, unified load/cache/readiness in `EditorAssetDragDropBridge` plus resource managers, Scene View preview orchestration in `SceneView`, and editor user presentation in panels. This mirrors Unity's responsibility split without copying Unity internals verbatim.

## Research

See `research.md` for Unity 2018.4 reference anchors and Nullus decisions. Key references:

- Unity preview scenes: `D:/VSProject/Unity2018.4.0f1/Editor/Src/SceneManager/EditorSceneManager.cpp`
- Unity prefab records and modifications: `D:/VSProject/Unity2018.4.0f1/Editor/Src/Prefabs/PrefabInstance.h`, `PropertyModification.h`, `Prefab.cpp`
- Unity serialized prefab scene records: `D:/VSProject/Unity2018.4.0f1/Runtime/Serialize/SerializedFileTests.cpp`
- Unity asset identity and local file id helpers: `D:/VSProject/Unity2018.4.0f1/Editor/Mono/AssetDatabase/AssetDatabase.bindings.cs`
- Unity persistent object/file cache: `D:/VSProject/Unity2018.4.0f1/Runtime/Serialize/PersistentManager.cpp`

## Data Model

See `data-model.md`. The authoritative model is:

- `PrefabSourceIdentity`
- `PrefabInstanceRecord`
- `PrefabObjectCorrespondence`
- `PrefabModification`
- `UnifiedPrefabLoadRequest`
- `PrefabRuntimeCacheEntry`
- `PreviewScenePrefabInstance`
- `ResourceOwnerToken`
- `ResourceHandle<T>`
- `ScenePrefabStreamingJob`
- `PrefabEditorState`

## Phases

### Phase 0 - Baseline And Contracts

- Capture current failures with focused tests and manual telemetry before changing behavior.
- Freeze pass/fail gates for drag preview, drop stall, white model, save/reload, scene-load divergence, and deletion CPU/GPU release.
- Document the existing `unity-ready-model-drop` baseline and which tasks remain manual evidence gates.

### Phase 1 - Unity-Style PrefabInstance Core

- Make document-level prefab-instance records authoritative for new saves.
- Strengthen source/instance correspondence and modification targeting.
- Preserve legacy `scenePrefab` load compatibility.
- Preserve missing-source recovery.
- Support normal prefab scene-local apply/revert and model-prefab read-only apply rejection.

### Phase 2 - Unified Prefab Load Request

- Introduce one shared load request and cache identity used by scene load, preview, final drop, duplicate, and repeated instantiation.
- Make manifest/prefab/mesh/material/texture artifact stamps part of one invalidation contract.
- Ensure scene-loaded and drag-created instances acquire resources through the same ResourceHandle/lifetime path.

### Phase 3 - PreviewScene Mouse-Follow Preview

- Keep private preview-scene object lifecycle separate from active scene lifecycle.
- Begin prefab graph and renderer-resource prewarm on hover/selection/idle before release.
- Render actual mesh/material/texture preview once ready; never present white model as ready.
- Commit exactly one connected instance from the last valid preview placement.

### Phase 4 - Resource Lifetime And UnloadUnusedAssets

- Make scene, preview, inspector, and async job ownership explicit.
- Close raw-pointer gaps in long-lived prefab paths.
- Ensure deletion, scene unload, preview cancellation, and reimport release owners and cancel/detach jobs.
- Trim only zero-owner resources with deterministic LRU/budget behavior.

### Phase 5 - Scene Load Streaming Parity

- Use the same load request and readiness gate during scene load as during drag/drop.
- Activate scene object graph before large renderer-resource streaming completes.
- Reveal each generated/model prefab only when mesh/material/texture are ready together.
- Cancel/detach jobs on scene switch and object deletion.

### Phase 6 - Editor UX Parity

- Add hierarchy/inspector state for connected, overridden, read-only model-prefab, missing source, pending resource, and unpacked states.
- Add apply/revert affordances where supported.
- Surface clear diagnostics for read-only model prefab apply and missing prefab recovery.

### Phase 7 - Verification And Review

- Run focused tests, build `NullusUnitTests`, `git diff --check`.
- Capture manual editor validation for preview/drop/save/load/delete/trim/scene-load.
- Capture RenderDoc or equivalent evidence for textured preview and committed draw bindings.
- Run `/plan-review` before implementation sign-off or commit.

## Validation

Automated validation:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:1
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PrefabUtilityFacadeTests.*:SceneObjectGraphSerializationTests.*Prefab*:EditorAssetDragDropTests.*Prefab*:GameObjectAssetImportTests.*Prefab*:ResourceLifetimeRegistryTests.*:RenderSceneCacheTests.*Prefab*
git diff --check
```

Manual validation:

```text
1. Open a saved scene with normal prefab and imported/model prefab instances; verify connected state and overrides.
2. Drag the same imported/model prefab into Scene View; verify textured mouse-follow preview before release.
3. Release the drag; verify one connected committed instance appears at preview placement without a long stall.
4. Save, close, reopen, and switch scenes; verify prefab instances and overrides survive.
5. Delete prefab instances; verify owner release, cancelled jobs, draw-source removal, and CPU/GPU work drop.
6. Run unused-resource trim; verify shared resources remain alive and zero-owner resources unload deterministically.
7. Break a source prefab artifact; verify missing-prefab recovery state and no silent deletion.
8. Capture renderer evidence showing preview and committed paths bind intended mesh/material/texture resources together.
```

## Complexity Tracking

No constitution violations are expected. The feature is complex because Unity prefab behavior crosses editor UI, scene serialization, asset loading, and resource lifetime, but the work is staged into independently verifiable increments.

## Baseline Evidence Plan

Phase 1 freezes the comparison target before deeper prefab lifecycle edits:

- **Project**: `D:/VSProject/Nullus/TestProject/TestProject.nullus`
- **Scene**: `D:/VSProject/Nullus/TestProject/Assets/Scenes/New Scene.scene`
- **Large imported model sources**:
  - `Assets/Model/main_sponza/NewSponza_Main_Yup_003.fbx`
  - `Assets/Model/main_sponza/NewSponza_Main_Zup_003.fbx`
  - `Assets/Model/pkg_a_curtains/NewSponza_Curtains_FBX_YUp.fbx`
  - `Assets/Model/pkg_a_curtains/NewSponza_Curtains_FBX_ZUp.fbx`
- **Known generated prefab artifact anchor**: `TestProject/Library/Artifacts/e09367e3-98fd-4971-94f8-1e86a10a2b23/manifest.json`, primary sub-asset key `prefab:NewSponza_Curtains_FBX_YUp`.
- **Normal prefab control asset**: `TestProject/Assets/Scenes/Validation Cube.prefab`.

The current manual baseline must record:

- Scene View drag preview state before release: expected current failure is that the preview may still be missing or delayed for some imported/model prefabs.
- Mouse release stall: record whether release starts cold artifact-to-memory work and how long the editor blocks.
- White-model state: record whether any ready path shows mesh without material/texture bindings.
- Scene-load divergence: compare opening `New Scene.scene` with directly dragging the same generated/model prefab.
- Deletion release: record whether CPU/GPU work drops after removing prefab instances and running trim.
- Save/reload: record whether prefab identity and scene-local overrides survive without manually reopening the scene.

Current user-reported baseline captured before Phase 2 implementation:

| Area | Baseline observation | Evidence source | Quantitative follow-up |
|------|----------------------|-----------------|------------------------|
| Scene View drag preview | Dragging model/prefab into Scene View still does not show the real textured model following the mouse; previous attempts showed only a label/crosshair or no model. | User repeated manual checks during `unity-ready-model-drop` follow-up. | Capture first textured preview time with telemetry/manual pass in T070. |
| Mouse release stall | Model/prefab loading is still perceived as happening on release, causing a visible pause before or during placement. | User manual report: "只有在释放的时候才会加载", "会卡一下". | Record release UI-thread work budget result in T070. |
| White-model/no-white invariant | Prior drag/drop path could show white model or no visible model while selected outline still existed; this remains a failure mode to prevent. | User manual reports: "只看到了白模", "松开放置不可见，选中有 outline". | RenderDoc/equivalent binding evidence in T069. |
| Scene-load vs direct drag divergence | Loading a saved scene can show prefab content while dragging the same asset path can fail or behave differently; opening a scene can still wait before models appear. | User manual reports: saved scene visible, asset drag invisible; scene load still slow. | Compare `UnifiedPrefabLoadRequest` cache keys and stage summaries in Phase 2/T070. |
| Deletion/resource release | A removed prefab previously continued consuming CPU and frame rate did not recover. | User manual report: "prefab从场景中移除后还在继续消耗cpu". | Use `CreateDiagnosticSnapshot()` plus runtime trace/telemetry in T070. |
| Save/reload | Prefab scene persistence improved enough that saved scenes can show on explicit scene load, but startup/open-scene behavior and source connection parity still need Unity-style validation. | User manual report: "加载保存的场景能看到" plus earlier startup scene missing until double-click. | Validate PrefabInstance record and editor state in US1/US5. |

Baseline hooks added for this phase:

- `NLS::Core::Assets::SummarizeArtifactLoadTelemetry()` aggregates telemetry by load stage and path so drag, drop, scene load, and repeated warm paths can be compared by clearing telemetry per scenario and then comparing path-bucketed summaries without parsing raw event streams.
- `NLS::Core::ResourceManagement::ResourceLifetimeRegistry::CreateDiagnosticSnapshot()` exposes live resource count, owner count, active leases, zero-owner resources, trim candidates, and byte totals for owner/lifetime comparisons while excluding evicted tombstones from resident zero-owner pressure.

Focused validation completed for these hooks:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetImportPipelineTests.ArtifactLoadTelemetrySummarizesStageBaselineForPrefabPathComparison --gtest_color=no
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ResourceLifetimeRegistryTests.DiagnosticSnapshotReportsBaselineOwnerAndTrimState --gtest_color=no
```

Exact millisecond timings and CPU/GPU deltas are not yet claimed; they remain tracked by T070 after the shared baseline hooks can be sampled from the editor.

## Phase 9 Evidence Captured

Automated checks completed on 2026-06-05:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:1
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PrefabUtilityFacadeTests.*:SceneObjectGraphSerializationTests.*Prefab*:EditorAssetDragDropTests.*Prefab*:GameObjectAssetImportTests.*Prefab*:GameObjectAssetImportTests.GeneratedModelSceneRestoreDefersRuntimeResourcePrewarmUntilStreaming:ResourceLifetimeRegistryTests.*:RenderSceneCacheTests.*Prefab* --gtest_color=no
git diff --check
```

Results:

- `NullusUnitTests` Debug build completed successfully.
- Focused prefab/load/preview/lifetime/streaming suite ran 90 tests across 6 suites and passed.
- `git diff --check` exited 0 with only repository CRLF conversion warnings.
- `GameObjectAssetImportTests.GeneratedModelSceneRestoreDefersRuntimeResourcePrewarmUntilStreaming` was red/green verified: temporarily disabling `deferAssetReferenceResolution` caused synchronous `MeshManager`/`MaterialManager` registration and test failure; restoring defer made it pass.

Scene-load and committed-instance editor validation:

- Debug Editor existing-scene readback command wrote `Build/Validation/043-unity-prefab-parity/scene_existing_readback.txt` and exited 0 in about 31.3 s.
- Existing scene readback summary: `width=2560`, `height=1269`, `nonBlackPixels=3248640`, `averageRgb=160.607`, `readbackStatus=success`.
- Release Editor validation with `--editor-validation-create-asset Assets/Model/pkg_a_curtains/NewSponza_Curtains_FBX_YUp.fbx` wrote `scene_create_readback.txt`.
- Created instance readback summary: `width=1656`, `height=823`, `nonBlackPixels=1362888`, `averageRgb=29.0081`, `readbackStatus=success`.
- Release validation log `TestProject/Logs/2026-06-05_02-13-01.log` records `Editor validation created asset instance ... root=RootNode`, scene-load resource queue `tasks=810`, created prefab queue `tasks=86`, and a later renderer snapshot with `recordedDraws=347`, `queuedGBufferDraws=346`, `visibleOpaqueDraws=346`, `sceneOpaqueDrawables=346`.

Renderer evidence:

- RenderDoc runner succeeded after passing the explicit project path:

```powershell
py -3 Tools\RenderDoc\renderdoc_runner.py --target editor --backend dx12 --config Release --project TestProject\TestProject.nullus --capture --capture-after-frames 240 --terminate-after-capture --timeout 180 --app-arg=--editor-validation-exclusive-view --app-arg=scene --app-arg=--editor-validation-focus-view --app-arg=scene --app-arg=--editor-validation-open-frame-info --app-arg=--editor-validation-create-asset --app-arg=Assets/Model/pkg_a_curtains/NewSponza_Curtains_FBX_YUp.fbx --app-arg=--editor-log-render-draw-path
```

- Capture: `Build/RenderDocCaptures/editor/dx12/editor_dx12_DX12_capture.rdc`, D3D12, 46 events, 39 indexed draws.
- `py -3 Tools\RenderDoc\rdc_analyze.py Build\RenderDocCaptures\editor\dx12\editor_dx12_DX12_capture.rdc` reports a focus draw with `TriangleList`, PS binding count 1, sample binding `texture0`.
- Runtime draw-path logs in `TestProject/Logs/2026-06-05_02-13-01.log` show deferred GBuffer draws with `materialBindingSet=1`, `mesh=1`, `vertexBuffer=1`, and `indexBuffer=1`, including large imported-model draws such as `vertices=302693` and `vertices=432836`.

Known verification limits:

- The Release validation command wrote readback evidence but did not always terminate promptly through the shell wrapper; the process was terminated manually after evidence was written. This appears to be a validation harness/window-close issue rather than a renderer failure.
- The captured RenderDoc frame plus draw-path logs prove committed-path backend resource binding for the validated DX12 run. Preview-scene behavior is covered by focused automated tests (`EditorAssetDragDropTests.*Prefab*` and `RenderSceneCacheTests.SceneRendererDrawsExistingAndPreviewPrefabInstancesSharingAssetReferences`), but a manual real-mouse drag RenderDoc capture remains recommended before broad sign-off.
- Exact first-textured-preview latency and CPU/GPU drop-after-delete measurements were not captured in this pass; they remain follow-up performance evidence rather than correctness evidence.

P1 remediation added after review on 2026-06-05:

- `EditorAssetDragDropTests.ImportedPrefabDragPreviewReleaseDoesNotLeaveStaleCommitPlacement` red/green verifies preview cancellation or replacement clears stale hover placement before any later commit handoff.
- `PrefabUtilityFacadeTests.PrefabEditorStateDisablesApplyWithoutEditableSourceArtifactContext` red/green verifies Inspector-style copied source graphs do not expose a clickable Apply action without an editable source artifact writeback context.
- `GameObjectAssetImportTests.GraphOnlySceneRestoreKeySkipsRendererArtifactFileStamps` red/green verifies graph-only scene restore keys skip synchronous renderer artifact file stamps while final drop keys still invalidate on renderer artifact changes.
- `EditorAssetDragDropTests.ImportedPrefabDragPreviewCachesRendererPathsAfterCreation` verifies preview-session renderer dependency paths are cached after preview creation so per-frame prewarm no longer recursively scans the full preview tree.
- `PrefabEditorWorkflowTests.InspectorPrefabCommandSurfaceRequiresEditableSourceContextForApply` red/green verifies Inspector command descriptors no longer advertise Apply as executable unless an editable source artifact context is present.
- `PrefabEditorWorkflowTests.RegistryTracksConnectedPrefabPresentationStates` now verifies unpacked prefab instances surface an explicit Hierarchy presentation state and color token.
- `EditorAssetDragDropTests.PrefabDeletionCleanupReleasesNestedPrefabRootsUnderDestroyedParent` red/green verifies deleting a parent object releases nested prefab root owners and removes their registry records.
- `ResourceLifetimeRegistryTests.EvictionReservationCanBeCancelledByNewOwnerAcquire` red/green verifies a preview or scene owner can cancel a pending eviction before binding a cached resource.
- `ResourceLifetimeRegistryTests.ManagerTrimSkipsRegisteredRawResourcesUntilHandleOwnershipIsClosed` red/green verifies manager trim does not destroy directly registered raw resources until component/resource ownership is closed through `ResourceHandle`.
- Scene View preview cached mesh/material/texture hits now acquire the preview resource owner before binding cached pointers, so trim sees preview ownership even when no async request was needed.
- Automated verification rerun after these fixes: Debug `NullusUnitTests` build passed; focused prefab/preview/lifetime/scene-streaming suite ran 97 tests across 7 suites and passed; `git diff --check` exited 0 with only CRLF conversion warnings.

Remaining review-driven implementation risks not yet closed:

- Large-prefab hot cache still returns value-style `PrefabArtifact` results in public load APIs, so repeated warm loads may still pay graph copy cost even when disk parsing is avoided.
- First Scene View drag of a cold large prefab can still perform synchronous graph load and readiness key construction before a preview scene exists.
- Scene open still restores prefab graphs synchronously before renderer-resource streaming; only renderer binding is currently sliced across frames.
- Hot-cache retained-byte accounting now includes prefab artifact payload bytes, but still uses estimated deep graph/string capacity rather than exact allocator-retained memory.

Evidence still required before checking T068-T070 again:

- A real Scene View mouse-drag manual pass proving textured model follows the cursor before release.
- RenderDoc or equivalent capture for the live preview path, not only the committed Scene View path.
- Measured first-textured-preview latency, release stall budget, and delete/trim CPU-GPU drop evidence.
