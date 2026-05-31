# Tasks: Unity-Ready Model Drop

**Input**: `specs/unity-ready-model-drop/spec.md`, `specs/unity-ready-model-drop/plan.md`
**Prerequisites**: Existing texture compression/mipmap artifact work and generated model drag/drop tests

## Phase 1: Readiness Gate MVP

- [x] T001 Add RED tests in `Tests/Unit/EditorAssetDragDropTests.cpp` proving a generated model drop with missing texture artifact does not create a scene object.
- [x] T002 Add RED tests in `Tests/Unit/EditorAssetDragDropTests.cpp` proving a generated model drop with valid material and `.ntex` artifacts commits normally.
- [x] T003 Implement manifest renderer dependency validation in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`.
- [x] T004 Route validation failure to `pendingImport`/diagnostic result before `AssetDragDropWorkflow` is called.

## Phase 2: Legacy Async Hardening

- [x] T005 Add RED contract coverage for non-`.ntex` texture paths in `Tests/Unit/EditorRenderPathContractTests.cpp`.
- [x] T006 Change `BindDeferredMaterialTextures()` in `Project/Editor/Core/EditorActions.cpp` so missing unqueueable texture paths count as failed material slots.
- [x] T007 Add cancellation restoration coverage in `Tests/Unit/EditorRenderPathContractTests.cpp`.
- [x] T008 Restore hidden root active state on non-destroy cancellation in `Project/Editor/Core/EditorActions.cpp`.

## Phase 3: Verification

- [x] T009 Build `NullusUnitTests`.
- [x] T010 Run focused drag/drop and renderer path tests.
- [x] T011 Run `git diff --check`.
- [ ] T012 Capture or inspect RenderDoc evidence for the reported model once the editor run is available.
