# Implementation Plan: Prefer Project DXC And Gate DX12 SM6

**Branch**: `033-debugdraw-batching` | **Date**: 2026-05-25 | **Spec**: [spec.md](spec.md)

## Summary

Make DX12 startup fail clearly on machines that cannot run the project's SM6/DXIL path, and make shader compilation resolve the project-bundled DXC before environment or SDK copies. The implementation keeps existing fallback paths, adds an explicit SM6 capability gate immediately after D3D12 device creation, records the confirmed shader model on valid resources, and covers both behaviors with targeted tests.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus shader compiler, DX12 backend, Direct3D 12 headers/runtime
**Storage**: N/A
**Testing**: GoogleTest via `NullusUnitTests` and source-level contract tests where hardware-specific mocking is unavailable
**Target Platform**: Windows DX12 initialization and cross-machine shader compiler lookup
**Project Type**: Desktop engine/editor runtime
**Performance Goals**: No runtime-frame overhead; DXC search remains one-time per compile/reflect call and SM6 check happens once during DX12 resource initialization
**Constraints**: Do not edit generated files; preserve existing DXC fallback behavior; do not claim Vulkan/OpenGL validation; keep product startup failure actionable
**Scale/Scope**: Narrow shader compiler lookup and DX12 device initialization changes with focused tests

## Constitution Check

- Spec scope: Required because this changes rendering backend and shader/toolchain behavior under `Runtime/`.
- Generated boundaries: No files under `Runtime/*/Gen/` are edited.
- Backend validation: Tests validate lookup ordering and DX12 initialization ordering; live DX12 success/failure depends on host hardware.
- Product runtime: Unsupported DX12 SM6 hardware fails early with a diagnostic instead of letting Scene View black-screen after PSO creation failure.
- Evidence path: Add failing regression tests, implement narrow code changes, run targeted unit tests/build if available, then run required quality review.

## Project Structure

```text
specs/035-dx12-sm6-dxc/
├── spec.md
├── plan.md
└── tasks.md

Runtime/Rendering/ShaderCompiler/
├── ShaderCompiler.cpp
└── ShaderCompiler.h

Runtime/Rendering/RHI/Backends/DX12/
├── DX12Device.cpp
└── DX12Device.h

Tests/Unit/
├── ShaderCompilerTests.cpp
└── EditorLaunchArgsTests.cpp
```

**Structure Decision**: Keep DXC discovery local to the shader compiler backend and anchor project-internal lookup to the source tree before using the process working directory. Keep SM6 gating local to DX12 device-resource creation, use the same query for adapter qualification and final resource validation, and expose only small resource-state fields for validation and diagnostics.

## Complexity Tracking

No constitution violations expected. If a lower-shader-model fallback is later desired, it should be a separate spec because it would affect shader profiles, PSO creation, shader cache identity, and backend capability reporting.
