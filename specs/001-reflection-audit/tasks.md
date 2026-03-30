# Tasks: Reflection Audit And Coverage

**Input**: Design documents from `/specs/001-reflection-audit/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `quickstart.md`

**Tests**: Automated reflection tests are required for this feature because stable entrypoints already exist in `NullusUnitTests` and `Tools/ReflectionTest`. Generated-artifact assertions remain part of the validation surface.

**Organization**: Tasks are grouped by user story so the audit, tests, coverage fixes, and bilingual docs can each be reviewed independently.

**Follow-up planning**: Additional post-audit optimization work is tracked separately in `optimization-roadmap.md` so this completed task list remains a record of what was already executed.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the maintained audit and test-support destinations used by the rest of the work.

- [X] T001 Create the maintained audit deliverable in `specs/001-reflection-audit/reflection-audit.md`
- [X] T002 Create the bilingual documentation home in `Docs/Reflection/ReflectionWorkflow.en.md` and `Docs/Reflection/ReflectionWorkflow.zh-CN.md`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Establish shared reflection test support and the concrete audit inventory before story work expands.

**⚠️ CRITICAL**: No user story work should rely on ad hoc reflection assertions after this phase.

- [X] T003 Create shared runtime reflection expectation helpers in `Tests/Unit/ReflectionTestUtils.h`
- [X] T004 Capture the concrete consumer matrix, correct usages, and gap inventory in `specs/001-reflection-audit/reflection-audit.md`

**Checkpoint**: Shared reflection test support exists and the audit inventory is explicit.

---

## Phase 3: User Story 1 - Audit Reflection Coverage And Registration Rules (Priority: P1) 🎯 MVP

**Goal**: Produce the authoritative audit of the current reflection/code generation system and the repository-specific registration rules.

**Independent Test**: Read `specs/001-reflection-audit/reflection-audit.md` and confirm it explains the current pipeline, optimization opportunities, correct usages, gaps, and registration heuristics for classes, enums, properties, and functions.

### Implementation for User Story 1

- [X] T005 [US1] Document the current declaration-to-generation-to-runtime flow in `specs/001-reflection-audit/reflection-audit.md`
- [X] T006 [US1] Record confirmed optimization opportunities and current incorrect or incomplete usage patterns in `specs/001-reflection-audit/reflection-audit.md`
- [X] T007 [US1] Record the repository registration rules and intentional exclusions in `specs/001-reflection-audit/reflection-audit.md`

**Checkpoint**: The reflection audit stands alone as a reviewable repository artifact.

---

## Phase 4: User Story 2 - Strengthen Automated Reflection Regression Coverage (Priority: P2)

**Goal**: Improve the reflection test structure and cover the maintained repository patterns with clearer, more extensible assertions.

**Independent Test**: Run the reflection-related `NullusUnitTests` coverage and confirm the suite verifies reflected classes, enums, external declarations, private external bindings, explicit properties, and auto properties through shared helpers instead of duplicated assertion blocks.

### Tests for User Story 2 ⚠️

- [X] T008 [US2] Refactor `Tests/Unit/ReflectionRuntimeTests.cpp` to use `Tests/Unit/ReflectionTestUtils.h`
- [X] T009 [P] [US2] Expand runtime coverage in `Tests/Unit/ReflectionRuntimeTests.cpp` for external declarations, private external bindings, serialization structs, and consumer-facing reflected fields
- [X] T010 [P] [US2] Expand generated-artifact coverage in `Tests/Unit/MetaParserGenerationTests.cpp` for auto properties, explicit properties, private binding helpers, and external reflection outputs
- [X] T011 [US2] Align the standalone smoke scope in `Tools/ReflectionTest/src/main.cpp` with the maintained runtime expectations

### Implementation for User Story 2

- [X] T012 [US2] Update `Tests/Unit/CMakeLists.txt` only if the refactored reflection test support requires include or source adjustments

**Checkpoint**: Reflection regression coverage is broader and easier to extend.

---

## Phase 5: User Story 3 - Replace Reflection Gaps That Block Current Consumers (Priority: P3)

**Goal**: Register the missing high-value members required by active reflection consumers and let those consumers rely on the reflected data.

**Independent Test**: Confirm the selected runtime type exposes the missing reflected members in automated tests and that the corresponding consumer no longer hard-codes a separate path for data now covered by reflection.

### Tests for User Story 3 ⚠️

- [X] T013 [US3] Add failing runtime and generation assertions for the missing `MaterialRenderer` reflected properties in `Tests/Unit/ReflectionRuntimeTests.cpp` and `Tests/Unit/MetaParserGenerationTests.cpp`

### Implementation for User Story 3

- [X] T014 [US3] Add the required reflection markers for consumer-facing `MaterialRenderer` accessors in `Runtime/Engine/Components/MaterialRenderer.h`
- [X] T015 [US3] Update `Project/Editor/Panels/Inspector.cpp` to use reflected `MaterialRenderer` fields when available and keep fallback logic only where reflection data is still absent
- [X] T016 [US3] Regenerate and review the affected generated outputs in `Runtime/Engine/Gen/Components/MaterialRenderer.generated.h`, `Runtime/Engine/Gen/Components/MaterialRenderer.generated.cpp`, and `Runtime/Engine/Gen/MetaGenerated.cpp` through the normal build flow

**Checkpoint**: The selected consumer-facing reflection gap is closed and covered by tests.

---

## Phase 6: User Story 4 - Publish A Bilingual Reflection Usage Guide (Priority: P4)

**Goal**: Ship one maintained reflection workflow guide in English and Chinese, and make the README point to both.

**Independent Test**: Open the README and confirm it links directly to both `Docs/Reflection/ReflectionWorkflow.en.md` and `Docs/Reflection/ReflectionWorkflow.zh-CN.md`, and that both guides describe the same maintained workflow.

### Implementation for User Story 4

- [X] T017 [P] [US4] Write the maintained English guide in `Docs/Reflection/ReflectionWorkflow.en.md`
- [X] T018 [P] [US4] Write the maintained Chinese guide in `Docs/Reflection/ReflectionWorkflow.zh-CN.md`
- [X] T019 [US4] Update `README.md` to fix the reflection entry point and link to both maintained docs
- [X] T020 [US4] Remove legacy `ReflectionBindingWorkflow` entry files and point readers directly to `Docs/Reflection/ReflectionWorkflow.en.md` and `Docs/Reflection/ReflectionWorkflow.zh-CN.md`

**Checkpoint**: Reflection guidance has one bilingual home and the README points to it.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Run the final validation, review generated diffs, and close the audit loop.

- [X] T021 Run the quickstart validation commands from `specs/001-reflection-audit/quickstart.md`
- [X] T022 Review generated diffs and confirm no file under `Runtime/*/Gen/` was hand-edited
- [X] T023 Update `specs/001-reflection-audit/reflection-audit.md` with final evidence, remaining unverified scope, and summary conclusions

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies; can start immediately
- **Foundational (Phase 2)**: Depends on Setup and blocks all user stories
- **User Story 1 (Phase 3)**: Depends on Phase 2
- **User Story 2 (Phase 4)**: Depends on Phase 2 and benefits from the audit inventory in US1
- **User Story 3 (Phase 5)**: Depends on Phase 2 and on the failing assertions from US2
- **User Story 4 (Phase 6)**: Depends on Phase 2 and should use the finalized audit findings from US1
- **Polish (Phase 7)**: Depends on all desired user stories being complete

### User Story Dependencies

- **US1**: No story dependency after Foundational
- **US2**: No story dependency after Foundational, but it should absorb the audit terminology from US1
- **US3**: Depends on US2’s failing assertions being in place before implementation
- **US4**: Depends on US1’s audit conclusions so the bilingual guides reflect the final registration rules

### Within Each User Story

- New or expanded automated assertions MUST exist before the implementation they guard
- Declaration changes come before generated-output review
- Documentation updates come after the final workflow and registration rules are known
- Final audit conclusions come after code, tests, and docs are all validated

### Parallel Opportunities

- `T009` and `T010` can run in parallel after `T008`
- `T017` and `T018` can run in parallel after the audit conclusions are stable

---

## Parallel Example: User Story 2

```text
Task: "Expand runtime coverage in Tests/Unit/ReflectionRuntimeTests.cpp for external declarations, private external bindings, serialization structs, and consumer-facing reflected fields"
Task: "Expand generated-artifact coverage in Tests/Unit/MetaParserGenerationTests.cpp for auto properties, explicit properties, private binding helpers, and external reflection outputs"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Setup
2. Complete Foundational
3. Complete User Story 1
4. Validate that `specs/001-reflection-audit/reflection-audit.md` already answers the audit and registration-rule questions clearly

### Incremental Delivery

1. Land the audit and shared test support
2. Expand automated reflection coverage
3. Close the highest-value consumer-facing reflection gap
4. Publish the bilingual maintained guide and README links
5. Run the full reflection validation flow and record evidence

### Parallel Team Strategy

With multiple contributors:

1. One contributor can own the audit artifact and registration rules
2. One contributor can expand runtime and generation test coverage after shared helpers land
3. One contributor can draft the English and Chinese docs after the audit conclusions stabilize

---

## Notes

- `[P]` tasks only apply when files do not overlap
- User story labels map directly to the stories in `spec.md`
- Generated files are reviewed outputs, not direct edit targets
- Final claims must stay bounded to the exact validation evidence gathered in this feature
