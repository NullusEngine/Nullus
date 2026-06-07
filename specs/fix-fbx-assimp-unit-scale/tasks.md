# Tasks: Fix FBX Assimp Unit Scale

**Input**: Design documents from `specs/fix-fbx-assimp-unit-scale/`  
**Prerequisites**: `plan.md`, `spec.md`

**Tests**: TDD is required for this bug fix. The regression test has already been observed failing because the generated mesh and prefab transform combine to a 100x instantiated extent.

## Phase 1: Setup

- [x] T001 Confirm dirty-worktree ownership and restrict new edits to `Runtime/Rendering/Resources/Parsers/AssimpParser.cpp`, `Project/Editor/Assets/ExternalAssetImporter.cpp`, `Tests/Unit/AssetImportPipelineTests.cpp`, and `specs/fix-fbx-assimp-unit-scale/`.

## Phase 2: User Story 1 - Import FBX Prefabs At glTF Scale (Priority: P1)

**Goal**: Explicit Assimp FBX imports instantiate at meter scale instead of 100x glTF scale.

**Independent Test**: Import `App/Assets/Engine/Models/Cube.fbx` with `MODEL_FBX_READER=assimp`, assert the generated mesh remains under `2.0` units, and assert generated mesh bounds multiplied by prefab transform scale remain under `2.0`.

### Tests

- [x] T002 [US1] Add mesh and effective prefab scale assertions to `Tests/Unit/AssetImportPipelineTests.cpp`.
- [x] T003 [US1] Run the focused Assimp FBX test and confirm it fails because the generated prefab instantiates 100x too large.

### Implementation

- [x] T004 [US1] Use the actual source format when building Assimp detailed scene records in `Runtime/Rendering/Resources/Parsers/AssimpParser.cpp`.
- [x] T005 [US1] Normalize only the Assimp FBX synthetic root node scale in `Runtime/Rendering/Resources/Parsers/AssimpParser.cpp`.
- [x] T006 [US1] Apply FBX global unit scale flags in `Project/Editor/Assets/ExternalAssetImporter.cpp`.
- [x] T007 [US1] Re-run the focused Assimp FBX test and nearby Assimp parser/import tests.

## Phase 3: Review And Evidence

- [x] T008 Run code self-review and required plan-review for the changed files.
- [x] T009 Record root cause, changed files, and validation evidence in the final response.

## Dependencies & Execution Order

- T001 before all edits.
- T002 before T003.
- T003 before T004 and T005.
- T004 through T006 before T007.
- T007 before T008 and T009.
