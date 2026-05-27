# Tasks: Loaded Primitive Rendering

**Input**: Design documents from `/specs/fix-loaded-primitive-rendering/`
**Prerequisites**: plan.md, spec.md

**Tests**: Required. This is a behavior-changing bug fix and follows test-first workflow.

## Phase 1: Setup

- [X] T001 Confirm saved scene object graph contains `builtin:Primitive/Cube` and empty mesh renderer material list in `TestProject/Assets/Scenes/New Scene.scene`

## Phase 2: User Story 1 - Saved Cube Stays Visible (Priority: P1)

**Goal**: A loaded primitive cube with no explicit material still creates a visible render scene draw.

**Independent Test**: Run the targeted render scene cache regression test and verify one opaque drawable is produced.

### Tests for User Story 1

- [X] T002 [US1] Add failing regression test in `Tests/Unit/RenderSceneCacheTests.cpp` for a loaded primitive cube using fallback material with no explicit material references

### Implementation for User Story 1

- [X] T003 [US1] Ensure cold built-in primitive mesh references resolve through `Runtime/Engine/Components/MeshFilter.cpp`
- [X] T004 [US1] Ensure scene fallback material path produces a valid fallback for empty material lists in `Runtime/Engine/Rendering/BaseSceneRenderer.cpp` or `Runtime/Engine/Rendering/RenderScene.cpp`

## Phase 3: Validation And Review

- [X] T005 Run targeted `NullusUnitTests` filters for render scene cache and scene object graph serialization
- [X] T006 Run `/plan-review` quality gate and fix blocking findings

## Dependencies & Execution Order

- T001 before T002
- T002 must fail before T003/T004 implementation
- T005 after implementation
- T006 after targeted verification
