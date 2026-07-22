# Editor Scene View Camera Performance Implementation Plan

> **For the AI agent worker:** Required sub-skill: use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax to track progress.

**Goal:** Reduce Editor Scene View camera-movement stutter on Windows DX12, prove the change with identical low-overhead Debug and Release before/after benchmarks, and preserve exact-frame rendering consumers.

**Architecture:** Add a profiler-independent benchmark recorder around `Application::TickFrame`, then remove the measured CPU waste in three conservative layers: share one absolute deadline across slot/fence waits, reuse static draw preparation through scene-owned identities and revision stamps, and reuse object ranges/opaque sort tokens only when their revision contracts are trusted. Every optimization is a hint with the current implementation as its fallback, and all telemetry is cumulative so benchmark collection does not add per-frame logging.

**Tech Stack:** C++20, CMake/MSVC multi-config builds, GoogleTest, Nullus DX12 threaded renderer, nlohmann JSON, PowerShell benchmark scripts.

---

## File Structure

- Create `Project/Editor/Core/EditorCameraPerformanceBenchmark.h`: benchmark inputs, cumulative telemetry snapshots, summary data, and recorder API.
- Create `Project/Editor/Core/EditorCameraPerformanceBenchmark.cpp`: nearest-rank percentile calculation, delta calculation, JSON serialization, and atomic end-of-run write.
- Create `Tests/Unit/EditorCameraPerformanceBenchmarkTests.cpp`: deterministic math, warm-up exclusion, telemetry deltas, and JSON schema tests.
- Modify `Runtime/Rendering/Settings/EngineDiagnosticsSettings.h`: benchmark output path, warm-up count, measured count, and fixed-step settings.
- Modify `Project/Editor/Core/EditorLaunchArgs.cpp`: parse and validate benchmark CLI arguments and print usage.
- Modify `Tests/Unit/EditorLaunchArgsTests.cpp`: CLI success/error coverage and source-order contract checks.
- Modify `Project/Editor/Core/Application.h` and `Project/Editor/Core/Application.cpp`: time only camera-step `TickFrame` calls, snapshot telemetry, write once after sampling, and exit nonzero on write failure.
- Modify `Project/Editor/Core/Editor.h` and `Project/Editor/Core/Editor.cpp`: expose completed deterministic step count and Scene View publication state without logging.
- Modify `Project/Editor/Panels/AView.h` and `Project/Editor/Panels/AView.cpp`: expose the existing last-threaded-publication result.
- Create `Tools/Performance/RunEditorCameraBenchmark.ps1`: run three sequential trials per configuration and retain raw JSON files.
- Create `Tools/Performance/CompareEditorCameraBenchmark.ps1`: select the median run and emit a Markdown/JSON before-after comparison.
- Create `Tests/Unit/EditorCameraPerformanceScriptTests.cpp`: source-level safeguards for identical Debug/Release arguments and three-run median selection.
- Modify `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h` and `.cpp`: deadline-aware reusable-slot reservation and measured wait telemetry.
- Modify `Runtime/Rendering/Context/DriverAccess.h` and `Runtime/Rendering/Context/Driver.cpp`: propagate one absolute deadline through slot reservation and deferred-fence validation.
- Modify `Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.h` and `.cpp`: accept the publication deadline and use it for prepared-slot reservation.
- Modify `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`: create one editor retirement deadline per capture attempt.
- Modify `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`: deadline, timeout, stale-output isolation, and reservation-release tests.
- Modify `Tests/Unit/RendererFrameObjectBindingTests.cpp`: remaining-budget and deferred-fence tests.
- Modify `Runtime/Rendering/Data/DrawableObjectDescriptor.h`: optional trusted scene draw identity and revision metadata.
- Modify `Runtime/Engine/Rendering/RenderScene.h` and `.cpp`: cache camera-independent opaque sort tokens and emit stable draw revision data.
- Modify `Tests/Unit/RenderSceneCacheTests.cpp` and `Tests/Unit/SceneVisibilityPipelineTests.cpp`: token invalidation and ordering tests.
- Modify `Runtime/Rendering/Core/ABaseRenderer.h` and `.cpp`: revision-validated stable prepared-draw fast path with bounded age eviction.
- Modify `Runtime/Rendering/Core/RendererStats.h` and `.cpp`: static fast-path hit/miss telemetry.
- Modify `Tests/Unit/RendererStatsTests.cpp` and `Tests/Unit/DeferredSceneRendererMaterialCacheTests.cpp`: cache invalidation, non-mutating hits, and counters.
- Modify `Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.h` and `.cpp`: slot-local object identity/revision metadata and fast-path reuse.
- Modify `Runtime/Rendering/Data/FrameInfo.h`: benchmark-visible cache/object/sort counters.
- Modify `Tests/Unit/RendererFrameObjectBindingTests.cpp`: same-slot/range revision hits and conservative fallbacks.
- Create `Docs/superpowers/performance/2026-07-22-editor-camera-performance-results.md`: exact benchmark command, environment, raw artifact paths, and Debug/Release before-after table.

### Task 1: Low-Overhead Benchmark Math and CLI

**Files:**
- Create: `Project/Editor/Core/EditorCameraPerformanceBenchmark.h`
- Create: `Project/Editor/Core/EditorCameraPerformanceBenchmark.cpp`
- Create: `Tests/Unit/EditorCameraPerformanceBenchmarkTests.cpp`
- Modify: `Runtime/Rendering/Settings/EngineDiagnosticsSettings.h`
- Modify: `Project/Editor/Core/EditorLaunchArgs.cpp`
- Modify: `Tests/Unit/EditorLaunchArgsTests.cpp`

- [ ] **Step 1: Write failing benchmark math and serialization tests**

Add tests with a known sample vector and cumulative snapshots:

```cpp
TEST(EditorCameraPerformanceBenchmarkTests, UsesMeasuredSamplesAndNearestRankPercentiles)
{
    const std::vector<double> samples { 10.0, 20.0, 30.0, 40.0, 50.0 };
    const auto summary = BuildEditorCameraPerformanceSummary(
        EditorCameraPerformanceMetadata { "Debug", "DX12", false, 30u, 5u },
        samples,
        EditorCameraPerformanceTelemetry {},
        EditorCameraPerformanceTelemetry {});

    EXPECT_DOUBLE_EQ(summary.meanFrameMs, 30.0);
    EXPECT_DOUBLE_EQ(summary.meanFps, 1000.0 / 30.0);
    EXPECT_DOUBLE_EQ(summary.p95FrameMs, 50.0);
    EXPECT_DOUBLE_EQ(summary.p99FrameMs, 50.0);
    EXPECT_DOUBLE_EQ(summary.maxFrameMs, 50.0);
}

TEST(EditorCameraPerformanceBenchmarkTests, SubtractsCumulativeTelemetryWithoutUnderflow)
{
    EditorCameraPerformanceTelemetry before;
    before.publishedFrameCount = 40u;
    before.reservedSlotWaitTotalNs = 100u;
    EditorCameraPerformanceTelemetry after = before;
    after.publishedFrameCount = 44u;
    after.reservedSlotWaitTotalNs = 350u;

    const auto delta = CalculateEditorCameraPerformanceTelemetryDelta(before, after);
    EXPECT_EQ(delta.publishedFrameCount, 4u);
    EXPECT_EQ(delta.reservedSlotWaitTotalNs, 250u);
}
```

- [ ] **Step 2: Run the new tests and verify red**

Run from the worktree root:

```powershell
cmake -S . -B build-editor-camera-perf -DNLS_BUILD_TESTS=ON -DNLS_ENABLE_TEST_HOOKS=ON -DNLS_ENABLE_TIMELINE_PROFILER=OFF -DNLS_RENDER_API=DX12
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='EditorCameraPerformanceBenchmarkTests.*' --gtest_brief=1
```

Expected: compile failure because `EditorCameraPerformanceBenchmark.h` and its functions do not exist.

- [ ] **Step 3: Implement the minimal pure benchmark component**

Define fixed-width cumulative counters and summary fields. Use nearest-rank percentiles with `ceil(percentile * count) - 1`, and serialize only after all samples are collected:

```cpp
struct EditorCameraPerformanceSummary
{
    EditorCameraPerformanceMetadata metadata;
    std::vector<double> measuredFrameMs;
    double meanFrameMs = 0.0;
    double meanFps = 0.0;
    double p95FrameMs = 0.0;
    double p99FrameMs = 0.0;
    double maxFrameMs = 0.0;
    EditorCameraPerformanceTelemetry telemetryDelta;
    uint64_t publishedCameraStepCount = 0u;
};
```

Write to `<output>.tmp`, close successfully, then rename to the requested path. Return an error string on open/write/rename failure.

- [ ] **Step 4: Write failing CLI parsing tests**

Cover these exact arguments and rejection cases:

```cpp
--editor-camera-performance-output <absolute-json-path>
--editor-camera-performance-warmup-frames 30
--editor-camera-performance-frames 300
```

Assert zero frames, missing values, an empty output path, Timeline trace combination, and Profiler-panel combination set `hasError=true`. Assert successful parsing sets `editorValidationCameraForwardFrames` to `warmup + measured` without overflow.

- [ ] **Step 5: Implement CLI/settings support and verify green**

Add settings defaults `warmup=30`, `measured=300`, fixed delta `1.0f/60.0f`, and an empty output path. Perform final cross-field validation after parsing all arguments so argument order cannot bypass incompatible-mode checks.

Run:

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='EditorCameraPerformanceBenchmarkTests.*:EditorLaunchArgsTests.*' --gtest_brief=1
```

Expected: all selected tests pass from the worktree root.

- [ ] **Step 6: Commit the benchmark foundation**

```powershell
git add Project/Editor/Core/EditorCameraPerformanceBenchmark.h Project/Editor/Core/EditorCameraPerformanceBenchmark.cpp Runtime/Rendering/Settings/EngineDiagnosticsSettings.h Project/Editor/Core/EditorLaunchArgs.cpp Tests/Unit/EditorCameraPerformanceBenchmarkTests.cpp Tests/Unit/EditorLaunchArgsTests.cpp
git commit -m "feat: add editor camera performance benchmark core"
```

### Task 2: Benchmark Runtime Integration and Scripts

**Files:**
- Modify: `Project/Editor/Core/Application.h`
- Modify: `Project/Editor/Core/Application.cpp`
- Modify: `Project/Editor/Core/Editor.h`
- Modify: `Project/Editor/Core/Editor.cpp`
- Modify: `Project/Editor/Panels/AView.h`
- Modify: `Project/Editor/Panels/AView.cpp`
- Create: `Tools/Performance/RunEditorCameraBenchmark.ps1`
- Create: `Tools/Performance/CompareEditorCameraBenchmark.ps1`
- Create: `Tests/Unit/EditorCameraPerformanceScriptTests.cpp`

- [ ] **Step 1: Write failing integration contract tests**

Assert source and public API contracts for timing and publication sampling:

```cpp
EXPECT_NE(applicationSource.find("std::chrono::steady_clock::now()"), std::string::npos);
EXPECT_NE(applicationSource.find("TickFrame(kEditorCameraPerformanceFixedDeltaSeconds, true)"), std::string::npos);
EXPECT_NE(applicationSource.find("GetValidationCameraForwardCompletedFrames"), std::string::npos);
EXPECT_NE(applicationSource.find("WasLastSceneViewThreadedFramePublished"), std::string::npos);
EXPECT_LT(applicationSource.find("m_cameraPerformanceRecorder.AddMeasuredFrame"),
          applicationSource.find("WriteEditorCameraPerformanceSummary"));
```

Add script tests asserting Debug and Release share project, backend, VSync, scene, warm-up, measured count, and fixed camera arguments; assert `-Trials 3` and median-by-mean selection.

- [ ] **Step 2: Run the contracts and verify red**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='EditorCameraPerformanceBenchmarkTests.*:EditorCameraPerformanceScriptTests.*:EditorLaunchArgsTests.*' --gtest_brief=1
```

Expected: missing runtime accessors/scripts cause failures.

- [ ] **Step 3: Integrate timing with the deterministic camera steps**

In benchmark mode, `Application::Run` must:

```cpp
const auto completedBefore = m_editor->GetValidationCameraForwardCompletedFrames();
const auto frameStart = std::chrono::steady_clock::now();
TickFrame(kEditorCameraPerformanceFixedDeltaSeconds, true);
const auto frameEnd = std::chrono::steady_clock::now();
const auto completedAfter = m_editor->GetValidationCameraForwardCompletedFrames();
if (completedAfter > completedBefore)
    m_cameraPerformanceRecorder.AddCameraStep(completedAfter, frameEnd - frameStart, published);
```

Do not call `PaceIdleFrameIfNeeded` during samples. Take cumulative telemetry immediately before the first measured step and immediately after the last. Write JSON after the last stopwatch has stopped, log one final path/status line, set the window close flag, and return exit code 1 from the editor entry point when writing fails.

- [ ] **Step 4: Implement the three-trial runner and comparator**

`RunEditorCameraBenchmark.ps1` launches only one editor process at a time and waits for each exit. It passes DX12, exclusive Scene View, VSync-off project settings, fixed camera, 30 warm-up, and 300 measured frames. `CompareEditorCameraBenchmark.ps1` parses all six run summaries, chooses the median mean-frame-time run within each config/stage group, and emits both JSON and Markdown.

- [ ] **Step 5: Verify focused tests and build both editors**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests Editor --parallel 8
cmake --build build-editor-camera-perf --config Release --target NullusUnitTests Editor --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='EditorCameraPerformanceBenchmarkTests.*:EditorCameraPerformanceScriptTests.*:EditorLaunchArgsTests.*' --gtest_brief=1
.\build-editor-camera-perf\bin\Release\NullusUnitTests.exe --gtest_filter='EditorCameraPerformanceBenchmarkTests.*:EditorCameraPerformanceScriptTests.*:EditorLaunchArgsTests.*' --gtest_brief=1
```

Expected: both builds and both selected test runs exit 0.

- [ ] **Step 6: Commit runtime benchmark integration**

```powershell
git add Project/Editor/Core/Application.h Project/Editor/Core/Application.cpp Project/Editor/Core/Editor.h Project/Editor/Core/Editor.cpp Project/Editor/Panels/AView.h Project/Editor/Panels/AView.cpp Tools/Performance/RunEditorCameraBenchmark.ps1 Tools/Performance/CompareEditorCameraBenchmark.ps1 Tests/Unit/EditorCameraPerformanceScriptTests.cpp
git commit -m "feat: integrate editor camera performance benchmark"
```

### Task 3: Capture the Low-Overhead Before Baseline

**Files:**
- Create artifacts under: `build-editor-camera-perf/perf/editor-camera/before/`

- [ ] **Step 1: Verify benchmark preconditions**

Confirm the benchmark executable is not built with Timeline export enabled, the test project is `D:/Code/Nullus/build/perf-editor-camera-warm/TestProject/TestProject.nullus`, backend is DX12, only Scene View is open, and VSync is off. Record GPU/driver, viewport size, commit, and local time into `environment.json`.

- [ ] **Step 2: Run three Debug trials sequentially**

```powershell
powershell -ExecutionPolicy Bypass -File .\Tools\Performance\RunEditorCameraBenchmark.ps1 -Stage before -Configuration Debug -Trials 3 -BuildDirectory .\build-editor-camera-perf -ProjectPath 'D:\Code\Nullus\build\perf-editor-camera-warm\TestProject\TestProject.nullus'
```

Expected: three valid JSON summaries, each with 300 measured frames and no device loss, quarantine, descriptor failure, or object-data overflow.

- [ ] **Step 3: Run three Release trials sequentially**

```powershell
powershell -ExecutionPolicy Bypass -File .\Tools\Performance\RunEditorCameraBenchmark.ps1 -Stage before -Configuration Release -Trials 3 -BuildDirectory .\build-editor-camera-perf -ProjectPath 'D:\Code\Nullus\build\perf-editor-camera-warm\TestProject\TestProject.nullus'
```

Expected: the same validation as Debug.

- [ ] **Step 4: Freeze the baseline artifacts**

Generate `before-summary.json` with the median run for each configuration. Copy the six raw JSON files and environment metadata to a timestamped directory without regenerating them after optimization code lands.

### Task 4: One Absolute Retirement Deadline

**Files:**
- Modify: `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`
- Modify: `Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp`
- Modify: `Runtime/Rendering/Context/DriverAccess.h`
- Modify: `Runtime/Rendering/Context/Driver.cpp`
- Modify: `Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.h`
- Modify: `Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp`
- Modify: `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`
- Modify: `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`
- Modify: `Tests/Unit/RendererFrameObjectBindingTests.cpp`

- [ ] **Step 1: Write failing lifecycle deadline tests**

Add `ReserveReusableSlotIndexExcludingUntil(excluded, deadline)` tests that occupy every slot, release one after 3 ms, and verify success before an 8 ms deadline. Add an expired-deadline case that returns immediately, increments timeout once, and leaves every occupied slot unchanged. Existing duration overloads remain compatibility wrappers that compute one deadline.

- [ ] **Step 2: Verify lifecycle tests fail for the missing API**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='ThreadedRenderingLifecycleTests.*Deadline*' --gtest_brief=1
```

Expected: compile failure for the new deadline overload.

- [ ] **Step 3: Implement deadline-aware lifecycle waiting**

Use `condition_variable::wait_until` and retain actual elapsed telemetry:

```cpp
const bool hasReusableSlot = m_slotAvailable.wait_until(
    lock,
    retirementDeadline,
    [this, &excludedSlotIndices]() {
        return FindReservableSlotReadOnlyLocked(excludedSlotIndices) != nullptr;
    });
```

Never reserve excluded, submitted, render-ready, or otherwise unretired resources.

- [ ] **Step 4: Write failing driver/provider remaining-budget tests**

Use a fake deferred-frame fence waiter that records the timeout it receives. Simulate 5 ms consumed by lifecycle reservation and assert the fence gets at most the remaining ~3 ms, not a fresh 8 ms. Assert fence timeout releases the lifecycle reservation.

- [ ] **Step 5: Propagate the single deadline through Driver and provider**

Create the deadline once in `DeferredSceneRenderer` using `ResolveEditorThreadedPublishRetirementWaitMs()`. Pass it to `EngineFrameObjectBindingProvider`, lifecycle reservation, and deferred fence validation. Compute remaining time with a saturating helper before every wait; if expired, skip publication and preserve the last Scene View texture.

- [ ] **Step 6: Verify deadline and exact-consumer regressions**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='ThreadedRenderingLifecycleTests.*:RendererFrameObjectBindingTests.*:SceneViewPickingPolicyTests.*:EditorHitProxyPickingContractTests.*' --gtest_brief=1
```

Expected: selected tests exit 0; stale-output retirement tests prove output-identity isolation and swapchain/readback/picking remain exact.

- [ ] **Step 7: Commit the deadline fix**

```powershell
git add Runtime/Rendering/Context/ThreadedRenderingLifecycle.h Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp Runtime/Rendering/Context/DriverAccess.h Runtime/Rendering/Context/Driver.cpp Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.h Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp Runtime/Engine/Rendering/DeferredSceneRenderer.cpp Tests/Unit/ThreadedRenderingLifecycleTests.cpp Tests/Unit/RendererFrameObjectBindingTests.cpp
git commit -m "perf: share editor frame retirement deadline"
```

### Task 5: Cached Opaque Sort Tokens

**Files:**
- Modify: `Runtime/Engine/Rendering/RenderScene.h`
- Modify: `Runtime/Engine/Rendering/RenderScene.cpp`
- Modify: `Runtime/Rendering/Data/DrawableObjectDescriptor.h`
- Modify: `Runtime/Rendering/Data/FrameInfo.h`
- Modify: `Tests/Unit/RenderSceneCacheTests.cpp`
- Modify: `Tests/Unit/SceneVisibilityPipelineTests.cpp`

- [ ] **Step 1: Write failing opaque-token tests**

Build a cached opaque command, gather it from two camera positions, and assert its camera-independent token is rebuilt once and reused twice. Change material render state and mesh content revision independently and assert each rebuilds the command/token. Assert transparent and decal ordering still changes with camera distance.

- [ ] **Step 2: Run the new token tests and verify red**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='RenderSceneCacheTests.*OpaqueSortToken*:SceneVisibilityPipelineTests.*Sort*' --gtest_brief=1
```

Expected: missing cached token/counters cause compile or assertion failures.

- [ ] **Step 3: Cache and emit the token**

Add `uint64_t opaqueSortToken` to `RenderCachedDrawCommand`, calculate it when `buildSerial` changes, and copy it into `DrawableObjectDescriptor`. `FinalizeOpaqueQueue` uses `{opaqueSortToken, stablePrimitiveIdentity}`. Transparent/decal code continues to calculate distance every gather.

- [ ] **Step 4: Verify ordering and dynamic-instancing behavior**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='RenderSceneCacheTests.*:SceneVisibilityPipelineTests.*:SceneLODTests.*' --gtest_brief=1
```

Expected: tests pass, including stable opaque order, transparent order, LOD transitions, and dynamic instance grouping.

- [ ] **Step 5: Commit opaque token caching**

```powershell
git add Runtime/Engine/Rendering/RenderScene.h Runtime/Engine/Rendering/RenderScene.cpp Runtime/Rendering/Data/DrawableObjectDescriptor.h Runtime/Rendering/Data/FrameInfo.h Tests/Unit/RenderSceneCacheTests.cpp Tests/Unit/SceneVisibilityPipelineTests.cpp
git commit -m "perf: cache opaque scene sort tokens"
```

### Task 6: Revision-Validated Static Draw Preparation

**Files:**
- Modify: `Runtime/Rendering/Data/DrawableObjectDescriptor.h`
- Modify: `Runtime/Engine/Rendering/RenderScene.h`
- Modify: `Runtime/Engine/Rendering/RenderScene.cpp`
- Modify: `Runtime/Rendering/Core/ABaseRenderer.h`
- Modify: `Runtime/Rendering/Core/ABaseRenderer.cpp`
- Modify: `Runtime/Rendering/Core/RendererStats.h`
- Modify: `Runtime/Rendering/Core/RendererStats.cpp`
- Modify: `Runtime/Rendering/Data/FrameInfo.h`
- Modify: `Tests/Unit/DeferredSceneRendererMaterialCacheTests.cpp`
- Modify: `Tests/Unit/RendererStatsTests.cpp`

- [ ] **Step 1: Write failing stable fast-path tests**

Create a descriptor with stable primitive identity, command build serial, mesh/material/shader revisions, pass binding identity, pipeline overrides, device identity, transform revision, and group identity. Prime the existing general cache, then repeat the draw and assert a stable hit occurs without a full-key build or LRU splice. Mutate each revision field in a parameterized test and assert a miss followed by refresh.

- [ ] **Step 2: Verify the tests fail for missing revision metadata/counters**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='DeferredSceneRendererMaterialCacheTests.*Stable*:RendererStatsTests.*StaticFastPath*' --gtest_brief=1
```

Expected: missing metadata and counters fail compilation/assertions.

- [ ] **Step 3: Emit trusted scene revision stamps**

Only `RenderScene` cached commands may set `hasTrustedStaticDrawRevision=true`. Single primitives use stable handle identity. Merged opaque groups set a deterministic ordered group identity and `allowsSingleObjectDataReuse=false`. Synthetic, fullscreen, transparent, decal, and missing-revision descriptors leave the trust flag false.

- [ ] **Step 4: Implement the stable lookup before the general key build**

Use a compact lookup key of stable scene identity plus pass/light/pipeline/device state. Store the complete revision stamp and `PreparedRecordedDrawStaticBase*` ownership-safe entry. A valid hit updates only `lastUsedFrame`; insertion, revision refresh, and bounded sweep may modify LRU links. General cache resolution remains authoritative on any miss.

- [ ] **Step 5: Verify invalidation, eviction, and device-reset tests**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='DeferredSceneRendererMaterialCacheTests.*:RendererStatsTests.*' --gtest_brief=1
```

Expected: all selected tests pass; cache sizes remain bounded and device invalidation clears both indexes.

- [ ] **Step 6: Commit static draw fast path**

```powershell
git add Runtime/Rendering/Data/DrawableObjectDescriptor.h Runtime/Engine/Rendering/RenderScene.h Runtime/Engine/Rendering/RenderScene.cpp Runtime/Rendering/Core/ABaseRenderer.h Runtime/Rendering/Core/ABaseRenderer.cpp Runtime/Rendering/Core/RendererStats.h Runtime/Rendering/Core/RendererStats.cpp Runtime/Rendering/Data/FrameInfo.h Tests/Unit/DeferredSceneRendererMaterialCacheTests.cpp Tests/Unit/RendererStatsTests.cpp
git commit -m "perf: reuse revision-stable draw preparation"
```

### Task 7: Revision-Aware Object Data Reuse

**Files:**
- Modify: `Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.h`
- Modify: `Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp`
- Modify: `Runtime/Rendering/Data/DrawableObjectDescriptor.h`
- Modify: `Runtime/Rendering/Core/RendererStats.h`
- Modify: `Runtime/Rendering/Core/RendererStats.cpp`
- Modify: `Runtime/Rendering/Data/FrameInfo.h`
- Modify: `Tests/Unit/RendererFrameObjectBindingTests.cpp`
- Modify: `Tests/Unit/RendererStatsTests.cpp`

- [ ] **Step 1: Write failing slot-local object revision tests**

Prime slot 0 for stable identity 42, transform revision 7, object index 12, count 1. Repeat in slot 0 and assert no validity scan, `memcmp`, transpose, or upload. Repeat in slot 1, then change transform revision, object index, count, merged-group flag, reset slot, rebuild capacity, and device; assert every case falls back to comparison/upload.

- [ ] **Step 2: Verify the object fast-path tests fail**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='RendererFrameObjectBindingTests.*Revision*:RendererStatsTests.*ObjectDataRevision*' --gtest_brief=1
```

Expected: missing slot metadata and counters cause failures.

- [ ] **Step 3: Implement trusted same-slot/range reuse**

Store `{stableIdentity, transformRevision, objectIndex, objectCount, valid}` alongside each slot shadow range. Check it before the current validity-byte scan and `memcmp`. Refresh metadata only after the normal upload succeeds. Clear it with every shadow invalidation. Descriptors lacking trust and merged groups always use the old path.

- [ ] **Step 4: Verify focused object and renderer tests**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='RendererFrameObjectBindingTests.*:RendererStatsTests.*:DeferredSceneRendererMaterialCacheTests.*' --gtest_brief=1
```

Expected: selected tests pass with no object overflow or stale-transform reuse.

- [ ] **Step 5: Commit object-data reuse**

```powershell
git add Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.h Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp Runtime/Rendering/Data/DrawableObjectDescriptor.h Runtime/Rendering/Core/RendererStats.h Runtime/Rendering/Core/RendererStats.cpp Runtime/Rendering/Data/FrameInfo.h Tests/Unit/RendererFrameObjectBindingTests.cpp Tests/Unit/RendererStatsTests.cpp
git commit -m "perf: reuse unchanged frame object data"
```

### Task 8: Telemetry Integration and Focused Regression Matrix

**Files:**
- Modify: `Project/Editor/Core/EditorCameraPerformanceBenchmark.h`
- Modify: `Project/Editor/Core/EditorCameraPerformanceBenchmark.cpp`
- Modify: `Project/Editor/Core/Application.cpp`
- Modify: `Runtime/Rendering/Data/FrameInfo.h`
- Modify: `Tests/Unit/EditorCameraPerformanceBenchmarkTests.cpp`

- [ ] **Step 1: Write failing complete-schema tests**

Parse generated JSON and assert publication ratio, blocked publications, slot wait count/timeouts/time, latest published/retired IDs, general cache hits/misses, static fast-path hits/misses, object revision hits/fallbacks, opaque token hits/rebuilds, descriptor failures, device loss, quarantine, and object overflow are present with exact integer types.

- [ ] **Step 2: Implement cumulative snapshot plumbing**

Read `DriverRendererAccess::GetThreadedFrameTelemetry` and renderer stats only at the measurement boundaries. Use saturating deltas. Compute publication ratio as `publishedCameraStepCount / measuredFrameCount`; do not query locks or log per measured frame except the already available boolean publication result.

- [ ] **Step 3: Verify both configurations and focused regressions**

```powershell
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests Editor --parallel 8
cmake --build build-editor-camera-perf --config Release --target NullusUnitTests Editor --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='EditorCameraPerformanceBenchmarkTests.*:ThreadedRenderingLifecycleTests.*:RendererFrameObjectBindingTests.*:RendererStatsTests.*:DeferredSceneRendererMaterialCacheTests.*:RenderSceneCacheTests.*:SceneVisibilityPipelineTests.*:SceneLODTests.*:SceneHLODTests.*:SceneViewPickingPolicyTests.*:EditorHitProxyPickingContractTests.*' --gtest_brief=1
.\build-editor-camera-perf\bin\Release\NullusUnitTests.exe --gtest_filter='EditorCameraPerformanceBenchmarkTests.*:ThreadedRenderingLifecycleTests.*:RendererFrameObjectBindingTests.*:RendererStatsTests.*:DeferredSceneRendererMaterialCacheTests.*:RenderSceneCacheTests.*:SceneVisibilityPipelineTests.*:SceneLODTests.*:SceneHLODTests.*:SceneViewPickingPolicyTests.*:EditorHitProxyPickingContractTests.*' --gtest_brief=1
```

Expected: both builds and both filtered runs exit 0 from the worktree root.

- [ ] **Step 4: Commit telemetry completion**

```powershell
git add Project/Editor/Core/EditorCameraPerformanceBenchmark.h Project/Editor/Core/EditorCameraPerformanceBenchmark.cpp Project/Editor/Core/Application.cpp Runtime/Rendering/Data/FrameInfo.h Tests/Unit/EditorCameraPerformanceBenchmarkTests.cpp
git commit -m "feat: report editor camera optimization telemetry"
```

### Task 9: After Benchmarks, Acceptance, and Results

**Files:**
- Create artifacts under: `build-editor-camera-perf/perf/editor-camera/after/`
- Create: `Docs/superpowers/performance/2026-07-22-editor-camera-performance-results.md`

- [ ] **Step 1: Run three optimized Debug trials**

```powershell
powershell -ExecutionPolicy Bypass -File .\Tools\Performance\RunEditorCameraBenchmark.ps1 -Stage after -Configuration Debug -Trials 3 -BuildDirectory .\build-editor-camera-perf -ProjectPath 'D:\Code\Nullus\build\perf-editor-camera-warm\TestProject\TestProject.nullus'
```

- [ ] **Step 2: Run three optimized Release trials**

```powershell
powershell -ExecutionPolicy Bypass -File .\Tools\Performance\RunEditorCameraBenchmark.ps1 -Stage after -Configuration Release -Trials 3 -BuildDirectory .\build-editor-camera-perf -ProjectPath 'D:\Code\Nullus\build\perf-editor-camera-warm\TestProject\TestProject.nullus'
```

- [ ] **Step 3: Generate and inspect the comparison**

```powershell
powershell -ExecutionPolicy Bypass -File .\Tools\Performance\CompareEditorCameraBenchmark.ps1 -BeforeDirectory .\build-editor-camera-perf\perf\editor-camera\before -AfterDirectory .\build-editor-camera-perf\perf\editor-camera\after -JsonOutput .\build-editor-camera-perf\perf\editor-camera\comparison.json -MarkdownOutput .\Docs\superpowers\performance\2026-07-22-editor-camera-performance-results.md
```

Confirm Debug mean frame time improves at least 15%, Release P99 improves at least 30%, no attempt exceeds one 8 ms wait budget beyond scheduler tolerance, publication ratio does not regress, and all safety counters remain zero. If an acceptance threshold misses, use a post-change Timeline trace only for diagnosis, never for the comparison table.

- [ ] **Step 4: Run the fresh completion verification**

```powershell
git status --short
cmake --build build-editor-camera-perf --config Debug --target NullusUnitTests Editor --parallel 8
cmake --build build-editor-camera-perf --config Release --target NullusUnitTests Editor --parallel 8
.\build-editor-camera-perf\bin\Debug\NullusUnitTests.exe --gtest_filter='EditorCameraPerformanceBenchmarkTests.*:EditorLaunchArgsTests.*:EditorCameraPerformanceScriptTests.*:ThreadedRenderingLifecycleTests.*:RendererFrameObjectBindingTests.*:RendererStatsTests.*:DeferredSceneRendererMaterialCacheTests.*:RenderSceneCacheTests.*:SceneVisibilityPipelineTests.*:SceneLODTests.*:SceneHLODTests.*:SceneViewPickingPolicyTests.*:EditorHitProxyPickingContractTests.*' --gtest_brief=1
.\build-editor-camera-perf\bin\Release\NullusUnitTests.exe --gtest_filter='EditorCameraPerformanceBenchmarkTests.*:EditorLaunchArgsTests.*:EditorCameraPerformanceScriptTests.*:ThreadedRenderingLifecycleTests.*:RendererFrameObjectBindingTests.*:RendererStatsTests.*:DeferredSceneRendererMaterialCacheTests.*:RenderSceneCacheTests.*:SceneVisibilityPipelineTests.*:SceneLODTests.*:SceneHLODTests.*:SceneViewPickingPolicyTests.*:EditorHitProxyPickingContractTests.*' --gtest_brief=1
```

Read every exit code and failure count before claiming completion.

- [ ] **Step 5: Commit the reproducible result report**

```powershell
git add -f Docs/superpowers/performance/2026-07-22-editor-camera-performance-results.md
git commit -m "docs: report editor camera performance gains"
```

