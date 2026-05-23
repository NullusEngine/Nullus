# Implementation Plan: Object Graph Serialization

**Branch**: `018-object-serialization` | **Date**: 2026-05-08 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `specs/018-object-serialization/spec.md`

## Summary

Replace the current scene-specific GameObject serialization path with a unified Object Graph persistence system. The design introduces a shared GUID foundation, strong `ObjectId` and `AssetId` identities, `Scene` as a reflected object graph root, staged scene/prefab loading, deterministic text archives, structured diagnostics, and prefab patch operations. The implementation intentionally does not keep `GameobjectSerialize.cpp` as a legacy adapter and removes persistent `worldID` semantics.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus reflection/MetaParser pipeline, json11-based JSON support already present in the repository, existing GTest test infrastructure
**Storage**: Deterministic text Object Graph documents for `.scene` and `.prefab`; future cooked/binary output generated from the same document model
**Testing**: `NullusUnitTests`, `ReflectionTest`, targeted golden-file style serialization tests
**Target Platform**: Nullus desktop runtime/editor targets, with validation first on Windows Debug build
**Project Type**: Native game engine runtime/editor architecture change
**Performance Goals**: Scene/prefab save/load must be deterministic and correct first; text format remains appropriate for editor workflows and version control
**Constraints**: No hand edits to generated files; reflection changes must use MetaParser input declarations; Editor and Game must remain runnable during staged work; no old scene-format runtime compatibility path
**Scale/Scope**: Foundational serialization architecture for scenes, game objects, components, prefabs, asset references, and future cooked runtime packages

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec scope**: PASS. This major Runtime/serialization/reflection change is covered by `specs/018-object-serialization/`.
- **Generated-file boundaries**: PASS. Generated files under `Runtime/*/Gen/` will not be hand-edited; reflection changes must flow through headers and MetaParser.
- **Validation expectations**: PASS. Reflection and serialization changes require `NullusUnitTests`, `ReflectionTest`, and focused serialization/golden-file tests. Claims are limited to validated Windows Debug unless other platforms are explicitly tested.
- **Product runtime viability**: PASS WITH STAGING. Editor/Game should remain buildable after each task group. During the removal of old serialization entry points, any temporary breakage must be contained to the current task and restored before checkpoint.
- **Final evidence path**: PASS. Planned evidence is Debug build of affected targets, `NullusUnitTests`, `ReflectionTest`, deterministic save tests, and prefab round-trip tests.

Post-design re-check: PASS. Research, data model, contracts, and tasks preserve the same generated-file, validation, and product runtime constraints.

## Project Structure

### Documentation (this feature)

```text
specs/018-object-serialization/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── object-graph-document.md
│   └── serialization-services.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/Base/
├── Guid.h
├── Guid.cpp
└── Reflection/
    └── RuntimeMetaProperties.h

Runtime/Engine/
├── GameObject.h
├── GameObject.cpp
├── Components/
│   └── Component.h
├── SceneSystem/
│   ├── Scene.h
│   └── Scene.cpp
└── Serialize/
    ├── ObjectId.h
    ├── ObjectGraphDocument.h
    ├── ObjectGraphReader.h
    ├── ObjectGraphWriter.h
    ├── ObjectGraphSerializer.h
    ├── ObjectGraphInstantiator.h
    ├── SerializationDiagnostic.h
    ├── PrefabDocument.h
    └── PickRegistry.h

Tests/Unit/
├── GuidTests.cpp
├── ObjectGraphDocumentTests.cpp
├── SceneObjectGraphSerializationTests.cpp
├── PrefabObjectGraphSerializationTests.cpp
└── SerializationDiagnosticTests.cpp
```

**Structure Decision**: Place the generic GUID value in `Runtime/Base` so all modules can use it. Keep engine-specific identities and Object Graph persistence under `Runtime/Engine/Serialize`. Update scene/game object/component ownership in existing Runtime/Engine files. Add focused unit tests under `Tests/Unit`.

## Complexity Tracking

No constitution violations requiring justification.
