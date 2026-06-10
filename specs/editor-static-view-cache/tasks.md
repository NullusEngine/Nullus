# Tasks: Editor Static View Cache

**Input**: Design documents from `specs/editor-static-view-cache/`
**Prerequisites**: [spec.md](spec.md), [plan.md](plan.md)

## Phase 1: Tests First

- [x] T001 Add renderer call counters to `SnapshotProbeRenderer` in `Tests/Unit/PanelWindowHookTests.cpp`.
- [x] T002 Add failing static-cache test proving a second unchanged render does not advance `BeginFrame`, `DrawFrame`, or `EndFrame`.
- [x] T003 Add failing dirty-input test proving camera changes, scene render-content revision changes, and resize each force redraw.
- [x] T004 Add scene revision test in `Tests/Unit/SceneObjectGraphSerializationTests.cpp` proving repeated dirty marks advance render-content revision.

## Phase 2: Scene Revision Source

- [x] T005 Add `Scene::GetRenderContentRevision()` and `Scene::MarkRenderContentChanged()` in `Runtime/Engine/SceneSystem/Scene.h`.
- [x] T006 Implement render-content revision storage and increment behavior in `Runtime/Engine/SceneSystem/Scene.cpp`.
- [x] T007 Advance render-content revision from `SceneManager::MarkCurrentSceneDirty()` every call in `Runtime/Engine/SceneSystem/SceneManager.cpp`.

## Phase 3: AView Static Cache Infrastructure

- [x] T008 Add protected cache hooks and state to `Project/Editor/Panels/AView.h`.
- [x] T009 Implement base static-frame cache key hashing in `Project/Editor/Panels/AView.cpp`.
- [x] T010 Return early before renderer submission on cache hits in `AView::Render()`.
- [x] T011 Invalidate the cache from `AView::ApplyResolvedViewSize()` when framebuffer size changes.
- [x] T012 Keep cache hits from producing synthetic `FrameInfo`; preserve the last successful snapshot.

## Phase 4: Scene View Opt-In And Safety Gates

- [x] T013 Override `SceneView::ShouldUseStaticFrameCache()` to enable caching for Scene View only.
- [x] T014 Add Scene View cache key inputs for selection, highlight, gizmo state, focus state, prefab-stage state, and drag-preview state.
- [x] T015 Add Scene View force-render gates for camera motion/control, picking requests, pending click picking, drag preview, and validation readback.

## Phase 5: Validation

- [x] T016 Verify RED state before implementation with compile/test failures for missing cache APIs.
- [x] T017 Build `NullusUnitTests`.
- [x] T018 Run focused tests for `PanelWindowHookTests.AViewStaticFrameCache*`.
- [x] T019 Run focused test for `SceneObjectGraphSerializationTests.SceneManagerDirtyMarksAdvanceRenderContentRevisionEveryTime`.
- [x] T020 Run broader adjacent regression tests for PanelWindow/Scene serialization/threaded view lifecycle.
- [x] T021 Run `git diff --check`.
- [x] T022 Run plan-review quality gate or document the unavailable tool boundary.

## Phase 6: Trace-Driven Follow-Up

- [x] T023 Parse `TestProject/Logs/trace.json` and confirm static cache hits were absent while Scene View still rendered every frame.
- [x] T024 Add static-frame cache decision reason telemetry scopes in `Project/Editor/Panels/AView.cpp`.
- [x] T025 Add focused test coverage for static cache hit and miss reasons in `Tests/Unit/PanelWindowHookTests.cpp`.
- [x] T026 Add hover-picking policy coverage proving an existing picking sample does not force a full static-frame redraw.
- [x] T027 Update `SceneView::ShouldForceStaticFrameRender()` to allow static cache reuse for hover picking when a sample already exists.
- [x] T028 Add Scene View static cache key segment telemetry for base camera/scene/viewport, highlight, gizmo, focus, selection, and drag-preview changes.
- [x] T029 Add regression coverage proving empty `Scene::CollectGarbages()` calls do not advance render-content revision.
- [x] T030 Fix empty `Scene::CollectGarbages()` so idle editor frames do not rebuild scene component caches or invalidate static Scene View frames.
- [x] T031 Parse the follow-up trace and confirm static cache hits while idle Scene View frames skip renderer submission.
- [x] T032 Mark scene render content changed when prefab renderer resource resolution binds mesh/material resources so idle static-cache frames repaint loaded prefabs.
- [x] T033 Split scene-load prefab streaming into smaller per-frame slices than drag-preview streaming to reduce camera-move frame hitches.
- [x] T034 Parse the camera-move trace and identify that moving frames now spend most time in `DeferredSceneRenderer::BeginFrame`, not prefab streaming or idle static-cache misses.
- [x] T035 Remove the per-frame copy of previous HZB primitive inputs by moving the retained observation vector before rebuilding the current frame packets.
- [x] T036 Add `ParseScene` phase profiler scopes for RenderScene sync, visibility gather, HZB packet build, streaming dependency registration, and visible drawable append.
- [x] T037 Keep indexed-object shader support results cached across frames in `EngineFrameObjectBindingProvider` so moving camera frames do not re-query identical shader reflection for every prepared draw.
- [x] T038 Parse the refreshed camera-move trace and confirm the remaining large outliers come from `Picking` full-pass spikes and `Panel::Draw:Profiler`, while `DeferredSceneRenderer::BeginFrame` is reduced to about 6.6ms average.
- [x] T039 Suppress Scene View picking-frame requests while camera control is active, including stale pending click picks, so camera movement cannot enqueue an expensive picking render pass.
- [x] T040 Add picking pass profiler scopes for draw, threaded input build, capture phases, and pixel readback to make the next trace attribute any remaining picking spike to a concrete sub-phase.
- [x] T041 Validate the camera-control picking policy and adjacent readback/static-cache behavior with focused unit tests, `NullusUnitTests` build, and `git diff --check`.
- [x] T042 Add top-level editor main-loop profiler scopes for tick, resize flush, resize follow-up, editor update/post-update, event polling, and clock update so trace blanks are attributable.
- [x] T043 Budget timeline trace export to a small number of frames per draw and add `ProfilerPanel::UpdateTraceExport` scope so `ProfilerPanel::DrawTimeline` cannot flush a large backlog in one UI frame.
- [x] T044 Add an idle editor frame pacing policy that sleeps only when there is no transient input, no mouse button drag, and no pending/active resize work.
- [x] T045 Validate profiler trace export budgeting, transient input detection, idle pacing policy, and adjacent resize/input/profiler contracts with focused unit tests.

## Dependencies

- T001-T004 must happen before production implementation.
- T005-T007 block cache key scene invalidation.
- T008-T012 block Scene View opt-in.
- T013-T015 block runtime usefulness for editor large-scene idle frames.
- T020-T022 close the quality gate.
- T023 must happen before T024-T027 so the follow-up remains trace-driven rather than speculative.
- T028-T030 close the observed `KeyChanged` root cause before collecting a new trace.
- T031-T033 close the prefab streaming/static-cache interaction before collecting the next movement trace.
- T034-T037 close the first camera-move CPU self-time reduction pass and add enough telemetry to target the next highest moving-frame cost.
- T038-T041 close the observed picking spike path before collecting the next movement trace; the profiler-panel spike remains a separate follow-up bottleneck.
- T042-T045 close the profiler-panel and trace-readability follow-up by making main-loop idle time explicit, preventing timeline trace export backlog spikes, and pacing truly idle editor frames without throttling active camera/input/resize frames.

## Implementation Strategy

Keep the optimization at the editor view boundary first. Do not modify global RHI thread scheduling until static view cache evidence shows remaining overhead. The first measurable win should be fewer renderer/RHI submissions on idle Scene View frames; moving-camera frames still render normally.
