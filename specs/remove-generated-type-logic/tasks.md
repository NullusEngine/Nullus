# Tasks: Remove Generated Type Logic

**Input**: Design documents from `specs/remove-generated-type-logic/`
**Prerequisites**: plan.md, spec.md

## Phase 1: Setup

- [x] T001 Confirm existing generator/runtime files and tests that cover vector/array reflection.

## Phase 2: User Story 1 - Keep Generated Reflection Thin (Priority: P1)

**Goal**: MetaParser classifies field kinds before emission, and generated `.generated.cpp` files contain only typed resolver calls.

**Independent Test**: MetaParser vector/array fixture generation test fails before implementation when checking absence of local prefix parsing, then passes after template/helper changes.

- [x] T002 [US1] Add a failing assertion in `Tests/Unit/MetaParserGenerationModuleTests.cpp` that generated fixture source does not contain `arrayPrefix`, `vectorPrefix`, or local `resolveRegisteredFieldType` logic.
- [x] T003 [US1] Add typed field resolver declarations in `Runtime/Base/Reflection/ReflectionDatabase.h`.
- [x] T004 [US1] Implement typed field resolvers in `Runtime/Base/Reflection/ReflectionDatabase.cpp` without runtime container string parsing.
- [x] T005 [US1] Update `Tools/MetaParser/src/MetaParserTool.Generation.cs` and `Tools/MetaParser/src/Templates/HeaderGenerated.cpp.tt` to emit MetaParser-selected resolver expressions.
- [x] T006 [US1] Regenerate affected reflection outputs through the existing MetaParser/build flow.

## Phase 3: User Story 2 - Preserve Runtime Reflection Semantics (Priority: P2)

**Goal**: Editor/serialization reflection consumers continue to see correct array and `PPtr` field types.

**Independent Test**: Existing reflection runtime and property drawer tests pass.

- [x] T007 [US2] Run targeted MetaParser generation test for vector/array fixture.
- [x] T008 [US2] Run targeted reflection runtime/editor array field tests.
- [x] T009 [US2] Run `ReflectionTest` or equivalent available reflection smoke test.

## Phase 4: Review And Gate

- [x] T010 Review changed files for generated boundary compliance and user-change preservation.
- [x] T011 Run `/plan-review` quality gate per repository rules before reporting completion.
- [x] T012 Summarize validation evidence and any skipped checks.
