# Quickstart: Validate Unity-Aligned Sub-Asset Expansion

## Pre-Change Baseline (2026-07-11)

- Branch: `059-fix-subasset-expansion`; the pre-existing dirty worktree was preserved and excluded from the spec commit.
- Build cache exposes `NLS_ENABLE_TEST_HOOKS`; `NullusUnitTests` and `Editor` are existing CMake targets.
- Baseline command:

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetBrowserPresentationTests.*:EditorAssetDatabaseTests.ReadOnlySnapshotSharesLargeObjectReferenceSnapshots:AssetDatabaseFacadeTests.*" --gtest_brief=1
```

- Baseline result: nonzero with three pre-existing source-contract failures unrelated to 059 behavior:
  - `AssetBrowserPresentationTests.ThumbnailTelemetrySummaryIncludesGlobalArtifactStageTotals`
  - `AssetBrowserPresentationTests.GpuPreviewReadbackWaitsForRetiredReadbacksBeforeStartingAnotherReadback`
  - `AssetBrowserPresentationTests.ThumbnailLatencyTelemetrySplitsGpuPreviewIntoPrepareRecordSubmitAndReadback`
- 059 validation must introduce no additional failure and will use new focused filters before comparing the broader baseline.

## Configure and Build Focused Tests

```powershell
cmake -S . -B Build -G "Visual Studio 17 2022" -A x64 -DNLS_BUILD_TESTS=ON -DNLS_ENABLE_TEST_HOOKS=ON
cmake --build Build --config Debug --target NullusUnitTests -- /m:4 /nr:false /v:minimal
```

## Run TDD Slices

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetBrowserPresentationTests.ExpansionProjection*:EditorAssetDatabaseTests.FacadePublishedState*:EditorAssetDatabaseTests.SnapshotIndexError*" --gtest_brief=1

.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetBrowserPresentationTests.PresentationBundle*:AssetBrowserPresentationTests.PresentationCoordinator*:AssetBrowserPresentationTests.PickerCache*" --gtest_brief=1

.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetBrowserPresentationTests.GeneratedGroupGeometry*:AssetBrowserPresentationTests.GeneratedGroupImGuiRaster*" --gtest_brief=1
```

## Run Broader Regression and Editor Build

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetBrowserPresentationTests.*:EditorAssetDatabaseTests.*:AssetDatabaseFacadeTests.*" --gtest_brief=1
ctest --test-dir Build -C Debug --output-on-failure
cmake --build Build --config Debug --target Editor -- /m:4 /nr:false /v:minimal
```

## Migration Gates

Verify removed legacy APIs and caches have no references:

```powershell
rg -n "GetObjectReferencePickerAssetSnapshots|GetFreshObjectReferencePickerAssetSnapshots|GetObjectReferencePickerAssetSnapshotsView|GetFreshObjectReferencePickerAssetSnapshotsView|ForEachObjectReferencePickerAssetSnapshot|ForEachFreshObjectReferencePickerAssetSnapshot|GetObjectReferencePickerAssetSnapshotsStorageForTesting|BuildObjectReferencePickerEntriesFromSnapshots|BuildProgressiveAssetBrowserDisplayItems|generatedSubAssetCountHints|generatedSubAssetRevealCounts|AdvanceProjectAssetSubAssetRevealCounts|kMaxAssetBrowserGeneratedSubAssetRevealStep|kMaxAssetBrowserInteractiveGeneratedSubAssetRevealStep|m_projectAssetSubAssetRevealCounts|m_projectAssetSubAssetChildCountHints|m_projectAssetSubAssetSnapshotView|m_projectAssetSubAssetSnapshotsBySource|m_projectAssetSubAssetItemsBySource|m_projectAssetSubAssetMaterializeOffsets" Project Tests

rg -n "selectionResourcePath\.(find|rfind)\('#'\)|subAssetDelimiter" Project/Editor/Panels/AssetBrowser.cpp Project/Editor/Assets/AssetBrowserPresentation.cpp Tests/Unit/AssetBrowserPresentationTests.cpp
```

Expected: no production/test references except intentional migration assertions that quote symbol names.

## Manual Editor Verification

1. Open a project folder containing at least two imported model/prefab sources with multiple generated mesh/material/texture children.
2. Expand both sources in grid mode at narrow and wide panel widths. Verify exact counts, correct ownership, continuous row-segment backgrounds, and no joining across a row wrap.
3. Repeat in list mode. Scroll so clipping starts and ends inside a group; verify no background paints outside the content region.
4. Exercise source-only, child-only, both, and neither search matches plus each type filter.
5. Change Browser filters/expansion while presentation work is pending; verify old Browser selection, actions, and thumbnails become non-actionable. Separately refresh/reimport or replace an asset at the same path; verify old project-wide picker entries also become non-actionable until the current facade-state result publishes.
6. Confirm source cards/rows remain outside the child background and hover/selection overlays remain item-local.

Record exact Windows build/test output and manual observations. Do not claim Linux/macOS correctness without corresponding CI/runtime evidence.
