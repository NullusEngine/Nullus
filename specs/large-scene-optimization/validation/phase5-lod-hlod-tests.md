# Phase 5 LOD/HLOD Validation

Date: 2026-06-04
Worktree: `D:\VSProject\Nullus\.worktrees\large-scene-optimization`
Build: `build\windows`, Debug

## Scope

Validated User Story 3 tasks T049-T062:

- Read-only `RepresentationResidencySnapshot` for current primitive/proxy readiness.
- Screen-relative LOD selection with bias, hysteresis, and forced LOD.
- HLOD proxy selection, child suppression, proxy readiness fallback, transparent/order-dependent safety, and editor selected-child override.
- Visibility pipeline representation integration before command eligibility, including real `RenderScene::GatherVisibleCommands` submission paths.
- Exact `ScenePrimitiveHandle` membership checks for LOD/HLOD representation records, including stale generation rejection.
- HLOD proxy primitives stay inactive unless the cluster actually selects the proxy, preventing children+proxy double submission.
- HLOD streaming interest propagation through `SceneVisibilityPipelineResult` and `RenderSceneVisibilitySnapshot` without issuing runtime loads.
- View-keyed LOD hysteresis history so Game View, Scene View, and inspection views do not contaminate each other.
- Imported hierarchy HLOD metadata extraction uses a shared schema contract and only emits metadata when an explicit proxy mesh artifact exists.

## Commands And Results

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1
```

Result: passed, `NullusUnitTests.exe` generated. The `/m:1` suffix avoids stale MSBuild/CL tlog lock contention observed after an earlier timed-out parallel build.

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="SceneLODTests.*:SceneHLODTests.*:SceneVisibilityPipelineTests.*:RenderSceneCacheTests.*LOD*:RenderSceneCacheTests.*HLOD*:AssetPrefabPipelineTests.GeneratedModelPrefabExtractsImportedHierarchyHLODMetadata:AssetPrefabPipelineTests.GeneratedModelPrefabDoesNotInferHLODWithoutProxyArtifact:AssetPrefabPipelineTests.GeneratedModelPrefabInstantiationResolvesSubAssetHintsToArtifacts:EditorRenderPathContractTests.*HLOD*"
```

Result: 26 tests from 6 suites passed.

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="SceneSpatialIndexTests.*:SceneVisibilityPipelineTests.*:RenderSceneCacheTests.*:RendererFrameObjectBindingTests.*"
```

Result: 159 tests from 4 suites passed.

Observed existing RHI teardown quarantine log lines in the object-binding regression run, but the process exited 0 and all tests passed. Phase 5 does not touch GPU sync, frame retirement fences, or RHI resource barriers; this evidence is CPU-side representation/visibility validation only.

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetPrefabPipelineTests.GeneratedModelPrefabExtractsImportedHierarchyHLODMetadata:AssetPrefabPipelineTests.GeneratedModelPrefabDoesNotInferHLODWithoutProxyArtifact:AssetPrefabPipelineTests.GeneratedModelPrefabInstantiationResolvesSubAssetHintsToArtifacts"
```

Result: 3 tests from 1 suite passed.

```powershell
git diff --check --ignore-submodules -- Runtime\Engine\Rendering\RenderScene.cpp Runtime\Engine\Rendering\RenderScene.h Runtime\Engine\Rendering\SceneVisibilityPipeline.cpp Runtime\Engine\Rendering\SceneVisibilityPipeline.h Runtime\Engine\Assets\ModelPrefabBuilder.cpp Tests\Unit\SceneVisibilityPipelineTests.cpp Tests\Unit\RenderSceneCacheTests.cpp Tests\Unit\AssetPrefabPipelineTests.cpp
```

Result: passed. Git reported only LF-to-CRLF checkout warnings for existing Windows line-ending behavior.

```powershell
git diff -- Runtime\Engine\Gen Project\Editor\Gen Runtime\Rendering\Gen --stat
```

Result: no generated-file diffs.

## Notes

- Real `RenderScene` integration is covered by `GatherVisibleCommandsAppliesRegisteredLODGroups`, `GatherVisibleCommandsAppliesRegisteredHLODClusters`, `GatherVisibleCommandsKeepsSelectedHLODChildInspectable`, and `SpatialVisibilityWithoutFrustumStillAppliesRegisteredLODGroups`.
- `RegisteredLODHistoryIsIsolatedPerViewKey` covers renderer-owned LOD hysteresis state isolation across independent view keys.
- `HLODProxyPrimitiveIsInactiveUnlessClusterSelectsIt` covers HLOD proxy primitives being excluded from ordinary visibility unless selected by the cluster.
- `HLODMissingProxyInterestPropagatesFromRenderSceneVisibility` covers `RenderSceneVisibilitySnapshot` carrying read-only streaming interest for missing HLOD proxies.
- Spatial visibility expands candidate handles with hash-based de-duplication and passes only candidate-related LOD/HLOD records to `SceneVisibilityPipeline`, avoiding a hidden full representation scan after spatial culling.
- LOD selection history is provided by the renderer-owned representation registry and keyed by `RenderSceneVisibilityOptions::lodHistoryViewKey`.
- Empty snapshots report zero primitive count, and representation membership checks resolve exact handles through the snapshot lookup before reading visibility bits.
- HLOD selection uses explicit proxy readiness from registered proxy handles, emits streaming interest only as data, and does not issue loads, uploads, or evictions.
- HLOD child suppression is view-local through `SceneVisibilityPipelineResult`; source snapshot records remain unchanged.
- Imported HLOD metadata is conservative: direct imported hierarchy groups receive metadata only when `ArtifactManifest` contains an explicit `GeneratedModelPrefabHLODSchema::ProxySubAssetKeyPrefix + <node>` mesh artifact with a non-empty artifact path.
- GPU occlusion, HZB, and streaming budgets remain future phases and are not claimed by this validation.
