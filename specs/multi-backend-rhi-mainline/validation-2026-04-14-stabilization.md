# Validation Note: 2026-04-14 Startup And Backend Gating Stabilization

## Scope

This validation pass covers the selective stabilization slice ported onto `main` from the earlier
RHI cleanup work:

- `Project/Editor/Core/*`
- `Project/Game/Core/*`
- `Project/Game/Main.cpp`
- `Project/Game/LaunchArgs.*`
- `Runtime/Rendering/Context/Driver.*`
- backend-selection and fallback helpers in `Runtime/Rendering/Settings/GraphicsBackendUtils.h`

The intent of this slice is to keep product startup truthful to the current backend support matrix,
preserve explicit startup failure messaging, and keep interactive resize-driven swapchain refresh
behavior stable.

## Automated Validation

### Configure

```powershell
cmake -S . -B build
```

Result: passed on 2026-04-14.

### Full Debug Build

```powershell
cmake --build build --config Debug -- /m:1
```

Result: passed on 2026-04-14.

Note: an earlier build attempt hit transient Windows/MSBuild file-lock errors while compiling
`NullusUnitTests` and `ReflectionTest`. Re-running the affected targets and then re-running the full
build succeeded without source changes.

### Unit Tests

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Result: passed on 2026-04-14.

## Runtime Smoke Evidence

The repository-local `verify_all_backends.ps1` helper referenced during previous iterations is not
present in this checkout. For this pass, backend smoke validation used a direct PowerShell harness
that launched `Editor.exe` and `Game.exe` against `TestProject/TestProject.nullus`, waited 6
seconds, and then recorded whether the process stayed alive or exited immediately.

### Editor

| Backend | Result | Evidence |
|---------|--------|----------|
| `DX12` | Still running after 6s | Process remained alive until the smoke harness terminated it |
| `Vulkan` | Still running after 6s | Process remained alive until the smoke harness terminated it |
| `DX11` | Exited immediately | stderr logged `Editor startup failed: could not create a validated runtime for backend DX11.` |
| `OpenGL` | Exited immediately | stderr logged `Editor startup failed: could not create a validated runtime for backend OpenGL.` |

### Game

| Backend | Result | Evidence |
|---------|--------|----------|
| `DX12` | Still running after 6s | Process remained alive until the smoke harness terminated it |
| `Vulkan` | Still running after 6s | Process remained alive until the smoke harness terminated it |
| `DX11` | Exited immediately | stderr logged `Game startup failed: could not create a validated runtime for backend DX11.` |
| `OpenGL` | Exited immediately | stderr logged `Game startup failed: could not create a validated runtime for backend OpenGL.` |

## Remaining Gaps

- This pass did not capture a new RenderDoc frame.
- This pass did not validate the minimum demo set under `Project/RenderingDemos/`.
- The smoke harness proves startup behavior and backend gating only; it does not prove rendering
  correctness beyond the process reaching a stable running state.
