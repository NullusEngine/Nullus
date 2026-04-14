# Rendering Stabilization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stabilize the current rendering/RHI migration so backend selection, startup, and baseline scene rendering are trustworthy again on the actively supported backend matrix.

**Architecture:** Treat this as a stabilization pass on top of the in-flight RHI cleanup. Fix the concrete startup/rendering crashes first, then make backend readiness/capability reporting truthful, then repair validation entrypoints and bring spec/docs/tasks back in sync with the real code state.

**Tech Stack:** C++17, CMake, GoogleTest, Formal RHI backends (`DX12`, `Vulkan`, `DX11`, `OpenGL`), Editor/Game launchers, PowerShell backend smoke scripts.

---

## Scope

This plan covers the concrete issues confirmed during the 2026-04-08 rendering review:

1. `DX12` root-signature / pipeline-layout construction uses invalid lifetime for descriptor-range storage and fails at runtime.
2. Unsupported or incomplete backends can still flow through `Driver` and then crash on null `explicitDevice` access.
3. `DX11` is marked as effectively complete in plan/tasks, but critical Formal RHI objects still return `nullptr`.
4. `Game.exe` does not actually parse `--backend` or project-path arguments, so backend smoke scripts produce misleading results.
5. Backend verification scripts and architecture/task documents are out of sync with the real backend matrix.

## File Map

**Likely code files**
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/Context/Driver.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/Settings/GraphicsBackendUtils.h`
- Modify: `d:/VSProject/Nullus/Project/Editor/Core/Context.cpp`
- Modify: `d:/VSProject/Nullus/Project/Game/Core/Context.cpp`
- Modify: `d:/VSProject/Nullus/Project/Game/Main.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/RHI/Backends/DX11/DX11ExplicitDeviceFactory.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/RHI/Backends/OpenGL/OpenGLExplicitDeviceFactory.cpp`

**Likely test files**
- Modify: `d:/VSProject/Nullus/Tests/Unit/GraphicsBackendUtilsTests.cpp`
- Modify: `d:/VSProject/Nullus/Tests/Unit/UIAndToolingBackendAwarenessTests.cpp`
- Create if needed: `d:/VSProject/Nullus/Tests/Unit/GameLaunchOptionsTests.cpp`
- Create if needed: `d:/VSProject/Nullus/Tests/Unit/DX12PipelineLayoutTests.cpp`

**Validation/docs/spec files**
- Modify: `d:/VSProject/Nullus/verify_backends_simple.ps1`
- Modify: `d:/VSProject/Nullus/verify_all_backends.ps1`
- Modify: `d:/VSProject/Nullus/Docs/Rendering/RHIMultiBackendArchitecture.md`
- Modify: `d:/VSProject/Nullus/specs/002-rhi-framework-cleanup/plan.md`
- Modify: `d:/VSProject/Nullus/specs/002-rhi-framework-cleanup/tasks.md`

## Phase Order

1. Baseline capture and failing reproduction
2. DX12 crash fix
3. Backend creation / fallback hardening
4. Game CLI and smoke-test correctness
5. Backend matrix truthfulness (`DX11`, `OpenGL`, `Metal`, UI bridge)
6. Validation rerun and spec/docs/task sync

## Task 1: Capture A Clean Baseline

**Files:**
- Modify: none
- Test: runtime logs under `d:/VSProject/Nullus/App/Win64_Debug_Runtime_Static/`

- [ ] **Step 1: Rebuild the current tree**

Run:

```powershell
cmake --build build --config Debug
```

Expected: `Editor.exe`, `Game.exe`, and `NullusUnitTests.exe` rebuild successfully.

- [ ] **Step 2: Re-run automated tests**

Run:

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: existing unit tests pass before code changes.

- [ ] **Step 3: Reproduce the reviewed runtime failures**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\verify_backends_simple.ps1 -Backend dx12
powershell -ExecutionPolicy Bypass -File .\verify_backends_simple.ps1 -Backend dx11
powershell -ExecutionPolicy Bypass -File .\verify_backends_simple.ps1 -Backend vulkan
powershell -ExecutionPolicy Bypass -File .\verify_backends_simple.ps1 -Backend opengl
```

Expected:
- `Editor --backend vulkan <project>` survives the smoke window.
- `Editor --backend dx12/dx11/opengl <project>` currently exits.
- `Game` results are known-bad until CLI parsing is fixed.

- [ ] **Step 4: Save baseline evidence**

Record:
- the latest app log names
- the DX12 root-signature error
- the current Editor/Game survival matrix

Expected: later phases can compare against the same runtime evidence.

## Task 2: Fix The DX12 Root-Signature Lifetime Bug

**Files:**
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp`
- Create if needed: `d:/VSProject/Nullus/Tests/Unit/DX12PipelineLayoutTests.cpp`
- Test: runtime `DX12` launch via `Editor.exe` and `Game.exe`

- [ ] **Step 1: Extract or isolate descriptor-table construction into owned storage**

Implementation target:
- stop storing `pDescriptorRanges` pointers into `D3D12_ROOT_PARAMETER` from a loop-local `std::vector`
- keep one owning container alive until `D3D12SerializeRootSignature()` returns

Expected: root-parameter descriptors reference stable memory.

- [ ] **Step 2: Add a focused test around layout/range grouping if extraction is practical**

Preferred test target:
- pure helper that converts `RHIPipelineLayoutDesc` into grouped DX12 descriptor ranges
- assert the grouped ranges match expected `type/registerSpace/baseRegister/count`

Expected: no need to boot the renderer just to verify grouping logic.

- [ ] **Step 3: Rebuild and run the focused unit test**

Run:

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure -R "NullusUnitTests"
```

Expected: compile succeeds and the new/updated test passes.

- [ ] **Step 4: Re-run a DX12 game smoke test**

Run:

```powershell
$exe = 'D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\Game.exe'
$wd = 'D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static'
$proj = 'D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\TestProject\TestProject.nullus'
$proc = Start-Process -FilePath $exe -ArgumentList "`"$proj`"" -WorkingDirectory $wd -PassThru
Start-Sleep 6
$proc.Refresh()
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
```

Expected: the previous `Unsupported RangeType` / PSO creation failure no longer appears in the latest app log.

## Task 3: Harden Backend Creation And Fallback Paths

**Files:**
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/Context/Driver.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/Settings/GraphicsBackendUtils.h`
- Modify: `d:/VSProject/Nullus/Project/Editor/Core/Context.cpp`
- Modify: `d:/VSProject/Nullus/Project/Game/Core/Context.cpp`
- Modify: `d:/VSProject/Nullus/Tests/Unit/GraphicsBackendUtilsTests.cpp`

- [ ] **Step 1: Make unsupported backends explicit at factory level**

Implementation target:
- `CreateRhiDevice()` must log the requested backend correctly
- unsupported backends (`Metal` on current Windows build, and any backend intentionally not compiled) must return a clearly unsupported state or fail in a way callers can branch on deliberately

Expected: no silent “warning then continue blindly” path remains.

- [ ] **Step 2: Guard `Driver` against null device access**

Implementation target:
- `EvaluateEditorMainRuntimeFallback()`
- `EvaluateGameMainRuntimeFallback()`
- `GetEditorPickingReadbackWarning()`
- `GetActiveGraphicsBackend()`

Expected: these functions handle `explicitDevice == nullptr` without dereferencing it.

- [ ] **Step 3: Make Editor/Game fallback decisions null-safe**

Implementation target:
- if requested backend cannot create a device, fallback evaluation should still work
- product code should either fall back to `OpenGL` intentionally or abort with a clear message before creating the rest of the render stack

Expected: unsupported backend selection is deterministic, not crash-driven.

- [ ] **Step 4: Extend backend capability tests**

Add/update tests to cover:
- null / unsupported backend path
- fallback messaging for not-ready backend
- no null dereference requirement for fallback helpers

Expected: the failure mode becomes covered by unit tests.

## Task 4: Fix Game Launch Argument Handling

**Files:**
- Modify: `d:/VSProject/Nullus/Project/Game/Main.cpp`
- Modify: `d:/VSProject/Nullus/Project/Game/Core/Context.cpp`
- Create if needed: `d:/VSProject/Nullus/Tests/Unit/GameLaunchOptionsTests.cpp`
- Modify: `d:/VSProject/Nullus/verify_backends_simple.ps1`

- [ ] **Step 1: Define a minimal `Game` launch-options model**

Implementation target:
- backend override
- optional project file override
- RenderDoc settings

Expected: `Game` accepts the same essential runtime-routing inputs that the Editor smoke flow depends on.

- [ ] **Step 2: Parse `--backend` and project path in `Project/Game/Main.cpp`**

Implementation target:
- preserve existing RenderDoc flags
- wire backend/project override into `Game::Core::Application` and/or environment-based context resolution

Expected: `Game.exe --backend vulkan <project>` actually reaches the requested backend.

- [ ] **Step 3: Add at least one focused launch-options test**

Preferred assertions:
- backend aliases parse correctly for the game path
- project path override reaches the resolver without ambiguity

Expected: smoke scripts stop being the only proof of argument handling.

- [ ] **Step 4: Re-run backend smoke for `Game`**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\verify_backends_simple.ps1 -Backend dx12
powershell -ExecutionPolicy Bypass -File .\verify_backends_simple.ps1 -Backend vulkan
```

Expected: latest log backend matches the requested backend instead of always drifting to the project default.

## Task 5: Restore A Truthful Backend Support Matrix

**Files:**
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/RHI/Backends/DX11/DX11ExplicitDeviceFactory.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/RHI/Backends/OpenGL/OpenGLExplicitDeviceFactory.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp`
- Modify: `d:/VSProject/Nullus/Runtime/Rendering/Settings/GraphicsBackendUtils.h`
- Modify: `d:/VSProject/Nullus/Tests/Unit/UIAndToolingBackendAwarenessTests.cpp`
- Modify: `d:/VSProject/Nullus/Tests/Unit/GraphicsBackendUtilsTests.cpp`

- [ ] **Step 1: Decide short-term truth for each backend**

Use this stabilization rule:
- `Vulkan`: supported and validated when smoke/runtime checks pass
- `DX12`: supported only after the crash fix and smoke verification pass
- `OpenGL`: supported only if startup path survives smoke and capability flags match reality
- `DX11`: mark incomplete / unsupported for main runtime until missing binding objects and UI path are implemented
- `Metal`: mark unsupported on the current Windows build, not “implicitly maybe”

Expected: no backend is advertised beyond current evidence.

- [ ] **Step 2: Align runtime capability flags with real implementation**

Implementation target:
- `backendReady`
- `supportsCurrentSceneRenderer`
- `supportsOffscreenFramebuffers`
- `supportsUITextureHandles`
- `supportsDepthBlit`
- `supportsCubemaps`

Expected: fallback policy reflects actual feature completeness, not aspirational status.

- [ ] **Step 3: Align UI bridge behavior with backend support**

Implementation target:
- if `DX11` still uses `NullUIBridge`, corresponding runtime support text and capability reporting must say so
- if `OpenGL` is kept as fallback, verify its UI path is actually usable

Expected: UI startup behavior and backend descriptions agree.

- [ ] **Step 4: Investigate and fix `OpenGL` editor crash or explicitly gate it**

Run:

```powershell
$exe = 'D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\Editor.exe'
$wd = 'D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static'
$proj = 'D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\TestProject\TestProject.nullus'
$proc = Start-Process -FilePath $exe -ArgumentList '--backend','opengl',"`"$proj`"" -WorkingDirectory $wd -PassThru
Start-Sleep 6
$proc.Refresh()
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
```

Expected:
- either the crash is fixed and `OpenGL` survives as fallback,
- or fallback/documentation explicitly stop promising `OpenGL` readiness until repaired.

## Task 6: Repair Validation Tooling And Sync The Planning Artifacts

**Files:**
- Modify: `d:/VSProject/Nullus/verify_all_backends.ps1`
- Modify: `d:/VSProject/Nullus/verify_backends_simple.ps1`
- Modify: `d:/VSProject/Nullus/Docs/Rendering/RHIMultiBackendArchitecture.md`
- Modify: `d:/VSProject/Nullus/specs/002-rhi-framework-cleanup/plan.md`
- Modify: `d:/VSProject/Nullus/specs/002-rhi-framework-cleanup/tasks.md`

- [ ] **Step 1: Fix the broken aggregate backend script**

Implementation target:
- `verify_all_backends.ps1` currently fails to parse before it can report results
- repair the broken string/formatting issue

Expected: the aggregate script runs from repo root without PowerShell parse errors.

- [ ] **Step 2: Make smoke scripts report trustworthy backend evidence**

Implementation target:
- include actual requested backend
- include actual active backend if it can be logged
- separate Editor/Game logs so one process does not hide the other

Expected: smoke output is actionable for future regressions.

- [ ] **Step 3: Update architecture docs to match the real support matrix**

Implementation target:
- remove stale claims about `DX11`, `OpenGL`, and `Metal`
- describe the current validated matrix and any temporary degradation

Expected: docs no longer conflict with runtime behavior.

- [ ] **Step 4: Update `specs/002-rhi-framework-cleanup/plan.md` and `tasks.md`**

Implementation target:
- reopen any task incorrectly marked complete
- add a stabilization follow-up section if needed
- ensure checked items reflect actual evidence, not assumptions

Expected: spec artifacts satisfy the constitution requirement that tasks and validation notes stay in sync with reality.

## Final Validation Gate

- [ ] **Step 1: Rebuild**

```powershell
cmake --build build --config Debug
```

- [ ] **Step 2: Re-run unit tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

- [ ] **Step 3: Re-run backend smoke**

```powershell
powershell -ExecutionPolicy Bypass -File .\verify_all_backends.ps1
```

Expected:
- script runs successfully
- requested backend routing is trustworthy
- validated Editor/Game outcomes are clearly reported

- [ ] **Step 4: Capture runtime evidence for supported backends**

Minimum evidence:
- `Vulkan` Editor smoke survival
- `DX12` smoke after the root-signature fix
- `OpenGL` survival or explicit unsupported gating
- `DX11` unsupported gating or demonstrated runtime fix

- [ ] **Step 5: Optional RenderDoc verification for supported explicit backends**

Run only after the backend survives smoke:

```powershell
$env:NLS_RENDERDOC_CAPTURE='1'
$env:NLS_RENDERDOC_CAPTURE_AFTER_FRAMES='60'
```

Then launch the validated backend and store the capture path in the change notes.

## Done Criteria

This stabilization plan is complete only when all of the following are true:

1. `DX12` no longer fails with the reviewed root-signature / PSO error.
2. Unsupported backends cannot crash through a null-device path.
3. `Game` smoke commands truly honor backend selection.
4. Validation scripts run and report meaningful backend-specific outcomes.
5. Docs/spec/tasks describe the actual backend matrix instead of an aspirational one.

## Suggested Commit Boundaries

1. `fix: stabilize dx12 pipeline layout root signature storage`
2. `fix: harden backend creation and null-device fallback`
3. `fix: wire game backend launch options into runtime selection`
4. `fix: align backend capability reporting with real support`
5. `docs: sync backend validation scripts and rhi cleanup artifacts`
