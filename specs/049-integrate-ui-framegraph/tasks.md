# Tasks: Integrate UI FrameGraph

**Input**: Design documents from `/specs/049-integrate-ui-framegraph/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`

**Tests**: Required by SC-006. Add or update tests before the corresponding implementation tasks.

**Organization**: Tasks are grouped by user story so DX12 UI overlay migration, UI-only frames, and UI resource compatibility can be implemented and validated incrementally.

## Phase 1: Setup (Shared Test And Validation Infrastructure)

**Purpose**: Establish source guards and test entrypoints before production code changes.

- [X] T001 [P] Create migrated-path source guard tests in `Tests/Unit/RHIUiOverlaySourceGuardTests.cpp`
- [X] T002 [P] Create UI draw snapshot test entrypoint in `Tests/Unit/UiDrawDataSnapshotTests.cpp`
- [X] T003 [P] Create RHI UI overlay pass and backend capability-gating test entrypoint in `Tests/Unit/RHIUiOverlayPassTests.cpp`
- [X] T004 [P] Create UI texture registry test entrypoint in `Tests/Unit/RHIUiTextureRegistryTests.cpp`
- [X] T005 Add the final validation checklist fields to `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Shared model, capability, and pass plumbing required by all stories.

**Critical**: No user story implementation should proceed until the snapshot model, capability gate, and pass identity compile.

- [X] T006 [P] Define `Render::UI::UiDrawDataSnapshot` and copied draw-list data types in `Runtime/Rendering/UI/UiDrawDataSnapshot.h`
- [X] T007 [P] Implement ImGui draw-data capture and empty/callback validation in `Runtime/Rendering/UI/UiDrawDataSnapshot.cpp`
- [X] T008 [P] Define `Render::UI::UiTextureId` and registry entry types in `Runtime/Rendering/UI/RHIImGuiTextureRegistry.h`
- [X] T009 [P] Define `Render::UI::RHIImGuiFontAtlas` resource interface in `Runtime/Rendering/UI/RHIImGuiFontAtlas.h`
- [X] T010 [P] Define `Render::UI::RHIImGuiOverlayRenderer` interface and recording result types in `Runtime/Rendering/UI/RHIImGuiOverlayRenderer.h`
- [X] T011 Add `RHIDeviceFeature::UIOverlayFrameGraph` and synchronized legacy flag support in `Runtime/Rendering/RHI/RHITypes.h`
- [X] T012 Add DX12 `UIOverlayFrameGraph` capability plumbing while keeping product routing not runtime-selectable, and add explicit unsupported reasons for non-migrated backends in `Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp` and `Runtime/Rendering/RHI/Core/RHIDevice.h`
- [X] T013 Add `RenderPassCommandKind::UIOverlay` and pass debug-name mapping in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h` and `Runtime/Rendering/Context/RenderScenePackageBuilder.cpp`
- [X] T014 Add UI overlay payload fields to `RenderPassCommandInput` and `RenderScenePackage` in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`
- [X] T015 Add `RHIFrameGraph::UIOverlay` metadata helpers in `Runtime/Rendering/FrameGraph/FrameGraphExecutionTypes.h`
- [X] T016 Extend pass plan application and visible-count accounting for UI overlay passes in `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h`
- [X] T017 Add pending UI snapshot storage and synchronization fields to `Runtime/Rendering/Context/DriverInternal.h`
- [X] T018 Add `DriverUIAccess` publish/consume declarations for UI overlay snapshots and UI texture registration/release APIs in `Runtime/Rendering/Context/DriverAccess.h`
- [X] T019 Update TimelineProfiler UI event classification for `RHIFrameGraph::UIOverlay` in `Runtime/UI/ImGuiExtensions/TimelineProfiler/ProfilerTraceCursor.h`

**Checkpoint**: Snapshot types, capability feature, pass kind, and accessors compile with failing behavior tests still expected.

---

## Phase 3: User Story 1 - UI Rendering Uses The Frame Lifecycle (Priority: P1) MVP

**Goal**: DX12 Editor UI draw work is recorded as the final RHI swapchain overlay pass instead of direct native UI bridge submission.

**Independent Test**: Run DX12 Editor with the Profiler panel open and confirm UI is visible while TimelineProfiler no longer records per-frame `DX12UIBridge::WaitForBackbufferReuse` on the main UI render path.

### Tests for User Story 1

- [X] T020 [P] [US1] Add snapshot lifetime, unsupported callback, and stable texture-generation copy tests in `Tests/Unit/UiDrawDataSnapshotTests.cpp`
- [X] T021 [P] [US1] Add scene-pass then `RHIFrameGraph::UIOverlay` ordering, resource access, dependency edge, exported swapchain transition, and font atlas first-use tests in `Tests/Unit/RHIUiOverlayPassTests.cpp`
- [X] T022 [P] [US1] Add source guard asserting `UIManager::Render` no longer calls `m_uiBridge->RenderDrawData` and `RHIImGuiOverlayRenderer`/`RHIImGuiFontAtlas` do not call native DX12 submit/signal/present APIs on migrated path in `Tests/Unit/RHIUiOverlaySourceGuardTests.cpp`
- [X] T023 [P] [US1] Add source guard asserting `Editor::RenderEditorUI` and Launcher no longer require UI semaphore/submit calls on migrated DX12 path in `Tests/Unit/RHIUiOverlaySourceGuardTests.cpp`
- [X] T024 [P] [US1] Add Profiler classification and no-second-UI-submit trace classification tests for `RHIFrameGraph::UIOverlay` in `Tests/Unit/ProfilerDestinationTests.cpp`

### Implementation for User Story 1

- [X] T025 [US1] Change `UIManager::Render` to capture and publish `UiDrawDataSnapshot` instead of immediately calling `RenderDrawData` in `Runtime/UI/UIManager.cpp`
- [X] T026 [US1] Add UI snapshot publication APIs, UI texture registration/release API wiring, and storage handling while leaving product DX12 runtime selection disabled until all migrated paths are complete in `Runtime/Rendering/Context/Driver.cpp`
- [X] T027 [US1] Attach visible UI snapshots to scene `RenderScenePackage` instances in `Runtime/Rendering/Context/RenderScenePackageBuilder.cpp`
- [X] T028 [US1] Build a `UIOverlay` pass input with load/store swapchain attachment semantics, concrete `TextureResourceAccess`/`BufferResourceAccess`, dependency edges, exported swapchain transition, and font/texture shader-read subresource ranges in `Runtime/Rendering/Context/RenderScenePackageBuilder.cpp`
  - Progress 2026-06-13: scene-frame ordering, swapchain render-target write access, exported present transition, and scene-to-UI dependency edge are covered. The package no longer declares null dynamic UI vertex/index buffer resources; concrete dynamic-buffer visibility is recorded when the renderer owns the actual per-frame buffers. Remaining: concrete graph-visible font atlas and registered UI texture shader-read subresource ranges.
  - Completed 2026-06-14: package build still declares package-level overlay ordering, swapchain render-target write, scene-to-UI dependency, and exported present transition. Concrete dynamic vertex/index buffer reads, font atlas shader-read range plus first-upload `CopyDst -> ShaderRead` visibility, and registered UI texture shader-read ranges are appended by `RhiThreadCoordinator::PrepareUiOverlayPassInput()` after `RHIImGuiOverlayRenderer::PrepareFrameResources()` creates/owns the concrete RHI resources. This keeps the builder from publishing null placeholder resources while satisfying the full effective pass-input contract before `BeginPassCommandPlan`.
- [X] T029 [US1] Implement core `RHIImGuiOverlayRenderer` pipeline, shader artifact registration through the existing ShaderArtifact/ShaderManager path, dynamic buffer sizing, scissor setup, font atlas first-use upload/binding, and indexed draw recording in `Runtime/Rendering/UI/RHIImGuiOverlayRenderer.cpp` and `Runtime/Rendering/UI/RHIImGuiFontAtlas.cpp`
  - Progress 2026-06-13: dynamic CPU-to-GPU vertex/index buffer sizing, per-frame-resource-slot buffer ownership, upload, pre-pass HostWrite-to-GenericRead visibility barrier recording, prepared-buffer binding, renderer-owned graphics pipeline request/cache/bind contract, scissor setup, split-path global draw-list offsets, unsupported texture-ID command skipping, and indexed draw recording are implemented. Public `Record(...)` convenience paths were removed so dynamic uploads must use `PrepareFrameResources()` before pass begin and `RecordPrepared()` inside the pass; `RecordPrepared()` rejects snapshots that do not match the data prepared for the frame-resource slot. Renderer pipeline creation now requires an explicit RHI pipeline layout plus vertex/fragment shader modules before `CreateGraphicsPipeline()`, and fails closed when any step fails.
  - Progress 2026-06-14: overlay vertex/fragment shader modules are now loaded through `ShaderManager`/`ShaderArtifact` instead of placeholder bytecode, `App/Assets/Engine/Shaders/RHIImGuiOverlay.hlsl` was added, and the pipeline layout/recording path now declares and pushes overlay projection constants. Remaining: font atlas first-use upload/binding and registered texture binding.
  - Progress 2026-06-14: font atlas first-use upload now builds ImGui RGBA32 atlas pixels into an RHI texture/view/sampler/binding set, the overlay pipeline layout declares the font atlas texture/sampler binding layout, prepared recording binds the font atlas binding set before indexed draws, and the overlay pixel shader samples the font atlas.
  - Completed 2026-06-14: registered UI textures now resolve through `RHIImGuiTextureRegistry::ResolveForFrame()`, create/cache RHI binding sets with the overlay texture/sampler layout, and bind per draw without native descriptor handles. The renderer also exposes prepared font/registered texture views and dynamic buffers for frame-graph visibility.
- [X] T030 [US1] Record `UIOverlay` pass payloads and upload-to-read visibility for atlas and dynamic buffers from `Detail::RecordThreadedRhiWork` in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
  - Progress 2026-06-13: threaded worker and serial paths prepare and record the `UIOverlay` payload through the split `RHIImGuiOverlayRenderer` API, pass the active frame resource slot through every overlay recording path, and record dynamic-buffer upload-to-read visibility before `BeginPassCommandPlan` once the concrete buffers exist. Remaining: atlas upload-to-read visibility and full frame-graph resource registration for atlas/registered UI textures.
  - Completed 2026-06-14: all UI overlay recording paths enrich the effective pass input after resource preparation with concrete vertex/index buffer read accesses, font atlas shader-read texture access plus copy/upload-to-shader-read visibility, and registered UI texture shader-read accesses before `BeginPassCommandPlan`.
- [X] T031 [US1] Ensure `UIOverlay` failures mark command recording/device-lost state through existing submission telemetry in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
- [X] T032 [US1] Update Editor UI rendering to publish snapshots and remove migrated-path UI signal/submit flow in `Project/Editor/Core/Editor.cpp`
- [X] T033 [US1] Update Launcher UI rendering to publish snapshots and remove migrated-path `SubmitUIRendering` flow in `Project/Launcher/Core/Launcher.cpp`
- [X] T034 [US1] Gate `DX12UIBridge::RenderDrawData`, `WaitForAllocatorReuse`, `ExecuteCommandLists`, and `SubmitCommandBuffer` so DX12 `UIOverlayFrameGraph` never selects direct submit in `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp`
- [X] T035 [US1] Update `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` null/fallback behavior for unsupported UI overlay capability
- [X] T036 [US1] Run US1 focused tests and record overlay ordering, resource access, font atlas MVP, and direct-submit guard results in `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`

**Checkpoint**: User Story 1 is functional when DX12 scene + UI frames show visible UI through the RHI overlay pass with no main-path direct UI bridge submission.

---

## Phase 4: User Story 2 - UI-Only Frames Use The Same Present Path (Priority: P2)

**Goal**: UI remains responsive without scene work and still uses normal swapchain acquire, submit, fence, and present.

**Independent Test**: Publish a visible UI snapshot with no scene package and confirm a normal swapchain `RenderScenePackage` with only `RHIFrameGraph::UIOverlay` is submitted and presented.

### Tests for User Story 2

- [X] T037 [P] [US2] Add UI-only package publication and single RHI submit/present ownership tests in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`
- [X] T038 [P] [US2] Add source guard preventing standalone UI explicit frame startup when `UIOverlayFrameGraph` is supported in `Tests/Unit/RHIUiOverlaySourceGuardTests.cpp`
- [X] T039 [P] [US2] Add resize-during-UI-only retention test in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`

### Implementation for User Story 2

- [X] T040 [US2] Add `DriverUIAccess::PublishUiOnlyFrame` implementation using normal prepared frame publication in `Runtime/Rendering/Context/Driver.cpp`
- [X] T041 [US2] Build UI-only `RenderScenePackage` instances with only `UIOverlay` pass inputs in `Runtime/Rendering/Context/RenderScenePackageBuilder.cpp`
- [X] T042 [US2] Skip `BeginStandaloneUiExplicitFrame` when `UIOverlayFrameGraph` is supported in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
- [X] T043 [US2] Route `DriverUIAccess::PresentSwapchain` resize handling through normal frame retirement for migrated UI frames in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
- [X] T044 [US2] Preserve explicit unsupported-backend and not-runtime-selectable diagnostics instead of silent direct-submit fallback in `Runtime/UI/UIManager.cpp`
- [X] T045 [US2] Run US2 focused tests and record results in `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`

**Checkpoint**: UI-only frames are visible and never invoke the old standalone UI explicit frame path on DX12 migrated capability.

---

## Phase 5: User Story 3 - UI Resources Stay Stable Across Frames (Priority: P3)

**Goal**: Fonts, icons, and UI texture previews continue to render correctly through renderer-owned RHI resources.

**Independent Test**: Render text/icons and at least one registered RHI texture view across multiple frames, resize, and release.

### Tests for User Story 3

- [X] T046 [P] [US3] Add stable `UiTextureId` registration through `DriverUIAccess`, generation validation, stale-ID fallback, renderer-owned registry boundary, and lookup tests in `Tests/Unit/RHIUiTextureRegistryTests.cpp`
- [X] T047 [P] [US3] Add texture release plus in-flight view/binding-set/descriptor allocation retention tests in `Tests/Unit/RHIUiTextureRegistryTests.cpp`
- [X] T048 [P] [US3] Add font atlas rebuild, descriptor-retirement, and upload-without-native-direct-submit tests in `Tests/Unit/RHIUiOverlayPassTests.cpp`
- [X] T049 [P] [US3] Add source guard proving `Image`, `ButtonImage`, and migrated UI texture binding code no longer depend on native DX12 descriptor handles or native DX12 submit/signal/present APIs in `Tests/Unit/RHIUiOverlaySourceGuardTests.cpp`

### Implementation for User Story 3

- [X] T050 [US3] Implement `RHIImGuiTextureRegistry` registration, generation-checked lookup, release request, visible fallback, binding-set/descriptor retention, and frame-retirement hooks in `Runtime/Rendering/UI/RHIImGuiTextureRegistry.cpp`
- [X] T051 [US3] Update `UIManager::ResolveTextureView` to call `DriverUIAccess` texture registration APIs and return stable UI texture identities for migrated overlay rendering in `Runtime/UI/UIManager.cpp`
- [X] T052 [US3] Update `Image` widget texture ID usage in `Runtime/UI/Widgets/Visual/Image.cpp`
- [X] T053 [US3] Update `ButtonImage` widget texture ID usage in `Runtime/UI/Widgets/Buttons/ButtonImage.cpp`
- [X] T054 [US3] Implement `RHIImGuiFontAtlas` rebuild/retire behavior with frame-retired texture/view/binding-set/descriptor lifetimes in `Runtime/Rendering/UI/RHIImGuiFontAtlas.cpp`
- [X] T055 [US3] Bind registered texture binding sets during overlay draw recording without native descriptor handles in `Runtime/Rendering/UI/RHIImGuiOverlayRenderer.cpp`
- [X] T056 [US3] Route `NotifyFontAtlasChanged`, `NotifySwapchainWillResize`, and `ReleaseTextureViewHandle` through overlay resources in `Runtime/UI/UIManager.cpp`
- [X] T057 [US3] Add final DX12 `UIOverlayFrameGraph` runtime-selectable enablement guarded by US1, US2, and US3 migrated-path validation state; run US3 focused tests before recording the feature as selectable in `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`

**Checkpoint**: UI text/icons/texture previews remain stable through frame turnover, resize, and texture release.

---

## Phase 6: Polish & Cross-Cutting Validation

**Purpose**: Final checks for backend scope, performance evidence, and review gates.

- [X] T058 Update any stale migration notes in `specs/049-integrate-ui-framegraph/quickstart.md`
- [X] T059 Run the full targeted unit test command from `specs/049-integrate-ui-framegraph/quickstart.md` and write output summary to `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`
- [X] T060 Run source guard grep from `specs/049-integrate-ui-framegraph/quickstart.md` and write findings to `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`
- [X] T061 Capture before/after 300-frame DX12 Editor TimelineProfiler traces with the Profiler panel open and write wait-count, submit-count, present-owner, snapshot-copy, dynamic-buffer, hardware/build/workload evidence to `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`
- [X] T062 Capture or attempt DX12 RenderDoc evidence using `Tools/RenderDoc/renderdoc_runner.py` and write pass order, resource-state/subresource, transition, submit/present, capture path or blocker evidence to `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`
- [X] T063 Run Editor, Launcher, and Game DX12 smoke validation plus unsupported non-DX12 capability diagnostics and write product-runtime evidence to `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`
- [X] T064 Run mandatory `plan-review` quality gate for the implementation and write summary to `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`
- [X] T065 Run mandatory multi-agent review for GPU sync/RHI abstraction changes and write P0/P1/P2 summary to `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Setup test entrypoints.
- **User Story 1 (Phase 3)**: Depends on Foundational and builds the scene+UI overlay MVP, including minimum font atlas visibility, but does not make DX12 product routing runtime-selectable by itself.
- **User Story 2 (Phase 4)**: Depends on Foundational and US1 overlay primitives; DX12 product routing remains not runtime-selectable until UI-only frames are normal swapchain packages.
- **User Story 3 (Phase 5)**: Depends on Foundational and US1/US2 routing; final DX12 runtime selection is enabled only after texture/font resource retention and direct-submit guards pass.
- **Polish (Phase 6)**: Depends on selected story phases being complete.

### User Story Dependencies

- **US1**: Required scene+UI overlay MVP with minimum font atlas support and no direct UI bridge submit on the tested scene-frame path.
- **US2**: Extends routing for UI-only frames and is required before DX12 product routing can be runtime-selectable.
- **US3**: Extends texture/font resource compatibility and performs the final DX12 runtime selection enablement.

### Parallel Opportunities

- T001-T004 can run in parallel.
- T006-T010 can run in parallel once the `Runtime/Rendering/UI` namespace is chosen.
- US1 tests T020-T024 can run in parallel.
- US2 tests T037-T039 can run in parallel.
- US3 tests T046-T049 can run in parallel.
- Texture registry implementation can proceed in parallel with Editor/Launcher call-site updates after T008-T010, but final DX12 runtime selection waits for T057.

---

## Parallel Example: User Story 1

```text
Task: "Add snapshot lifetime and unsupported callback tests in Tests/Unit/UiDrawDataSnapshotTests.cpp"
Task: "Add scene-pass then RHIFrameGraph::UIOverlay ordering tests in Tests/Unit/RHIUiOverlayPassTests.cpp"
Task: "Add source guard asserting UIManager and RHI UI overlay/font atlas code avoid legacy bridge and native DX12 direct submit in Tests/Unit/RHIUiOverlaySourceGuardTests.cpp"
Task: "Add Profiler classification test for RHIFrameGraph::UIOverlay in Tests/Unit/ProfilerDestinationTests.cpp"
```

---

## Implementation Strategy

### MVP First

1. Complete Setup and Foundational tasks.
2. Complete User Story 1 and validate the internal scene+UI overlay path with minimum font atlas support.
3. Complete User Story 2 so UI-only frames use the normal swapchain package path.
4. Complete User Story 3 resource compatibility, then enable DX12 runtime selection and run product smoke.

### Incremental Delivery

1. Snapshot and capability primitives.
2. DX12 overlay pass path for scene frames with concrete resource access/transition records and minimum font atlas support.
3. UI-only normal frame routing.
4. Texture/font resource parity plus final DX12 runtime-selectable capability.
5. Full DX12 trace/RenderDoc/review gates with before/after submit/present/performance evidence.

### Safety Notes

- Do not edit `Runtime/*/Gen/`.
- Do not claim non-DX12 backend support until backend-specific validation exists.
- Do not remove legacy bridge code until migrated DX12 path and fallback behavior are verified.
- Treat any new CPU/GPU wait, direct native queue submit, or hidden standalone UI explicit frame as a regression.
