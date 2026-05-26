# Tasks: Optional Assimp FBX Import

**Input**: Design documents from `/specs/036-assimp-fbx-option/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Required. Follow TDD: write each behavior test first, run it to confirm the expected failure, then implement.

**Organization**: Tasks are grouped by user story to preserve independently testable increments.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Confirm current state and prepare the smallest shared surfaces.

- [X] T001 Inspect existing FBX/Assimp routing and note current failing contract expectations in `Tests/Unit/FbxSdkIntegrationContractTests.cpp`
- [X] T002 [P] Inspect model importer setting serialization in `Project/Editor/Assets/AssetImporterSettings.*` and `Project/Editor/Assets/AssetImporterFacade.*`
- [X] T003 [P] Inspect current Assimp CMake importer options in `ThirdParty/CMakeLists.txt`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add shared model for reader selection and build availability before story routing.

**CRITICAL**: No user story implementation can begin until this phase is complete.

- [X] T004 Write failing tests for FBX reader setting defaults and persistence in `Tests/Unit/AssetImporterFacadeTests.cpp`
- [X] T005 Add `FbxReaderSelection` model setting and serialization helpers in `Project/Editor/Assets/AssetImporterSettings.h`
- [X] T006 Implement FBX reader setting parsing/serialization in `Project/Editor/Assets/AssetImporterSettings.cpp`
- [X] T007 Integrate FBX reader setting persistence into `Project/Editor/Assets/AssetImporterFacade.h` and `Project/Editor/Assets/AssetImporterFacade.cpp`
- [X] T008 Run targeted importer facade tests and confirm T004 passes in `NullusUnitTests`
- [X] T009 Write failing CMake contract tests for narrow Assimp FBX option in `Tests/Unit/FbxSdkIntegrationContractTests.cpp`
- [X] T010 Add narrow Assimp FBX build option and availability define in `ThirdParty/CMakeLists.txt` and `Runtime/Rendering/CMakeLists.txt`
- [X] T011 Run targeted FBX/CMake contract tests and confirm T009 passes in `NullusUnitTests`

**Checkpoint**: Reader selection can be stored, and build availability can be tested.

---

## Phase 3: User Story 1 - Preserve Existing FBX Imports By Default (Priority: P1) MVP

**Goal**: Default and legacy FBX imports continue to use Autodesk first, including built-in mesh artifact generation.

**Independent Test**: Import/routing contract tests verify no-setting FBX paths choose Autodesk and do not attempt Assimp fallback.

### Tests for User Story 1

- [X] T012 [US1] Update failing default-routing tests in `Tests/Unit/FbxSdkIntegrationContractTests.cpp` for the new explicit reader-selection helper while preserving Autodesk default expectations
- [X] T013 [P] [US1] Add default-setting coverage in `Tests/Unit/AssetImportPipelineTests.cpp` proving missing FBX reader setting resolves to Autodesk

### Implementation for User Story 1

- [X] T014 [US1] Add reader-selection helper for FBX scene and mesh import in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [X] T015 [US1] Preserve Autodesk default for built-in mesh artifact generation in `Runtime/Core/ResourceManagement/MeshManager.cpp`
- [X] T016 [US1] Run targeted US1 tests and confirm default behavior passes in `NullusUnitTests`

**Checkpoint**: MVP complete. Existing FBX behavior is preserved without explicit Assimp use.

---

## Phase 4: User Story 2 - Choose Assimp For A Specific FBX Asset (Priority: P1)

**Goal**: A single FBX asset can explicitly use Assimp when Assimp FBX support is enabled, without changing other assets.

**Independent Test**: Set an FBX asset to Assimp, import it, and verify Assimp path diagnostics/dependencies/artifacts through targeted tests.

### Tests for User Story 2

- [X] T017 [US2] Write failing explicit-Assimp import routing tests in `Tests/Unit/AssetImportPipelineTests.cpp`
- [X] T018 [P] [US2] Write failing unavailable-Assimp diagnostic test in `Tests/Unit/AssetImportPipelineTests.cpp`

### Implementation for User Story 2

- [X] T019 [US2] Implement explicit Assimp FBX scene import routing in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [X] T020 [US2] Implement explicit Assimp FBX native mesh-cache routing in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [X] T021 [US2] Add Assimp FBX availability checks and actionable errors in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [X] T022 [US2] Run targeted US2 tests and confirm explicit Assimp behavior passes in `NullusUnitTests`

**Checkpoint**: Explicit per-asset Assimp FBX import works or fails clearly when unavailable.

---

## Phase 5: User Story 3 - Controlled Fallback When Autodesk FBX Import Is Unavailable Or Fails (Priority: P2)

**Goal**: An asset can try Autodesk first and fall back to Assimp only when explicitly configured.

**Independent Test**: Force Autodesk unavailability/failure in tests, enable fallback mode, and verify Assimp succeeds with a fallback warning.

### Tests for User Story 3

- [X] T023 [US3] Write failing fallback-success diagnostic tests in `Tests/Unit/AssetImportPipelineTests.cpp`
- [X] T024 [P] [US3] Write failing no-fallback-on-default failure tests in `Tests/Unit/FbxSdkIntegrationContractTests.cpp` or `Tests/Unit/AssetImportPipelineTests.cpp`

### Implementation for User Story 3

- [X] T025 [US3] Implement Autodesk-then-Assimp fallback routing in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [X] T026 [US3] Emit fallback warning and preserve failure diagnostics when both readers fail in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [X] T027 [US3] Run targeted US3 tests and confirm fallback behavior passes in `NullusUnitTests`

**Checkpoint**: Fallback is explicit, diagnostic-rich, and disabled by default.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, validation, and review gates.

- [X] T028 [P] Update Assimp build-option documentation in `Docs/Testing.md`
- [X] T029 Run focused unit-test filter for asset importer settings, asset import pipeline, and FBX SDK integration contracts
- [X] T030 Run broader build/test validation that is practical in the current worktree
- [X] T031 Run `/plan-review` quality gate and complete required review loops before reporting completion
- [X] T032 Update `specs/036-assimp-fbx-option/tasks.md` task statuses as implementation completes

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Setup and blocks all user stories.
- **US1 (Phase 3)**: Depends on Foundational and is the MVP.
- **US2 (Phase 4)**: Depends on Foundational and should be implemented after US1 to preserve default behavior first.
- **US3 (Phase 5)**: Depends on US1 and US2.
- **Polish (Phase 6)**: Depends on selected user stories being complete.

### User Story Dependencies

- **US1**: No dependency on US2/US3.
- **US2**: Uses the reader-selection model from Foundational and routing helper from US1.
- **US3**: Uses both Autodesk and Assimp routing from US1/US2.

### Parallel Opportunities

- T002 and T003 can run in parallel.
- T013 can be prepared after T012 red test expectations are understood.
- T018 can be written independently from T017.
- T024 can be written independently from T023.
- T028 can run in parallel after build-option naming is stable.

## Implementation Strategy

### MVP First

1. Complete Setup and Foundational.
2. Complete US1 and validate that default FBX imports stay Autodesk.
3. Stop and verify before adding explicit Assimp behavior.

### Incremental Delivery

1. Preserve defaults.
2. Add explicit Assimp selection.
3. Add controlled fallback.
4. Update docs and run review/validation gates.

### TDD Rule

Each behavior-changing task starts with a failing test and a recorded targeted test command before production changes are made.
