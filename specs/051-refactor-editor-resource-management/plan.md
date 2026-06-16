# Implementation Plan: Editor Resource Management Refactor

**Branch**: `051-refactor-editor-resource-management` | **Date**: 2026-06-15 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/051-refactor-editor-resource-management/spec.md`

## Summary

Create one logical editor resource catalog that resolves stable Nullus-style IDs through a shared backend abstraction. Development builds read loose files from `App/Assets`, release builds can read packaged resources through the same catalog, and all editor callers move to executable-relative resource roots. The refactor also prunes unused icon files, removes Unity-related naming from retained resources, and separates static editor icons from generated thumbnails.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Existing Nullus engine/editor code, ImGui font atlas, current resource loaders, filesystem APIs  
**Storage**: Files on disk in development; packaged resource backend in release; thumbnail cache remains file-based  
**Testing**: NullusUnitTests and targeted editor/resource tests  
**Target Platform**: Desktop editor/runtime on Windows first, with existing cross-platform path behavior preserved  
**Project Type**: Desktop engine/editor application  
**Performance Goals**: Resource lookup should remain effectively constant-time for common editor UI accesses; thumbnail loading remains cached and asynchronous where already supported  
**Constraints**: Must not rely on current working directory; must not edit generated `Runtime/*/Gen/`; must preserve editor/runtime separation; must remove unused Unity-named icon files from the retained editor resource set  
**Scale/Scope**: Shared editor/runtime resource infrastructure, editor startup paths, asset browser icons, thumbnail fallback behavior, launcher/editor resource loading, and packaging boundaries

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Spec-first change required: satisfied by `specs/051-refactor-editor-resource-management/`.
- Generated-code boundary respected: no `Runtime/*/Gen/` edits planned.
- Validation will match the subsystem: targeted unit tests plus focused runtime verification for path and resource loading.
- Product runtime preservation: editor and launcher continue to work while resource lookup migrates.
- Evidence path known: unit tests for catalog/path behavior plus build/runtime verification for editor startup and asset browser fallback IDs.

## Project Structure

### Documentation (this feature)

```text
specs/051-refactor-editor-resource-management/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
└── tasks.md
```

### Source Code (repository root)

```text
Project/Editor/Core/
├── EditorResources.h
├── EditorResources.cpp
├── Context.cpp
└── EditorResourceCatalog.*          # new catalog and backend abstraction

Project/Editor/Assets/
├── AssetBrowserPresentation.cpp
├── AssetThumbnailService.cpp
└── (tests updated for new resource IDs)

Project/Editor/Panels/
├── AssetBrowser.cpp
└── Toolbar.cpp

Project/Launcher/Core/
└── Launcher.cpp

Project/Game/Core/
└── Context.cpp

Runtime/UI/
└── UIManager.cpp

App/Assets/
├── Editor/
│   ├── Brand/
│   ├── Fonts/
│   ├── Icons/
│   ├── Generated/
│   ├── ResourceCatalog/
│   ├── Models/
│   ├── Settings/
│   └── Shaders/
└── Engine/
    └── Brand/
```

**Structure Decision**: Keep editor-only resources under `App/Assets/Editor` and runtime-owned shared brand assets under `App/Assets/Engine/Brand`. Introduce a single catalog layer in `Project/Editor/Core` that resolves logical IDs to either loose-file paths or packaged-resource entries. Asset browser and thumbnail code consume the catalog but still own thumbnail caching and preview generation separately.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Shared catalog + backend abstraction | One ID namespace must work for loose files and packaged resources | Hard-coding file paths would keep development and release behavior divergent |
| EXE-relative root resolver | Startup must ignore current working directory | Relative `../Assets/...` paths break when launched from IDEs, launchers, or packaged installs |
| Separate thumbnail cache path from static icon catalog | Previews and icons have different lifecycles | Folding preview images into the icon tree would blur ownership and cache invalidation |
| Icon pruning + rename pass | User requested only the used icons and no Unity naming | Leaving legacy files around would keep the resource tree noisy and ambiguous |

