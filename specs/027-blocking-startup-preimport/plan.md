# Implementation Plan: Blocking Startup Preimport

**Branch**: `027-blocking-startup-preimport` | **Date**: 2026-05-17 | **Spec**: `specs/027-blocking-startup-preimport/spec.md`  
**Input**: Feature specification from `specs/027-blocking-startup-preimport/spec.md`

## Summary

Move the full `EditorStartup` asset preimport stage out of Asset Browser first draw and into editor startup before the first visible editor frame, while keeping post-open imports limited to watcher/copy/move changed-path requests.
Generated model drag/drop then instantiates the imported prefab immediately and resolves renderer resources through a non-modal queue, so cold meshes/materials cannot turn the drop operation into a blocking editor task.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Existing `AssetDatabaseFacade`, `AssetPreimportScheduler`, `ImportProgressTracker`, editor startup progress window  
**Storage**: Existing project `Library/Artifacts/<asset-guid>/manifest.json` artifact cache  
**Testing**: `NullusUnitTests`, focused gtest filters, Editor target build  
**Target Platform**: Desktop editor; window remains hidden until startup completion on all platforms  
**Project Type**: Native editor/runtime C++ project  
**Performance Goals**: UI is not visible until initial scene-droppable asset imports are complete; post-open changes avoid full-project reimport  
**Constraints**: Preserve generated directories, do not revert existing dirty work, keep watcher/copy incremental behavior  
**Scale/Scope**: Startup ordering and preimport orchestration only; importer parse-speed optimization is out of scope

## Constitution Check

- **Spec-first major change**: Pass. This bundle contains `spec.md`, `plan.md`, and `tasks.md`.
- **TDD/test-with-change**: Pass. New failing tests cover startup preimport behavior and ordering before production code.
- **Generated code boundary**: Pass. No `Runtime/*/Gen/` files are hand-edited.
- **Validation matches subsystem**: Pass. Uses asset pipeline unit tests, source-contract tests, and editor target build.
- **Cross-platform assumption check**: Pass. Window visibility is kept hidden until startup completion without a Windows-only exception.

## Project Structure

### Documentation

```text
specs/027-blocking-startup-preimport/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Project/Editor/Assets/
├── EditorStartupAssetPreimport.h
└── EditorStartupAssetPreimport.cpp

Project/Editor/Core/
├── Application.cpp
└── Context.cpp

Project/Editor/Panels/
└── AssetBrowser.cpp

Tests/Unit/
├── EditorAssetDatabaseTests.cpp
├── EditorRenderPathContractTests.cpp
└── CMakeLists.txt
```

**Structure Decision**: The blocking startup preimport helper lives beside editor asset services because it composes `AssetDatabaseFacade` and `AssetPreimportScheduler` without depending on UI panels. `Application.cpp` owns the startup ordering call because it already controls first frame rendering and progress completion. Asset Browser keeps only post-open watcher/copy scheduling.

## Design

1. `RunBlockingStartupAssetPreimport` creates a local `AssetDatabaseFacade` for the project root and a local `ImportProgressTracker`.
2. It builds the startup plan before running it so callers can log planned/imported counts.
3. It runs `AssetPreimportScheduler::Run(..., EditorStartup)` synchronously and returns only after all jobs are terminal.
4. `Application` creates/arms project and engine `AssetFileWatcher` instances before running the blocking startup preimport so external edits during startup are buffered.
5. `Application` calls the blocking startup preimport helper after the watchers are armed, but before constructing `Editor`, creating panels, refreshing Asset Browser, rendering the first frame, or calling `CompleteStartupProgress()`.
6. If the blocking startup preimport fails or returns with jobs still running, `Application` logs the counts and aborts startup before the normal editor UI can open.
7. After constructing `Editor`, `Application` moves the already-armed watcher instances into `AssetBrowser`, runs blocking changed-path `FileWatcherChanged` preimport before the first frame, renders the first frame, then runs a final blocking watcher preimport before opening the editor.
8. Startup progress maps import events into `PresentStartupProgressFrame` rather than using `Context::importProgressTracker`, avoiding task-progress dialog closure during startup.
9. `Context::CompleteStartupProgress` moves the native progress dialog into a local owner, closes it, and lets RAII destroy it before showing the editor window.
10. `AssetBrowser::OnBeforeDrawWidgets` still refreshes browser state and consumes post-open watcher changes, but no longer schedules `EditorStartup`.
11. Watcher and copy/move paths continue to schedule `FileWatcherChanged` and `AssetCopiedOrMoved` requests with changed paths.
12. `Context` filters internal `asset-resolution` progress events out of the native task-progress dialog because they represent post-drop renderer binding work, not user-facing import work.
13. Generated-model renderer resolution binds only cached materials on the editor thread; cold material artifacts remain represented by path hints and fallback slots until a later cache/preload path can make them hot.
14. Scene View and Hierarchy accept model/prefab scene instantiation only from `EditorAssetDragPayload`; legacy `"File"` string drags remain for scene loading, lightweight non-model preview, and Asset Browser internal copy/move operations, but cannot bypass the imported-artifact handle contract or synchronously load model resources.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Blocking startup import | User explicitly requires no cold initial assets after UI opens | Background startup import allows drag/drop to observe pending artifacts |
| Source-contract ordering test | Full editor startup is expensive to instantiate in unit tests | Without a contract test, the ordering could regress silently |
