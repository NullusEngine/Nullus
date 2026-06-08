# Phase 3 Focused Test Evidence

```text
Report: US1 observability focused regression tests
Date: 2026-06-04
Commit: c5c6acac
Branch: large-scene-optimization
Machine: local developer workstation
OS: Windows
Backend: None for unit-test renderer setup
Build configuration: Debug
Scene fixture: synthetic unit-test scenes
Camera path: deterministic unit-test camera inputs
Feature gates: large-scene observability and retained RenderScene fallback path
Validation command: .\build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=RenderSceneCacheTests.PrimitiveSnapshotKeepsImmutableHandleAndCommandOffsetState:PanelWindowHookTests.FrameInfoPanelFormatsLargeSceneTelemetrySnapshot:RenderSceneCacheTests.*LargeScene*:RendererStatsTests.*LargeScene*:ReflectionRuntimeTestFixture.RegistersEngineReflectionTypes:ReflectionRuntimeTestFixture.RegistersSpecialCasePropertyBindingsWithExpectedTypes:MetaParserGenerationEngineTests.GeneratesExpectedEngineReflectionBindings
Result: 16 tests from 5 test suites passed, 0 failed
```

## Fresh Build Evidence

```text
Report: US1 observability Debug rebuild
Date: 2026-06-04
Commit: c5c6acac
Branch: large-scene-optimization
Build command: cmake --build build/windows --target NullusUnitTests --config Debug
Result: build succeeded; MetaParser regenerated local NLS_Render and NLS_Engine bindings; no Runtime/*/Gen files are present in git diff
```

```text
Report: US1 snapshot identity follow-up rebuild
Date: 2026-06-04
Commit: c5c6acac
Branch: large-scene-optimization
Build command: cmake --build build/windows --target NullusUnitTests --config Debug
Result: build succeeded after removing duplicate snapshot identity storage
```

## Full Suite Evidence

```text
Report: US1 observability full unit suite
Date: 2026-06-04
Commit: c5c6acac
Branch: large-scene-optimization
Validation command: .\build\windows\bin\Debug\NullusUnitTests.exe
Result: 2243 tests from 131 test suites ran; 2231 passed; 12 skipped; 0 failed
```

## Covered Regression Areas

- Large-scene telemetry exposes sync touched, sweep slot, dirty bounds, slot reuse, tombstone, visibility, queue-finalization, draw, residency, and timing counters.
- Phase 3 reports retained full-scan candidates through `fullScanCandidateCount`; `spatialCandidateCount` remains reserved for US2 spatial-index candidates.
- Phase 3 reports baseline retained primitives as `unclassifiedPrimitiveCount` rather than pretending static/dynamic classification is implemented.
- Queue-finalization telemetry counts command slots touched during finalization separately from visible raw draws, and command-offset table preparation is no longer confused with cached draw-command rebuilds.
- `ScenePrimitiveSnapshotRecord` keeps `ScenePrimitiveHandle handle` as the only primitive identity field; derived dense/offset tables remain separate lookup structures.
- Stable primitive snapshots keep immutable handle and command-offset state, and stable frames do not report all reused primitives as dirty sync handles.
- `BaseSceneRenderer::ParseScene` publishes and aggregates large-scene telemetry into FrameInfo across additive retained scenes.
- `RendererStats` aggregates large-scene counters per frame and resets them on the next frame.
- FrameInfo formatting includes primitive, visibility, sync, finalization, residency, and timing snapshots without querying live scene state.
- Camera layer-mask reflection and MetaParser generation remain covered by focused reflection tests.

## Notes

- This evidence signs off the US1 observability/baseline execution phase only.
- It does not claim US2 spatial indexing, sparse finalization, LOD/HLOD, HZB occlusion, or streaming-budget behavior.
- The test run emitted expected `None` backend warnings from the editor unit-test setup; no runtime RHI device was created for this CPU-side verification.
