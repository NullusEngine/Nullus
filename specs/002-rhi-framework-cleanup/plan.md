# Implementation Plan: RHI Framework Cleanup - Stabilization Sync

**Branch**: `002-rhi-framework-cleanup` | **Date**: 2026-04-08 | **Spec**: [spec.md](./spec.md)
**Input**: Sync the existing RHI cleanup bundle with the 2026-04-08 runtime review and reopened backend stabilization work

## Summary

The original cleanup goal remains: remove the legacy `IRenderDevice` abstraction and keep product rendering on the Formal RHI path. The 2026-04-08 review found that the spec bundle had drifted ahead of runtime evidence, so the active plan is now a stabilization pass before any completion claim:

1. Fix the concrete `DX12` root-signature / pipeline-layout runtime failure.
2. Harden backend creation and fallback so unsupported backends do not crash via null `RHIDevice` access.
3. Repair `Game.exe` backend argument routing and backend smoke scripts so validation results are trustworthy.
4. Reopen `DX11`, `OpenGL`, and `Metal` support claims until their actual runtime state is either implemented or explicitly gated.
5. Keep docs, tasks, and validation evidence synchronized with the real backend matrix.

## Technical Context

**Language/Version**: C++17 with CMake and Visual Studio generator on the current Windows validation environment
**Primary Dependencies**: `Runtime/Rendering/RHI/`, `Runtime/Rendering/Context/`, `Runtime/Rendering/Settings/`, `Runtime/UI/`, `Project/Editor/`, `Project/Game/`
**Testing**: `cmake --build build --config Debug`, `ctest --test-dir build -C Debug --output-on-failure`, backend smoke scripts, and RenderDoc capture for supported explicit backends after smoke survival
**Target Platform**: Current evidence is Windows debug build; non-Windows support claims require separate validation
**Project Type**: C++ game engine with multi-backend renderer
**Performance Goals**: Preserve existing frame loop behavior; no performance claim is valid until the backend survives smoke and targeted runtime checks
**Constraints**: `Editor` and `Game` must fail explicitly or fall back intentionally; they must not rely on crashes to expose unsupported backends
**Scale/Scope**: Stabilization across factory, driver startup, DX12 pipeline layout, UI bridge/capability reporting, Game launch parsing, and verification scripts

## Constitution Check

| Gate | Status | Notes |
|------|--------|-------|
| Spec-First Major Changes | PASS | Existing bundle under `specs/002-rhi-framework-cleanup/` is being updated in place |
| Validation Matches Subsystem | CONDITIONAL | Build/tests pass, but backend runtime evidence is incomplete; this plan adds required smoke and RenderDoc gates |
| Generated Code Boundaries | PASS | No generated files under `Runtime/*/Gen/` are targeted |
| Incremental Verified Delivery | PASS | Stabilization is split into backend, startup, CLI, validation, and documentation phases |
| Product Runtime Preservation | CONDITIONAL | Current review found product startup failures; recovery work must restore explicit fallback or supported runtime evidence before completion |

## Current Evidence Snapshot

### Validated On 2026-04-08

- `cmake --build build --config Debug` completed successfully
- `ctest --test-dir build -C Debug --output-on-failure` completed successfully
- `Editor --backend vulkan <project>` survived a 6-second smoke window on Windows

### Reopened By 2026-04-08 Review

- `DX12` game runtime hit `NativeDX12PipelineLayout: D3D12SerializeRootSignature failed: Unsupported RangeType value ...` during first-frame pipeline creation
- `Editor --backend dx12 <project>`, `Editor --backend dx11 <project>`, and `Editor --backend opengl <project>` exited during smoke startup
- `Game.exe --backend <backend> <project>` is not trustworthy because `Project/Game/Main.cpp` only parses RenderDoc flags today
- `verify_all_backends.ps1` failed to execute because of a PowerShell parse error
- `DX11` still has unimplemented Formal RHI creation paths for sampler, binding layout, binding set, and pipeline layout
- `Metal` has no runnable backend in the current Windows build and must be explicitly unsupported rather than flowing through a null-device startup path

### Follow-Up Evidence After DX12 Stabilization Work

- `Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp` now builds root-signature descriptor tables through owned helper storage, and fresh unit coverage was added for DX12 pipeline-layout and input-layout helpers.
- Fresh `cmake --build build --config Debug` and `ctest --test-dir build -C Debug --output-on-failure` runs still pass after the DX12 follow-up changes.
- Latest Game smoke log `App/Win64_Debug_Runtime_Static/2026-04-08_11-48-17.log` confirms the reviewed `Unsupported RangeType` / `D3D12SerializeRootSignature failed` error is gone.
- `Runtime/Rendering/RHI/Backends/DX12/DX12TextureUploadUtils.*`, `Tests/Unit/DX12TextureUploadUtilsTests.cpp`, and the DX12 initial-texture upload path were added; the focused upload-plan tests pass.
- Uploading and transitioning the default cubemap texture alone was not sufficient: `App/Win64_Debug_Runtime_Static/2026-04-08_13-57-35.log` still removed the device as soon as the procedural skybox material bound `cubeTex`.
- `Runtime/Rendering/RHI/Backends/DX12/DX12TextureViewUtils.*` and `Tests/Unit/DX12TextureViewUtilsTests.cpp` were added, and `NativeDX12TextureView` now builds cubemap SRVs with `D3D12_SRV_DIMENSION_TEXTURECUBE` while avoiding RTV/DSV creation for sampled-only cubemaps.
- With that texture-view fix in place, the real procedural-sky `cubeTex` path is now stable on `Game.exe <TestProject.nullus>`; latest Game smoke log `App/Win64_Debug_Runtime_Static/2026-04-08_14-30-29.log` survived a 12-second smoke window with repeated `Present returned hr=0` and logged `skybox=1`.
- Latest short `Editor --backend dx12 <project>` smoke survived a 12-second window (`EXITCODE=STILL_RUNNING` before manual termination), so the earlier startup access violation is no longer reproduced in this short-run check.
- Null-device startup and fallback handling was hardened in `RHIDeviceFactory.cpp`, `Driver.cpp`, `Project/Editor/Core/Context.cpp`, and `Project/Game/Core/Context.cpp`, with focused coverage added in `Tests/Unit/DriverNullDeviceFallbackTests.cpp`.
- Game launch options now thread backend/project overrides from `Project/Game/Main.cpp` through `Project/Game/Core/Application.*` and `Project/Game/Core/Context.*`, with short `dx12` and `vulkan` smoke runs surviving the validation window.
- `verify_backends_simple.ps1` and `verify_all_backends.ps1` were repaired and now execute with requested-backend, per-process status/exit code, and per-process log-path reporting.
- `py -3 Tools/RenderDoc/renderdoc_runner.py --target game --backend dx12 --capture --capture-after-frames 1 --timeout 60` and `--capture-after-frames 180 --timeout 120` still report `capture not found before timeout`, but `.rdc` files were still written and `py -3 Tools/RenderDoc/rdc_analyze.py Build/RenderDocCaptures/game/dx12/game_dx12_DX12_frame180.rdc` confirms an inspectable D3D12 capture whose focus draw binds `cubeTex`.

## Current State Analysis

### Completed Foundations

- Legacy `IRenderDevice` files and backend wrapper files have been deleted from the active worktree
- `Driver` now stores and uses an explicit Formal RHI device directly
- Descriptor unification and typed native handle migration have been largely completed
- Existing unit tests and current build pass

### Remaining Blocking Work

1. **DX12 Runtime Stabilization Is Partial**: the reviewed root-signature corruption and Game-side cubemap binding crash are fixed, and Editor DX12 now survives a short smoke window, but RenderDoc runner completion still misreports timeout even when captures are written and broader backend-matrix validation is still incomplete.
2. **Overstated Backend Claims**: `DX11`, `OpenGL`, and `Metal` support states are not aligned with implementation and smoke evidence.
3. **Evidence Interpretation Gap**: scripts now run, but backend outcomes still require matrix-level triage and gating decisions before support claims can be finalized.
4. **Docs/Tasks Drift**: previous task checkmarks treated build success as backend completion; runtime evidence invalidated those completion claims.

## Project Structure

```text
specs/002-rhi-framework-cleanup/
├── spec.md              # Updated: includes runtime review, US6, and reopened requirements
├── plan.md              # Updated: current stabilization plan
├── research.md          # Existing Phase 0 output
├── data-model.md        # Existing Phase 1 output
├── quickstart.md        # Existing Phase 1 output
└── tasks.md             # Updated: completed foundation + reopened stabilization tasks
```

### Source Code Areas

```text
Runtime/
├── Rendering/
│   ├── Context/
│   │   └── Driver.cpp                         # Needs null-device hardening
│   ├── Settings/
│   │   └── GraphicsBackendUtils.h             # Needs truthful fallback/capability policy
│   └── RHI/
│       ├── Backends/
│       │   ├── RHIDeviceFactory.cpp           # Needs unsupported-backend behavior and executor cleanup
│       │   ├── DX12/DX12ExplicitDeviceFactory.cpp
│       │   ├── DX11/DX11ExplicitDeviceFactory.cpp
│       │   └── OpenGL/OpenGLExplicitDeviceFactory.cpp
│       └── Utils/RHIUIBridge.cpp              # Needs bridge behavior aligned with support matrix
├── UI/
│   └── UIManager.cpp                          # Needs behavior validated for null/unsupported bridge states
Project/
├── Editor/Core/Context.cpp                    # Needs startup fallback path hardening
└── Game/
    ├── Main.cpp                               # Needs backend/project CLI parsing
    └── Core/Context.cpp                       # Needs backend/project override plumbing
```

## Recovery Phases

### Phase 1: Baseline Capture And Regression Guard

**Goal**: Preserve exact evidence before changing stabilization code.

**Needed Work**:
1. Re-run build and unit tests.
2. Re-run direct Editor smoke for `dx12`, `dx11`, `vulkan`, and `opengl`.
3. Capture latest log names and failure messages.
4. Treat Game backend results as unreliable until CLI routing is fixed.

**Exit Criteria**:
- Baseline evidence is recorded with absolute date and backend names.
- `DX12` root-signature failure is reproducible or a newer failure is documented.

### Phase 2: DX12 Pipeline Layout Stabilization

**Goal**: Remove the root-signature descriptor-range lifetime bug that corrupts `DX12` pipeline layout creation.

**Needed Work**:
1. Keep `D3D12_DESCRIPTOR_RANGE` storage alive until `D3D12SerializeRootSignature()` completes.
2. Prefer extracting descriptor-range grouping into a testable helper if practical.
3. Add focused unit coverage for grouping/lifetime assumptions if the helper can be isolated from D3D12 device creation.
4. Smoke test `DX12` after the fix.

**Exit Criteria**:
- Latest `DX12` startup log no longer contains `Unsupported RangeType value`.
- Build and unit tests pass after the fix.
- Follow-up failures, if any, are recorded separately as new blockers instead of being misattributed to the original root-signature corruption.

### Phase 3: Backend Creation And Fallback Hardening

**Goal**: Unsupported or unavailable backends must not crash through null-device access.

**Needed Work**:
1. Make `CreateRhiDevice()` log the requested backend correctly.
2. Make unsupported backend outcomes explicit for `Metal` and any build-disabled backend.
3. Guard `Driver::EvaluateEditorMainRuntimeFallback()`, `Driver::EvaluateGameMainRuntimeFallback()`, `Driver::GetEditorPickingReadbackWarning()`, and `Driver::GetActiveGraphicsBackend()` against `explicitDevice == nullptr`.
4. Update Editor/Game startup to fall back or abort cleanly when backend creation fails.
5. Add unit coverage for null-device or not-ready backend fallback policy.

**Exit Criteria**:
- Selecting an unsupported backend produces explicit fallback/unsupported behavior.
- No startup path dereferences a null `explicitDevice`.

### Phase 4: Game CLI And Validation Tooling Repair

**Goal**: Backend smoke results must reflect the requested backend.

**Needed Work**:
1. Parse `--backend` and project path in `Project/Game/Main.cpp`.
2. Thread backend/project overrides through `Project/Game/Core/Application` and `Project/Game/Core/Context` or a small launch-options structure.
3. Fix `verify_all_backends.ps1` parse failure.
4. Update `verify_backends_simple.ps1` to report requested backend, actual active backend evidence where available, and separate Editor/Game log paths.

**Exit Criteria**:
- `Game.exe --backend vulkan <project>` does not silently run as the project default backend.
- Backend scripts execute without parser errors and produce useful per-backend evidence.

### Phase 5: Truthful Backend Matrix

**Goal**: Backend capability reporting, UI bridge behavior, documentation, and tasks must agree with reality.

**Needed Work**:
1. Reopen `DX11` completion work or mark `DX11` unsupported until missing Formal RHI objects and UI path are implemented.
2. Investigate `OpenGL` startup failure; fix it or gate `OpenGL` as unsupported in the current phase.
3. Mark `Metal` unsupported explicitly on current non-Apple builds and avoid null-device fallback crashes.
4. Align `RHIUIBridge` behavior and `GraphicsBackendUtils` support descriptions with the supported matrix.

**Exit Criteria**:
- No backend is advertised as supported unless the code path and validation evidence match.
- `DX11`, `OpenGL`, and `Metal` no longer have contradictory task/doc/runtime states.

### Phase 6: Final Validation And Artifact Sync

**Goal**: Close the stabilization work with evidence matched to the rendering subsystem.

**Needed Work**:
1. Run build and unit tests.
2. Run repaired backend smoke scripts.
3. Capture RenderDoc evidence for any explicit backend still claimed supported after smoke survival.
4. Update `Docs/Rendering/RHIMultiBackendArchitecture.md` and this spec bundle with exact final evidence.

**Exit Criteria**:
- `spec.md`, `plan.md`, and `tasks.md` match the code and validation evidence.
- Final summary states exactly which backends are validated, unsupported, or pending.

## Risks And Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| DX12 root-signature fix reveals a later PSO/resource-binding bug | High | Fix one runtime failure at a time; record the next failure separately with logs |
| Null-device hardening hides genuine backend creation failures | Medium | Log backend, requested path, and fallback/unsupported decision explicitly |
| Game CLI changes drift from Editor CLI behavior | Medium | Share parsing helpers or mirror the Editor parsing contract with unit tests |
| OpenGL fallback is not currently reliable | High | Do not assume it is fallback-ready; fix or gate it before using it as product fallback |
| DX11 completion scope grows | High | Either implement the missing Formal RHI objects or mark DX11 unsupported until a separate backend-completion plan is approved |
| Metal support claims remain platform-ambiguous | Medium | State unsupported on non-Apple/current Windows builds and require separate Apple validation before support claims |

## Execution Order

```text
Phase 1: Baseline evidence
    ↓
Phase 2: DX12 root-signature stabilization
    ↓
Phase 3: Backend creation/fallback hardening
    ↓
Phase 4: Game CLI + validation script repair
    ↓
Phase 5: Truthful backend matrix
    ↓
Phase 6: Final validation + docs/spec sync
```

## Validation Commands

```powershell
# Build
cmake --build build --config Debug

# Unit tests
ctest --test-dir build -C Debug --output-on-failure

# Direct Editor smoke examples
$editor = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\Editor.exe"
$wd = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static"
$project = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\TestProject\TestProject.nullus"
Start-Process -FilePath $editor -ArgumentList "--backend","vulkan","`"$project`"" -WorkingDirectory $wd -PassThru

# Repaired backend matrix script
powershell -ExecutionPolicy Bypass -File .\verify_all_backends.ps1

# RenderDoc after smoke survival
$env:NLS_RENDERDOC_CAPTURE = "1"
$env:NLS_RENDERDOC_CAPTURE_AFTER_FRAMES = "60"
```

## Open Questions

No new user decision is required to sync the bundle. Implementation may still require a product decision if the team wants to complete `DX11` and `Metal` immediately instead of gating them unsupported during this stabilization pass.
