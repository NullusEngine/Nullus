# Phase 7 Streaming Residency Core Validation

Date: 2026-06-04
Worktree: `.worktrees/large-scene-optimization`

## Scope

This evidence covers the US5 streaming residency core and frame-retirement pinning slice:

- `Runtime/Engine/Rendering/SceneStreamingResidency.h`
- `Runtime/Engine/Rendering/SceneStreamingResidency.cpp`
- `Runtime/Engine/Rendering/SceneVisibilityPipeline.h`
- `Runtime/Engine/Rendering/SceneVisibilityPipeline.cpp`
- `Tests/Unit/SceneStreamingResidencyTests.cpp`
- `Tests/Unit/SceneVisibilityPipelineTests.cpp`
- `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`
- `Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp`
- `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`
- `Project/Editor/Assets/AssetImporterFacade.h`
- `Project/Editor/Assets/AssetImporterFacade.cpp`
- `Tests/Unit/AssetImporterFacadeTests.cpp`

Covered tasks: T079, T080, T081, T082, T083, T084, T085, T086, T087, T088, and T089.

## TDD Evidence

Streaming residency RED:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1

SceneStreamingResidencyTests.cpp(9,32): error C2039:
"ResidencyTicketState": is not a member of "NLS::Engine::Rendering"

SceneStreamingResidencyTests.cpp(11,32): error C2039:
"SceneStreamingResidency": is not a member of "NLS::Engine::Rendering"

SceneStreamingResidencyTests.cpp(15,32): error C2039:
"StreamingResourceDependency": is not a member of "NLS::Engine::Rendering"
```

Visibility-to-streaming bridge RED:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1

SceneVisibilityPipelineTests.cpp(604,46): error C2039:
"BuildStreamingResidencyInput": is not a member of "NLS::Engine::Rendering::SceneVisibilityPipeline"
```

Frame-retirement pin RED:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1

ThreadedRenderingLifecycleTests.cpp(2456,24): error C2039:
"streamingDependencyPins": is not a member of "NLS::Render::Context::RenderScenePackage"

ThreadedRenderingLifecycleTests.cpp(2461,5): error C2039:
"CollectStreamingDependencyPins": is not a member of "NLS::Render::Context::ThreadedRenderingLifecycle"
```

Editor import budget RED:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nr:false

AssetImporterFacadeTests.cpp(648,12): error C2039:
"SetEditorImportBudget": is not a member of "NLS::Editor::Assets::AssetImporterFacade"

AssetImporterFacadeTests.cpp(648,55): error C2039:
"MakeEditorImportBudget": is not a member of "NLS::Editor::Assets::AssetImporterFacade"

AssetImporterFacadeTests.cpp(650,34): error C2039:
"TryReserveEditorImportBudget": is not a member of "NLS::Editor::Assets::AssetImporterFacade"

AssetImporterFacadeTests.cpp(689,5): error C2065:
"EditorImportBudgetSnapshot": undeclared identifier

AssetImporterFacadeTests.cpp(697,5): error C2065:
"EditorImportBudgetRequest": undeclared identifier
```

GREEN build:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nr:false
Exit code: 0
```

Focused tests after streaming core:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneStreamingResidencyTests.*:SceneVisibilityPipelineTests.StreamingPlanInputUsesFinalVisibilityAndRepresentationInterest

[==========] Running 6 tests from 2 test suites.
[  PASSED  ] 6 tests.
```

Focused tests after frame-retirement pinning:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneStreamingResidencyTests.*:SceneVisibilityPipelineTests.StreamingPlanInputUsesFinalVisibilityAndRepresentationInterest:ThreadedRenderingLifecycleTests.PreparedFrameStreamingDependencyPinsLiveUntilFrameRetirement

[==========] Running 7 tests from 3 test suites.
[  PASSED  ] 7 tests.
```

Focused tests after editor import budget sharing:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneStreamingResidencyTests.*:SceneVisibilityPipelineTests.StreamingPlanInputUsesFinalVisibilityAndRepresentationInterest:ThreadedRenderingLifecycleTests.PreparedFrameStreamingDependencyPinsLiveUntilFrameRetirement:AssetImporterFacadeTests.EditorImportBudgetSharesLargeSceneStreamingLimitsAndTracksReservations:AssetImporterFacadeTests.BudgetedSaveAndReimportRejectsBackgroundImportWithoutQueueingWhenBudgetIsExhausted:AssetImporterFacadeTests.BudgetedSaveAndReimportReleasesReservationWhenImportDoesNotStart:AssetImporterFacadeTests.BudgetedSaveAndReimportRejectsMismatchedBudgetRequestPath:AssetImporterFacadeTests.EditorImportBudgetAdmissionsAreThreadSafeAndDoNotOversubscribe

[==========] Running 12 tests from 4 test suites.
[  PASSED  ] 12 tests.
```

## Covered Requirements

- `StreamingResourceDependency` represents concrete mesh/material/texture/HLOD/cell/placeholder resource dependencies with CPU bytes, GPU bytes, IO bytes, GPU upload bytes, CPU commit cost, child dependency edges, and fallback dependency edges.
- `ResidencyTicket` records deduplicated dependency requests with stable ticket id, dependency id, priority, state, request frame, last-interest frame, pin count, cancel reason, and coalesced request count.
- `SceneStreamingResidency::Plan` expands visible primitive and representation interest into deterministic dependency closure, deduplicates shared dependencies, coalesces duplicate primitive/proxy requests into one ticket, preserves first request frame, applies priority aging, and reports requested CPU/GPU bytes plus dependency/ticket counters.
- `SceneStreamingResidency::Commit` advances tickets through `Requested -> LoadingCpu -> PendingGpuUpload -> Resident/VisibleResident` under CPU, IO, GPU upload, CPU memory, and GPU memory budgets.
- Budget exhaustion leaves requests conservative and reports requested bytes rather than pretending the resource is resident.
- Frame pins block eviction of resident resources.
- Dependency-aware eviction requires a resident fallback dependency before evicting a larger primary resource.
- `SceneVisibilityPipeline::BuildStreamingResidencyInput` converts the final visibility result into streaming plan input using final visible handles plus deduplicated representation streaming interest from LOD/HLOD selection.
- `RenderScenePackage::streamingDependencyPins` carries prepared package resource dependency ids without introducing an Engine dependency into the Render layer.
- `ThreadedRenderingLifecycle::CollectStreamingDependencyPins` exposes deduplicated pins for non-retired in-flight slots only.
- Prepared package pins remain visible while the frame is Published, RenderScenePreparing, RenderReady, and RhiSubmitting, then are released when `RetireFrame` marks the slot retired.
- Slot reuse, stale external-output retirement, snapshot publication, and retained-resource release clear slot-level streaming pins to avoid leaking obsolete residency protection.
- `AssetImporterFacade::MakeEditorImportBudget` maps large-scene streaming CPU, IO, GPU upload, CPU memory, and GPU memory limits into an editor import budget snapshot.
- `AssetImporterFacade::TryReserveEditorImportBudget` admits editor background import work only when all shared budget dimensions have remaining capacity, records reservations, and returns deterministic exhaustion reasons.
- Budgeted `AssetImporterFacade::SaveAndReimport` rejects background import before queueing when the shared budget is exhausted, preserving dirty importer state for a later frame.
- Budgeted `AssetImporterFacade::SaveAndReimport` rolls back its reservation when the import does not start or fails, so failed editor work does not leak shared frame budget.
- Budgeted `AssetImporterFacade::SaveAndReimport` rejects mismatched request/import paths before queueing or reserving budget.
- Editor import budget admission is guarded by a mutex so concurrent background import workers cannot oversubscribe shared CPU, IO, GPU upload, CPU memory, or GPU memory budgets.

## Open Follow-Ups

- Runtime streaming IO/upload execution is still modeled as deterministic state transitions for unit-contract coverage; real loader/uploader integration belongs to later slices.
