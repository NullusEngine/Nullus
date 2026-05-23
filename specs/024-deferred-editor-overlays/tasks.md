# Tasks: Deferred Editor Overlays

**Input**: `specs/024-deferred-editor-overlays/`

## Phase 1: Setup

- [x] T001 Inspect current deferred and debug renderer pass package tests in `Tests/Unit/RendererFrameObjectBindingTests.cpp` and `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`.
- [x] T002 Confirm no generated files under `Runtime/*/Gen/` are part of the intended edit set.

## Phase 2: Test First

- [x] T003 Add a failing contract test that `DebugSceneRenderer` is not based on `ForwardSceneRenderer` and is based on `DeferredSceneRenderer` in `Tests/Unit/EditorRenderPathContractTests.cpp`.
- [x] T004 Add a failing package-order test for deferred debug renderer output plus editor helper/picking inputs in `Tests/Unit/EditorRenderPathContractTests.cpp`.

## Phase 3: Runtime Deferred Extension

- [x] T005 Add a generic deferred package helper declaration in `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h`.
- [x] T006 Implement the helper in `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp` so deferred GBuffer/Lighting inputs are preserved and caller-provided pass inputs are appended.
- [x] T007 Expose protected deferred prepared-package building hooks from `Runtime/Engine/Rendering/DeferredSceneRenderer.h/.cpp` for subclasses without adding editor dependencies.

## Phase 4: Editor Debug Renderer Migration

- [x] T008 Change `Project/Editor/Rendering/DebugSceneRenderer.h` to inherit from `DeferredSceneRenderer`.
- [x] T009 Update `Project/Editor/Rendering/DebugSceneRenderer.cpp` metadata/input builders from forward base descriptors to deferred base descriptors.
- [x] T010 Update `DebugSceneRenderer::BuildFrameSnapshot` and `BuildPreparedRenderSceneBuilder` to call deferred base behavior and append editor pass inputs after deferred lighting.
- [x] T011 Preserve picking preferred readback registration in the deferred debug package path.

## Phase 5: Validation And Cleanup

- [x] T012 Run targeted renderer/debug tests from `specs/024-deferred-editor-overlays/quickstart.md`.
- [x] T013 Run full `NullusUnitTests`.
- [x] T014 Self-review runtime/editor boundary, generated file boundary, and pass ordering assumptions.
- [x] T015 [US2] Add regression coverage that deferred Scene View grid/camera/light helper passes declare external color writes in `Tests/Unit/EditorRenderPathContractTests.cpp`.
- [x] T016 [US2] Ensure deferred Scene View helper pass inputs append external color write resource accesses in `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp`.
- [x] T017 Run targeted editor overlay contract validation for the helper color write regression.
- [ ] T018 Run plan-review quality gate for this regression fix.

## Dependencies

- T003 and T004 must fail before production changes.
- T005-T007 unblock T008-T011.
- T012-T014 depend on implementation tasks.

## Independent Test Criteria

- User Story 1 passes when tests prove Scene View debug renderer uses deferred base rendering.
- User Story 2 passes when tests prove editor helper/debug pass inputs are appended after deferred lighting.
- User Story 3 passes when tests prove picking pass input and preferred readback registration survive.
