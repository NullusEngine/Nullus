# Implementation Plan: Editor HitProxy Picking

**Branch**: `editor-hitproxy-picking` | **Date**: 2026-06-10 | **Spec**: `specs/editor-hitproxy-picking/spec.md`  
**Input**: Feature specification from `specs/editor-hitproxy-picking/spec.md`

## Summary

Redesign editor picking around a UE-style hit-proxy cache: hover picking reuses compatible readable frames, click picking waits for a fresh compatible frame, and selected-object outline remains separate from the picking buffer. Add diagnostics so traces and FrameInfo explain whether picking rebuilt, reused, skipped, or waited.

## Technical Context

**Language/Version**: C++17, HLSL only if existing shader state requires no new picking shader  
**Primary Dependencies**: Existing Nullus editor renderer, `PickingRenderPass`, `SceneView`, `SceneViewPickingPolicy`, `SelectionOutlineMaskRenderer`, `FrameInfo`/renderer stats  
**Storage**: In-memory per-SceneView/per-renderer picking cache metadata  
**Testing**: `NullusUnitTests` focused policy/contract tests plus targeted editor runtime trace/FrameInfo verification  
**Target Platform**: Windows editor, DX12 first  
**Project Type**: Desktop engine/editor  
**Performance Goals**: Avoid per-frame heavy pickable draw capture during stationary hover and reduce camera-move picking spikes in large scenes  
**Constraints**: Preserve async readback lifecycle, gizmo suppression, text-input blocking, product runtime viability, and generated-file boundaries  
**Scale/Scope**: Large imported prefab scenes with thousands of visible pickable draw sources

## Constitution Check

- **Spec-first major change**: PASS. This rendering/editor behavior change is scoped under `specs/editor-hitproxy-picking/`.
- **Validation matches subsystem**: PASS. Plan includes focused unit tests, trace evidence, FrameInfo diagnostics, and DX12 editor runtime verification.
- **Generated code/backend boundaries**: PASS. No generated files are in scope. DX12 evidence will not be claimed as Vulkan/macOS/Linux proof.
- **Incremental verified delivery**: PASS. Tasks are split into policy tests, cache metadata, render-pass reuse, SceneView request routing, selection separation, and diagnostics.
- **Product runtime preservation**: PASS. The plan preserves existing fallback behavior when picking readback is unsupported or delayed.

## Project Structure

```text
specs/editor-hitproxy-picking/
├── spec.md
├── research.md
├── plan.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── picking-cache-contract.md
└── tasks.md

Project/Editor/Panels/
├── SceneView.cpp
└── SceneViewPickingPolicy.h

Project/Editor/Rendering/
├── PickingRenderPass.h
├── PickingRenderPass.cpp
├── PickingReadbackLifecycle.h
└── SelectionOutlineMaskRenderer.*

Runtime/Rendering/Data/
└── FrameInfo.h

Runtime/Rendering/Core/
└── RendererStats.cpp

Tests/Unit/
├── SceneViewPickingPolicyTests.cpp
├── EditorRenderPathContractTests.cpp
└── RendererStatsTests.cpp
```

**Structure Decision**: Keep implementation in existing editor rendering boundaries. Add small helper structs near `PickingRenderPass` or `SceneViewPickingPolicy` rather than introducing a separate subsystem unless tests prove the policy grows too large.

## Phase 0 Research Output

See `research.md`.

## Phase 1 Design Output

See `data-model.md`, `contracts/picking-cache-contract.md`, and `quickstart.md`.

## Post-Design Constitution Check

- The plan remains a single focused editor picking feature.
- No `Runtime/*/Gen/` files are touched.
- DX12 validation is explicit; cross-backend claims are out of scope.
- Selection outline remains product-visible even if picking cache is disabled.
- The final evidence path is known: policy tests, render path contract tests, renderer stats tests, trace review, and manual Scene View selection smoke.
