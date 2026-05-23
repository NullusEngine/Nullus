# Tasks: RHI Framework Cleanup - Stabilization Sync

**Input**: Design documents from `/specs/002-rhi-framework-cleanup/`
**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `quickstart.md`
**Updated**: 2026-04-14 after backend truthfulness gating, Game launch-args extraction, editor resize/tooling refresh stabilization, and a fresh build/test/smoke revalidation pass

**Organization**: Tasks are grouped by completion state and by the new stabilization user story (US6). Earlier migration work that is still valid is summarized as completed foundation work. Runtime/backend claims invalidated by the 2026-04-08 review are reopened below instead of left as completed.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task maps to, for example `[US1]`, `[US6]`

---

## Phase 1: Completed Foundation Snapshot

**Purpose**: Preserve the real progress that survived review without continuing to over-claim backend completion.

- [x] T001 [US1] Unify active texture and buffer descriptor usage across `Runtime/Rendering/RHI/`, `Runtime/Rendering/Buffers/`, and `Runtime/Rendering/FrameGraph/`
- [x] T002 [US4] Replace the legacy dual factory direction with `CreateRhiDevice()` in `Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp`
- [x] T003 [US2] Move UI texture resolution toward Formal RHI typed handles in `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` and `Runtime/UI/`
- [x] T004 [US3] Replace raw native resource handle assumptions with tagged `NativeHandle` usage in RHI resource consumers
- [x] T005 [US5] Delete legacy `IRenderDevice` backend files and remove active `IRenderDevice` ownership from `Runtime/Rendering/Context/Driver.cpp`
- [x] T006 Validate the foundation snapshot with `cmake --build build --config Debug` and `ctest --test-dir build -C Debug --output-on-failure`

**Checkpoint**: The foundation work builds and unit tests pass, but backend runtime support remains incomplete and is reopened below.

---

## Phase 2: User Story 6 - Baseline Runtime Evidence (Priority: P1)

**Goal**: Capture current backend runtime evidence before changing stabilization code.

**Independent Test**: A later reviewer can compare new smoke results against the 2026-04-08 baseline and see whether each backend improved, regressed, or stayed gated.

- [x] T007 [US6] Re-run `cmake --build build --config Debug` from repo root and record the result in `specs/002-rhi-framework-cleanup/tasks.md`
- [x] T008 [US6] Re-run `ctest --test-dir build -C Debug --output-on-failure` from repo root and record the result in `specs/002-rhi-framework-cleanup/tasks.md`
- [x] T009 [US6] Run direct `Editor.exe --backend dx12 <TestProject.nullus>` smoke and record latest log path plus exit/survival result
- [x] T010 [US6] Run direct `Editor.exe --backend dx11 <TestProject.nullus>` smoke and record latest log path plus exit/survival result
- [x] T011 [US6] Run direct `Editor.exe --backend vulkan <TestProject.nullus>` smoke and record latest log path plus exit/survival result
- [x] T012 [US6] Run direct `Editor.exe --backend opengl <TestProject.nullus>` smoke and record latest log path plus exit/survival result
- [x] T013 [US6] Record that pre-`T028` `Game.exe --backend` results were not authoritative until `Project/Game/Main.cpp` backend parsing was implemented

**Checkpoint**: Runtime evidence is explicit; no backend support statement is inferred from build success alone.

**2026-04-13 baseline refresh evidence**

- `powershell -ExecutionPolicy Bypass -File .\verify_backends_simple.ps1 -Backend dx11` now reports `EditorStatus=Exited`, `EditorExitCode=0`, `GameStatus=Exited`, and `GameExitCode=1`. The latest authoritative Game log `App/Win64_Debug_Runtime_Static/2026-04-13_20-27-21.log` records explicit DX11 unsupported gating instead of an access violation; the Editor path exits cleanly without emitting a fresh dedicated app-side log.
- `powershell -ExecutionPolicy Bypass -File .\verify_backends_simple.ps1 -Backend opengl` now reports `EditorStatus=Exited`, `EditorExitCode=0`, `GameStatus=Exited`, and `GameExitCode=1`. The latest authoritative Game log `App/Win64_Debug_Runtime_Static/2026-04-13_20-27-46.log` records explicit OpenGL unsupported gating instead of an access violation; the Editor path again exits cleanly without a fresh dedicated app-side log.
- The historical limitation from `T013` is now closed by `T028-T036` plus the new `GameLaunchArgsTests`: current `Game.exe --backend` smoke results are authoritative again.

---

## Phase 3: User Story 6 - DX12 Runtime Stabilization (Priority: P1)

**Goal**: Fix the reviewed `DX12` root-signature / pipeline-layout corruption that blocks first-frame rendering.

**Independent Test**: `DX12` startup no longer logs `NativeDX12PipelineLayout: D3D12SerializeRootSignature failed: Unsupported RangeType value ...`.

- [x] T014 [US6] Fix descriptor-range storage lifetime in `Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp`
- [x] T015 [P] [US6] Add focused unit coverage for DX12 descriptor-range grouping or root-signature layout helper behavior in `Tests/Unit/` if the helper can be isolated from device creation
- [x] T016 [US6] Rebuild with `cmake --build build --config Debug`
- [x] T017 [US6] Re-run `ctest --test-dir build -C Debug --output-on-failure`
- [x] T018 [US6] Run `Game.exe <TestProject.nullus>` on the current default backend and confirm the reviewed DX12 `Unsupported RangeType` error is gone
- [x] T019 [US6] Run `Editor.exe --backend dx12 <TestProject.nullus>` smoke and record the new result

**2026-04-08 follow-up evidence**

- `Runtime/Rendering/RHI/Backends/DX12/` now routes descriptor-table construction through owned helper storage, and `Tests/Unit/DX12PipelineLayoutUtilsTests.cpp` plus `Tests/Unit/DX12GraphicsPipelineUtilsTests.cpp` cover the new DX12 helper behavior.
- Fresh verification passed with `cmake --build build --config Debug` and `ctest --test-dir build -C Debug --output-on-failure`.
- `Game.exe <TestProject.nullus>` latest smoke log: `App/Win64_Debug_Runtime_Static/2026-04-08_11-48-17.log`. The reviewed `Unsupported RangeType` / `D3D12SerializeRootSignature failed` error is gone, which confirms the original root-signature corruption is fixed.
- Added `Runtime/Rendering/RHI/Backends/DX12/DX12TextureUploadUtils.*`, `Tests/Unit/DX12TextureUploadUtilsTests.cpp`, and an explicit DX12 initial-data upload path in `DX12ExplicitDeviceFactory.cpp`; the focused upload-plan tests pass.
- Uploading and transitioning the default cubemap texture alone was **not** sufficient: `App/Win64_Debug_Runtime_Static/2026-04-08_13-57-35.log` still removed the device immediately after the skybox material bound `cubeTex`.
- Added `Runtime/Rendering/RHI/Backends/DX12/DX12TextureViewUtils.*`, `Tests/Unit/DX12TextureViewUtilsTests.cpp`, and updated `NativeDX12TextureView` creation so cubemaps now create `D3D12_SRV_DIMENSION_TEXTURECUBE` SRVs while sampled-only cubemaps no longer create RTV/DSV views.
- With the `DX12TextureView` fix in place and the real `cubeTex` path restored, `Game.exe <TestProject.nullus>` latest smoke log `App/Win64_Debug_Runtime_Static/2026-04-08_14-30-29.log` survived a 12-second smoke window with repeated `Present returned hr=0`; the same log also shows `[ForwardSceneRenderer] Parsed scene drawables: opaque=0, transparent=0, skybox=1`, so the procedural-sky path is now stable without the earlier `Material.cpp` skip.
- Latest direct `Editor.exe --backend dx12 <TestProject.nullus>` smoke survived a 12-second window in the 2026-04-08 follow-up validation pass (`EXITCODE=STILL_RUNNING` before manual termination), so the prior startup access violation is no longer reproduced in this short-run check.

**Checkpoint**: DX12 no longer fails at the reviewed root-signature construction point.

---

## Phase 4: User Story 6 - Backend Creation And Fallback Hardening (Priority: P1)

**Goal**: Prevent unsupported or unavailable backends from crashing through null `RHIDevice` paths.

**Independent Test**: Requesting an unsupported backend falls back or reports unsupported without dereferencing `Driver::m_impl->explicitDevice`.

- [x] T020 [US6] Make requested backend logging accurate in `Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp`
- [x] T021 [US6] Make unsupported backend outcomes explicit in `Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp`
- [x] T022 [US6] Guard `Driver::EvaluateEditorMainRuntimeFallback()` against null `explicitDevice` in `Runtime/Rendering/Context/Driver.cpp`
- [x] T023 [US6] Guard `Driver::EvaluateGameMainRuntimeFallback()` against null `explicitDevice` in `Runtime/Rendering/Context/Driver.cpp`
- [x] T024 [US6] Guard `Driver::GetEditorPickingReadbackWarning()` and `Driver::GetActiveGraphicsBackend()` against null `explicitDevice` in `Runtime/Rendering/Context/Driver.cpp`
- [x] T025 [US6] Update Editor startup fallback handling in `Project/Editor/Core/Context.cpp` so backend creation failure is explicit
- [x] T026 [US6] Update Game startup fallback handling in `Project/Game/Core/Context.cpp` so backend creation failure is explicit
- [x] T027 [P] [US6] Extend `Tests/Unit/GraphicsBackendUtilsTests.cpp` or add a focused driver-access test to cover unsupported/not-ready backend fallback behavior

**2026-04-08 null-device hardening evidence**

- `CreateRhiDevice` now logs the requested backend with concrete names and emits explicit unsupported reasons for `METAL` and `NONE`.
- `Driver::EvaluateEditorMainRuntimeFallback`, `Driver::EvaluateGameMainRuntimeFallback`, `Driver::GetEditorPickingReadbackWarning`, and `Driver::GetActiveGraphicsBackend` now handle `explicitDevice == nullptr` safely.
- Added `Tests/Unit/DriverNullDeviceFallbackTests.cpp` to cover missing-device fallback decisions and backend/reporting behavior.
- `Project/Editor/Core/Context.cpp` and `Project/Game/Core/Context.cpp` now fail startup explicitly (with clear error text) when backend creation and OpenGL fallback both fail, instead of continuing into late null-device crash paths.
- `Project/Game/Main.cpp` now catches startup exceptions and returns `EXIT_FAILURE` with explicit stderr output.
- Validation: `cmake -S . -B build`, `cmake --build build --config Debug -- /m:1`, `NullusUnitTests --gtest_filter=DriverNullDeviceFallbackTests.*`, `NullusUnitTests --gtest_filter=GraphicsBackendUtilsTests.*`, and `NullusUnitTests --gtest_filter=DX12TextureViewUtilsTests.ReturnsInvalidSrvHandleForZeroSizedTextureView` all pass.
- Runtime smoke: `Editor.exe --backend dx12 <TestProject.nullus>` and `Game.exe` both survived a 10-second window (`STILL_RUNNING`) before manual termination.

**Checkpoint**: Backend creation failure becomes a controlled state, not a crash path.

---

## Phase 5: User Story 6 - Game CLI And Backend Validation Tooling (Priority: P1)

**Goal**: Make backend validation trustworthy for both Editor and Game.

**Independent Test**: `Game.exe --backend <name> <project>` routes to the requested backend or logs explicit fallback behavior tied to that request.

- [x] T028 [US6] Add backend override parsing for `--backend` and `-b` in `Project/Game/Main.cpp`
- [x] T029 [US6] Add project path parsing in `Project/Game/Main.cpp` matching the Editor's `.nullus` file behavior where practical
- [x] T030 [US6] Thread Game backend and project overrides through `Project/Game/Core/Application.*` and `Project/Game/Core/Context.*`
- [x] T031 [P] [US6] Add focused launch-options coverage in `Tests/Unit/` for Game backend parsing if the parsing helper can be made testable
- [x] T032 [US6] Fix the PowerShell parse failure in `verify_all_backends.ps1`
- [x] T033 [US6] Update `verify_backends_simple.ps1` to report requested backend, per-process exit/survival status, and per-process log path
- [x] T034 [US6] Update `verify_all_backends.ps1` to report requested backend, per-process exit/survival status, and per-process log path
- [x] T035 [US6] Re-run `verify_backends_simple.ps1 -Backend dx12` and confirm Game no longer silently ignores the backend request
- [x] T036 [US6] Re-run `verify_backends_simple.ps1 -Backend vulkan` and confirm Game no longer silently ignores the backend request

**2026-04-08 Game CLI wiring evidence**

- `Project/Game/Main.cpp` now parses `--backend` / `-b` and an optional `project_path` argument (`.nullus` file or project directory), with explicit usage/help text and invalid-option failure.
- Backend/project overrides now flow through `Project/Game/Core/Application.*` into `Project/Game/Core/Context.*`.
- `Game::Context` project resolution now accepts command-line project override first, then keeps the previous environment/package/search fallback order.
- Validation smoke: `Game.exe --backend dx12 <TestProject.nullus>` and `Game.exe --backend vulkan <TestProject.nullus>` both survived an 8-second window (`STILL_RUNNING`) before manual termination.
- Script validation output now includes `RequestedBackend`, per-process `Status`, per-process `ExitCode`, and per-process `LogPath`.
- Log verification confirms backend routing is no longer silent: `2026-04-08_16-26-08.log` includes `CreateRhiDevice: requested backend = DX12`, and `2026-04-08_16-26-55.log` includes `CreateRhiDevice: requested backend = Vulkan`.
- `Project/Game/LaunchArgs.*` now exposes the launch parsing helper used by `Project/Game/Main.cpp`, and `Tests/Unit/GameLaunchArgsTests.cpp` covers backend override parsing, project-path parsing, `--capture-after-frames`, invalid backend rejection, and `--help`.
- Targeted validation now passes with `Build/bin/Debug/NullusUnitTests.exe --gtest_filter=GameLaunchArgsTests.*:GraphicsBackendUtilsTests.*:DriverNullDeviceFallbackTests.*:UIAndToolingBackendAwarenessTests.*` (25 tests, all passed).

**Checkpoint**: Smoke scripts are usable evidence sources again.

---

## Phase 6: User Story 6 - Truthful Backend Support Matrix (Priority: P1)

**Goal**: Align implementation, capability reporting, UI bridge behavior, docs, and tasks with actual backend support.

**Independent Test**: Each backend is either validated as supported or explicitly reported unsupported/gated with no hidden crash path.

- [x] T037 [US5] Reopen DX11 completion in `Runtime/Rendering/RHI/Backends/DX11/DX11ExplicitDeviceFactory.cpp` by either implementing sampler, binding layout, binding set, and pipeline layout support or gating DX11 unsupported until those pieces are implemented
- [x] T038 [US6] Investigate the current `Editor.exe --backend opengl <TestProject.nullus>` startup failure and either fix the OpenGL path or gate OpenGL unsupported in the current phase
- [x] T039 [US5] Mark Metal unsupported explicitly for current non-Apple builds in backend creation, fallback policy, and support text
- [x] T040 [US2] Align `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` bridge creation with the truthful backend support matrix
- [x] T041 [US6] Align `Runtime/Rendering/Settings/GraphicsBackendUtils.h` support descriptions and fallback decisions with the truthful backend support matrix
- [x] T042 [P] [US6] Update `Tests/Unit/UIAndToolingBackendAwarenessTests.cpp` to match the truthful UI bridge support matrix
- [x] T043 [P] [US6] Update `Tests/Unit/GraphicsBackendUtilsTests.cpp` to match the truthful backend support matrix

**Checkpoint**: No backend is documented or reported as supported beyond its current runtime evidence.

**2026-04-13 truthful backend matrix evidence**

- `Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp` now gates `DX11` and `OpenGL` unsupported on the current Windows validation matrix before device creation; `Metal` remains explicitly unsupported on non-Apple builds.
- `Runtime/Rendering/Settings/GraphicsBackendUtils.h` now keeps the current Windows support matrix truthful: `DX12` and `Vulkan` remain supported, while `DX11`, `OpenGL`, and `Metal` surface explicit unsupported warnings and no longer pretend a validated OpenGL fallback exists.
- `Runtime/UI/UIManager.cpp` and `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` now stop bootstrapping renderer backends for gated runtimes on the current Windows matrix, so unsupported backends resolve to `NullUIBridge` behavior instead of drifting into misleading UI setup.
- `Tests/Unit/GraphicsBackendUtilsTests.cpp`, `Tests/Unit/UIAndToolingBackendAwarenessTests.cpp`, and `Tests/Unit/DriverNullDeviceFallbackTests.cpp` now cover the truthful support matrix, and the targeted 25-test backend/tooling suite passes.
- `powershell -ExecutionPolicy Bypass -File .\verify_all_backends.ps1` on 2026-04-13 now reports `dx12` and `vulkan` as `StillRunning`, while `dx11` and `opengl` exit cleanly (`EditorExitCode=0`, `GameExitCode=1`) instead of returning `-1073741819` access violations.
- Latest authoritative unsupported-backend Game logs are `App/Win64_Debug_Runtime_Static/2026-04-13_20-27-21.log` for DX11 and `App/Win64_Debug_Runtime_Static/2026-04-13_20-27-46.log` for OpenGL; both record explicit unsupported messaging and explicit startup failure.

---

## Phase 7: Docs, Spec, And Final Validation

**Purpose**: Close the stabilization pass with artifacts that match the code and evidence.

- [x] T044 Update `Docs/Rendering/RHIMultiBackendArchitecture.md` to describe the validated backend matrix, unsupported backends, and required evidence gates
- [x] T045 Sync `specs/002-rhi-framework-cleanup/spec.md` with the 2026-04-08 runtime review and US6 stabilization scope
- [x] T046 Sync `specs/002-rhi-framework-cleanup/plan.md` with the recovery execution plan
- [x] T047 Sync `specs/002-rhi-framework-cleanup/tasks.md` with completed foundation work and reopened stabilization tasks
- [x] T048 Rebuild with `cmake --build build --config Debug` after stabilization code changes
- [x] T049 Run `ctest --test-dir build -C Debug --output-on-failure` after stabilization code changes
- [x] T050 Run `powershell -ExecutionPolicy Bypass -File .\verify_all_backends.ps1` after script and runtime routing fixes
- [x] T051 [P] Run RenderDoc capture for every backend still claimed supported after smoke survival and record capture path or reason skipped

**2026-04-08 late validation evidence**

- Post-fix validation passed again with `cmake --build build --config Debug`, `ctest --test-dir build -C Debug --output-on-failure`, and targeted rebuilds of `Game` and `Editor` during smoke verification.
- `Game.exe <TestProject.nullus>` latest stable DX12 smoke log is `App/Win64_Debug_Runtime_Static/2026-04-08_14-30-29.log`; the process survived 12 seconds until terminated manually for the smoke window.
- Latest `Editor.exe --backend dx12 <TestProject.nullus>` smoke survived a 12-second window (`EXITCODE=STILL_RUNNING`) and only stopped due to manual termination for smoke control. No fresh `.log` file was emitted in that forced-stop run, so ongoing evidence should continue to use both process-survival output and log-path checks.
- `powershell -ExecutionPolicy Bypass -File .\verify_all_backends.ps1` now executes end-to-end and reports per-process status/exit-code/log-path fields. Latest run summary: `dx12` and `vulkan` survived smoke windows for both `Editor` and `Game`, while `dx11` and `opengl` exited with `-1073741819`.
- `py -3 Tools/RenderDoc/renderdoc_runner.py --target game --backend dx12 --capture --capture-after-frames 1 --timeout 60` and `--capture-after-frames 180 --timeout 120` both still report `capture not found before timeout`, but RenderDoc capture files were written to `Build/RenderDocCaptures/game/dx12/game_dx12_DX12_frame1.rdc` and `Build/RenderDocCaptures/game/dx12/game_dx12_DX12_frame180.rdc`.
- `py -3 Tools/RenderDoc/rdc_analyze.py Build/RenderDocCaptures/game/dx12/game_dx12_DX12_frame180.rdc` confirms the Game capture is inspectable as `D3D12` and that the focus draw binds `cubeTex`; T051 remains open because the runner completion/reporting path is still misleading and the overall backend support matrix is not finalized yet.
- Added scene-output sampling stabilization for explicit backends:
  `Runtime/Engine/Rendering/FrameGraphSceneTargets.h` now marks scene color targets with `Sampled|ColorAttachment`, `Runtime/Rendering/Core/ABaseRenderer.cpp` now transitions offscreen output textures to `ShaderRead` before explicit frame submission, and `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` now remaps DX12 UI texture SRVs into the bridge-owned shader-visible heap used by ImGui.
- Added `Tests/Unit/FrameGraphSceneTargetsTests.cpp`; the new test fails on old behavior and now passes with the sampling-usage fix (`NullusUnitTests --gtest_filter=FrameGraphSceneTargetsTests.*`).
- Added explicit-frame stabilization for DX12 and Vulkan editor/game rendering paths:
  `Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp` now records and restores attachment transitions for all render-pass attachments (not only backbuffer passes), `Runtime/Rendering/Context/Driver.cpp` now respects `acquireSwapchainImage` in `BeginExplicitFrame()` and keeps on-demand acquire in `PresentSwapchain()`, and `Project/Editor/Panels/SceneView.cpp` now uses the safer scene renderer path for DX12/Vulkan with debug-only descriptor logic guarded behind `DebugSceneRenderer` checks.
- Revalidation run at 2026-04-08 17:44:
  `powershell -ExecutionPolicy Bypass -File .\verify_backends_simple.ps1 -Backend dx12` and `-Backend vulkan` both reported `EditorStatus: StillRunning` and `GameStatus: StillRunning`.
- Latest DX12 smoke log `App/Win64_Debug_Runtime_Static/2026-04-08_17-43-56.log` shows backend override + `CreateRhiDevice: requested backend = DX12`, repeated `Parsed scene drawables: ... skybox=1`, and repeated `NativeDX12Queue::Present: Present returned hr=0` with no `DEVICE_REMOVED`/`DEVICE_HUNG` signatures.
- Latest Vulkan smoke log `App/Win64_Debug_Runtime_Static/2026-04-08_17-44-21.log` shows backend override + `CreateRhiDevice: requested backend = Vulkan`, successful swapchain creation, repeated scene-drawable parsing with skybox, and repeated `Game::PostUpdate: calling PresentSwapchain` without crash/assert signatures.
- Targeted regression suite still passes after the above frame-path changes:
  `Build/bin/Debug/NullusUnitTests.exe --gtest_filter=FrameGraphSceneTargetsTests.*:UIAndToolingBackendAwarenessTests.*:DX12*` (15 tests, all passed).

**2026-04-13 final validation evidence**

- Final incremental validation after the truthful-matrix and launch-args work passed with `cmake --build build --config Debug -- /m:1` and `ctest --test-dir build -C Debug --output-on-failure`.
- `powershell -ExecutionPolicy Bypass -File .\verify_all_backends.ps1` was re-run after the final `Game/Main.cpp` launch-args refactor; results stayed consistent: `dx12` and `vulkan` still survive the smoke window for both `Editor` and `Game`, while `dx11` and `opengl` now exit in a controlled way instead of crashing.
- `rdc doctor` now passes on the validation workstation, confirming the RenderDoc CLI and replay stack are available.
- Supported-backend captures were refreshed at `Build/RenderDocCaptures/game/dx12/game_dx12_DX12_capture.rdc` and `Build/RenderDocCaptures/game/vulkan/game_vulkan_Vulkan_capture.rdc`.
- `py -3 Tools/RenderDoc/rdc_analyze.py Build/RenderDocCaptures/game/dx12/game_dx12_DX12_capture.rdc` confirms an inspectable `D3D12` capture with one color pass and a sampled `cubeTex` draw.
- `py -3 Tools/RenderDoc/rdc_analyze.py Build/RenderDocCaptures/game/vulkan/game_vulkan_Vulkan_capture.rdc` confirms the Vulkan capture opens correctly; the latest startup-frame capture still lands before the first draw (`Events: 3`, `Draws: 0`), so the capture path is proven even though deeper Vulkan frame analysis may still prefer a later capture frame.

**2026-04-14 completion-pass evidence**

- Fresh completion verification passed again with `cmake -S . -B build`, `cmake --build build --config Debug -- /m:1`, and `ctest --test-dir build -C Debug --output-on-failure`.
- `powershell -ExecutionPolicy Bypass -File .\verify_all_backends.ps1` on 2026-04-14 kept the truthful support matrix stable: `dx12` and `vulkan` survived the smoke window for both `Editor` and `Game`, while `dx11` and `opengl` exited cleanly with `EditorExitCode=0` and `GameExitCode=1`.
- Latest 2026-04-14 smoke logs were `App/Win64_Debug_Runtime_Static/2026-04-14_09-25-38.log` for DX12 editor survival, `App/Win64_Debug_Runtime_Static/2026-04-14_09-34-36.log` for DX12 game survival / DX11 editor gating, `App/Win64_Debug_Runtime_Static/2026-04-14_09-34-48.log` for DX11 game gating / Vulkan editor survival, `App/Win64_Debug_Runtime_Static/2026-04-14_09-35-01.log` for Vulkan game survival / OpenGL editor gating, and `App/Win64_Debug_Runtime_Static/2026-04-14_09-35-13.log` for OpenGL game gating.
- Editor resize/tooling follow-up coverage now includes `Tests/Unit/ResizeRefreshPolicyTests.cpp`, `Tests/Unit/PanelWindowHookTests.cpp`, and `Tests/Unit/CanvasDockspaceBackgroundTests.cpp`; the updated `NullusUnitTests` binary was rebuilt and the aggregate `ctest` run passed on the completion pass.

**Checkpoint**: The spec bundle, architecture doc, build/test output, smoke results, and supported backend matrix agree.

---

## Dependencies & Execution Order

### Phase Dependencies

| Phase | Depends On | Reason |
|-------|------------|--------|
| Phase 1 - Baseline Runtime Evidence | None | Establishes current truth before fixing |
| Phase 2 - DX12 Runtime Stabilization | Phase 1 | Needs reproducible DX12 failure evidence |
| Phase 3 - Backend Creation/Fallback Hardening | Phase 1 | Can proceed after baseline; independent from DX12 details |
| Phase 4 - Game CLI/Validation Tooling | Phase 1 | Needed before Game backend smoke evidence is trusted |
| Phase 5 - Truthful Backend Support Matrix | Phases 2-4 | Needs real crash/fallback/routing results |
| Phase 7 - Docs/Spec/Final Validation | Phases 2-6 | Must reflect the final stabilized code state |

### Critical Path

```text
Phase 1 → Phase 2 → Phase 3 → Phase 4 → Phase 6 → Phase 7
```

### Parallel Opportunities

- T015, T027, T031, T042, T043, and T051 can run in parallel with non-overlapping implementation work once their prerequisites are satisfied.
- Phase 3 fallback hardening and Phase 4 Game CLI parsing can proceed in parallel after Phase 1 because they touch different product paths.
- DX11/Metal/OpenGL truthfulness work should wait until fallback and script evidence is reliable.

## Implementation Strategy

### MVP Stabilization Order

1. **DX12 crash fix**: Complete T014-T019 first because it blocks the default Windows backend path.
2. **Null-device fallback hardening**: Complete T020-T027 so unsupported backend selection becomes deterministic.
3. **Game CLI and scripts**: Complete T028-T036 so runtime validation evidence is trustworthy.
4. **Backend matrix cleanup**: Complete T037-T043 so support statements match implementation.
5. **Final evidence**: Complete T044-T051 before claiming the migration is complete.

### Validation Rule

Build success and unit-test success are necessary but not sufficient. Any backend support claim must be backed by direct smoke evidence, and RenderDoc evidence is preferred for final supported explicit backend claims.
