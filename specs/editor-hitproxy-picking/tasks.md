# Tasks: Editor HitProxy Picking

**Input**: Design documents from `specs/editor-hitproxy-picking/`  
**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `contracts/picking-cache-contract.md`, `quickstart.md`

**Tests**: Required. Behavior changes must use test-first where stable entrypoints exist.

**Organization**: Tasks are grouped by independently testable user story slices.

## Phase 1: Setup And Baseline

**Purpose**: Confirm branch isolation and current picking hotspots before implementation.

- [X] T001 Record `git status --short --branch` from `D:\VSProject\Nullus\.worktrees\editor-hitproxy-picking` in this tasks file. Evidence: `## editor-hitproxy-picking` with only the new spec bundle, `Tests/Unit/SceneViewPickingPolicyTests.cpp`, and `Project/Editor/Panels/SceneViewPickingPolicy.h` changes in progress.
- [X] T002 Inspect current picking request flow in `Project/Editor/Panels/SceneView.cpp`, `Project/Editor/Panels/SceneViewPickingPolicy.h`, `Project/Editor/Rendering/PickingRenderPass.h`, and `Project/Editor/Rendering/PickingRenderPass.cpp`. Evidence: `SceneView` owns hover/click request timing and pending click serials; `PickingRenderPass` owns async readback, pick ID registry, visible draw-source capture, and hover visible-draw budget skip.
- [X] T003 Inspect selection outline separation in `Project/Editor/Rendering/SelectionOutlineMaskRenderer.h`, `Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp`, and `Project/Editor/Rendering/DebugGameObjectSelectionCollector.h`. Evidence: selection outline already has an independent mask/composite path and cached mask signatures, so the new picking work should preserve this separation rather than route highlight through picking readback.
- [X] T004 Record baseline trace evidence for `PickingRenderPass::CapturePickableModelSources` from the latest user-provided moving-camera trace if available. Evidence: prior investigation recorded this scope as the current picking hotspot, with earlier traces around ~54 ms worst-case and later moving-camera trace around ~192 ms worst-case.

---

## Phase 2: Foundational Cache Policy

**Purpose**: Build pure, testable decision logic before touching renderer state.

- [X] T005 [P] Add failing unit tests in `Tests/Unit/SceneViewPickingPolicyTests.cpp` for cache reuse when signatures match and rebuild when render extent, camera hash, draw-source hash, or scene revision differ. Evidence: added `ReusesReadablePickingFrameWhenSignatureMatches`, `RejectsPickingFrameReuseWhenAnySignatureFieldDiffers`, and `DoesNotReuseWhenNoReadablePickingFrameExists`.
- [X] T006 [P] Add failing unit tests in `Tests/Unit/SceneViewPickingPolicyTests.cpp` proving hover budget can skip hover requests but never click requests. Evidence: added `HoverBudgetCanSkipHoverButNeverClick` and `ZeroHoverBudgetDisablesBudgetSkip`.
- [X] T007 [P] Add failing unit tests in `Tests/Unit/SceneViewPickingPolicyTests.cpp` proving click requests require a readable serial greater than or equal to the click minimum serial. Evidence: added `ClickResolveRequiresFreshReadableSerialAndMatchingSignature` and `HoverResolveRequiresReadableMatchingSignatureButNoMinimumSerial`.
- [X] T008 Implement picking signature/request helper structs and policy functions in `Project/Editor/Panels/SceneViewPickingPolicy.h`. Evidence: added `HitProxyPickingRequestKind`, `HitProxyPickingSignature`, `HitProxyPickingSignaturesMatch`, `ShouldReuseHitProxyPickingFrame`, `ShouldSkipHitProxyPickingForVisibleDrawBudget`, and `ShouldResolveHitProxyPickingRequest`; existing `ShouldSkipSceneHoverPickingForVisibleDrawBudget` now delegates to the new request-kind helper.
- [X] T009 Run `NullusUnitTests.exe --gtest_filter=SceneViewPickingPolicyTests.*` and record evidence in this tasks file. Evidence: `cmake --build build/editor-hitproxy-picking-verify --config Debug --target NullusUnitTests -- /m:4 /nr:false /p:CL_MPCount=4 /p:UseMultiToolTask=true /v:minimal` passed; the latest focused run passed 36 tests after the readback invalidation, RHI-frame-bound generation-token completion, click signature, and hover budget ordering fixes.

---

## Phase 3: User Story 1 - Stable Cached Picking (Priority: P1)

**Goal**: Reuse compatible hit-proxy frames and avoid heavy hover picking rebuilds during idle/large-scene camera interaction.

**Independent Test**: Moving and stationary Scene View tests show compatible frames are reused, stale click frames are rejected, and click selection remains correct.

- [X] T010 [P] Add render path contract tests for picking pass cache reuse vs rebuild decisions. Evidence: added `Tests/Unit/EditorHitProxyPickingContractTests.cpp` because this worktree has no `EditorRenderPathContractTests.cpp`; tests verify hover reuse, click bypass, hover budget isolation, skip-before-signature ordering, readback invalidation before submission, camera/light helper signature coverage, readback wait/resolve scopes, and selection outline separation.
- [X] T011 Add `HitProxyPickingSignature` and cached-frame metadata to `Project/Editor/Rendering/PickingRenderPass.h`. Evidence: `PickingReadbackFrame` now stores `HitProxyPickingSignature`; `PickingRenderPass` builds current signatures, stores submitted/readable-frame signatures, exposes signature-aware resolve checks, and checks readable-frame compatibility.
- [X] T012 Update `Project/Editor/Rendering/PickingRenderPass.cpp` so `Draw()` skips rebuild when the current signature matches a readable frame and records a reuse diagnostic. Evidence: `Draw()` calls `CanReuseReadablePickingFrameForSignature()`, emits `EditorPicking::Reuse`, records `PickingDiagnostics.reusedFrames`, and invalidates stale completed readback texture history before submitting a new picking frame.
- [X] T013 Update `Project/Editor/Rendering/PickingRenderPass.cpp` so click picking can request a fresh frame even when hover picking is skipped by visible draw budget. Evidence: reuse and hover-budget skip are both gated by `!requestPickingFrameForClick` / hover request kind; `EditorHitProxyPickingContractTests.HoverBudgetSkipDoesNotApplyToClickPicking` covers this.
- [X] T014 Update `Project/Editor/Rendering/PickingRenderPass.cpp` so `DecodePickingResult()` validates registry object liveness or scene revision before returning a GameObject. Evidence: `DecodePickingResult()` now requires `gameObjectUnderMouse->IsAlive()`.
- [X] T015 Update `Project/Editor/Panels/SceneView.cpp` so click requests record minimum readable serial after requesting a fresh picking frame and resolve only against compatible frames. Evidence: click minimum serial is routed through `ComputePendingClickMinimumReadablePickingFrameSerial()` and carried into `DebugSceneDescriptor.clickMinimumPickingFrameSerial`; pending clicks also store the submitted picking signature and resolve through `PickingRenderPass::CanResolvePickingRequest()`, while mismatched readable frames cancel the pending click instead of selecting from a different frame.
- [X] T016 Run `NullusUnitTests.exe --gtest_filter=SceneViewPickingPolicyTests.*:EditorRenderPathContractTests.*Picking*` and record evidence. Evidence: replacement filter `SceneViewPickingPolicyTests.*:PickingReadbackLifecycleTests.*:EditorHitProxyPickingContractTests.*` passed as part of the 21-test focused run listed in T009.

---

## Phase 4: User Story 2 - Selection Highlight Is Separate From Picking (Priority: P1)

**Goal**: Make selection outline updates independent from picking cache invalidation.

**Independent Test**: Selection changes update outline without forcing picking rebuild and outline remains stable while picking readback is pending.

- [X] T017 [P] Add contract tests proving selection outline path does not depend on picking readback availability. Evidence: `EditorHitProxyPickingContractTests.SelectionOutlinePathStaysSeparateFromPickingReadback` verifies `SelectionOutlineMaskRenderer` uses mask/composite metadata and does not consume `PickingReadbackLifecycle`.
- [X] T018 Update `Project/Editor/Panels/SceneView.cpp` so selection changes do not clear or invalidate picking samples unless pickable state changed. Evidence: selection state remains routed through selection outline/static frame cache only; picking signatures are based on render extent, camera, scene mutation token, visible pickable draw-source hash, and view id, not selected-object state.
- [X] T019 Update or add comments/contracts near `Project/Editor/Rendering/SelectionOutlineMaskRenderer.h` to document that selection outline must not consume picking-buffer state. Evidence: added explicit comment above `SelectionOutlineMaskRenderer`.
- [X] T020 Run `NullusUnitTests.exe --gtest_filter=EditorRenderPathContractTests.*Selection*:EditorRenderPathContractTests.*Picking*` and record evidence. Evidence: replacement filter `EditorHitProxyPickingContractTests.*` passed as part of the 21-test focused run listed in T009.

---

## Phase 5: User Story 3 - Readable Diagnostics For Picking Cost (Priority: P2)

**Goal**: Make picking behavior visible in FrameInfo and profiler traces.

**Independent Test**: Unit tests and runtime trace show rebuild/reuse/skip/wait/resolve states as separate readable values.

- [X] T021 [P] Add renderer stats tests in `Tests/Unit/RendererStatsTests.cpp` for picking rebuild, reuse, hover skip, pending readback, and serial aggregation. Evidence: added `RendererStatsTracksPickingDiagnosticsAndResetsPerFrame`.
- [X] T022 Add picking diagnostic fields to `Runtime/Rendering/Data/FrameInfo.h`. Evidence: added `PickingDiagnostics` and `FrameInfo::picking`.
- [X] T023 Update `Runtime/Rendering/Core/RendererStats.cpp` to aggregate picking diagnostics across visible render views. Evidence: `RendererStats::RecordPickingDiagnostics()` accumulates counters, ORs pending readback, and keeps newest serials; `FrameInfo.cpp` aggregates picking diagnostics across displayed views.
- [X] T024 Add profiler scopes in `Project/Editor/Rendering/PickingRenderPass.cpp` and `Project/Editor/Panels/SceneView.cpp` for rebuild, reuse, skip, wait, and resolve. Evidence: added `EditorPicking::Rebuild`, `EditorPicking::Reuse`, `EditorPicking::SkipHoverBudget`, `EditorPicking::WaitReadback`, and `EditorPicking::ResolveClick`; contract test verifies SceneView scopes.
- [X] T025 Update FrameInfo presentation code to show picking diagnostics as separate table fields without slash-separated multi-values. Evidence: `FrameInfoRendererStats.cpp` adds separate `Picking` rows for rebuilt frames, reused frames, hover budget skips, pending readback, submitted/readable/click serials, and visible pickable draws.
- [X] T026 Run `NullusUnitTests.exe --gtest_filter=RendererStatsTests.*Picking*:PanelWindowHookTests.*FrameInfo*` and record evidence. Evidence: focused run passed `RendererStatsTests.RendererStatsTracksPickingDiagnosticsAndResetsPerFrame`, `PanelWindowHookTests.FrameInfoPanelFormatsSuppliedRenderViewSnapshot`, and `PanelWindowHookTests.FrameInfoPanelAggregatesDisplayedRenderViewSnapshots` as part of the 21-test focused run listed in T009.

---

## Phase 6: Runtime Validation And Review

**Purpose**: Prove the UE-style picking split solves the observed editor interaction issue.

- [X] T027 Build `NullusUnitTests` with the command from `quickstart.md` and record evidence. Evidence: serial `/m:1` build was too slow after public `FrameInfo.h` changes; equivalent target build with `/m:4 /p:CL_MPCount=4 /p:UseMultiToolTask=true` passed and produced `build\editor-hitproxy-picking-verify\bin\Debug\NullusUnitTests.exe`.
- [X] T028 Run focused picking/selection/stats tests and record evidence. Evidence: focused `NullusUnitTests` run passed 36 tests from `DriverSwapchainResizeTests.CanInvalidateSpecificCompletedReadbackTexture`, `DriverSwapchainResizeTests.CompletedReadbackGenerationDoesNotPromoteLateOldSubmission`, `DriverSwapchainResizeTests.CompletedReadbackGenerationUsesExplicitFrameTokenWhenProvided`, `FrameGraphSceneTargetsTests.*Readback*`, `ThreadedRenderingLifecycleTests.ThreadedPreparedFramePublishesCompletedPreferredReadbackTexture`, `SceneViewPickingPolicyTests`, `PickingReadbackLifecycleTests`, `EditorHitProxyPickingContractTests`, `RendererStatsTests.*Picking*`, and targeted FrameInfo/SceneView panel tests.
- [ ] T029 Launch the DX12 Editor, repeat the quickstart runtime smoke, and record whether hover rebuilds are bounded, click selection is correct, and selection outline remains stable.
- [ ] T030 Capture or inspect a moving-camera trace and confirm hover-only frames no longer repeatedly spend large blocks in `PickingRenderPass::CapturePickableModelSources`.
- [X] T031 Run `git diff --check` and record evidence. Evidence: `git diff --check` exited 0; Git printed LF-to-CRLF conversion warnings only, with no whitespace errors.
- [X] T032 Run required `/plan-review` quality gate before merge or completion claim. Evidence: first independent review found P0/P1 in readback completion, click signature resolve, hover skip ordering, and camera/light signature coverage; fixes were applied and validated. Follow-up review then found the texture-only readback invalidation still allowed a late old async completion to promote a new pending frame; the fix now binds generation tokens through `RenderPassCommandInput` -> `RenderScenePackage` -> `RHIFrameContext`, and completion records the actual frame token instead of guessing from a global texture queue. This is validated by `DriverSwapchainResizeTests.CompletedReadbackGenerationDoesNotPromoteLateOldSubmission`, `DriverSwapchainResizeTests.CompletedReadbackGenerationUsesExplicitFrameTokenWhenProvided`, `FrameGraphSceneTargetsTests.*Readback*`, and `ThreadedRenderingLifecycleTests.ThreadedPreparedFramePublishesCompletedPreferredReadbackTexture`. Final deeper audit reported no P0/P1/P2 findings and specifically verified the token path and reset/resize/standalone/threaded cleanup coverage.

**Additional validation note**: A full `NullusUnitTests.exe` run executed 2222 tests: 2208 passed, 12 skipped, and 2 failed. The two reproducible failures were `EditorAssetDragDropTests.RepeatedImportedFbxDropFastBindsThroughUnifiedHotCache` at `database.ImportAsset("Assets/Models/RepeatedDropCube.fbx")` and `SceneOcclusionTests.BaseSceneRendererBuildsHZBPacketsFromRetainedObservationCandidates` source-string expectations; neither file is modified by this change, but they remain global-suite blockers until addressed separately. A supplemental readback-focused run passed 14 tests from `RenderFrameworkContractTests.*ReadPixels*`, `ThreadedRenderingLifecycleTests.*Readback*`, `DriverSwapchainResizeTests.*Readback*`, and `FrameGraphSceneTargetsTests.*Readback*`.

## Dependencies & Execution Order

- Phase 1 blocks all implementation.
- Phase 2 blocks render-pass and SceneView changes.
- Phase 3 is the MVP and should be completed before selection/diagnostics refinements.
- Phase 4 depends on Phase 3 only for clear cache boundaries.
- Phase 5 can start after Phase 3 but should finish before runtime validation.
- Phase 6 depends on all implementation phases.

## Implementation Strategy

1. Build pure cache/request policy first.
2. Add picking cache metadata and reuse path without changing selection visuals.
3. Wire click freshness after cache reuse is stable.
4. Lock in selection/picking separation with tests.
5. Add diagnostics last so validation explains the new behavior.
