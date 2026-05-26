# Implementation Plan: DebugDrawPass Line Batching

**Branch**: `033-debugdraw-batching` | **Date**: 2026-05-24 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/033-debugdraw-batching/spec.md`

## Summary

Reduce selected-object debug overlay stalls by batching compatible consecutive debug line primitives inside `DebugDrawPass`. The implementation will collect each visible run of same-state line primitives, build one transient line mesh per run, and submit one draw per run while leaving point and triangle primitives on the existing path.

## Technical Context

**Language/Version**: C++20 runtime code, HLSL debug shader
**Primary Dependencies**: Existing Nullus rendering runtime, `CompositeRenderer`, `DebugDrawService`, `Resources::Mesh`, `Resources::Material`
**Storage**: N/A
**Testing**: `NullusUnitTests` with focused `DebugDrawPassTests`, `DebugDrawGeometryTests`, `DebugDrawTypesTests`
**Target Platform**: Nullus editor/runtime rendering path; validation in this task is unit-test backend unless runtime capture is explicitly run later
**Project Type**: Desktop engine/editor runtime
**Performance Goals**: 72 consecutive compatible bounds-sphere line segments produce 1 line draw submission instead of 72
**Constraints**: Do not hand-edit generated files; preserve debug category filtering and one-frame lifetime; do not claim cross-backend performance without backend-specific evidence
**Scale/Scope**: Line-heavy debug helpers such as bounds, grids, and selection overlays; point/triangle batching is out of scope

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Spec-first scope: PASS. Rendering/shader behavior change is tracked in `specs/033-debugdraw-batching/`.
- Validation matches subsystem: PASS with scoped evidence. Unit tests will verify batching behavior; runtime/RenderDoc evidence is recommended for final performance validation but not claimed by this code-only pass.
- Generated boundaries: PASS. No files under `Runtime/*/Gen/` will be edited.
- Incremental delivery: PASS. Work is split into tests, line batching implementation, shader mode support, and focused validation.
- Product runtime preservation: PASS. Existing primitive path remains available and point/triangle rendering is unchanged.

## Project Structure

### Documentation (this feature)

```text
specs/033-debugdraw-batching/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/Rendering/Debug/
├── DebugDrawPass.h
└── DebugDrawPass.cpp

App/Assets/Engine/Shaders/
└── DebugPrimitive.hlsl

Tests/Unit/
├── DebugDrawPassTests.cpp
└── DebugDrawGeometryTests.cpp
```

**Structure Decision**: Keep batching internal to `DebugDrawPass`, reuse existing `Resources::Mesh`/`Material` paths, and expose only a protected virtual test hook for grouped line rendering so behavior can be verified without a GPU backend.

## Phase 0 Research

### Decision: Batch runs use line color, depth mode, and line width

**Rationale**: These fields affect shader constants or PSO state for debug lines. Merging lines across these boundaries would change visible output or render state. Batching only consecutive compatible runs also preserves relative debug primitive ordering around points, triangles, and incompatible lines.

**Alternatives considered**: Include category/lifetime in the batch key. Rejected because visibility and lifetime are already resolved by `CollectVisiblePrimitives()` before rendering.

### Decision: Batch only line primitives in this delivery

**Rationale**: The measured issue comes from selected-object bounds and other line-heavy helpers. Keeping points and triangles on the old path reduces behavioral risk.

**Alternatives considered**: Batch all primitive types immediately. Rejected because triangle fill/wire modes and point sizing need separate state handling and are not the current bottleneck.

### Decision: Use a shader flag to choose vertex-position mode for line batches

**Rationale**: The existing shader derives positions from per-draw uniforms using `SV_VertexID`. Batched lines need per-vertex positions from the generated mesh while existing point/line/triangle rendering should remain compatible.

**Alternatives considered**: Replace the shader entirely with vertex-position rendering. Rejected because existing placeholder meshes and point/triangle paths depend on uniform-driven positions.

### Decision: Use transient CPU-to-GPU line meshes for the first implementation

**Rationale**: This immediately collapses many draw submissions into one per batch while staying inside existing mesh APIs. It is simple to validate and keeps the RHI untouched.

**Alternatives considered**: Add persistent dynamic buffer reuse now. Rejected because it touches lower-level resource lifetime and can be implemented later if profiling shows transient uploads are still costly.

## Phase 1 Design

### Data Model

See [data-model.md](data-model.md) for the line batch key and line segment entities used inside `DebugDrawPass`.

### Contracts

No external API contracts are added. The observable contract is covered by unit tests and existing debug drawing submission APIs.

### Quickstart

See [quickstart.md](quickstart.md) for focused validation commands.

## Constitution Check Post-Design

- Spec-first scope: PASS. Plan artifacts remain in one feature bundle.
- Validation matches subsystem: PASS. Tests cover behavior; runtime capture is recorded as optional performance evidence, not as a claimed result.
- Generated boundaries: PASS. Design avoids generated outputs.
- Incremental delivery: PASS. Line batching can land independently of future persistent buffer reuse.
- Product runtime preservation: PASS. The existing per-primitive path remains available and shader supports both modes.

## Complexity Tracking

No constitution violations require justification.
