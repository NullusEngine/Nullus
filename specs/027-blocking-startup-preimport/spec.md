# Feature Specification: Blocking Startup Preimport

**Feature Branch**: `027-blocking-startup-preimport`  
**Created**: 2026-05-17  
**Status**: Draft  
**Input**: User description: "Ensure all resources are imported before the editor UI opens. After the editor opens, only newly added or changed assets should be handled."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Open Editor With Hot Initial Assets (Priority: P1)

An editor user opens a project containing existing model and prefab assets and can drag any pre-existing scene-droppable asset immediately after the UI appears, because startup import has already completed.

**Why this priority**: This is the reported failure: the UI appears while startup preimport is still running, so drag/drop sees a cold handle and logs "not imported yet".

**Independent Test**: Run the startup preimport entry point against a project with a cold model source and verify the generated prefab artifact exists before the function returns.

**Acceptance Scenarios**:

1. **Given** a project has a cold `.gltf`, `.glb`, `.fbx`, `.obj`, or `.prefab` under `Assets`, **When** editor startup reaches the UI opening point, **Then** the artifact manifest and generated prefab sub-asset are already committed or an import diagnostic has been produced.
2. **Given** the user can see the editor window, **When** they drag an existing model from the Asset Browser into the scene, **Then** drag/drop must not depend on a still-running `EditorStartup` background import.

---

### User Story 2 - Incremental Imports After UI Opens (Priority: P2)

After the editor UI opens, only assets reported by the file watcher, copy/move/drop operations, or explicit reimport actions are imported.

**Why this priority**: The initial full scan should not repeat after UI creation; post-open work must remain incremental and bounded to changed paths.

**Independent Test**: Inspect the Asset Browser first-draw path and verify it starts watchers and refreshes UI state without scheduling `AssetPreimportReason::EditorStartup`; existing watcher/copy paths still schedule changed-path preimport.

**Acceptance Scenarios**:

1. **Given** the initial startup preimport has completed, **When** Asset Browser watchers become ready on first draw, **Then** the panel refreshes but does not schedule a full startup preimport.
2. **Given** a new or changed model/prefab appears under `Assets` after UI opens, **When** the watcher or copy/move path reports it, **Then** only matching changed paths are planned for forced reimport.

### Edge Cases

- Startup preimport may import zero assets when all scene-droppable assets are already warm.
- If a startup import fails, the editor should report diagnostics/logs and not open the normal editor UI.
- Startup progress must not close the native startup progress dialog as if it were a normal post-open background import.
- The editor window must stay hidden on every platform until startup preimport and the first frame preparation complete.
- File watchers must be armed before the editor window becomes visible so post-open added/changed assets cannot fall into a startup blind window.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The editor MUST run `EditorStartup` preimport synchronously during startup before rendering the first visible editor frame.
- **FR-002**: The editor MUST keep the editor window hidden until blocking startup preimport returns.
- **FR-003**: Startup preimport MUST use the same artifact freshness rules as `AssetPreimportScheduler::EditorStartup`.
- **FR-004**: Startup preimport MUST expose progress through startup progress presentation without closing the startup dialog as a task dialog.
- **FR-005**: Asset Browser MUST NOT schedule `AssetPreimportReason::EditorStartup` from first draw or watcher-startup completion after the UI opens.
- **FR-006**: Asset Browser MUST continue to schedule `FileWatcherChanged` preimport for changed project asset paths after UI opens.
- **FR-007**: Asset Browser copy/move/import paths MUST continue to schedule `AssetCopiedOrMoved` preimport for the affected destination path after UI opens.
- **FR-008**: The behavior MUST be covered by unit or source-contract tests that fail if startup preimport moves back behind UI drawing.
- **FR-009**: If blocking startup preimport fails, the editor MUST stop startup before constructing editor panels or showing the normal editor window.
- **FR-010**: If blocking startup preimport returns while jobs are still running, the editor MUST treat startup preimport as failed and keep the normal editor window hidden.
- **FR-011**: The editor MUST arm project and engine asset watchers before running blocking startup preimport, then consume watcher-observed project changes through blocking `FileWatcherChanged` preimport before calling `CompleteStartupProgress`.
- **FR-012**: Startup progress dialog shutdown MUST use owned RAII cleanup rather than leaking or detaching the native progress dialog.
- **FR-013**: Dragging an already-imported generated model into the scene MUST NOT show a native blocking task-progress dialog while renderer resources are resolved.
- **FR-014**: Dragging an already-imported generated model into the scene MUST NOT synchronously load cold mesh or material renderer resources on the drag/drop path; unresolved slots must remain visible through fallback material and path hints until the resource queue can bind cached resources.
- **FR-015**: Scene View, Hierarchy, and preview surfaces MUST NOT synchronously load model resources from legacy file-path drag payloads; scene instantiation MUST consume `EditorAssetDragPayload` imported-asset handles, matching Unity's Project Browser object-reference drag path.
- **FR-016**: Startup model-package prewarm and later drag/drop model loads MUST use one canonical project-relative `Library/...` cache key for project artifacts, so absolute artifact paths cannot trigger duplicate package loads or replace already-bound runtime resource pointers.
- **FR-017**: Startup material artifact prewarm MUST only run when material, shader, and texture manager services are available; otherwise it must skip safely rather than invoking a loader with missing transitive services.

### Key Entities

- **StartupAssetPreimportOptions**: Project-root configuration for blocking startup preimport.
- **StartupAssetPreimportResult**: Outcome of the blocking startup import stage, including planned count, imported count, diagnostics, and whether jobs remain running.
- **AssetPreimportRequest**: Existing changed-path request model used only for post-open incremental imports after this change.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A cold model fixture becomes a loadable generated prefab artifact before the startup preimport function returns.
- **SC-002**: Source-contract tests prove startup preimport is ordered before first editor frame rendering and `CompleteStartupProgress`.
- **SC-003**: Source-contract tests prove Asset Browser first draw does not contain `AssetPreimportReason::EditorStartup`.
- **SC-004**: Existing file watcher/copy preimport tests remain passing.
- **SC-005**: Source-contract tests prove `asset-resolution` progress events are filtered out of native blocking task progress.
- **SC-006**: Source-contract tests prove generated model renderer resource resolution checks material cache state without forcing material loads on the editor thread.
- **SC-007**: Source-contract tests prove legacy `"File"` drag targets cannot instantiate or synchronously preview model assets through Scene View, Hierarchy, or Asset View.
- **SC-008**: Unit tests prove a model package prewarmed through a relative `Library/...` key is reused by later absolute `.nmodel` requests without adding an absolute cache entry or replacing the package/mesh alias pointers.
- **SC-009**: Source-contract tests prove renderer resource resolution does not mark the main model package ready when prewarm fails, and startup material prewarm checks material, shader, and texture services before loading `.nmat` artifacts.

## Assumptions

- "All resources" means all user-visible scene-droppable source assets currently covered by the asset preimport scheduler: model-scene and prefab assets.
- Texture/material sub-assets used by imported models are handled by the model/prefab import pipeline and artifact dependency records.
- Further importer performance work, especially FBX parse speed, is separate from this startup ordering fix.
