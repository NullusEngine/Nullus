# Implementation Plan: Remove DX12 Legacy UI Bridge

**Branch**: `050-remove-dx12-ui-bridge` | **Date**: 2026-06-14 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/050-remove-dx12-ui-bridge/spec.md`

## Summary

Remove the old DX12 UI direct-submit bridge and its fallback selection path so DX12 UI rendering is owned only by the migrated RHI frame-graph overlay flow. Preserve platform/input backends and explicit unsupported-path reporting.

## Technical Context

**Language/Version**: C++17  
**Primary Dependencies**: ImGui, GLFW, Win32, DX12 RHI, existing Nullus rendering framework  
**Storage**: N/A  
**Testing**: NullusUnitTests, source-guard checks, targeted runtime validation  
**Target Platform**: Windows DX12 runtime; build-time unsupported-path behavior for other backends remains explicit  
**Project Type**: desktop-app / game-engine editor runtime  
**Performance Goals**: remove duplicate UI submission and fence waits from the DX12 frame path  
**Constraints**: keep `Runtime/*/Gen/` untouched; preserve platform backend input handling; no silent fallback to the removed bridge  
**Scale/Scope**: narrow renderer/runtime cleanup across `Runtime/Rendering/RHI`, `Runtime/UI`, `Project/Editor`, and `Project/Launcher`

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Spec-first required: satisfied; this is a major runtime/rendering change and has its own spec bundle.
- Generated files protected: no `Runtime/*/Gen/` changes planned.
- Validation must match subsystem: plan requires targeted unit tests and runtime verification for DX12 UI path removal.
- Backend/platform claims must be explicit: only DX12 migrated UI path is being removed; Win32/GLFW platform backends stay intact.
- Product runtime preservation: Editor and Launcher keep running on the migrated UI path or fail closed on unsupported paths.

## Project Structure

### Documentation (this feature)

```text
specs/050-remove-dx12-ui-bridge/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp
Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp
Runtime/UI/UIManager.cpp
Project/Editor/Core/Editor.cpp
Project/Launcher/Core/Launcher.cpp
Tests/Unit/RHIUiOverlaySourceGuardTests.cpp
Tests/Unit/UIAndToolingBackendAwarenessTests.cpp
Tests/Unit/RHIUiOverlayPassTests.cpp
```

**Structure Decision**: This is a focused renderer/runtime cleanup. The implementation stays inside the existing rendering and product runtime folders, with validation documented under the new feature bundle.

## Research Output

- Confirm the removed bridge is still referenced only as a legacy DX12 path.
- Confirm no other backend depends on the DX12 direct-submit bridge for platform input.
- Confirm the migrated overlay path already provides the active DX12 UI rendering route.

## Design Notes

- The legacy DX12 bridge should either be deleted outright or reduced to an unreachable compatibility stub only if compilation needs it during the transition. The target state is no runtime selection of the old path.
- `RHIUIBridge` selection should return null/unsupported for the removed DX12 path when overlay support is present.
- `UIManager`, `Editor`, and `Launcher` should keep platform backend initialization and event handling unchanged.
- Any remaining unsupported backend messaging should be explicit and not masquerade as fallback behavior.

## Complexity Tracking

No constitution violations require justification. This change is a narrow cleanup inside the already migrated UI runtime.
