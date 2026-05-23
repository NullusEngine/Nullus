# Tasks: Reduce Scene View Frame Stalls

**Input**: Design documents from `specs/015-fix-sceneview-drain/`  
**Prerequisites**: `spec.md`, `plan.md`

## Phase 1: Setup

- [x] T001 Confirm profiler evidence points to Scene View post-render threaded lifecycle drain in `Runtime/Rendering/Context/RenderThreadCoordinator.cpp`
- [x] T002 Create spec bundle in `specs/015-fix-sceneview-drain/`

## Phase 2: Foundational

- [x] T003 Identify existing delayed picking readback behavior in `Project/Editor/Rendering/PickingReadbackLifecycle.h`
- [x] T004 Identify current Scene View immediate readback flag in `Project/Editor/Panels/SceneView.cpp`

## Phase 3: User Story 1 - Smooth Scene View Interaction (Priority: P1)

**Goal**: Normal Scene View rendering should not request same-frame picking readback and force a post-render threaded lifecycle drain.

**Independent Test**: `PanelWindowHookTests.SceneViewPickingUsesDelayedReadbackByDefault`

- [x] T005 [US1] Add failing Scene View delayed-readback policy test in `Tests/Unit/PanelWindowHookTests.cpp`
- [x] T006 [US1] Add Scene View immediate picking readback policy in `Project/Editor/Panels/ViewFrameLifecycle.h`
- [x] T007 [US1] Wire Scene View to the policy in `Project/Editor/Panels/SceneView.cpp`
- [x] T008 [US1] Verify targeted test in `Build/bin/Debug/NullusUnitTests.exe`

## Phase 4: User Story 2 - Preserve Actor Picking (Priority: P2)

**Goal**: Actor picking continues to use the latest readable picking frame.

**Independent Test**: `PickingReadbackLifecycleTests.*`

- [x] T009 [US2] Verify existing delayed readback lifecycle tests in `Tests/Unit/PickingReadbackLifecycleTests.cpp`
- [x] T010 [US2] Confirm no production change bypasses `PickingReadbackLifecycle` in `Project/Editor/Rendering/PickingRenderPass.cpp`

## Phase 5: User Story 3 - Keep Resize Safety (Priority: P3)

**Goal**: Retirement-aware resize behavior remains unchanged for view resource safety.

**Independent Test**: `PanelWindowHookTests.RetirementAware*`

- [x] T011 [US3] Verify resize drain/defer policy tests in `Tests/Unit/PanelWindowHookTests.cpp`
- [x] T012 [US3] Confirm render drain policy still drains for explicit immediate readback consumers in `Project/Editor/Panels/ViewFrameLifecycle.h`

## Final Phase: Polish & Cross-Cutting

- [x] T013 Run focused editor/panel/picking tests with `NullusUnitTests`
- [x] T014 Review diff for unrelated changes and generated-file violations
- [ ] T015 Optional runtime validation: capture a Tracy or RenderDoc-backed run showing Scene View no longer has steady-state immediate-drain stalls

## Dependencies

- US1 depends on setup and foundational investigation.
- US2 depends on US1 only for confirming Scene View no longer forces immediate readback.
- US3 can be verified in parallel with US2 because it uses existing policy tests.

## Parallel Examples

- After T007, run `PanelWindowHookTests.RetirementAware*` and `PickingReadbackLifecycleTests.*` independently.
- Runtime Tracy validation can be done after unit validation without further code changes.

## Implementation Strategy

Deliver the MVP by completing US1: make Scene View default to delayed readback. Then verify US2 and US3 to ensure picking and resize safety remain intact.
