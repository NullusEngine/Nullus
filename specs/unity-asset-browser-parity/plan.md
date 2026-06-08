# Implementation Plan: Unity Asset Browser Parity

**Branch**: `unity-asset-browser-parity` | **Date**: 2026-06-08 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/unity-asset-browser-parity/spec.md`

## Summary

Replace the existing Nullus `Asset Browser` single-tree panel with a Unity-style project asset browser that shows project `Assets/` only. The panel will split navigation and content into a left folder tree and right current-folder thumbnail grid, expand generated imported sub-assets into visible grid entries, preserve existing asset workflows, and add persistent high-fidelity thumbnail caching under `Library/AssetThumbnails/`.

## Technical Context

**Language/Version**: C++17-style editor/runtime code  
**Primary Dependencies**: Existing ImGui/UI widgets, editor asset database/importer facade, editor resource managers, scene/prefab drag-drop workflows, renderer resources for previews  
**Storage**: Project files under `Assets/`, generated artifacts under `Library/Artifacts`, new thumbnail cache under `Library/AssetThumbnails/`  
**Testing**: Focused GoogleTest unit tests through `NullusUnitTests.exe`, plus focused editor/runtime manual verification for rendered thumbnails  
**Target Platform**: Windows editor/unit-test build for this phase  
**Project Type**: Desktop editor feature in a C++ engine/editor repository  
**Performance Goals**: Large visible folders remain scrollable while thumbnails are pending; unchanged thumbnails are reused after restart; no foreground source reparse for generated prefab drops  
**Constraints**: Do not hand-edit generated files; preserve existing Asset Browser panel identity; hide Engine built-in assets in v1; keep product Editor runnable; do not use screenshots as proof of rendering correctness when renderer evidence is needed  
**Scale/Scope**: One editor panel plus supporting presentation/thumbnail services and focused tests

## Constitution Check

- Spec scope: Required because the change affects Project editor workflow, asset management UI, drag/drop behavior, and thumbnail caching.
- Generated-file boundaries: No files under `Runtime/*/Gen/` or `Project/*/Gen/` will be hand-edited.
- Backend/platform validation: Automated validation targets the current Windows unit-test build. Rendered thumbnail visual quality will be validated with focused editor/runtime evidence and no cross-backend claim unless separately verified.
- Product runtime viability: The editor must remain runnable. Thumbnail failure must degrade to icons, not break browsing or drag/drop.
- Evidence path: Unit tests for deterministic presentation/cache behavior, focused workflow/manual verification for UI/thumbnail behavior, and required plan-review gate before completion.

## Project Structure

### Documentation

```text
specs/unity-asset-browser-parity/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── tasks.md
└── checklists/
    └── requirements.md
```

### Source Code

```text
Project/Editor/Panels/
├── AssetBrowser.h
└── AssetBrowser.cpp

Project/Editor/Assets/
├── AssetBrowserPresentation.h
├── AssetBrowserPresentation.cpp
├── AssetThumbnailCache.h
├── AssetThumbnailCache.cpp
├── AssetThumbnailService.h
└── AssetThumbnailService.cpp

Project/Editor/Rendering/
└── existing editor preview/default resources as needed

Tests/Unit/
├── AssetBrowserPresentationTests.cpp
├── AssetThumbnailCacheTests.cpp
└── existing asset drag/drop and panel tests as needed
```

**Structure Decision**: Keep the user-facing panel in `Project/Editor/Panels/AssetBrowser.*` to preserve registration and menu identity. Move deterministic item-building, filtering, and thumbnail cache decisions into `Project/Editor/Assets` so the large panel file does not own every concern.

## Research

- **Decision**: Replace the existing Asset Browser in place.
  **Rationale**: Preserves menu/panel identity while avoiding duplicate browser workflows.
  **Alternatives considered**: A parallel new panel lowers migration risk but creates cleanup debt; a bolt-on grid keeps too much old tree behavior.

- **Decision**: Hide Engine built-in assets from v1.
  **Rationale**: User explicitly said built-ins do not need to display, and Unity Project panel centers project content.
  **Alternatives considered**: A built-in asset toggle is deferred.

- **Decision**: Use current-folder search/type filtering.
  **Rationale**: User selected current-folder scope, reducing complexity while matching the requested first version.
  **Alternatives considered**: Full-project search requires a separate search result mode and ranking.

- **Decision**: Persist thumbnails under `Library/AssetThumbnails/`.
  **Rationale**: User selected persistent project Library caching, avoiding session-to-session regeneration.
  **Alternatives considered**: In-memory-only cache is simpler but weak for large projects.

- **Decision**: Test deterministic presentation and cache logic separately from renderer thumbnail generation.
  **Rationale**: Presentation/filter/cache behavior can be robustly unit tested; actual thumbnail pixels need focused runtime verification.
  **Alternatives considered**: Testing everything through the editor panel would be brittle and slow.

## Data Model

See [data-model.md](data-model.md). Core entities are Project Folder Selection, Asset Browser Item, Asset Type Filter, Thumbnail Request, Thumbnail Cache Entry, and Generated Sub-Asset Entry.

## Contracts

This feature does not introduce external APIs. The internal UI contract is:

- `AssetBrowserPresentation` produces project-only folder trees and current-folder grid items from filesystem and asset database inputs.
- `AssetThumbnailCache` answers whether a thumbnail request is fresh, stale, missing, or failed and resolves safe project-Library paths.
- `AssetThumbnailService` schedules or produces thumbnails and returns fallback icons when generation is unavailable.
- `AssetBrowser` consumes those services and preserves existing editor workflows.

## Validation Plan

- Add failing tests for project-only presentation and folder selection fallback.
- Add failing tests for current-folder search/type filtering.
- Add failing tests for generated sub-asset expansion and drag payload preservation.
- Add failing tests for thumbnail cache key/freshness/path containment.
- Add panel/workflow regression coverage where stable entrypoints exist.
- Run focused `NullusUnitTests.exe` filters for new tests and impacted existing asset drag/drop tests.
- Run focused manual editor verification from `quickstart.md` for layout, thumbnails, and workflow behavior.
- Run required `/plan-review` quality gate, including at least two review rounds and one deeper audit per repository instructions.
