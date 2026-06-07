# Tasks: glTF Texture Orientation

**Input**: Design documents from `specs/045-gltf-texture-orientation/`  
**Prerequisites**: `plan.md`, `spec.md`

**Tests**: TDD is required for this bug fix. The regression test must be observed failing before production code changes.

## Phase 1: Setup

- [x] T001 Confirm dirty-worktree ownership and isolate this fix to external model texture orientation, texture artifact decode state, and external texture postprocessor-version consumers.

## Phase 2: User Story 1 - Preserve glTF Texture Rows (Priority: P1)

**Goal**: glTF/GLB external model texture artifacts preserve encoded top-to-bottom image row order.

**Independent Test**: Import a minimal glTF with a 2x2 red-top/blue-bottom PNG and assert row 0 remains red in the generated texture artifact.

### Tests

- [x] T002 [US1] Add `ExternalGltfModelTextureArtifactsPreserveEncodedRowOrder` in `Tests/Unit/AssetImportPipelineTests.cpp`.
- [x] T003 [US1] Run the focused test and confirm it fails because row 0 is flipped.

### Implementation

- [x] T004 [US1] Add scoped orientation selection in `Project/Editor/Assets/ExternalAssetImporter.cpp` so `.gltf` and `.glb` texture payloads decode without vertical flipping.
- [x] T005 [US1] Bump the external texture postprocessor version in `Project/Editor/Assets/ExternalAssetImporter.cpp` so affected texture artifacts rebuild.
- [x] T006 [US1] Move the external texture postprocessor version into `Project/Editor/Assets/ExternalAssetImporter.h` and update `Project/Editor/Assets/AssetDatabaseFacade.cpp`, `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`, and `Tests/Unit/AssetImportPipelineTests.cpp` to consume the shared version key.
- [x] T007 [US1] Use thread-local STB flip state in `Runtime/Rendering/Assets/TextureArtifact.cpp` so concurrent glTF and non-glTF texture imports cannot race through global decode state.
- [x] T008 [US1] Re-run the focused orientation test and nearby external model texture tests.

## Phase 3: Review And Evidence

- [x] T009 Run code self-review and required plan-review for the changed files.
- [x] T010 Record whether a Sponza reimport and RenderDoc visual confirmation was performed or remains a manual follow-up.

## Dependencies & Execution Order

- T001 before all edits.
- T002 before T003.
- T003 before T004 and T005.
- T004 and T005 before T006 and T007.
- T006 and T007 before T008.
- T008 before T009 and T010.
