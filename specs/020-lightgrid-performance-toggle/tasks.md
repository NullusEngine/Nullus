# Tasks: LightGrid Performance Toggle

**Input**: Design documents from `/specs/020-lightgrid-performance-toggle/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Required by spec and TDD workflow.

## Phase 1: Setup

**Purpose**: Confirm current settings/rendering entry points.

- [x] T001 Inspect current EditorSettings reflection/persistence and LightGrid renderer call paths in `Project/Editor/Settings/EditorSettings.*`, `Runtime/Engine/Rendering/*SceneRenderer.cpp`, and `Runtime/Engine/Rendering/BaseSceneRenderer.*`

---

## Phase 2: Foundational Tests

**Purpose**: Add failing tests before production changes.

- [x] T002 [P] Add settings persistence/default tests for LightGrid editor setting in `Tests/Unit/EditorSettingsPersistenceTests.cpp`
- [x] T003 [P] Add renderer access/settings tests for default enabled and disabled LightGrid state in `Tests/Unit/RenderFrameworkContractTests.cpp`
- [x] T004 [P] Add renderer contract regression test proving per-frame LightGrid compile context caching in `Tests/Unit/RenderFrameworkContractTests.cpp`

---

## Phase 3: User Story 1 - Disable LightGrid From Editor Settings (Priority: P1)

**Goal**: User can toggle LightGrid from Project Settings and persist the choice.

**Independent Test**: Settings tests pass and Project Settings search can find the reflected LightGrid option.

- [x] T005 [US1] Add reflected editor rendering settings object with `enableLightGrid` default true in `Project/Editor/Settings/EditorSettings.h`
- [x] T006 [US1] Register editor rendering settings in `Project/Editor/Settings/EditorSettings.cpp`
- [x] T007 [US1] Propagate editor LightGrid setting into driver construction in `Project/Editor/Core/Context.cpp` and game/project context paths as applicable

---

## Phase 4: User Story 2 - Preserve Existing Rendering By Default (Priority: P1)

**Goal**: Existing projects keep LightGrid enabled unless explicitly disabled.

**Independent Test**: Default driver/render tests show LightGrid enabled without persisted settings.

- [x] T008 [US2] Add `enableLightGrid` to `Runtime/Rendering/Settings/DriverSettings.h` defaulting to true
- [x] T009 [US2] Store LightGrid state in `Runtime/Rendering/Context/DriverInternal.h` and expose it via existing renderer access helpers
- [x] T010 [US2] Make shared scene renderer LightGrid context generation skip LightGrid graph preparation when disabled while still producing valid render packages

---

## Phase 5: User Story 3 - Reduce Duplicate LightGrid Preparation (Priority: P2)

**Goal**: Threaded scene rendering reuses LightGrid context within the same frame.

**Independent Test**: Threaded lifecycle regression verifies duplicate preparation is avoided.

- [x] T011 [US3] Add per-frame LightGrid compile context cache to `Runtime/Engine/Rendering/BaseSceneRenderer.h`
- [x] T012 [US3] Implement cache invalidation/reuse in `Runtime/Engine/Rendering/BaseSceneRenderer.cpp`
- [x] T013 [US3] Reuse cached LightGrid context from existing forward/deferred threaded builder capture paths via `BaseSceneRenderer::BuildLightGridCompileContext`

---

## Phase 6: Validation & Polish

**Purpose**: Verify behavior, document evidence, and keep spec artifacts in sync.

- [x] T014 Run targeted tests from `specs/020-lightgrid-performance-toggle/quickstart.md`
- [x] T015 Self-review diffs for generated-file boundaries, backend assumptions, and unrelated changes
- [x] T016 Update `specs/020-lightgrid-performance-toggle/tasks.md` completion states

## Dependencies & Execution Order

- Phase 1 before all other phases.
- Phase 2 tests before implementation phases.
- US1 and US2 both required for usable toggle behavior.
- US3 depends on US2 LightGrid state and shared helper changes.
- Validation after all desired stories complete.

## Parallel Opportunities

- T002, T003, and T004 touch different test areas and can be drafted independently.
- US1 settings work and US2 driver setting plumbing can be reasoned about independently after tests exist, but implementation should be integrated sequentially because driver construction connects them.

## Implementation Strategy

1. Red: add failing tests for settings/default/disabled/cache behavior.
2. Green: add setting and driver plumbing with default enabled.
3. Green: skip LightGrid when disabled.
4. Green: cache per-frame LightGrid context.
5. Verify targeted tests and note remaining runtime profiling evidence.
