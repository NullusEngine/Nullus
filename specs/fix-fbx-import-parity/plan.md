# Implementation Plan: Fix FBX Import Parity

**Branch**: `fix-fbx-import-parity` | **Date**: 2026-06-07 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/fix-fbx-import-parity/spec.md`

## Summary

Fix FBX import parity gaps at the generated asset boundary: generated FBX materials should translate Phong shininess into PBR roughness, should not enable tangent-space normal mapping from unconverted bump/height channels, and already-imported FBX generated prefabs should demonstrate the same unified hot-cache repeated-drop behavior as glTF.
Follow-up rendering evidence showed generated FBX artifacts can also contain parser sentinel roughness values such as `u_Roughness=-2.200000`; those invalid PBR scalars must be filtered before serialization so shader lighting stays in valid material space.
Follow-up RenderDoc evidence from `Editor_DX12_frame2018.rdc` showed the remaining darkness starts before lighting: FBX GBuffer draws such as `EID 17374` output `NaN,NaN,NaN,1` to `GBufferNormal` while `u_EnableNormalMapping=1`. The shader normal-map path must therefore guard degenerate TBN and decoded normal-map vectors so old/generated assets cannot poison the deferred lighting input.
Current user evidence shows three remaining parity gaps after the shader safety fix: FBX is still darker than glTF, imported FBX drops are noticeably slower, and FBX decal material output is wrong. Artifact inspection of imported Sponza confirms FBX materials still carry `u_Albedo=0.5`, stale `u_Roughness=-2.2`, and `dirt_decal` serialized as `Opaque`; artifact size inspection also shows FBX generated mesh/texture payloads are much larger than the matching glTF artifacts. The next increment therefore fixes generated FBX material semantics, Assimp baked direction transforms, and model-scene importer versioning so old bad artifacts are invalidated.

## Technical Context

**Language/Version**: C++17-style engine/editor code, HLSL shaders  
**Primary Dependencies**: Existing Nullus asset import pipeline, Assimp FBX importer capability macro, GoogleTest  
**Storage**: Generated asset artifacts under `Library/Artifacts`  
**Testing**: `NullusUnitTests.exe` focused GoogleTest filters  
**Target Platform**: Windows editor/unit-test build for this fix  
**Project Type**: Desktop editor and runtime engine  
**Performance Goals**: Repeated imported FBX drop must hit hot cache and avoid prefab graph reload  
**Constraints**: Do not hand-edit generated files; do not reparse source FBX on foreground drop path; preserve explicit roughness; do not treat bump/height textures as normal-map data without conversion; shader fallback must preserve valid normal-map behavior while preventing NaN output  
**Scale/Scope**: Narrow importer/material conversion, parser mesh direction handling, importer versioning, shader normal safety, and drag/drop/artifact regression tests

## Constitution Check

- Spec scope: Required because the change affects Runtime material conversion and Project editor drag/drop behavior. This bundle is the source of truth.
- Generated-file boundaries: No `Runtime/*/Gen/` files will be edited.
- Backend/platform validation: Unit-test validation targets current Windows build; no cross-backend rendering claim will be made.
- Product runtime viability: Changes are limited to generated material parameters, normal-map enablement, and tests around existing loading path, preserving editor/game runtime flow.
- Evidence path: Focused material conversion and drag/drop unit tests, plus related existing cache test.

## Project Structure

### Documentation

```text
specs/fix-fbx-import-parity/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Runtime/Rendering/Assets/
└── MaterialConversion.cpp

App/Assets/Engine/Shaders/
├── DeferredGBuffer.hlsl
├── StandardPBR.hlsl
└── Standard.hlsl

Tests/Unit/
├── AssetMaterialConversionTests.cpp
└── EditorAssetDragDropTests.cpp
```

**Structure Decision**: Keep the fix in the existing material conversion module and existing editor drag/drop tests. No new production subsystem is needed.

## Research

- **Decision**: Use a Phong/Blinn exponent approximation `roughness = sqrt(2 / (shininess + 2))`, clamped to `[0, 1]`, only as a fallback when authored roughness scalar and texture inputs are absent.
  **Rationale**: This is a common low-cost conversion from legacy shininess to microfacet-style roughness and maps higher shininess to lower roughness.
  **Alternatives considered**: Ignoring shininess preserves current behavior but drops visible material information; changing shaders to consume `SpecularPower` would complicate both forward and deferred pipelines and still leave generated GBuffer roughness wrong.

- **Decision**: Add an FBX repeated-drop regression test rather than changing drag/drop loading logic first.
  **Rationale**: Code inspection shows imported assets already converge through `LoadImportedPrefabFast`; a test should establish whether the reported slowness is import-time cost or a cache-path regression.
  **Alternatives considered**: Optimizing renderer artifact stamping immediately risks changing cache invalidation without evidence.

- **Decision**: Preserve Assimp `aiTextureType_SHININESS` / `map_Ns` as a `shininess` channel instead of mapping it to PBR `roughness`.
  **Rationale**: Shininess/gloss textures have the inverse semantic of roughness textures; feeding them into `u_RoughnessMap` would keep FBX materials visually wrong.
  **Alternatives considered**: Converting gloss textures into roughness textures requires an explicit inversion/transcoding artifact path and is deferred as a separate fidelity improvement.

- **Decision**: Preserve FBX parser `normal` channels as normal-map inputs, but do not map parser `bump` channels directly to the generated `Normal` texture slot.
  **Rationale**: UE 4.27 first prefers `sNormalMap` and only falls back to `sBump` while importing that texture with normal-map settings; Unity 2018.4 binds and enables `_NORMALMAP` only when a resolved normal texture exists and has explicit normal-map import semantics. Nullus currently lacks a height/bump-to-normal conversion path, so binding `bump` directly to `u_NormalMap` can distort lighting.
  **Alternatives considered**: Keeping `bump` as a normal fallback preserves compatibility with some authored assets but keeps the dark/partially-unlit failure mode; implementing height-to-normal conversion now would be a larger texture pipeline feature.

- **Decision**: Treat parser roughness scalars outside `[0, 1]` as invalid material metadata, record a diagnostic, and omit the scalar from generated material factors.
  **Rationale**: PBR roughness is a unit-range input, and observed imported FBX artifacts contained `-2.200000`, which can produce invalid lighting behavior when multiplied by a roughness texture or used directly by deferred/forward PBR shaders.
  **Alternatives considered**: Clamping in the shader would hide the generated asset defect and keep bad data serialized; clamping in conversion would preserve a parser sentinel as if it were authored data. Ignoring invalid scalars keeps authored roughness textures neutral and allows shininess fallback when no valid roughness source remains.

- **Decision**: Add shader-local safe normalization and TBN fallback in Standard, StandardPBR, and DeferredGBuffer.
  **Rationale**: RenderDoc shows old/generated materials can still have `u_EnableNormalMapping=1`; if tangent, bitangent, or decoded normal-map data is zero/degenerate, current `normalize(0)` paths emit NaN into the GBuffer normal target. Shader-side guards keep previously imported assets renderable and complement the import-side bump/normal fix.
  **Alternatives considered**: Reimport-only fixes leave existing assets broken; disabling normal maps globally loses valid normal-map detail; clamping after lighting hides the first bad data point and still allows NaN GBuffer writes.

- **Decision**: Ignore neutral parser diffuse tints for FBX materials that already bind a diffuse/base-color texture.
  **Rationale**: Real artifacts show Assimp FBX commonly reports a neutral `0.5` diffuse color beside authored base-color textures, while glTF baseColorFactor remains `1.0`; multiplying texture samples by this parser default makes FBX visibly darker.
  **Alternatives considered**: Keeping the factor preserves every parser value but perpetuates the visible parity bug; always ignoring diffuse colors would break texture-less legacy FBX materials, so the rule is limited to textured FBX neutral tints.

- **Decision**: Treat FBX opacity textures as blend-capable for surface-mode inference.
  **Rationale**: Current FBX `dirt_decal` imports with an opacity texture but no opacity scalar and serializes as `Opaque`; glTF encodes the equivalent as blend and then `SurfaceModeForMaterial` correctly infers `Decal` from the name.
  **Alternatives considered**: Name-only decal inference for opaque materials could misclassify opaque decals; requiring only scalar opacity misses common texture-only alpha masks.

- **Decision**: Transform Assimp baked normal/tangent/bitangent streams with the direction part of the node matrix and normalize them.
  **Rationale**: FBX SDK already treats these as directions; Assimp's full-matrix multiplication can add translation into direction streams, corrupting lighting and normal mapping.
  **Alternatives considered**: Shader fallback can prevent NaNs but cannot recover correct per-surface lighting from bad imported direction vectors.

- **Decision**: Bump model-scene importer version for this semantic change.
  **Rationale**: Existing TestProject artifacts have importer version 6 and still contain `u_Roughness=-2.2`, dark FBX albedo factors, and opaque decals. A version bump is the project-standard invalidation mechanism.
  **Alternatives considered**: Asking users to manually delete artifacts or reimport from memory is fragile and leaves stale generated assets in normal startup paths.

## Validation Plan

- Run a red test for FBX shininess-derived roughness before production changes, including authored roughness scalar/texture precedence.
- Run a red test for Assimp `map_Ns` preserving shininess texture semantics instead of emitting a roughness texture channel.
- Run a red test for FBX bump-only parser materials keeping normal mapping disabled, with a true-normal regression in the same area.
- Run a red test for invalid FBX roughness scalars proving `-2.2` does not serialize into `u_Roughness`, roughness textures remain neutral, and shininess fallback is not suppressed.
- Run a red shader regression test proving PBR and deferred GBuffer normal-map paths contain safe normalization/TBN fallback logic.
- Run red material conversion tests for FBX textured neutral diffuse tint and FBX opacity-texture decal inference.
- Run a red Assimp baked transform test proving translation does not pollute normal/tangent/bitangent vectors.
- Run a red importer version test proving version 6 is no longer current after FBX generated artifact semantic changes.
- Run a red-or-skip FBX repeated-drop cache test before production changes.
- After implementation, run focused `NullusUnitTests.exe` filters covering material conversion normal/bump behavior, shader normal safety, and existing glTF repeated-drop cache behavior.
- Compile or otherwise validate modified HLSL shader syntax for the touched shader files.
- Run required plan-review gate before reporting completion.
