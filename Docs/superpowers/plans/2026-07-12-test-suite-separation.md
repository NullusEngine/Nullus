# Test Suite Separation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep deterministic behavior tests in the default `NullusUnitTests` target and move performance, stress, scale, and benchmark scenarios into an independent `NullusPerformanceTests` target that is disabled by default.

**Architecture:** `Tests/CMakeLists.txt` owns shared target configuration and conditionally adds the Unit and Performance subdirectories. Mixed test files are split by individual test intent: behavior cases remain under `Tests/Unit`, while benchmark/report/large-workload cases and their private helpers move under `Tests/Performance`. A shared `Tests/NullusTestMain.cpp` supplies the same runtime setup to both executables.

**Tech Stack:** CMake 3.18+, C++20, GoogleTest, CTest, MSBuild multi-configuration builds.

---

## File Map

- Modify `CMakeLists.txt`: define `NLS_BUILD_PERFORMANCE_TESTS`, derive the default test-hook setting, and include `Tests` when either test family is requested.
- Modify `Tests/CMakeLists.txt`: provide `nls_configure_test_target`, add Unit and Performance conditionally, and preserve the existing GoogleTest setup.
- Create `Tests/NullusTestMain.cpp`: shared GoogleTest entrypoint and Nullus runtime setup.
- Delete `Tests/Unit/TestMain.cpp`: its implementation moves unchanged to the shared entrypoint.
- Modify `Tests/Unit/CMakeLists.txt`: build only Unit and rendering behavior sources through the shared helper.
- Create `Tests/Performance/CMakeLists.txt`: build and register the opt-in performance executable with CTest label `performance`.
- Rename/split `Tests/Unit/AssetPrefabPerformanceTests.cpp` into `Tests/Unit/AssetPrefabBehaviorTests.cpp` and `Tests/Performance/AssetPrefabPerformanceTests.cpp`.
- Rename/split `Tests/Unit/AssetThumbnailPerformanceTests.cpp` into `Tests/Unit/AssetThumbnailBehaviorTests.cpp` and `Tests/Performance/AssetThumbnailPerformanceTests.cpp`.
- Modify `Tests/Unit/AssetImportPipelineTests.cpp` and create `Tests/Performance/AssetImportPerformanceTests.cpp` for the real-asset import benchmark.
- Modify `Tests/Unit/RenderSceneCacheTests.cpp` and create `Tests/Performance/RenderScenePerformanceTests.cpp` for large-scene stress workloads.
- Modify `Docs/Testing.md`: document the fast default suite and opt-in performance workflow.

## Classification Manifest

Move these prefab cases to `Tests/Performance/AssetPrefabPerformanceTests.cpp`:

- `MissingBaselineAndMismatchedScenarioComparisonsAreInvalid`
- `PrefabReportIncludesTopFiveAndComparisonOutput`
- `NewSponzaImportedPrefabPerformanceReport`
- `NewSponzaPreparedPrefabCacheRestartPerformanceReport`
- `LargePrefabScenariosEmitObjectAndComponentScaleCounters`
- `LargePrefabSceneRegistrationDefersFastAccessRebuilds`
- `LargePrefabComponentRestoreUsesCompiledComponentPlan`
- `LargePrefabComponentRestoreReportsReflectionSubstages`
- `LargePrefabComponentRestoreAppliesSimplePropertiesDirectly`
- `LargePrefabTransformRestoreBatchesLocalTransformProperties`
- `LargePrefabSceneRegistrationAvoidsLinearTrackedObjectLookup`
- `LargePrefabWithoutAssetReferencesSkipsExternalBindingScan`
- `LargePrefabSharedMeshAndMaterialReferencesReusePathResolution`
- `LargePrefabGameObjectStateBypassesReflectionFieldLookup`

Keep every other current prefab case as deterministic behavior and rename its suite to `AssetPrefabBehaviorTests`.

Move these thumbnail cases to `Tests/Performance/AssetThumbnailPerformanceTests.cpp`:

- `RapidScrollDuplicateRequestsReportBacklogAndCoalescingPressure`
- `RapidScrollBenchmarkHonorsCacheWriteBudgetPerFrame`
- `ThumbnailReportIncludesTopFiveAndSchedulerCounters`

Keep every other current thumbnail case as deterministic behavior and rename its suite to `AssetThumbnailBehaviorTests`. Budget, queueing, cancellation, retry, readiness, and bounded-work counter assertions remain Unit behavior even when they protect a performance optimization.

Move these clear performance/stress cases found outside the two mixed files:

- `AssetImportPipelineTests.NewSponzaStandalonePngImportPerformanceReport` to `AssetImportPerformanceTests`.
- `RenderSceneCacheTests.StableLargeSceneAvoidsFullPrimitiveSynchronizationAcrossFrames` to `RenderScenePerformanceTests`.
- `RenderSceneCacheTests.DynamicInstancingReducesOneThousandCompatibleOpaqueObjectsToOneSubmittedDraw` to `RenderScenePerformanceTests`.
- `RenderSceneCacheTests.DynamicInstancingReducesTraceScaleCompatibleObjectsToOneSubmittedDraw` to `RenderScenePerformanceTests`.

Keep formatter, telemetry schema, fake-clock budget, cache semantics, and benchmark-command construction tests in Unit because they assert deterministic product behavior without running a benchmark workload. Keep `PerformanceStageStatsTests.cpp` in Unit for the same reason.

### Task 1: Capture The Failing Boundary

**Files:**
- Inspect: `Tests/Unit/CMakeLists.txt`
- Inspect: `Tests/Unit/AssetPrefabPerformanceTests.cpp`
- Inspect: `Tests/Unit/AssetThumbnailPerformanceTests.cpp`

- [ ] **Step 1: Record the current target inventory**

Run:

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
Build\bin\Debug\NullusUnitTests.exe --gtest_list_tests |
    Select-String 'AssetPrefabPerformanceTests|AssetThumbnailPerformanceTests|NewSponzaStandalonePngImportPerformanceReport|DynamicInstancingReducesOneThousand|DynamicInstancingReducesTraceScale'
```

Expected: the command prints performance and stress cases from the default Unit executable, demonstrating the current boundary violation.

- [ ] **Step 2: Confirm the performance target is absent**

Run:

```powershell
cmake --build Build --config Debug --target NullusPerformanceTests
```

Expected: FAIL because the `NullusPerformanceTests` target does not exist yet.

### Task 2: Add Independent Test Target Infrastructure

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `Tests/CMakeLists.txt`
- Create: `Tests/NullusTestMain.cpp`
- Delete: `Tests/Unit/TestMain.cpp`
- Modify: `Tests/Unit/CMakeLists.txt`
- Create: `Tests/Performance/CMakeLists.txt`

- [ ] **Step 1: Add the top-level performance option and test-hook default**

Place the performance option immediately after `NLS_BUILD_TESTS` and derive hooks from either test target:

```cmake
option(NLS_BUILD_TESTS "Build Nullus behavior test targets" OFF)
option(NLS_BUILD_PERFORMANCE_TESTS "Build Nullus performance and stress tests" OFF)
if(NLS_BUILD_TESTS OR NLS_BUILD_PERFORMANCE_TESTS)
    set(_nls_default_enable_test_hooks ON)
else()
    set(_nls_default_enable_test_hooks OFF)
endif()
option(NLS_ENABLE_TEST_HOOKS "Expose test-only runtime hooks for Nullus tests" ${_nls_default_enable_test_hooks})
```

Change the tests subdirectory gate to:

```cmake
if(NLS_BUILD_TESTS OR NLS_BUILD_PERFORMANCE_TESTS)
    add_subdirectory("Tests")
endif()
```

- [ ] **Step 2: Move the shared test main**

Move the current contents of `Tests/Unit/TestMain.cpp` verbatim to `Tests/NullusTestMain.cpp`. Both targets must compile this file; do not duplicate `main` implementations.

- [ ] **Step 3: Add a shared target configuration helper**

Add this function to `Tests/CMakeLists.txt` after the GoogleTest setup:

```cmake
function(nls_configure_test_target target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/Runtime
        ${CMAKE_SOURCE_DIR}/Runtime/Base
        ${CMAKE_SOURCE_DIR}/Runtime/Base/Reflection
        ${CMAKE_SOURCE_DIR}/Runtime/Core
        ${CMAKE_SOURCE_DIR}/Runtime/Engine
        ${CMAKE_SOURCE_DIR}/Runtime/Math
        ${CMAKE_SOURCE_DIR}/Runtime/Rendering
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Project/Editor/Gen
        ${CMAKE_SOURCE_DIR}/Project/Editor
        ${CMAKE_SOURCE_DIR}/Project/Launcher
        ${CMAKE_SOURCE_DIR}/Project/Game
    )
    target_compile_definitions(${target_name} PRIVATE
        NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}"
        NLS_BUILD_DIR="${CMAKE_BINARY_DIR}"
        $<$<BOOL:${NLS_ENABLE_TEST_HOOKS}>:NLS_ENABLE_TEST_HOOKS=1>
        $<$<BOOL:${NLS_ENABLE_TIMELINE_PROFILER}>:NLS_ENABLE_TIMELINE_PROFILER=1>
        WITH_PROFILING=$<AND:$<BOOL:${WIN32}>,$<BOOL:${NLS_ENABLE_TIMELINE_PROFILER}>>
    )
    target_link_libraries(${target_name} PRIVATE
        gtest
        NLS_Base
        NLS_Core
        NLS_Math
        NLS_Render
        NLS_UI
        NLS_Engine
        EditorProject
        GameProject
        LauncherProject
        DirectXTex::DirectXTex
    )
    nls_target_precompile_headers(${target_name} "${CMAKE_SOURCE_DIR}/Tests/NullusTestsPch.h")
    if(APPLE)
        target_link_libraries(${target_name} PRIVATE "-framework CoreServices")
    endif()
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE Comctl32)
    endif()
    if(TARGET EditorProject_MetaGenerated)
        add_dependencies(${target_name} EditorProject_MetaGenerated)
    endif()
    set_target_properties(${target_name} PROPERTIES
        FOLDER Tests
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    nls_copy_shared_runtime_to_target(${target_name})
    nls_copy_editor_resources_to_target(${target_name})
endfunction()
```

Add conditional subdirectories:

```cmake
if(NLS_BUILD_TESTS)
    add_subdirectory(Unit)
endif()
if(NLS_BUILD_PERFORMANCE_TESTS)
    add_subdirectory(Performance)
endif()
```

- [ ] **Step 4: Simplify the Unit target**

Keep the existing Unit and rendering globs and non-Windows DX12 filtering. Add `${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp` to `NullusUnitTests`, retain the `.c` PCH exclusion and source groups, replace duplicated target setup with:

```cmake
nls_configure_test_target(NullusUnitTests)
add_test(NAME NullusUnitTests COMMAND NullusUnitTests)
set_tests_properties(NullusUnitTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LABELS "behavior"
)
```

- [ ] **Step 5: Create the Performance target**

Create `Tests/Performance/CMakeLists.txt` with a recursive source glob, platform filtering, shared main, and the shared helper:

```cmake
file(GLOB_RECURSE NULLUS_PERFORMANCE_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.inl"
)
if(NOT WIN32)
    list(FILTER NULLUS_PERFORMANCE_TEST_FILES EXCLUDE REGEX "[/\\\\]DX12.*\\.cpp$")
endif()
add_executable(NullusPerformanceTests
    ${NULLUS_PERFORMANCE_TEST_FILES}
    "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp"
)
nls_configure_test_target(NullusPerformanceTests)
add_test(NAME NullusPerformanceTests COMMAND NullusPerformanceTests)
set_tests_properties(NullusPerformanceTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LABELS "performance"
)
```

- [ ] **Step 6: Verify target gating**

Run:

```powershell
cmake -S . -B Build-test-boundary -DNLS_BUILD_TESTS=ON -DNLS_BUILD_PERFORMANCE_TESTS=OFF
cmake --build Build-test-boundary --config Debug --target NullusUnitTests -- /m:4
ctest --test-dir Build-test-boundary -C Debug -N
```

Expected: `NullusUnitTests` is listed and `NullusPerformanceTests` is absent.

Run:

```powershell
cmake -S . -B Build-performance-tests -DNLS_BUILD_TESTS=OFF -DNLS_BUILD_PERFORMANCE_TESTS=ON
cmake --build Build-performance-tests --config Debug --target NullusPerformanceTests -- /m:4
ctest --test-dir Build-performance-tests -C Debug -N
```

Expected: `NullusPerformanceTests` is listed with no Unit target required.

### Task 3: Split Prefab Behavior From Performance Workloads

**Files:**
- Rename: `Tests/Unit/AssetPrefabPerformanceTests.cpp` to `Tests/Unit/AssetPrefabBehaviorTests.cpp`
- Create: `Tests/Performance/AssetPrefabPerformanceTests.cpp`

- [ ] **Step 1: Move the 14 manifest-listed performance cases**

Move the exact prefab cases listed in the Classification Manifest and only the helper functions they require into the Performance file. Keep environment-controlled NewSponza helpers, report writers, large-prefab builders, elapsed-time capture, and benchmark comparison helpers private to that file.

- [ ] **Step 2: Rename the remaining suite**

Replace every remaining declaration:

```cpp
TEST(AssetPrefabPerformanceTests, ...)
```

with:

```cpp
TEST(AssetPrefabBehaviorTests, ...)
```

Remove elapsed-time/report calls from behavior cases when the assertion does not consume them. Preserve diagnostic-stage and deterministic counter assertions.

- [ ] **Step 3: Verify the individual classification**

Run:

```powershell
Build-test-boundary\bin\Debug\NullusUnitTests.exe --gtest_list_tests |
    Select-String 'AssetPrefabPerformanceTests|NewSponza|LargePrefab'
Build-performance-tests\bin\Debug\NullusPerformanceTests.exe --gtest_list_tests |
    Select-String 'AssetPrefabPerformanceTests'
```

Expected: Unit contains no prefab performance suite or listed large/NewSponza cases; Performance lists all 14 cases.

- [ ] **Step 4: Run both focused suites**

Run:

```powershell
Build-test-boundary\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabBehaviorTests.*
Build-performance-tests\bin\Debug\NullusPerformanceTests.exe --gtest_filter=AssetPrefabPerformanceTests.*
```

Expected: behavior cases pass; performance cases pass or explicitly skip only when their documented real asset/environment flag is absent.

### Task 4: Split Thumbnail Behavior From Performance Workloads

**Files:**
- Rename: `Tests/Unit/AssetThumbnailPerformanceTests.cpp` to `Tests/Unit/AssetThumbnailBehaviorTests.cpp`
- Create: `Tests/Performance/AssetThumbnailPerformanceTests.cpp`

- [ ] **Step 1: Move the three manifest-listed performance cases**

Move the rapid-scroll workloads and report-format case with their performance-only fixture builders and report helper. Keep small deterministic queue, budget, cache, async resource, cancellation, and retry cases in Unit.

- [ ] **Step 2: Rename the remaining suite**

Replace remaining declarations with:

```cpp
TEST(AssetThumbnailBehaviorTests, ...)
```

Remove report-writing calls from behavior cases while retaining telemetry and counter assertions that define functional contracts.

- [ ] **Step 3: Verify the individual classification**

Run:

```powershell
Build-test-boundary\bin\Debug\NullusUnitTests.exe --gtest_list_tests |
    Select-String 'AssetThumbnailPerformanceTests|RapidScroll|ThumbnailReportIncludesTopFive'
Build-performance-tests\bin\Debug\NullusPerformanceTests.exe --gtest_list_tests |
    Select-String 'AssetThumbnailPerformanceTests'
```

Expected: Unit prints none of the moved cases; Performance lists exactly the three classified cases.

- [ ] **Step 4: Run both focused suites**

Run:

```powershell
Build-test-boundary\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailBehaviorTests.*
Build-performance-tests\bin\Debug\NullusPerformanceTests.exe --gtest_filter=AssetThumbnailPerformanceTests.*
```

Expected: all cases pass.

### Task 5: Move Remaining Explicit Benchmarks And Stress Scenarios

**Files:**
- Modify: `Tests/Unit/AssetImportPipelineTests.cpp`
- Create: `Tests/Performance/AssetImportPerformanceTests.cpp`
- Modify: `Tests/Unit/RenderSceneCacheTests.cpp`
- Create: `Tests/Performance/RenderScenePerformanceTests.cpp`

- [ ] **Step 1: Move the NewSponza import benchmark**

Move `NewSponzaStandalonePngImportPerformanceReport` and only its real-asset/environment/report helpers to `AssetImportPerformanceTests.cpp`. Rename its suite to `AssetImportPerformanceTests`.

- [ ] **Step 2: Move the three render-scene scale workloads**

Move the three exact RenderScene cases from the Classification Manifest and their large-scene fixture builders to `RenderScenePerformanceTests.cpp`. Rename their suite to `RenderScenePerformanceTests`.

- [ ] **Step 3: Audit Unit for unclassified explicit performance intent**

Run:

```powershell
rg -n '^TEST(_F|_P)?\(.*(Performance|Benchmark|Stress)' Tests/Unit -g '*.cpp'
rg -n 'NLS_RUN_.*PERF|NLS_PERFORMANCE_REPORT_DIR|steady_clock::now\(\).*steady_clock::now' Tests/Unit -g '*.cpp'
```

Expected: remaining matches are deterministic tests of performance infrastructure, telemetry schemas, fake-clock budgets, or benchmark command construction. Add a short classification comment beside any remaining test whose name could otherwise look like an accidentally retained workload.

- [ ] **Step 4: Rebuild and verify inventories**

Run:

```powershell
cmake --build Build-test-boundary --config Debug --target NullusUnitTests -- /m:4
cmake --build Build-performance-tests --config Debug --target NullusPerformanceTests -- /m:4
Build-test-boundary\bin\Debug\NullusUnitTests.exe --gtest_list_tests |
    Select-String 'NewSponzaStandalonePngImportPerformanceReport|OneThousandCompatible|TraceScaleCompatible|StableLargeScene'
```

Expected: no output from the Unit inventory.

### Task 6: Document The Workflows

**Files:**
- Modify: `Docs/Testing.md`

- [ ] **Step 1: Document default behavior validation**

Add commands showing that the default build remains:

```powershell
cmake -S . -B Build -DNLS_BUILD_TESTS=ON
cmake --build Build --config Debug --target NullusUnitTests ReflectionTest -- /m:4
ctest --test-dir Build -C Debug -L behavior --output-on-failure
```

- [ ] **Step 2: Document opt-in performance validation**

Add:

```powershell
cmake -S . -B Build-performance -DNLS_BUILD_TESTS=OFF -DNLS_BUILD_PERFORMANCE_TESTS=ON
cmake --build Build-performance --config Release --target NullusPerformanceTests -- /m:4
ctest --test-dir Build-performance -C Release -L performance --output-on-failure
```

State that real-asset benchmarks may additionally require their existing `NLS_RUN_*_PERF` environment flags and that performance results should be collected from Release builds.

- [ ] **Step 3: Check documentation paths and formatting**

Run:

```powershell
rg -n 'NullusPerformanceTests|NLS_BUILD_PERFORMANCE_TESTS|performance' Docs/Testing.md
git diff --check -- Docs/Testing.md
```

Expected: both workflows are discoverable and no whitespace errors are reported.

### Task 7: Verify, Commit, And Merge The Current Branch

**Files:**
- Verify all files changed on `059-fix-subasset-expansion`

- [ ] **Step 1: Run the fast default validation**

Run:

```powershell
cmake --build Build-test-boundary --config Debug --target NullusUnitTests ReflectionTest -- /m:4
ctest --test-dir Build-test-boundary -C Debug -L behavior --output-on-failure
Build-test-boundary\bin\Debug\ReflectionTest.exe
```

Expected: the behavior suite and reflection test pass without running performance workloads.

- [ ] **Step 2: Run the opt-in performance validation**

Run:

```powershell
cmake --build Build-performance-tests --config Debug --target NullusPerformanceTests -- /m:4
ctest --test-dir Build-performance-tests -C Debug -L performance --output-on-failure
```

Expected: all local performance cases pass; real-asset cases report GTest skips when their flags/assets are unavailable.

- [ ] **Step 3: Review the complete branch diff**

Run:

```powershell
git diff --check
git status --short
git diff --stat main...HEAD
git diff --stat
```

Expected: no whitespace errors or generated `Runtime/*/Gen/` hand edits; every remaining working-tree file is intentionally part of the user's requested current-branch changes.

- [ ] **Step 4: Commit the remaining current-branch work**

Run:

```powershell
git add -A
git commit -m "refactor: integrate asset and rendering runtime updates"
```

Expected: the working tree becomes clean. Do not amend the already isolated design/plan commits.

- [ ] **Step 5: Update and merge into main**

Run:

```powershell
git switch main
git pull --ff-only
git merge --no-ff 059-fix-subasset-expansion
```

Expected: merge succeeds without conflicts. If `origin/main` advanced into overlapping files, resolve only after inspecting both sides and rerun all validation.

- [ ] **Step 6: Revalidate the merged result**

Run:

```powershell
cmake --build Build-test-boundary --config Debug --target NullusUnitTests ReflectionTest -- /m:4
ctest --test-dir Build-test-boundary -C Debug -L behavior --output-on-failure
Build-test-boundary\bin\Debug\ReflectionTest.exe
git status --short --branch
```

Expected: tests pass on `main`, and the working tree is clean.

- [ ] **Step 7: Delete the merged feature branch**

Run:

```powershell
git branch -d 059-fix-subasset-expansion
```

Expected: Git confirms deletion because the branch is fully merged.
