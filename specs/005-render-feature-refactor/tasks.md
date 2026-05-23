# Tasks: Render Feature Refactor

**Input**: Design documents from `D:/VSProject/Nullus/specs/005-render-feature-refactor/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/render-feature-ownership-contract.md`, `quickstart.md`

**Tests**: This feature explicitly requires validation evidence, so each user story includes focused test tasks plus the smoke/runtime checks referenced by `specs/005-render-feature-refactor/quickstart.md`.

**Organization**: Tasks are grouped by user story so each migration slice can be implemented and validated independently.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no unresolved dependencies)
- **[Story]**: Which user story the task belongs to (`[US1]`, `[US2]`, `[US3]`, `[US4]`)
- Every task includes exact file paths

## Phase 1: Setup (Shared Migration Guardrails)

**Purpose**: Establish the shared refactor scaffolding and validation entry points before moving ownership.

- [X] T001 Add renderer-owned migration scaffolding for compatibility state and shared accessors in `Runtime/Rendering/Core/CompositeRenderer.h` and `Runtime/Rendering/Core/CompositeRenderer.inl`
- [X] T002 [P] Extend the focused refactor validation entry points in `Tests/Unit/CompositeRendererExplicitDrawOrderTests.cpp` and `specs/005-render-feature-refactor/quickstart.md`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Reshape renderer orchestration so later slices can move ownership without reworking draw flow again.

**⚠️ CRITICAL**: No user story work should begin until this phase is complete.

- [X] T003 Define the transitional ownership rules and lifecycle-only boundary in `Runtime/Rendering/Features/ARenderFeature.h` and `Runtime/Rendering/Features/ARenderFeature.cpp`
- [X] T004 [P] Refactor the mandatory draw-preparation path in `Runtime/Rendering/Core/CompositeRenderer.cpp` so core preparation can run without optional feature hooks
- [X] T005 [P] Add regression coverage for explicit draw ordering and compatibility behavior in `Tests/Unit/CompositeRendererExplicitDrawOrderTests.cpp`

**Checkpoint**: Renderer orchestration is ready for ownership migration slices.

---

## Phase 3: User Story 1 - Preserve Frame Rendering While Removing Core Feature Dependency (Priority: P1) 🎯 MVP

**Goal**: Move frame/object draw preparation into renderer-owned state so normal scene rendering no longer depends on `ARenderFeature`.

**Independent Test**: Run a normal scene with the migrated renderer-owned binding path active; camera/object transforms remain correct and mandatory frame/object binding occurs before material binding without relying on an optional feature.

### Tests for User Story 1

- [X] T006 [P] [US1] Add renderer-owned frame/object binding tests in `Tests/Unit/RendererFrameObjectBindingTests.cpp`
- [X] T007 [P] [US1] Add scene-renderer regression coverage for the migrated binding path in `Tests/Unit/CompositeRendererExplicitDrawOrderTests.cpp`

### Implementation for User Story 1

- [X] T008 [US1] Implement renderer-owned frame/object binding state in `Runtime/Rendering/Core/FrameObjectBindingProvider.h` and `Runtime/Rendering/Core/FrameObjectBindingProvider.cpp`
- [X] T009 [US1] Integrate renderer-owned binding preparation into `Runtime/Rendering/Core/CompositeRenderer.h` and `Runtime/Rendering/Core/CompositeRenderer.cpp`
- [X] T010 [US1] Migrate scene renderer setup to the new binding provider in `Runtime/Engine/Rendering/BaseSceneRenderer.h` and `Runtime/Engine/Rendering/BaseSceneRenderer.cpp`
- [X] T011 [US1] Reduce `EngineBufferRenderFeature` to a compatibility shim in `Runtime/Engine/Rendering/EngineBufferRenderFeature.h` and `Runtime/Engine/Rendering/EngineBufferRenderFeature.cpp`
- [X] T012 [US1] Verify forward/deferred draw paths consume renderer-owned frame/object data in `Runtime/Engine/Rendering/ForwardSceneRenderer.cpp` and `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`

**Checkpoint**: Normal scene rendering works without `ARenderFeature` owning mandatory frame/object draw preparation.

---

## Phase 4: User Story 2 - Make Debug Drawing an Explicit Rendering Capability (Priority: P2)

**Goal**: Preserve debug draw submission semantics while moving rendering ownership into an explicit debug drawing capability.

**Independent Test**: Submit debug points/lines/shapes and verify category filtering, lifetime behavior, and frame limits continue working through the explicit debug drawing path.

### Tests for User Story 2

- [X] T013 [P] [US2] Add debug drawing queue and render-stage coverage in `Tests/Unit/DebugDrawPassTests.cpp`
- [X] T014 [P] [US2] Add submission semantics regression tests for categories and lifetimes in `Tests/Unit/DebugDrawTypesTests.cpp`

### Implementation for User Story 2

- [X] T015 [US2] Extract debug draw submission ownership into `Runtime/Rendering/Debug/DebugDrawService.h` and `Runtime/Rendering/Debug/DebugDrawService.cpp`
- [X] T016 [US2] Implement the explicit debug drawing stage in `Runtime/Rendering/Debug/DebugDrawPass.h` and `Runtime/Rendering/Debug/DebugDrawPass.cpp`
- [X] T017 [US2] Narrow `DebugShapeRenderFeature` to a compatibility wrapper around the new debug draw service in `Runtime/Rendering/Features/DebugShapeRenderFeature.h` and `Runtime/Rendering/Features/DebugShapeRenderFeature.cpp`
- [X] T018 [US2] Migrate editor renderer debug draw wiring to the explicit path in `Project/Editor/Rendering/DebugSceneRenderer.h` and `Project/Editor/Rendering/DebugSceneRenderer.cpp`
- [X] T019 [US2] Replace direct feature lookups for grid/debug helpers with the new debug draw capability in `Project/Editor/Rendering/GridRenderPass.cpp` and `Project/Editor/Rendering/GridRenderPass.h`

**Checkpoint**: Debug draw remains usable, but rendering ownership is explicit and no longer centered on `ARenderFeature`.

---

## Phase 5: User Story 3 - Provide Lighting Data Without Render Feature Hooks (Priority: P3)

**Goal**: Move lighting ownership to a shared scene/renderer data provider consumed by forward and deferred rendering.

**Independent Test**: Render lit and unlit scenes and confirm forward/deferred consumers read the same lighting source without relying on draw-time feature hooks.

### Tests for User Story 3

- [X] T020 [P] [US3] Add shared lighting-provider coverage in `Tests/Unit/LightingDataProviderTests.cpp`
- [X] T021 [P] [US3] Add forward/deferred lighting-source regression coverage in `Tests/Unit/ScenePipelineStatePresetsTests.cpp`

### Implementation for User Story 3

- [X] T022 [US3] Introduce a shared scene lighting provider in `Runtime/Engine/Rendering/SceneLightingProvider.h` and `Runtime/Engine/Rendering/SceneLightingProvider.cpp`
- [X] T023 [US3] Move scene light collection out of `LightingRenderFeature` into the provider in `Runtime/Engine/Rendering/BaseSceneRenderer.cpp` and `Runtime/Rendering/Features/LightingRenderFeature.cpp`
- [X] T024 [US3] Rewire forward/deferred renderer consumers to the shared lighting provider in `Runtime/Engine/Rendering/ForwardSceneRenderer.cpp` and `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`
- [X] T025 [US3] Narrow `LightingRenderFeature` to compatibility-only or remove its draw-time responsibilities in `Runtime/Rendering/Features/LightingRenderFeature.h` and `Runtime/Rendering/Features/LightingRenderFeature.cpp`

**Checkpoint**: Lighting data is authoritative scene/renderer data shared across renderer modes.

---

## Phase 6: User Story 4 - Keep Renderer Statistics Without Optional Feature Registration (Priority: P4)

**Goal**: Make frame statistics renderer-owned so diagnostics and panels stay accurate without `FrameInfoRenderFeature`.

**Independent Test**: Render both empty and populated scenes and confirm draw/instance/poly/vertex statistics update from renderer submission rather than optional feature registration.

### Tests for User Story 4

- [X] T026 [P] [US4] Add renderer statistics unit coverage in `Tests/Unit/RendererStatsTests.cpp`
- [X] T027 [P] [US4] Add editor-panel regression coverage for renderer-owned stats access in `Tests/Unit/PanelWindowHookTests.cpp`

### Implementation for User Story 4

- [X] T028 [US4] Add renderer-owned statistics state and query APIs in `Runtime/Rendering/Core/RendererStats.h` and `Runtime/Rendering/Core/RendererStats.cpp`
- [X] T029 [US4] Update draw submission and frame reset/finalize flow to use renderer-owned stats in `Runtime/Rendering/Core/CompositeRenderer.h` and `Runtime/Rendering/Core/CompositeRenderer.cpp`
- [X] T030 [US4] Migrate `FrameInfoRenderFeature` to a compatibility facade over renderer-owned stats in `Runtime/Rendering/Features/FrameInfoRenderFeature.h` and `Runtime/Rendering/Features/FrameInfoRenderFeature.cpp`
- [X] T031 [US4] Rewire editor panels and scene views to renderer-owned stats access in `Project/Editor/Panels/FrameInfo.cpp`, `Project/Editor/Panels/AssetView.cpp`, `Project/Editor/Panels/GameView.cpp`, and `Project/Editor/Panels/SceneView.cpp`

**Checkpoint**: Renderer statistics stay available even when optional stats features are absent.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Finish the migration boundary, remove stale compatibility code where safe, and run the full verification path.

- [X] T032 [P] Document the final ownership boundary in `Runtime/Rendering/Features/ARenderFeature.h` and `specs/005-render-feature-refactor/contracts/render-feature-ownership-contract.md`
- [X] T033 Prune remaining compatibility-only registrations and decide lifecycle-only vs removal in `Runtime/Rendering/Core/CompositeRenderer.cpp`, `Runtime/Engine/Rendering/BaseSceneRenderer.cpp`, and `Project/Editor/Rendering/DebugSceneRenderer.cpp`
- [X] T034 Run the focused build, unit, and smoke validation checklist from `specs/005-render-feature-refactor/quickstart.md`

---

## Phase 8: Remove ARenderFeature Entirely

**Purpose**: Delete the transitional feature system after all migrated owners are in place.

- [X] T035 Add and run a source-level regression check proving `ARenderFeature`/feature registry references still fail before removal
- [X] T036 Remove the feature registry from `Runtime/Rendering/Core/CompositeRenderer.h`, `Runtime/Rendering/Core/CompositeRenderer.cpp`, and `Runtime/Rendering/Core/CompositeRenderer.inl`
- [X] T037 Delete runtime compatibility feature files for engine buffers, frame info, lighting, and debug shapes
- [X] T038 Convert editor debug model, outline, and gizmo feature classes into explicit renderer helpers
- [X] T039 Rewire editor debug/grid/picking passes and views to explicit helpers and `DebugDrawService`
- [X] T040 Update tests and contracts so no runtime/project/test code depends on `ARenderFeature`
- [X] T041 Run focused source check, unit tests, Editor/Game builds, and smoke commands

---

## Phase 9: Upgrade Debug Draw To A General Primitive System

**Purpose**: Make Debug Draw a renderer-owned diagnostic drawing system that any runtime/editor subsystem can call without owning pipeline state, and migrate editor camera/light/bounds helpers to the same unified visibility controls.

### Tests for User Story 2 Upgrade

- [X] T042 [P] [US2] Add failing service tests for global enable, category visibility, and point/line/triangle submission without caller pipeline state in `Tests/Unit/DebugDrawTypesTests.cpp`
- [X] T043 [P] [US2] Add failing geometry helper tests for box, sphere, frustum, light volume, and bounds expansion in `Tests/Unit/DebugDrawGeometryTests.cpp`
- [X] T044 [P] [US2] Add failing debug pass coverage for point/line/triangle command consumption in `Tests/Unit/DebugDrawPassTests.cpp`

### Implementation for User Story 2 Upgrade

- [X] T045 [US2] Extend debug draw data types with primitive commands, style, global/category settings, depth mode, and fill mode in `Runtime/Rendering/Debug/DebugDrawTypes.h`
- [X] T046 [US2] Update `DebugDrawService` to own primitive submission without caller-provided `PipelineState` in `Runtime/Rendering/Debug/DebugDrawService.h` and `Runtime/Rendering/Debug/DebugDrawService.cpp`
- [X] T047 [US2] Add reusable high-level geometry helpers for frustums, light volumes, bounds, boxes, spheres, capsules, cones, and polylines in `Runtime/Rendering/Debug/DebugDrawGeometry.h` and `Runtime/Rendering/Debug/DebugDrawGeometry.cpp`
- [X] T048 [US2] Update `DebugDrawPass` to translate visible debug primitives into renderer draw calls and pass-owned pipeline state in `Runtime/Rendering/Debug/DebugDrawPass.h` and `Runtime/Rendering/Debug/DebugDrawPass.cpp`
- [X] T049 [US2] Migrate editor camera frustum, selected light volume, selected bounds, and grid submissions to the general debug draw API in `Project/Editor/Rendering/DebugSceneRenderer.cpp`, `Project/Editor/Rendering/GridRenderPass.h`, and `Project/Editor/Rendering/GridRenderPass.cpp`
- [X] T050 [US2] Replace scattered editor debug helper toggles with unified debug draw global/category controls in `Project/Editor/Panels/MenuBar.cpp` and `Project/Editor/Settings/EditorSettings.h`
- [X] T051 [US2] Update quickstart and ownership contract validation notes for the general debug draw system in `specs/005-render-feature-refactor/quickstart.md` and `specs/005-render-feature-refactor/contracts/render-feature-ownership-contract.md`
- [X] T052 [US2] Run focused debug draw tests, Editor build, and source checks for the upgraded debug draw path

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1: Setup**: No dependencies; start immediately.
- **Phase 2: Foundational**: Depends on Phase 1; blocks all user story slices.
- **Phase 3: US1**: Depends on Phase 2; establishes the renderer-owned draw-preparation baseline for the rest of the migration.
- **Phase 4: US2**: Depends on Phase 2 and should integrate after US1 if the explicit debug pass needs the migrated renderer-owned draw-preparation path.
- **Phase 5: US3**: Depends on Phase 2 and should integrate after US1 so lighting consumers use the new renderer-owned orchestration.
- **Phase 6: US4**: Depends on Phase 2 and can proceed after US1 once renderer-owned submission flow is stable.
- **Phase 7: Polish**: Depends on the desired user stories being complete.
- **Phase 9: Debug Draw Upgrade**: Depends on Phase 8 because it builds on the removed-feature final state and updates the explicit debug draw path instead of compatibility wrappers.

### User Story Dependencies

- **US1 (P1)**: No user-story dependency; this is the MVP migration slice.
- **US2 (P2)**: Independent in behavior, but recommended after US1 because debug rendering hooks currently share renderer orchestration points.
- **US3 (P3)**: Independent in behavior, but recommended after US1 because lighting consumers should land on top of the migrated renderer core.
- **US4 (P4)**: Independent in behavior, but recommended after US1 because stats should observe the renderer-owned submission path, not the legacy feature path.

### Within Each User Story

- Write and confirm focused tests before implementation updates.
- Land new shared runtime types before migrating existing feature wrappers.
- Rewire renderer/editor consumers after the new owner is in place.
- Remove or narrow compatibility behavior only after the replacement path is passing.

### Parallel Opportunities

- `T002` can run alongside `T001`.
- `T004` and `T005` can run in parallel once `T003` defines the migration boundary.
- In **US1**, `T006` and `T007` can run in parallel.
- In **US2**, `T013` and `T014` can run in parallel.
- In **US3**, `T020` and `T021` can run in parallel.
- In **US4**, `T026` and `T027` can run in parallel.
- After Phase 2, US2/US3/US4 can be staffed in parallel if US1's renderer-owned baseline has already landed cleanly.

---

## Parallel Example: User Story 1

```text
T006 [US1] Add renderer-owned frame/object binding tests in Tests/Unit/RendererFrameObjectBindingTests.cpp
T007 [US1] Add scene-renderer regression coverage in Tests/Unit/CompositeRendererExplicitDrawOrderTests.cpp
```

## Parallel Example: User Story 2

```text
T013 [US2] Add debug drawing queue and render-stage coverage in Tests/Unit/DebugDrawPassTests.cpp
T014 [US2] Add submission semantics regression tests in Tests/Unit/DebugDrawTypesTests.cpp
```

## Parallel Example: User Story 3

```text
T020 [US3] Add shared lighting-provider coverage in Tests/Unit/LightingDataProviderTests.cpp
T021 [US3] Add forward/deferred lighting-source regression coverage in Tests/Unit/ScenePipelineStatePresetsTests.cpp
```

## Parallel Example: User Story 4

```text
T026 [US4] Add renderer statistics unit coverage in Tests/Unit/RendererStatsTests.cpp
T027 [US4] Add editor-panel regression coverage in Tests/Unit/PanelWindowHookTests.cpp
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 and Phase 2.
2. Complete US1 to move mandatory frame/object draw preparation into renderer-owned state.
3. Run the focused validation path from `specs/005-render-feature-refactor/quickstart.md`.
4. Stop and verify normal scene rendering before taking on debug, lighting, or stats migration.

### Incremental Delivery

1. Establish shared migration guardrails and foundational draw orchestration.
2. Deliver **US1** as the architectural baseline and validate.
3. Deliver **US2** to preserve debug workflows through an explicit debug draw path.
4. Deliver **US3** to unify forward/deferred lighting ownership.
5. Deliver **US4** to move diagnostics to renderer-owned stats.
6. Finish with Phase 7 cleanup, documentation, and full validation.

### Suggested Team Strategy

1. Pair on Phase 1 and Phase 2 to stabilize the shared renderer contract.
2. Land **US1** first.
3. Once US1 is stable, split follow-on work:
   - Developer A: US2 debug drawing capability
   - Developer B: US3 lighting provider migration
   - Developer C: US4 renderer statistics migration
4. Rejoin for Phase 7 cleanup and final validation.

---

## Notes

- Keep `ARenderFeature` alive only as long as a compatibility slice still needs it.
- Prefer new runtime owners under renderer/core, scene-rendering, or debug-specific modules instead of adding new draw-time responsibilities back to `ARenderFeature`.
- Treat `Runtime/*/Gen/` as generated output and leave it untouched.
- Use RenderDoc only when a slice claims supported-backend visual correctness beyond what unit tests and smoke checks can prove.
