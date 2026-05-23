# Implementation Plan: MetaParser Resource Reflection

**Branch**: `030-metaparser-resource-reflection` | **Date**: 2026-05-22 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `specs/030-metaparser-resource-reflection/spec.md`

## Summary

Migrate Nullus-owned rendering resource classes that currently handwrite `StaticMetaTypeName()` and `GetObjectTypeName()` into the normal MetaParser `CLASS`/`GENERATED_BODY()` reflection path. Remove duplicate external manual resource class registrations while preserving stable resource type names, pointer type registration, and existing `PPtr<T>` behavior. The implemented branch also carries the asset/runtime/render support required by that migration: runtime manifest material prewarm, editor artifact database indexing improvements, portable MetaParser fixture execution, PPtr target macro guardrails, and FrameGraph uniform-buffer state fixes.

## Technical Context

**Language/Version**: C++20 runtime/editor code; C#/.NET 8 MetaParser generator already present  
**Primary Dependencies**: Nullus reflection runtime, MetaParser generated registration, existing CMake build flow  
**Storage**: N/A  
**Testing**: GoogleTest via `NullusUnitTests`; `ReflectionTest` smoke validation  
**Target Platform**: Primary local validation on Windows; tests and generator fixtures must not encode Windows Debug-only assumptions  
**Project Type**: Native engine/runtime reflection migration  
**Performance Goals**: Preserve existing type lookup behavior; avoid adding runtime string paths beyond generated compile-time names; avoid per-manifest central artifact DB saves during editor import batches  
**Constraints**: Do not hand-edit `Runtime/*/Gen/`; preserve current generated registration flow; avoid exposing GPU/RHI resource internals as reflected fields; do not revert unrelated dirty workspace changes  
**Scale/Scope**: Six rendering resource classes, rendering external reflection cleanup, runtime/editor asset pipeline integration, FrameGraph uniform-buffer state contract, and focused tests

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: Pass. Reflection/MetaParser behavior change uses `specs/030-metaparser-resource-reflection/`.
- **Validation matches subsystem**: Pass. Plan includes normal build generation plus focused `NullusUnitTests` and `ReflectionTest`.
- **Generated boundaries**: Pass. Generated files are outputs only; source changes occur in headers, external reflection, tests, and spec artifacts.
- **Incremental verified delivery**: Pass. TDD source-contract tests precede migration, then build/test validates generated output.
- **Product runtime preservation**: Pass. Validation includes existing resource/PPtr/reflection tests plus runtime material prewarm and FrameGraph uniform-buffer barrier checks needed to keep packaged resource references and render synchronization correct.

## Project Structure

### Documentation (this feature)

```text
specs/030-metaparser-resource-reflection/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
└── tasks.md
```

### Source Code

```text
Runtime/Rendering/Resources/
├── Mesh.h
├── Material.h
├── Shader.h
├── Texture.h
├── Texture2D.h
└── TextureCube.h

Runtime/Rendering/
└── ExternalReflection.h

Tests/Unit/
├── MetaParserGenerationModuleTests.cpp
├── GameLaunchArgsTests.cpp
├── AssetDatabaseFacadeTests.cpp
├── RenderFrameworkContractTests.cpp
├── PPtrTests.cpp
└── ReflectionRuntimeCoreTests.cpp
```

**Structure Decision**: Keep this migration inside the existing rendering resources and reflection test surfaces. Do not introduce new reflection macros unless current `CLASS`/`GENERATED_BODY()` cannot support fieldless resource classes.

## Phase 0: Research

### Decision: Use existing `CLASS`/`GENERATED_BODY()` for owned resource classes

**Rationale**: Generated headers already emit `StaticMetaTypeName`, `StaticMetaTypeKey`, and `GetObjectTypeName` for `NLS::Object`/`NLS::NamedObject` derived reflected types. This is the project-native path and avoids inventing a parallel lightweight object macro.

**Alternatives considered**:

- Add a new `NLS_OBJECT_BODY()` macro. Rejected for this change because it would create a second owned-object reflection path before proving `GENERATED_BODY()` is insufficient.
- Keep external type shells and remove only handwritten strings. Rejected because external registration cannot inject object virtual overrides and would keep an owned class outside the MetaParser path.

### Decision: Do not reflect resource implementation fields

**Rationale**: The migration is about generated type identity, not inspector-editable GPU resource internals. Existing MetaParser only emits fields/methods marked with reflection annotations, so adding `GENERATED_BODY()` without new `PROPERTY()` members preserves member exposure.

**Alternatives considered**:

- Reflect selected resource fields now. Rejected as separate behavior and serialization scope.

### Decision: Remove manual resource class registration from rendering external reflection

**Rationale**: Generated resource registrations already allocate type, pointer, and const-pointer entries. Keeping `RegisterResourceReferenceType<TResource>` for the same owned classes risks duplicate or order-dependent registration.

**Alternatives considered**:

- Leave manual registration as fallback. Rejected because it violates SSoT and can mask generated registration regressions.

## Phase 1: Design

### Data Model

- **ResourceReflectionIdentity**
  - `qualifiedName`: stable resource type name such as `NLS::Render::Resources::Mesh`
  - `headerPath`: owning resource header
  - `generatedHeader`: expected generated header emitted by MetaParser
  - `generatedSource`: expected generated registration source emitted by MetaParser

- **ExternalReflectionBoundary**
  - `ownedResourceTypes`: resource classes that must not be manually registered externally
  - `externalValueTypes`: value/external types that remain in `ExternalReflection.h`

### Contracts

No external public API contract is added. The internal contract is source-level and test-enforced:

- Owned reflected resource class headers must use `CLASS` and `GENERATED_BODY()`.
- Owned reflected resource class headers must not define handwritten type-name/object bridge implementations.
- Rendering external reflection must not manually register the migrated owned resource classes.
- Existing reflection names remain stable.

### Quickstart Validation

1. Run the source-contract regression test first to see it fail before migration:

   ```powershell
   .\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=MetaParserGenerationModuleTests.RenderingResourceReflectionOwnedClassesUseGeneratedBodies
   ```

2. Build through the normal generation path:

   ```powershell
   cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false
   ```

3. Run focused reflection/resource tests:

   ```powershell
   .\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=MetaParserGenerationModuleTests.*:ReflectionRuntimeTestFixture.*:PPtrTests.*
   ```

4. Run reflection smoke validation:

   ```powershell
   cmake --build Build --target ReflectionTest --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false
   .\Build\bin\Debug\ReflectionTest.exe
   ```

## Post-Design Constitution Check

- **Spec-first major change**: Pass. Spec, plan, and tasks are in one feature bundle.
- **Validation matches subsystem**: Pass. Validation targets MetaParser generation plus runtime reflection/PPtr tests.
- **Generated boundaries**: Pass. Generated outputs are produced by build only.
- **Incremental verified delivery**: Pass. TDD source-contract test is first implementation step.
- **Product runtime preservation**: Pass. Runtime material manifest prewarm and FrameGraph uniform-buffer barrier fixes are included because review found they were required to keep packaged resource references and render synchronization correct.

## Complexity Tracking

No constitution violations.

## Phase 2 Review Fixes

### Runtime material prewarm

Add `PrewarmRuntimeMaterialAssets()` in `Project/Game/RuntimeAssetManifestStartup.*`. It iterates runtime manifest entries, filters `ArtifactType::Material` or loader id `material`, and calls `MaterialManager::LoadArtifactWithoutTextures()` so shader/material state is available while textures remain deferred. `Game::Context` calls this after resource managers and runtime database are provided.

### Artifact database batching

Change `ArtifactDatabase::UpsertManifest()` to remove existing source records without triggering an intermediate rebuild, then rebuild once after adding the final records. In `AssetDatabaseFacade`, mark artifact DB cache entries dirty and defer disk saves while `m_assetEditing` is active; `StopAssetEditing()` flushes dirty central indexes once per database path. Disk I/O is performed outside the broad cache mutex and under the per-database path mutex.

### MetaParser test portability and PPtr target guard

Replace Windows Debug-only MetaParser fixture commands with a helper that resolves the built executable from common CMake configurations and quotes paths portably. Update `BuildSupportedPPtrObjectTargetTypeNames()` to parse wrapped macro invocations and throw an explicit error when `NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS` exists but yields no targets.

### FrameGraph uniform buffer state

Map FrameGraph uniform buffer read/write state to the CPU-visible `GenericRead` contract instead of `UniformBuffer`, allowing the resource-state tracker to suppress illegal upload-heap transitions.
