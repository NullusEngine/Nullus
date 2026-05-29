# Tasks: Editor Scene View CPU Frame-Time Optimization

**Input**: Design documents from `specs/037-scene-view-cpu/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Required. Write each regression test first and verify it fails before implementation.

## Phase 1: Setup

**Purpose**: Confirm baseline and locate executable entrypoints.

- [X] T001 Record current git status and active branch before implementation
- [X] T002 Confirm `NullusUnitTests` executable or build target location for focused filters in `specs/037-scene-view-cpu/quickstart.md`

---

## Phase 2: Foundational

**Purpose**: Establish tests before production edits.

- [X] T003 [P] Add failing stable-size GBuffer reuse regression test in `Tests/Unit/DeferredSceneRendererMaterialCacheTests.cpp`
- [X] T004 [P] Add failing incomplete trace event export regression test in `Tests/Unit/ProfilerDestinationTests.cpp`
- [X] T005 Run focused tests and record the expected failing assertions for T003 and T004

**Checkpoint**: Regression tests fail for the intended missing behavior.

---

## Phase 3: User Story 1 - Stable Scene View Rendering Is Smoother (Priority: P1)

**Goal**: Avoid redundant deferred Scene View resource preparation when dimensions are unchanged.

**Independent Test**: The renderer test proves stable-size calls reuse prepared GBuffer wrappers, while resize still refreshes them.

### Tests for User Story 1

- [X] T006 [US1] Verify `DeferredSceneRendererMaterialCacheTests` stable-size regression fails before implementation

### Implementation for User Story 1

- [X] T007 [US1] Expose minimal renderer test access for GBuffer target preparation in `Runtime/Engine/Rendering/DeferredSceneRenderer.h`
- [X] T008 [US1] Implement stable-size no-op behavior in `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`
- [X] T009 [US1] Preserve resize and zero-size invalidation behavior in `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`
- [X] T010 [US1] Run focused renderer tests and update validation notes in `specs/037-scene-view-cpu/quickstart.md`

**Checkpoint**: User Story 1 is independently testable and passing.

---

## Phase 4: User Story 2 - Frame-Time Evidence Is Trustworthy (Priority: P2)

**Goal**: Ensure trace export emits only completed non-negative duration events.

**Independent Test**: The profiler export test proves incomplete events are skipped and exported durations are valid.

### Tests for User Story 2

- [X] T011 [US2] Verify `ProfilerDestinationTests` trace export regression fails before implementation

### Implementation for User Story 2

- [X] T012 [US2] Extract or reuse trace export event eligibility logic in `Runtime/UI/ImGuiExtensions/TimelineProfiler/ProfilerWindow.cpp` or an included helper
- [X] T013 [US2] Filter incomplete or non-positive tick events during trace export in `Runtime/UI/ImGuiExtensions/TimelineProfiler/ProfilerWindow.cpp`
- [X] T014 [US2] Run focused profiler tests and update validation notes in `specs/037-scene-view-cpu/quickstart.md`

**Checkpoint**: User Story 2 is independently testable and passing.

---

## Phase 5: User Story 3 - Selected Object Debug Rendering Is Traceable And Avoids Hidden Work (Priority: P1)

**Goal**: Split the `Debug GameObject` trace cost into actionable child scopes, avoid selected-object debug work that cannot produce visible output, and preserve shared editor shader backend requirements.

**Independent Test**: Contract tests confirm editor debug scope markers are present, selected-object debug draw recursion is gated by visible categories, outline capture collects the selected tree once, and renderer binding tests prove the shared `Unlit.hlsl` helper shader stays on the legacy object constants path for backend compatibility.

### Tests for User Story 3

- [X] T019 [US3] Add failing contract coverage for `Debug GameObject` nested profiler scopes in `Tests/Unit/EditorRenderPathContractTests.cpp`
- [X] T020 [US3] Add failing `Unlit.hlsl` backend compatibility coverage in `Tests/Unit/RendererFrameObjectBindingTests.cpp`
- [X] T021 [US3] Run focused contract tests and record expected failures
- [X] T022 [US3] Add failing contract coverage for selected-object debug draw gating and single-collection outline capture in `Tests/Unit/EditorRenderPathContractTests.cpp`

### Implementation for User Story 3

- [X] T023 [US3] Add named CPU scopes to `Project/Editor/Rendering/DebugSceneRenderer.cpp` and `Project/Editor/Rendering/OutlineRenderer.cpp`
- [X] T024 [US3] Gate selected-object debug draw recursion before traversal when no selected-object debug categories can be visible
- [X] T025 [US3] Reuse one collected outline draw-item list for stencil and shell command emission in `Project/Editor/Rendering/OutlineRenderer.cpp`
- [X] T026 [US3] Keep `App/Assets/Engine/Shaders/Unlit.hlsl` on legacy `ObjectConstants` after review found shared editor backend risk
- [X] T027 [US3] Run focused renderer contract tests and update validation notes in `specs/037-scene-view-cpu/quickstart.md`
- [X] T028 [US3] Re-check generated-file boundaries and backend claim wording

**Checkpoint**: The next exported trace can attribute `Debug GameObject` internally, while source contracts enforce two low-risk selected-object CPU reductions without changing shared helper shader backend behavior.

---

## Phase 6: Review-Driven Deferred GBuffer Safety

**Purpose**: Apply final review feedback that incomplete or mismatched deferred GBuffer resources must not reach graph or threaded RHI pass inputs.

- [X] T029 Add failing threaded incomplete/mismatched GBuffer regression tests in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [X] T030 Implement complete and matching GBuffer resource guards in `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp`
- [X] T031 Update existing deferred prepared execution tests to use complete test GBuffer resources in `Tests/Unit/EditorRenderPathContractTests.cpp` and `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [X] T031a Add failing stale-depth helper regression coverage for invalid prepared GBuffer resources in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [X] T031b Declare selection-outline helper depth/stencil writes explicitly in `Project/Editor/Rendering/DebugSceneRenderer.cpp` and `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp`
- [X] T031c Consolidate deferred GBuffer format/usage/slot checks around shared constants in `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.*` and `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`

**Checkpoint**: Graph and threaded deferred execution both skip GBuffer/Lighting pass construction when GBuffer resources are incomplete or mismatched, explicit helpers are preserved without stale GBuffer depth attachment, and selection-outline helpers declare depth/stencil writes.

---

## Phase 7: Polish & Review

**Purpose**: Validate, self-review, and run required quality gates after every story and review-driven hardening task.

- [X] T032 Run combined focused test filter from `specs/037-scene-view-cpu/quickstart.md`
- [X] T033 Inspect diff for generated-file edits, backend overclaims, and unrelated churn
- [X] T034 Run `/plan-review` quality gate until repository thresholds are satisfied
- [X] T035 Summarize validation evidence and remaining runtime trace limitations

---

## Phase 8: Trace-Driven Debug GameObject Follow-Up

**Purpose**: Apply the next optimization after the 2026-05-28 recaptured trace still showed `Debug GameObject` dominating Scene View CPU time while `Picking` and `EnsureGBufferTargets` were no longer material.

- [X] T036 Parse the recaptured `App/Win64_Debug_Runtime_Shared/trace.json` and record current hotspot evidence in `specs/037-scene-view-cpu/quickstart.md`
- [X] T037 Add failing contract coverage that threaded Debug GameObject capture does not open an empty output render pass in `Tests/Unit/EditorRenderPathContractTests.cpp`
- [X] T038 Add failing contract coverage that Debug GameObject reuses one current-frame selected-tree mesh/transform collection for debug bounds and outline capture
- [X] T039 Implement Debug GameObject render-pass ownership so only non-threaded immediate outline drawing opens the output render pass
- [X] T040 Extend `Project/Editor/Rendering/DebugGameObjectSelectionCollector.h` and `Project/Editor/Rendering/OutlineRenderer.*` to reuse selected-tree mesh/transform data for bounds and outline while preserving selected camera icon outline behavior
- [X] T041 Run focused EditorRenderPath tests, the combined focused filter, and `Editor` build; update validation notes

**Checkpoint**: The next runtime trace should show whether the previous ~123 ms `Debug GameObject` parent scope was primarily empty render-pass overhead, selected-tree duplicate resolution, or remaining command-capture work.

---

## Phase 9: Review-Driven Debug GameObject Copy-Path Follow-Up

**Purpose**: Apply review feedback that the selected-object follow-up still copied large prepared helper command vectors and left a non-threaded zero-outline render-pass edge case.

- [X] T042 Add failing contract coverage that prepared editor helper inputs avoid recorded-command vector copies across pass getters, appended pass assembly, and deferred builder capture
- [X] T043 Change editor helper pass getters to return const references and move appended helper inputs into the deferred prepared builder closure
- [X] T044 Avoid copying appended helper inputs during prepared package execution unless offscreen external attachment rewriting is required
- [X] T045 Add a prepared-outline guard before non-threaded `Debug GameObject` opens an output render pass and reserve predictable outline recorded-command capacity
- [X] T046 Remove the obsolete selected `modelRenderers` duplicate field so `selectionMeshItems` is the selected mesh SSoT
- [X] T047 Strengthen `EditorRenderPathContractTests` around zero-pass guard ordering, selected-tree reuse, and copy-path avoidance

**Checkpoint**: Main-thread prepared-helper assembly no longer copies selected-outline recorded command vectors by default, non-threaded outline avoids zero-draw render-pass setup, and source/contract tests enforce the refined selected-tree SSoT.

---

## Phase 10: Trace-Driven Debug GameObject Material Binding Follow-Up

**Purpose**: Continue optimizing the 2026-05-28 trace where `Debug GameObject` remained the dominant Scene View CPU cost after GBuffer and Picking were no longer material.

- [X] T048 Re-check `App/Win64_Debug_Runtime_Shared/trace.json` and confirm `Debug GameObject` still dominates while `Picking` and `EnsureGBufferTargets` are low-cost
- [X] T049 Add failing contract coverage that appended prepared helper inputs are movable, not `const` objects passed to `std::move`
- [X] T050 Fix the appended helper input local so selected-outline recorded-command vectors are actually moved into the deferred builder
- [X] T051 Add failing contract coverage that outline shell color application avoids marking the material binding set dirty when the color is unchanged
- [X] T052 Cache the last applied outline color in `OutlineRenderer` and route capture/immediate shell passes through the guarded setter

**Checkpoint**: Prepared helper input transfer no longer falls back to copy through `const std::move`, and repeated selected-outline frames avoid unnecessary `m_outlineMaterial` binding-set invalidation when the outline color is stable.

---

## Phase 11: Review-Driven Prepared Helper Ownership Hardening

**Purpose**: Apply R2/deeper audit feedback after Phase 10 so the copy-avoidance contract cannot regress through lvalue framegraph calls or prepared-builder failure paths.

- [X] T053 Add lifecycle regression coverage that a throwing prepared render-scene builder completes a `PreparedBuilderMissing` package instead of leaving a slot in `RenderSceneResolving`
- [X] T054 Catch prepared builder exceptions in `ThreadedRenderingLifecycle` and complete a fallback render-scene package through the resolving path
- [X] T055 Make `CompileAndApplyPreparedDeferredLightGridSceneExecution` require rvalue `appendedPassInputs` ownership and update tests/callers to use `std::move`
- [X] T056 Consume matched appended helper pass inputs after moving them into package pass inputs so duplicate metadata cannot reuse moved-from command vectors

**Checkpoint**: Prepared helper inputs have explicit ownership transfer at the framegraph API boundary, duplicate helper metadata cannot append moved-from pass inputs, and a prepared-builder exception no longer strands the render-scene slot.

---

## Phase 12: Unity-Style Screen-Space Selection Outline Plan

**Purpose**: Replace the still-expensive selected-object stencil plus inflated shell output with a Unity-inspired selected mask plus fused composite path while preserving only explicit compatible legacy shell fallback cases.

- [X] T057 Parse the 2026-05-28 10:15:58 `App/Win64_Debug_Runtime_Shared/trace.json` and record exact selected-object hotspot plus profiler depth-cap evidence in `specs/037-scene-view-cpu/quickstart.md`
- [X] T058 Add failing profiler coverage that selected outline sub-scopes remain exportable despite the current depth-16 suppression, preferring shallow `SelectionOutlineMask::*` aggregate scopes; only raise `kTimelineProfilerMaxCpuScopeDepth` if tests bound event volume and recording overhead
- [X] T059 Add failing source/build integration coverage for `Project/Editor/Rendering/SelectionOutlineMaskRenderer.*`, shader asset entry `App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl`, channel SSoT include/table files, and build/source-list integration; do not use this task to assert internal implementation strings
- [X] T060 Add failing renderer contract coverage that screen-space selection outline preparation returns an ordered vector of helper pass inputs and matching metadata for mask and composite instead of packing work into one selection pass
- [X] T061 Add failing renderer contract coverage that valid mask resources emit no legacy inflated-shell output pass and produce an empty fallback decision
- [X] T062 Add failing channel-layout contract coverage that C++ mask channel constants and HLSL channel macros come from one SSoT or are mechanically checked for matching group, visible, selected, and classification swizzles; occlusion is derived as selected coverage minus visible coverage
- [X] T063 Add failing resource-lifetime coverage that stable-size mask/intermediate framebuffers are reused only when width, height, format, sample count, scene depth identity, output color identity, and view descriptors match
- [X] T064 Add failing fallback-injection coverage for zero-size targets, missing views, injected allocation failure, stale depth identity, unsupported backend support, and unsupported material mask semantics; tests must assert structured fallback reason enums
- [X] T065 Add failing framegraph/RHI contract coverage for the pass resource matrix: mask writes mask color and reads scene depth read-only; composite reads mask and writes Scene View output color
- [X] T066 Add failing RHI/barrier coverage that write-to-read transitions use render-target/color-attachment write to shader-read states, read-only depth uses depth-read/depth-stencil-read state, and composite output uses color-attachment write
- [X] T067 Add failing pipeline-state coverage that mask capture PSOs disable depth writes and stencil writes while using the read-only scene depth attachment
- [X] T068 Add failing resource-access coverage that the fused composite never reads and writes the same texture subresource and samples the mask through a declared shader-read dependency
- [X] T069 Add failing Unity-compatibility coverage for material `SceneSelectionPass`/alpha-cutout unsupported decisions, visible/occluded dual contribution states, and object-ID preservation during occluded contribution
- [X] T070 Add failing render-eligibility coverage that disabled renderers, hidden editor objects, missing render data, and current Scene View culling/layer ineligible items are excluded before mask command capture
- [X] T071 Add editor shader `App/Assets/Editor/Shaders/SelectionOutlineMask.hlsl` and mask channel include/table files with visible/occluded mask capture plus fused object-ID edge/soft-outline composite logic derived from the Unity reference semantics
- [X] T072 Add `Project/Editor/Rendering/SelectionOutlineMaskRenderer.h` and `Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp` to own stable-size mask/intermediate framebuffers, resource factories or test failure injection, materials, fallback reasons, profiler scopes, and selected-outline pass assembly
- [X] T073 Route `DebugGameObjectRenderPass` in `Project/Editor/Rendering/DebugSceneRenderer.cpp` through the screen-space mask renderer when resources and backend support are available, updating appended-helper assembly, metadata counts, graph pass names, consumption, and duplicate-name handling for multi-pass output; keep `OutlineRenderer` as fallback only
- [X] T074 Ensure the screen-space path reuses `DebugGameObjectSelectionCollector` output for mesh, transform, camera-icon, render eligibility, selection group, parent/child classification, and material mask mode without a second selected-tree traversal
- [X] T075 Add immediate/non-threaded fallback coverage so selection feedback remains visible when mask resources cannot be prepared or threaded helper support is unavailable
- [X] T076 Run focused EditorRenderPath, ProfilerDestination, FrameGraphSceneTargets, and ThreadedRenderingLifecycle tests, then build `Editor`
- [ ] T077 Validate runtime pass order and resource states with RenderDoc or an equivalent RHI event/trace check: mask capture -> composite, with expected attachments, read-only depth, composite output writes, and write-to-read barriers
- [X] T077a Add review-driven MSAA/sample-count fallback coverage so the screen-space mask path rejects unsupported output/depth sample counts before allocating intermediates and skips legacy 1x shell submission until compatible MSAA/resolve support exists
- [X] T077b Add selected-create backpressure coverage so reusable threaded prepared-frame slots can wait for retirement, and Editor threaded rendering resolves one cushion slot plus a bounded publication wait for Scene View bursts
- [X] T077c Validate selected `Validation Cube` plus Sponza creation in DX12 scene-only mode; confirm readback is non-black and the post-fix log has no skipped deferred capture or empty recorded-draw snapshots while scene drawables exist
- [ ] T078 Capture a comparable runtime trace with the same project, backend, selected root, selected item count, outline enabled state, Scene View size, camera/view, and threaded rendering mode; compare `Debug GameObject`, `SelectionOutlineMask::*`, `AView::DrawFrame`, and total `CPU Frame` against the 2026-05-28 10:15:58 baseline before claiming the FPS issue is fixed
- [ ] T079 Run the required `/plan-review` quality gate, including deeper audit for GPU sync/resource-access risk and cross-language channel SSoT, before finalizing Phase 12
- [X] T080 Parse the 2026-05-28 22:11:24 recaptured trace and record that outline still emits excess helper/RHI pass pressure with very high Scene View frame time
- [X] T081 Add RED coverage for fusing outline post-processing from five helper passes to two helper passes (`CaptureMask`, `Composite`)
- [X] T082 Implement C++/HLSL/pass-mode/resource matrix fusion so composite performs edge detection/softening directly from the mask and no longer samples an intermediate blur texture
- [X] T083 Run focused outline and threaded-helper regression filters after pass fusion
- [X] T084 Add RED coverage for the recaptured black-frame/fallback risks: legacy shell metadata output propagation, deferred GBuffer-depth preflight, view-subresource access, and large-selection grouping complexity
- [X] T085 Implement the fallback hardening and hot-path cleanup in `SelectionOutlineMaskRenderer.*` and `DebugSceneRenderer.cpp`
- [X] T086 Run focused outline fallback/resource/grouping tests and the expanded EditorRenderPath/FrameGraph/RendererFrameObjectBinding/Profiler filter after the hardening pass
- [ ] T087 Recapture a post-hardening trace from the rebuilt editor binary and verify it contains only the current `SelectionOutlineMask::CaptureMask` and `SelectionOutlineMask::Composite` outline passes before using it for FPS conclusions
- [X] T088 Add RED coverage that the fused outline composite reuses a cached 13-sample mask neighborhood instead of nesting five 5-tap edge filters
- [X] T089 Implement the fused composite shader sample reuse so the current two-pass path keeps Unity-style soft outline semantics without expanding the composite pass to 25 mask texture samples per pixel
- [X] T090 Add RED coverage that screen-space outline and legacy fallback reject Scene View color/depth attachments whose actual texture extents do not match the current frame render extent
- [X] T091 Implement frame-attachment extent validation before outline intermediate allocation or fallback shell capture, and include actual output/depth extents in the screen-space resource identity
- [X] T092 Strengthen the fused composite shader source contract to lock the 13 unique mask samples, five edge-filter neighborhoods, soft-outline weights, and removal of the old costly `ComputeIdEdge(float2)` entry point
- [X] T092a Tighten review-driven diagnostics and docs: report stale frame attachments separately, make legacy fallback logs mention extent/sample-count compatibility, and document unsupported material-mask skip-frame policy
- [X] T092b Add RED coverage for the selected-create black-frame risk where recorded fullscreen composite pipelines failed to force alpha blending or the current Scene View color format
- [X] T092c Implement explicit recorded-pipeline blend overrides, include the override in material pass keys, and make the selection-outline composite pipeline use the actual output color view format
- [X] T092d Add RED coverage that large stable selections reuse the mask only through a safe signature, never by caching per-frame recorded draw bindings
- [X] T092e Implement retired-frame-gated stable mask reuse for large selected trees so unchanged selection/camera/resource identity can skip `CaptureMask` and emit composite-only metadata
- [X] T092f Add review-driven RED coverage that stable mask reuse keys in-place mesh geometry revisions and commits cached-mask validity only after a successful prepared-frame publish with the actual frame id
- [X] T092g Implement pending cached-mask commit/discard hooks, successful RHI-retirement gating, and mesh content revisions for `Reload()`/successful `UpdateVertices()` so composite-only reuse cannot sample an unwritten or stale mask
- [X] T092h Add review-driven RED coverage that a failed RHI retirement remains visible after later successful retirements and cannot validate a cached selection-outline mask target
- [X] T092i Implement failed-retirement telemetry gating so composite-only mask reuse is rejected after any failed retired frame at or after the cached capture target
- [X] T092j Add review-driven RED coverage for no-submit offscreen RHI frames leaving descriptor/upload frames open or a reset unsignaled fence, and for serial `BeginPassCommandPlan` failures being counted as successful recorded passes
- [X] T092k Implement threaded RHI cleanup/failure hardening: finalize no-submit frame contexts, defer offscreen frame-fence reset until an actual submit, and mark command-recording failures so failed capture frames retire as failed instead of validating cached masks
- [X] T092l Add review-driven RED coverage that stable cached-mask frames reuse prepared selection capture groups/signatures instead of rebuilding unordered capture groups and full reuse signatures on every large-selection frame
- [X] T092m Implement selection-source prepared-group caching keyed by selected items, transforms, source material/mesh identity, mesh content revisions, camera helper mesh presence/revision, and camera helper transforms; reuse cached groups/signature only while still recapturing per-frame recorded draw bindings on cache misses
- [X] T092n Add RHI failure-path RED coverage for external-output serial zero-draw recording and parallel-translation visibility/post-pass barrier failures so selection-mask capture frames cannot retire as successful after missing required commands or transitions
- [X] T092o Implement RHI failure propagation for serial zero dispatch/draw cases and ordered visibility/post-pass transition recording failures, including failed-retirement telemetry so cached selection masks cannot be validated by incomplete capture frames
- [X] T093 Run post-R2 focused tests, shader compilation, full `NullusUnitTests`, and `Editor` build; `/plan-review` deeper audit loop remains required before final sign-off. Latest fresh verification on 2026-05-29 before the prepared-group/RHI failure-path follow-up: outline/RHI focused filters passed 70/70 plus 6/6, full `NullusUnitTests` passed 1903/1903, DXIL/SPIR-V `SelectionOutlineMask.hlsl` VS/PS compiles passed, and `Editor` rebuilt to `App\Win64_Debug_Runtime_Shared\Editor.exe` timestamp `2026-05-29 07:46:11`.
- [X] T094 Parse the 2026-05-29 09:40:13 user trace and record that the DX12 scene-only readback path did not reproduce black-frame symptoms, old five-pass outline scopes are absent, `SelectionOutlineMask::Composite` is no longer material, and the remaining selected-object pressure is coarse `Debug GameObject` plus `AView::RendererBeginFrame`
- [X] T095 Add RED coverage and implement delayed selected-outline prepared-frame slot reservation so composite-only cached-mask frames do not preflight or wait for object-data slots before `BuildPreparedOutput` decides whether `CaptureMask` is needed
- [X] T096 Replace direct legacy `PROFILE_CPU_SCOPE` usage in editor selection-outline files with project `NLS_PROFILE_NAMED_SCOPE` instrumentation so the rebuilt `Editor` links while retaining traceable outline scopes
- [X] T097 Rebuild `NullusUnitTests`, run focused selected-outline/profiler contracts, and rebuild `Editor`; latest rebuilt binary is `App\Win64_Debug_Runtime_Shared\Editor.exe` timestamp `2026-05-29 10:49:29`
- [X] T098 Add review-driven RED coverage that selected-outline fallback/no-output paths do not release an existing GBuffer prepared-frame reservation and still release reservations created later by selected-object draw capture
- [X] T099 Implement prepared-frame reservation ownership checks via `FrameObjectBindingProvider::HasReservedPreparedFrameResources()` and gate selected-outline fallback release on ownership before final push
- [X] T100 Add final review-driven RED coverage for selection-outline composite shader SSoT and closed pass-kind metadata so split composite logic is shared by `SelectionOutlineMask.hlsl`/`SelectionOutlineComposite.hlsl` and invalid pass kinds cannot enter `RenderPassCommandInput`
- [X] T101 Implement the shared `SelectionOutlineCompositeCore.hlsli`, guarded shader includes, explicit non-empty pass-name metadata, and pre-configuration pass-kind validation

**Checkpoint**: Selected-object outline output uses a bounded mask/composite path by default, the shell path is fallback-only and observable, profiler evidence can see the relevant outline phases, pass-order/resource-access evidence exists, and post-change runtime trace evidence is required for performance claims.

---

## Dependencies & Execution Order

- Phase 1 must complete before tests are added.
- Phase 2 must complete before production code changes.
- User Story 1 and User Story 2 can be implemented independently after Phase 2.
- Phase 5 depends on the re-exported trace after User Story 1.
- Phase 6 depends on review findings from the final quality gate.
- Phase 7 depends on all implemented stories and review-driven hardening.
- Phase 8 depends on the 2026-05-28 recaptured trace after Phase 7 and remains part of the same selected-object hotspot optimization.
- Phase 9 depends on review findings from Phase 8 and remains within the same selected-object hotspot optimization.
- Phase 10 depends on the Phase 9 validation pass and the same 2026-05-28 trace still showing `Debug GameObject` as the dominant coarse hotspot.
- Phase 11 depends on R2/deeper audit findings after Phase 10.
- Phase 12 depends on the 2026-05-28 10:15:58 trace and Unity 2018.4 reference analysis showing the current shell-outline algorithm remains the dominant selected-object cost.

## Parallel Opportunities

- T003 and T004 can be written in parallel because they affect different test files.
- US1 implementation files and US2 implementation files are independent after their tests fail.
- T019 and T020 can be written together because they affect independent contract/binding coverage and fail before implementation.
- T029 can be written before T030 because it fails against the existing threaded deferred path.
- T058 and T059 can be written in parallel because profiler export visibility and selection-outline renderer contracts affect different files.
- T062 and T069 can be prepared in parallel because channel SSoT and Unity material/mask semantics touch different contract surfaces.
- T063 through T068 should be implemented serially because resource identity, pass matrix, barriers, pipeline state, and fused composite resource access are tightly coupled.

## Implementation Strategy

1. Complete Setup and Foundational phases.
2. Deliver US1 first because it targets the dominant Scene View CPU cost.
3. Deliver US2 second because it improves future performance evidence quality.
4. Deliver US3 after the new trace identifies `Debug GameObject` as the next dominant Scene View CPU cost.
5. Apply review-driven GBuffer safety hardening.
6. Apply the trace-driven Debug GameObject follow-up if recapture still points to the selected-object pass.
7. Apply review-driven copy-path hardening for selected-object prepared helpers.
8. Apply the trace-driven material-binding follow-up for stable selected-outline color.
9. Apply review-driven ownership and lifecycle hardening.
10. Replace the selected-outline algorithm with the Unity-style mask/composite path.
11. Run focused tests after each story, then combined validation, runtime trace comparison, and plan-review.
