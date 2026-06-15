# Tasks: Remove DX12 Legacy UI Bridge

**Input**: Design documents from `/specs/050-remove-dx12-ui-bridge/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`

**Tests**: Required by the spec. Add and run focused guards before the removal work lands.

**Organization**: Tasks are grouped by user story so the legacy bridge removal, unsupported-path behavior, and platform backend preservation can be implemented and validated incrementally.

## Phase 1: Setup (Shared Validation Infrastructure)

**Purpose**: Establish regression coverage before changing runtime selection or deleting the old bridge.

- [X] T001 [P] [US1] Extend `Tests/Unit/RHIUiOverlaySourceGuardTests.cpp` with guards that fail if migrated DX12 code can still reach `DX12UIBridge::RenderDrawData`, `WaitForBackbufferReuse`, or `SubmitCommandBuffer`
- [X] T002 [P] [US2] Extend `Tests/Unit/UIAndToolingBackendAwarenessTests.cpp` with checks that unsupported UI selection returns null/unsupported rather than the legacy DX12 bridge
- [X] T003 [P] [US3] Extend `Tests/Unit/RHIUiOverlayPassTests.cpp` with coverage that platform backend setup remains separate from renderer bridge selection on the migrated DX12 path

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Remove the runtime selection hook and make the unsupported path explicit before deleting the implementation.

**Critical**: No legacy bridge deletion should land until the selector no longer chooses it.

- [X] T004 Update `Runtime/Rendering/RHI/Utils/RHIUIBridgeInternal.h` to remove the DX12 bridge factory declaration once no caller needs it
- [X] T005 Update `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` so migrated DX12 returns the null/unsupported bridge and no longer falls back to `CreateDX12RHIUIBridge`
- [X] T006 Update `Runtime/UI/UIManager.cpp` so migrated DX12 rendering uses the frame-graph snapshot path without depending on legacy bridge submit/signal branches

**Checkpoint**: The runtime selector no longer chooses the old DX12 bridge on the migrated path.

---

## Phase 3: User Story 1 - FrameGraph Only On DX12 (Priority: P1)

**Goal**: DX12 UI rendering is owned only by the migrated RHI frame-graph overlay path.

**Independent Test**: Start Editor or Launcher on DX12 and confirm UI renders while the legacy DX12 direct-submit bridge never appears in the trace or source guard output.

### Tests for User Story 1

- [X] T007 [P] [US1] Add source-guard coverage in `Tests/Unit/RHIUiOverlaySourceGuardTests.cpp` for the removed DX12 direct-submit path and queue-signal helpers
- [X] T008 [P] [US1] Add pass-selection coverage in `Tests/Unit/RHIUiOverlayPassTests.cpp` proving migrated DX12 uses the overlay path instead of the legacy bridge

### Implementation for User Story 1

- [X] T009 [US1] Remove the DX12 direct-submit implementation from `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp`
- [X] T010 [US1] Remove the DX12 bridge creation fallback from `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp`
- [X] T011 [US1] Remove migrated-path legacy bridge branches from `Project/Editor/Core/Editor.cpp`
- [X] T012 [US1] Remove migrated-path legacy bridge branches from `Project/Launcher/Core/Launcher.cpp`

**Checkpoint**: DX12 migrated UI rendering no longer depends on the old direct-submit bridge.

---

## Phase 4: User Story 2 - Unsupported Backends Fail Closed (Priority: P2)

**Goal**: Unsupported configurations report unsupported state instead of silently re-entering the removed DX12 bridge.

**Independent Test**: Start an unsupported configuration and verify the selector returns null/unsupported with explicit diagnostics.

### Tests for User Story 2

- [X] T013 [P] [US2] Add unsupported-selection coverage in `Tests/Unit/UIAndToolingBackendAwarenessTests.cpp`
- [X] T014 [P] [US2] Add regression coverage in `Tests/Unit/RHIUiOverlaySourceGuardTests.cpp` for explicit unsupported behavior instead of hidden fallback

### Implementation for User Story 2

- [X] T015 [US2] Keep `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` fail-closed for unsupported UI overlay capability states
- [X] T016 [US2] Update `Runtime/UI/UIManager.cpp` diagnostics so unsupported UI paths are explicit and do not masquerade as DX12 legacy fallback

**Checkpoint**: Unsupported configurations no longer have a hidden path back to the old bridge.

---

## Phase 5: User Story 3 - Platform Backends Stay Intact (Priority: P3)

**Goal**: Win32/GLFW input and window handling continue to work after the DX12 bridge removal.

**Independent Test**: Launch Editor and Launcher and verify platform input still works while UI rendering uses the migrated path.

### Tests for User Story 3

- [X] T017 [P] [US3] Add platform-backend preservation coverage in `Tests/Unit/UIAndToolingBackendAwarenessTests.cpp`
- [X] T018 [P] [US3] Add migrated-runtime coverage in `Tests/Unit/RHIUiOverlayPassTests.cpp` for UI rendering without changing platform backend initialization

### Implementation for User Story 3

- [X] T019 [US3] Keep `ImGui_ImplWin32` and `ImGui_ImplGlfw` setup, new-frame, and shutdown wiring intact in `Runtime/UI/UIManager.cpp`
- [ ] T020 [US3] Verify `Project/Editor/Core/Editor.cpp` still renders and presents through the normal migrated UI flow with a DX12 interactive smoke run
- [ ] T021 [US3] Verify `Project/Launcher/Core/Launcher.cpp` still renders and presents through the normal migrated UI flow with a DX12 interactive smoke run

**Checkpoint**: UI input and window handling remain functional while the renderer bridge is removed.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Final cleanup, validation evidence, and source-grep confirmation.

- [X] T022 Update `specs/050-remove-dx12-ui-bridge/validation/final-diagnostics.md` with build, test, and source-guard results
- [X] T023 Run `cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /p:CL_MPCount=1 /v:m` and record the result in `specs/050-remove-dx12-ui-bridge/validation/final-diagnostics.md`
- [X] T024 Run the focused unit tests from `specs/050-remove-dx12-ui-bridge/quickstart.md` and record pass/fail output in `specs/050-remove-dx12-ui-bridge/validation/final-diagnostics.md`
- [X] T025 Run source-grep validation for legacy DX12 bridge references and record findings in `specs/050-remove-dx12-ui-bridge/validation/final-diagnostics.md`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Setup and blocks all user-story implementation.
- **User Story 1 (Phase 3)**: Depends on Foundational and removes the legacy DX12 bridge path.
- **User Story 2 (Phase 4)**: Depends on Foundational and verifies unsupported behavior stays explicit.
- **User Story 3 (Phase 5)**: Depends on Foundational and confirms platform backend preservation.
- **Polish (Phase 6)**: Depends on desired story phases being complete.

### User Story Dependencies

- **US1**: Required core cleanup for DX12 frame-graph ownership.
- **US2**: Depends on the selector cleanup from US1/Foundation but remains independently testable.
- **US3**: Depends on preserving `UIManager` platform setup while removing legacy renderer ownership.

### Parallel Opportunities

- T001-T003 can run in parallel.
- T007-T008 can run in parallel.
- T013-T014 can run in parallel.
- T017-T018 can run in parallel.

## Parallel Example: User Story 1

```text
Task: "Add source-guard coverage in Tests/Unit/RHIUiOverlaySourceGuardTests.cpp for the removed DX12 direct-submit path and queue-signal helpers"
Task: "Add pass-selection coverage in Tests/Unit/RHIUiOverlayPassTests.cpp proving migrated DX12 uses the overlay path instead of the legacy bridge"
```

## Implementation Strategy

### MVP First

1. Complete Setup and Foundational tasks.
2. Remove the runtime selection hook and legacy DX12 bridge path.
3. Validate the DX12 migrated UI path still renders without direct-submit ownership.

### Incremental Delivery

1. Add guards first.
2. Remove selection fallback.
3. Delete legacy bridge submission code.
4. Confirm unsupported behavior is explicit.
5. Confirm platform backends still initialize and process input.
6. Record validation evidence.

### Safety Notes

- Do not edit `Runtime/*/Gen/`.
- Do not keep a silent DX12 fallback to the removed bridge.
- Do not remove `ImGui_ImplWin32` or `ImGui_ImplGlfw` platform backend wiring.
- Treat any reappearance of native DX12 queue submit/signal/present ownership in the UI path as a regression.
