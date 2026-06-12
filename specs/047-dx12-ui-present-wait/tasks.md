# Tasks: DX12 UI Present Wait Reduction

**Input**: Design documents from `/specs/047-dx12-ui-present-wait/`  
**Prerequisites**: spec.md, plan.md, quickstart.md

**Tests**: Required. This change follows TDD because it changes DX12 GPU synchronization behavior.

## Phase 1: Setup

- [x] T001 Create isolated worktree `047-dx12-ui-present-wait`
- [x] T002 Inspect `DX12UIBridge::RenderDrawData` wait path
- [x] T003 Inspect `DX12UIFrameFenceTracker` tests and helper

## Phase 2: RED Tests

- [x] T004 Add tests proving allocator slots are selected independently from backbuffer reuse in `Tests/Unit/DX12UIFrameFenceTrackerTests.cpp`
- [x] T005 Add tests proving exhausted allocator pool reports a specific fence to wait for
- [x] T006 Add source/trace name guard proving `DX12UIBridge::WaitForBackbufferReuse` is replaced with a more specific allocator wait scope
- [x] T007 Run targeted tests and confirm the new tests fail before implementation

## Phase 3: Implementation

- [x] T008 Extend `DX12UIFrameFenceTracker` with UI allocator pool selection/reset state
- [x] T009 Update `DX12UIBridge` to create allocator slots independent of backbuffer index
- [x] T010 Replace per-backbuffer CPU fence wait with allocator pool selection and exhausted-pool wait
- [x] T011 Preserve scene wait, command submission, UI fence signalling, and texture handle retention behavior

## Phase 4: Validation

- [x] T012 Run targeted DX12 UI frame fence tests
- [x] T013 Build `NullusUnitTests`
- [x] T014 Run threaded rendering + DX12 UI relevant test subset
- [x] T015 Run full unit suite or document the closest available validation
- [x] T016 Inspect `git diff --check` and generated-file hygiene
- [x] T017 Run required plan-review/multi-agent quality gate for GPU synchronization changes

## Review Notes

- This touches DX12 command allocator/fence lifetime and must be reviewed as GPU synchronization work.
- Do not hand-edit generated files under `Runtime/*/Gen/`.
- Do not claim runtime trace success until an after-change DX12 editor capture is inspected.

## Validation Evidence

- 2026-06-12: `cmake --build Build --target NullusUnitTests --config Debug` passed.
- 2026-06-12: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=DX12UIFrameFenceTrackerTests.*:ProfilerDestinationTest.DX12UiBridgeUsesAllocatorReuseWaitScope --gtest_break_on_failure=1` passed 11/11 tests.
- 2026-06-12: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.*:DX12UIFrameFenceTrackerTests.* --gtest_break_on_failure=1` passed 182/182 tests. Expected failure-path log lines were emitted by lifecycle tests.
- 2026-06-12: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_break_on_failure=1` was attempted and stopped in unrelated `GameObjectAssetImportTests.ColdFbxRawModelDropForwardsBackgroundImportDiagnostics`; the failure expected `external-model-importer-autodesk-fbx-unavailable` but observed FBX fallback/source-parse diagnostics in an Autodesk FBX SDK unavailable environment.
- 2026-06-12: `git diff --check` reported only CRLF normalization warnings, no whitespace errors.
- 2026-06-12: `git diff --name-only -- 'Runtime/*/Gen/*' 'Project/*/Gen/*'` returned no generated-file edits.
- 2026-06-12: Multi-agent review round 1 found 0 P0/P1. P2 findings addressed: source test now anchors to `NLS_ROOT_DIR`, quickstart uses the new profiler guard filter, and DX12 UI command allocator/list reset/close HRESULTs are checked.
- 2026-06-12: Deeper GPU audit found 2 P1 in pre-submit recording failure handling. Both were fixed by routing reset/close failures through `HandlePreSubmitCommandRecordingFailure`, which checks device-lost state and releases swapchain UI recording resources so a poisoned command list is not reused.
- 2026-06-12: Final deeper GPU audit reported `0 P0/P1 remain`.
- Runtime DX12 editor trace verification remains pending; do not claim SC-003 until a post-change trace is captured and inspected.
