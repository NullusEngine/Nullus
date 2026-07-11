# Implementation Plan: Unity-Aligned Sub-Asset Expansion

**Branch**: `059-fix-subasset-expansion` | **Date**: 2026-07-11 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/059-fix-subasset-expansion/spec.md`

## Summary

Replace the Asset Browser's independently refreshed child hints, snapshot vectors, progressive display prefixes, and per-item background fills with one coherent pipeline. `AssetDatabaseFacade` publishes an immutable aggregate state; pure background builders derive exact filtered membership, structured actions, thumbnail roots, and group IDs from immutable roots/state/expansion/filter views; the panel single-flight coordinator publishes only a fully current bundle; grid/list drawing resolves one clipped fill per contiguous group segment.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus `AssetDatabaseFacade`, `BackgroundJobQueue`/`JobSystem`, Dear ImGui draw lists, C++ standard library smart pointers/futures/containers
**Storage**: In-memory immutable manifest/current-path/snapshot views; no artifact, importer, or serialized-format changes
**Testing**: GoogleTest in `NullusUnitTests`, CTest, deterministic test hooks under `NLS_ENABLE_TEST_HOOKS`, test-local ImGui software rasterizer
**Target Platform**: Nullus desktop Editor; local behavioral/build evidence on Windows x64/MSVC. Linux/macOS remain source-portability constraints and CI follow-up targets, not locally validated claims
**Project Type**: Desktop game-engine editor
**Performance Goals**: Presentation build `O(r + s + n + B)` off the UI thread; facade candidate comparison `O(M + K + S + B)` off-lock; lock commit and main-thread publication `O(1)` excluding moved-container destruction; segment drawing linear in visible items/segments
**Constraints**: Preserve current importer/runtime behavior; do not hand-edit `Runtime/*/Gen/`; no stale action/thumbnail/picker state; one active plus one latest queued job per coordinator; no per-frame retry after unchanged failure; preserve current dirty worktree changes
**Scale/Scope**: Current folder roots and relevant sub-assets in large editor projects; changes centered on 8 editor files and focused unit tests

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

### Pre-Research Gate

- **Spec-first major change**: PASS. Behavior-changing `Project/` work is contained in `specs/059-fix-subasset-expansion/` before production edits.
- **Subsystem-matched validation**: PASS. Focused facade/presentation/scheduler/geometry tests, broader editor-asset regression tests, `Editor` build, and exact manual grid/list verification are defined.
- **Generated/backend boundaries**: PASS. No generated files, RHI APIs, shaders, render backends, or runtime asset formats are modified.
- **Incremental verified delivery**: PASS. Tasks are split into facade publication, pure builder/coordinator, picker migration, and geometry; each begins with failing tests.
- **Product runtime preservation**: PASS. Editor remains runnable after each phase; Game/runtime loading behavior is out of scope and unchanged.

### Post-Design Gate

- Research resolved all concurrency, identity, failure, geometry, and test-entry questions without `NEEDS CLARIFICATION` markers.
- Internal contracts define fail-closed state transitions and API removal/migration gates.
- Local claims are limited to Windows validation; cross-platform code must avoid platform-specific dependencies and will rely on existing CI for later evidence.
- Final evidence path is fixed in [quickstart.md](quickstart.md).

**Result**: PASS. No constitution exceptions or temporary runtime breakages are required.

## Project Structure

### Documentation (this feature)

```text
specs/059-fix-subasset-expansion/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── facade-published-state.md
│   ├── presentation-builder.md
│   ├── picker-cache.md
│   └── group-geometry.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
Project/Editor/Assets/
├── EditorAssetPath.h
├── AssetDatabaseFacade.h
├── AssetDatabaseFacade.cpp
├── AssetBrowserPresentation.h
└── AssetBrowserPresentation.cpp

Project/Editor/Panels/
├── AssetBrowser.h
├── AssetBrowser.cpp
└── ReflectedPropertyDrawer.cpp

Tests/Unit/
├── AssetBrowserPresentationTests.cpp
└── EditorAssetDatabaseTests.cpp
```

**Structure Decision**: Keep the existing editor asset/panel ownership boundaries. Consumer-neutral snapshot/publication and pure presentation types live in `Project/Editor/Assets`; panel scheduling and ImGui drawing remain in `Project/Editor/Panels`; tests extend the existing unit-test translation units.

## Design Phases

### Phase 1 - Immutable Facade State

Add canonical project-path normalization, consumer-neutral snapshot/index/diagnostic types, and one immutable `FacadePublishedState`. Build semantic candidates outside `m_manifestMutex`; compare-and-publish by base pointer under the lock; migrate and remove all old snapshot vector/view/iterator/testing APIs.

### Phase 2 - Atomic Presentation Bundle

Define validated immutable roots, expansion, filter, action identity, build key/result, and bundle types. Implement one pure exhaustive projection/filter builder. Add an explicit `Idle/Loading/Success/Failure/Closed` single-flight/latest-key coordinator with nonthrowing scheduling, retained retry identity, failure diagnostics, structured selection/action resolution, and fail-closed epoch exhaustion. Carry bundle/action identity plus an immutable complete `AssetThumbnailRequest` and public `BuildAssetThumbnailCacheKey` through generation, decode, and upload. The worker performs `EvaluateAssetThumbnailCache(request, Full)` and returns the evaluation; the main-thread pump performs no Full cache evaluation or image hash, and binds only when worker status is `Fresh` and a newly rebuilt current request still has the same bundle/action/cache-key identity.

### Phase 3 - State-Bound Picker

Make picker provider/results diagnostic-bearing, bind each global main-thread-only cache instance to a retained owner lifetime plus source facade state, invalidate synchronously on state changes, and migrate the async panel cursor to both identities plus nonthrowing single-flight scheduling. Closing owner A is terminal for lifetime A; installing owner B creates a fresh lifetime that rejects all late A completions.

### Phase 4 - Connected Geometry

Resolve maximal same-group grid-row and visible-list segments in pure helpers. Draw one intersect-clipped fill per segment below item-local overlays. Verify exact bounds and rasterized seam/clip behavior.

### Phase 5 - Cleanup and Verification

Delete count-hint, materialized-child, reveal-prefix, delimiter-selection, and per-child filmstrip paths. Run zero-reference checks, targeted/broad tests, Editor build, manual narrow/wide grid and list checks, then mandatory `plan-review` cycles.

## Validation Strategy

- TDD red/green cycles use deterministic facade hooks and fake schedulers; no test depends on worker timing.
- Focused filters cover projection/state errors, facade publication, coordinator behavior, picker cache, geometry, and ImGui raster output.
- Broad unit regression covers `AssetBrowserPresentationTests`, `EditorAssetDatabaseTests`, and `AssetDatabaseFacadeTests`.
- `ctest` and the `Editor` target validate integration.
- Manual verification records grid/list behavior at narrow and wide widths with filtering and refresh/replacement actions.
- Cross-platform correctness is not claimed until Linux/macOS CI evidence exists.

## Complexity Tracking

No constitution violations require justification. The aggregate immutable state and asynchronous bundle are deliberate replacements for existing duplicated caches and progressive partial publication; they reduce mixed-generation state rather than add a parallel architecture.
