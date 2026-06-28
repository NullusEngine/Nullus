# Implementation Plan: Unity-Aligned Asset Reuse

**Branch**: `052-unity-aligned-asset-reuse` | **Date**: 2026-06-17 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/052-unity-aligned-asset-reuse/spec.md`

## Summary

Model import should reduce duplicate texture assets by resolving material texture references to stable project texture asset identities before falling back to model-owned texture sub-assets. The implementation will add a small texture reference resolution layer, preserve model-owned identity for non-texture sub-assets, keep material payloads path-compatible for phase one, expose resolution/remap state in Asset Properties, and validate the behavior through existing asset import pipeline tests.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus editor asset pipeline, runtime asset metadata/manifests, material conversion, existing Assimp/glTF/OBJ import support, existing UI widgets and GoogleTest
**Storage**: Project files under `Assets/`, `.meta` sidecar files, `Library/Artifacts/<asset-guid>/manifest.json`, artifact database records
**Testing**: `NullusUnitTests` with focused GoogleTest cases in asset import and editor asset tests
**Target Platform**: Nullus editor on Windows first, with path handling kept portable for existing Linux/macOS-compatible helpers
**Project Type**: Desktop editor and runtime asset pipeline
**Performance Goals**: Model import remains linear in discovered texture references plus indexed project asset candidates; no full content-hash scan of all assets
**Constraints**: No global content-addressed dedup in phase one; do not hand-edit generated files under `Runtime/*/Gen/`; preserve atomic artifact write behavior and existing fallback import behavior; remap/report text formats must be versioned and percent-encoded for special characters
**Scale/Scope**: One importer workflow spanning external model import, texture asset lookup, material conversion paths, manifest dependencies, Asset Properties display, and targeted unit tests

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. This feature has `spec.md`, this `plan.md`, and will generate `tasks.md` before implementation.
- **Validation matches subsystem**: PASS. Validation is centered on `NullusUnitTests` asset import cases plus focused manual Asset Properties verification; no rendering backend correctness claims are made.
- **Generated code and backend boundaries**: PASS. No generated files under `Runtime/*/Gen/` are in scope, and no RHI/backend behavior is changed.
- **Incremental, verified delivery**: PASS. Plan splits resolver, importer, metadata/UI, and test work into reviewable stages with targeted tests.
- **Product runtime preservation**: PASS. Existing models remain unchanged until reimport; model-local fallback preserves import usability when external resolution fails.

## Project Structure

### Documentation (this feature)

```text
specs/052-unity-aligned-asset-reuse/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── model-texture-remap-settings.md
│   ├── texture-resolution-report.md
│   └── import-diagnostics.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
Project/Editor/Assets/
├── AssetDatabaseFacade.cpp/.h             # Existing import/reimport facade; used by resolver for texture artifact lookup and optional texture import
├── AssetImporterSettings.cpp/.h           # Add model texture resolution settings and remap setting helpers
├── EditorAssetPath.h                      # Existing project-relative path resolution helpers
├── ExternalAssetImporter.cpp              # Integrate resolver output into model import, texture payload filtering, diagnostics, dependencies
├── ModelTextureReferenceResolver.cpp/.h   # New focused resolver for stable source keys, path/name matching, remaps, dependencies
└── ModelTextureResolutionReport.cpp/.h    # New versioned report serialization/parsing, atomic report path helpers, display rows

Project/Editor/Panels/
├── AssetProperties.cpp/.h                 # Add model-level texture settings, remap rows, and resolution report display
└── AssetView.cpp/.h                       # No feature logic; remains preview-only

Runtime/Rendering/Assets/
├── MaterialConversion.cpp/.h              # Keep material payload path format; consume resolved texture resource path map
└── ImportedScene.h                        # Use existing imported texture records as resolver inputs; no schema change planned

Runtime/Core/Assets/
├── ArtifactManifest.h/.cpp                # No structural change expected; use dependencies and existing imported artifact fields
└── AssetMeta.cpp/.h                       # No structural change expected; settings map stores remaps/settings

Tests/Unit/
├── AssetImportPipelineTests.cpp           # Main resolver/import/material/manifest/stale dependency/legacy reimport coverage
├── AssetDatabaseFacadeTests.cpp           # No planned changes; use only if implementation exposes a facade-specific regression
└── AssetProperties tests                  # Add lightweight tests only for pure parsing/report/display-row helpers, not immediate-mode UI rendering
```

**Structure Decision**: Keep resolver and report logic out of `ExternalAssetImporter.cpp` and `AssetProperties.cpp` so the large existing files only orchestrate feature flow. The resolver owns stable source-key derivation, matching policy, remap validation, and deterministic candidate ordering. The report owns versioned serialization, stale detection inputs, and displayable resolution state. The importer owns artifact generation, material path map construction keyed by material texture keys, dependency recording, and successful-commit report replacement.

## Design Guardrails

- Stable asset identity is `AssetMeta.id + subAssetKey`; source path and name search are resolution heuristics, not persisted authority.
- Stable texture remap keys are versioned (`mtxsrc:v1`) and derived from texture source kind, source key, normalized URI, buffer view key, embedded index, stable format/channel discriminators, and display-name fallback only when no stronger identity exists. Order-derived collision suffixes are a last resort and must be surfaced as warnings.
- Material payload compatibility is maintained by mapping `materialTextureKey -> resourcePath`; remap keys must never replace the `ImportedScene` texture key used by material conversion.
- Resolution reports are editor-only sidecars under the committed model artifact root and are atomically replaced only on successful import.
- Dependencies must include target texture GUID freshness, target texture artifact freshness, and path/name lookup mapping freshness, including negative and ambiguous candidate sets.
- Legacy model-owned texture sub-assets disappear from the current manifest only after reimport and only for texture references externally resolved to project texture assets; mesh/material/prefab/skeleton/animation sub-asset keys remain stable.
- Unique-name search is project-root scoped, texture-type-only, deterministic, and ambiguity-preserving; it never uses content hashes.

## Complexity Tracking

No constitution violations are required. The feature spans several existing files because Nullus already separates import, material conversion, artifact manifests, and editor panels.

## Phase 0: Research Output

See [research.md](research.md).

## Phase 1: Design Output

See [data-model.md](data-model.md), [quickstart.md](quickstart.md), and contracts under [contracts/](contracts/).

## Constitution Check (Post-Design)

- **Spec-first major change**: PASS. Design artifacts remain under one spec bundle.
- **Validation matches subsystem**: PASS. Quickstart and task plan use unit tests for asset pipeline behavior plus manual editor verification for UI.
- **Generated code and backend boundaries**: PASS. The design explicitly avoids generated files and backend-specific renderer changes.
- **Incremental, verified delivery**: PASS. Data model and contracts support staged tests before importer integration.
- **Product runtime preservation**: PASS. Existing model artifacts remain until reimport and fallback behavior is explicitly preserved.
