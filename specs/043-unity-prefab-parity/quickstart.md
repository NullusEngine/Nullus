# Quickstart: Unity Prefab Parity Phase 2 Validation

## Automated Checks

Build unit tests:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:1
```

Run focused tests:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PrefabUtilityFacadeTests.*:SceneObjectGraphSerializationTests.*Prefab*:EditorAssetDragDropTests.*Prefab*:GameObjectAssetImportTests.*Prefab*:ResourceLifetimeRegistryTests.*:RenderSceneCacheTests.*Prefab*
```

Run Phase 1 baseline hook checks:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetImportPipelineTests.ArtifactLoadTelemetrySummarizesStageBaselineForPrefabPathComparison:ResourceLifetimeRegistryTests.DiagnosticSnapshotReportsBaselineOwnerAndTrimState --gtest_color=no
```

Check whitespace:

```powershell
git diff --check
```

## Manual Editor Scenario

1. Start the editor with `D:/VSProject/Nullus/TestProject/TestProject.nullus`.
2. Open `D:/VSProject/Nullus/TestProject/Assets/Scenes/New Scene.scene`.
3. Confirm scene object graph activates before all large prefab renderer resources finish streaming.
4. Select each prefab and confirm connected/missing/pending/override state is visible in hierarchy or inspector.
5. Drag `Assets/Model/pkg_a_curtains/NewSponza_Curtains_FBX_YUp.fbx` from the asset browser into Scene View. The generated artifact anchor is `TestProject/Library/Artifacts/e09367e3-98fd-4971-94f8-1e86a10a2b23/manifest.json` with `prefab:NewSponza_Curtains_FBX_YUp`.
6. Confirm a textured preview appears before release and follows mouse placement.
7. Release the mouse and confirm exactly one committed connected instance appears at the last preview placement.
8. Save, close, reopen, and switch scenes.
9. Confirm prefab instances and scene-local overrides survive.
10. Delete one shared prefab instance and confirm the remaining shared instance stays visible.
11. Run unused-resource trim and confirm only zero-owner resources unload.
12. Delete the final owner and confirm CPU/GPU work drops after queues observe cancellation.
13. Use `Assets/Scenes/Validation Cube.prefab` as the normal prefab control case for apply/revert and save/reload behavior.

## Renderer Evidence

Capture or inspect a frame showing:

- Preview path draw uses intended mesh buffers.
- Preview path draw binds intended material and texture resources.
- Committed instance draw uses the same ready resource set.
- No white-model ready draw occurs for the generated/model prefab path.

Preferred evidence is RenderDoc for the validated backend. If RenderDoc cannot capture the environment, record the equivalent backend-specific inspection and explain the limitation.
