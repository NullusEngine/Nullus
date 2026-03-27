# Tasks: Rendering Backend Asset And Validation Unification

**Input**: Design documents from `/specs/example-rendering-major-change/`
**Prerequisites**: `plan.md`, `spec.md`

**Tests**: Include focused runtime verification and RenderDoc evidence when rendering behavior changes.

**Organization**: Tasks are grouped by user story so each slice stays independently reviewable.

## Phase 1: Setup

- [ ] T001 Create or update the major-change bundle under `specs/example-rendering-major-change/`
- [ ] T002 Confirm the bundle names the backend, affected rendering surfaces, and validation expectations

---

## Phase 2: Foundational Analysis

- [ ] T003 Inspect the affected runtime and editor asset loading paths in `Runtime/` and `Project/`
- [ ] T004 Record shader convention expectations and binding assumptions that the backend change depends on
- [ ] T005 Identify the first backend/platform pair that will be used for focused validation

**Checkpoint**: The scope, validation path, and backend assumptions are explicit before implementation starts.

---

## Phase 3: User Story 1 - Validate Editor Rendering Assets On A New Backend (Priority: P1) 🎯 MVP

**Goal**: Keep the editor and runtime default rendering assets usable on the target backend during bring-up.

**Independent Test**: Launch the editor, load a simple scene, and verify the grid, gizmos, and representative lit geometry render correctly on the selected backend.

### Tests And Validation For User Story 1

- [ ] T006 [US1] Capture focused runtime evidence for default material and shader loading
- [ ] T007 [US1] Capture a RenderDoc frame or equivalent backend-specific evidence for the scene view passes

### Implementation For User Story 1

- [ ] T008 [US1] Update backend-facing shader or resource expectations in the affected runtime/editor code paths
- [ ] T009 [US1] Update default asset references or resource wiring needed for the selected backend
- [ ] T010 [US1] Re-check the editor scene view after the asset and backend changes

**Checkpoint**: The first backend pass is demonstrably usable for the editor rendering path.

---

## Phase 4: User Story 2 - Keep Runtime And Editor Shader Conventions Aligned (Priority: P2)

**Goal**: Make shader convention expectations explicit enough that runtime and editor assets stay in sync.

**Independent Test**: Compare a representative runtime shader and editor shader against the documented convention rules and confirm both satisfy them.

- [ ] T011 [US2] Update shared shader convention notes in `Docs/Rendering/`
- [ ] T012 [US2] Align representative runtime and editor shader assets with the documented convention
- [ ] T013 [US2] Review default resources for naming or binding drift introduced by the rendering change

**Checkpoint**: Contributors can follow one documented convention set for both runtime and editor shader work.

---

## Phase 5: User Story 3 - Preserve Cross-Platform Verification Notes (Priority: P3)

**Goal**: Make review-time validation honest about what was and was not tested.

**Independent Test**: Read the bundle and confirm it states which backend/platform pair was validated, what evidence was collected, and what remains unverified.

- [ ] T014 [US3] Record tested backend and platform details in the bundle or review notes
- [ ] T015 [US3] Record any unverified platforms, unverified backends, and known follow-up checks
- [ ] T016 [US3] Summarize final validation evidence before the change is marked complete

**Checkpoint**: The bundle does not overstate validation coverage.

---

## Final Verification

- [ ] T017 Re-read `AGENTS.md` and `Docs/AIWorkflow.md` to confirm the example still matches repository workflow rules
- [ ] T018 Ensure the example bundle contains `spec.md`, `plan.md`, and `tasks.md`
- [ ] T019 Verify the official helper script still scaffolds a new major-change spec in an isolated smoke-test repository
