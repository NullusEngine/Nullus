# Tasks: Fix FBX Import Parity

**Input**: Design documents from `specs/fix-fbx-import-parity/`
**Prerequisites**: `plan.md`, `spec.md`
**Tests**: Required by TDD for this bug fix.

## Phase 1: Setup

- [x] T001 Confirm current FBX parser capabilities and existing focused test entrypoints.

## Phase 2: User Story 1 - FBX Materials Preserve Authoring Brightness (Priority: P1)

**Goal**: FBX shininess-only materials generate a PBR roughness value consumed by current shaders.

**Independent Test**: Run the focused `AssetMaterialConversionTests` shininess fallback test.

- [x] T002 [US1] Add failing shininess-to-roughness conversion test in `Tests/Unit/AssetMaterialConversionTests.cpp`.
- [x] T003 [US1] Implement minimal shininess fallback in `Runtime/Rendering/Assets/MaterialConversion.cpp`.
- [x] T004 [US1] Verify explicit roughness scalar and texture inputs still take precedence in `Tests/Unit/AssetMaterialConversionTests.cpp`.

## Phase 3: User Story 2 - Imported FBX Drops Use Unified Hot Cache (Priority: P2)

**Goal**: Repeated drops of an already-imported FBX generated model demonstrate the same hot-cache behavior as glTF.

**Independent Test**: Run the focused `EditorAssetDragDropTests` repeated FBX drop cache test.

- [x] T005 [US2] Add failing or capability-skipped FBX repeated-drop cache test in `Tests/Unit/EditorAssetDragDropTests.cpp`.
- [x] T006 [US2] Adjust drag/drop loading only if the new test exposes a cache-path defect in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp` (no production drag/drop change needed; test proved the existing hot-cache path).

## Phase 4: Validation And Review

- [x] T007 Run focused `NullusUnitTests.exe` filters for material conversion and drag/drop cache tests.
- [x] T008 Self-review touched files for regressions, missing tests, and generated-file boundaries.
- [x] T009 Run required plan-review gate before final completion.
- [x] T010 [Review] Preserve Assimp shininess/gloss texture semantics and verify `map_Ns` no longer emits a roughness texture channel.

## Phase 5: User Story 3 - FBX Bump Maps Do Not Masquerade As Tangent Normals (Priority: P1)

**Goal**: FBX bump-only parser materials keep runtime normal mapping disabled unless a true normal-map channel exists.

**Independent Test**: Run the focused `AssetMaterialConversionTests.FbxBumpOnlyChannelDoesNotEnableNormalMapping` test.

- [x] T011 [US3] Add failing FBX bump-only normal-map enablement regression test in `Tests/Unit/AssetMaterialConversionTests.cpp`.
- [x] T012 [US3] Update FBX parser material conversion in `Runtime/Rendering/Assets/MaterialConversion.cpp` so `bump` no longer binds the `Normal` slot for FBX.
- [x] T013 [US3] Verify true FBX `normal` channels still bind `u_NormalMap` and enable normal mapping.
- [x] T014 [US3] Run focused material conversion tests and required review gate.

## Phase 6: User Story 4 - FBX Parser Roughness Sentinels Stay Out Of PBR Shaders (Priority: P1)

**Goal**: Invalid parser roughness scalars such as `-2.2` do not serialize into generated FBX material PBR uniforms.

**Independent Test**: Run `AssetMaterialConversionTests.FbxInvalidRoughnessScalarDoesNotPollutePbrUniform`.

- [x] T015 [US4] Add failing invalid FBX roughness scalar regression test in `Tests/Unit/AssetMaterialConversionTests.cpp`.
- [x] T016 [US4] Update parser material conversion in `Runtime/Rendering/Assets/MaterialConversion.cpp` to ignore and diagnose roughness scalars outside `[0, 1]`.
- [x] T017 [US4] Verify invalid roughness with a roughness texture keeps neutral `u_Roughness`, and invalid roughness without a texture still permits shininess fallback.

## Phase 7: User Story 5 - FBX Normal Mapping Cannot Poison GBuffer Normals (Priority: P1)

**Goal**: Deferred and PBR normal-map paths never write NaN normals when imported assets provide degenerate tangent frames or decoded normal-map vectors.

**Independent Test**: Run `AssetMaterialConversionTests.PbrShadersGuardDegenerateNormalMapInputs`.

- [x] T018 [US5] Add failing shader normal-safety regression test in `Tests/Unit/AssetMaterialConversionTests.cpp`.
- [x] T019 [US5] Add safe normalization and tangent-frame fallback in `App/Assets/Engine/Shaders/Standard.hlsl`, `StandardPBR.hlsl`, and `DeferredGBuffer.hlsl`.
- [x] T020 [US5] Verify focused shader/material tests and compile touched HLSL shaders where practical.
- [x] T021 [US5] Reconcile RenderDoc evidence with the fix and run required review gate.

## Phase 8: User Story 6 - FBX Textured Diffuse Materials Match glTF Brightness (Priority: P1)

**Goal**: FBX diffuse/base-color textures are not multiplied darker by parser diffuse factor defaults.

**Independent Test**: Run `AssetMaterialConversionTests.FbxTexturedDiffuseDoesNotDarkenBaseColorTexture`.

- [x] T022 [US6] Add failing FBX textured diffuse brightness regression test in `Tests/Unit/AssetMaterialConversionTests.cpp`.
- [x] T023 [US6] Update FBX parser material conversion in `Runtime/Rendering/Assets/MaterialConversion.cpp` so textured FBX diffuse factors do not darken `u_Albedo`.
- [x] T024 [US6] Verify texture-less FBX diffuse and non-FBX parser tint compatibility remain covered.

## Phase 9: User Story 7 - FBX Decal-Like Materials Generate Decal Surfaces (Priority: P1)

**Goal**: FBX opacity-texture decal materials serialize as Decal surfaces like matching glTF imports.

**Independent Test**: Run `AssetMaterialConversionTests.FbxOpacityTextureWithDecalNameSerializesAsDecalSurface`.

- [x] T025 [US7] Add failing FBX opacity-texture decal material regression test in `Tests/Unit/AssetMaterialConversionTests.cpp`.
- [x] T026 [US7] Update parser material conversion so FBX opacity texture participates in blend/decal surface inference.
- [x] T027 [US7] Verify non-decal opacity materials remain transparent instead of decal.

## Phase 10: User Story 8 - Assimp Baked Direction Streams Stay Directional (Priority: P1)

**Goal**: Assimp baked transform processing preserves normals/tangents/bitangents as normalized directions.

**Independent Test**: Run `AssetImportPipelineTests.AssimpBakedNodeTransformsDoNotTranslateDirectionStreams`.

- [x] T028 [US8] Add failing Assimp baked direction-stream regression test in `Tests/Unit/AssetImportPipelineTests.cpp`.
- [x] T029 [US8] Update `Runtime/Rendering/Resources/Parsers/AssimpParser.cpp` to transform baked normal/tangent/bitangent streams as directions and normalize them.
- [x] T030 [US8] Verify source-space shared mesh parser behavior remains unchanged.

## Phase 11: Stale Artifact Invalidation And Final Review

**Goal**: Previously generated importer version 6 FBX artifacts are treated as stale so users do not keep loading old dark/opaque/invalid material payloads.

- [x] T031 Add failing importer version regression test proving current model-scene importer version is above 6.
- [x] T032 Bump model-scene importer version in `Runtime/Core/Assets/AssetMeta.cpp`.
- [x] T033 Run focused material conversion, Assimp parser, importer version, and drag/drop cache tests.
- [x] T034 Run required plan-review gate before final completion.

## Phase 12: Review Follow-Up

**Goal**: Address first-round plan-review P1 findings before final completion.

- [x] T035 Add failing authored textured FBX diffuse tint test in `Tests/Unit/AssetMaterialConversionTests.cpp`.
- [x] T036 Preserve clearly authored textured FBX diffuse tints while keeping neutral textured FBX diffuse compatibility policy in `Runtime/Rendering/Assets/MaterialConversion.cpp`.
- [x] T037 Precompute Assimp baked normal/direction transforms once per mesh in `Runtime/Rendering/Resources/Parsers/AssimpParser.cpp`.
- [x] T038 Add Assimp scaled-node direction-stream tests covering non-uniform scale and singular-scale fallback.
- [x] T039 Re-run focused FBX material, Assimp parser, importer version, and drag/drop cache tests after review fixes.

## Phase 13: Multi-Agent Review Follow-Up

**Goal**: Address multi-agent plan-review P1 findings before final completion.

- [x] T040 Add failing conversion-layer compatibility-policy tests proving neutral textured FBX tints are ignored by default and can be preserved by override.
- [x] T041 Remove FBX textured diffuse default classification from `Runtime/Rendering/Resources/Parsers/AssimpParser.cpp` and keep the neutral-tint decision in `Runtime/Rendering/Assets/MaterialConversion.cpp`.
- [x] T042 Add ignored FBX bump/height diagnostic coverage in `Tests/Unit/AssetMaterialConversionTests.cpp`.
- [x] T043 Add Assimp identity/positive-uniform direction-stream fast-path guard in `Tests/Unit/AssetImportPipelineTests.cpp`.
- [x] T044 Add identity/positive-uniform direction-stream copy fast path in `Runtime/Rendering/Resources/Parsers/AssimpParser.cpp`.
- [x] T045 Add deferred decal albedo-only MRT write-mask regression in `Tests/Unit/RendererFrameObjectBindingTests.cpp`.
- [x] T046 Update deferred decal overrides so color decals blend albedo only and suppress normal/material writes.
- [x] T047 Strengthen repeated glTF/FBX drag-drop hot-cache tests to reject synchronous renderer dependency scans.
- [x] T048 Re-run focused material, Assimp parser, decal/frame-graph, drag/drop, GameObject cache, importer-version, and diff checks.
- [x] T049 Wire `MODEL_FBX_IGNORE_TEXTURED_NEUTRAL_DIFFUSE_TINT` through serialized importer settings, scene import settings, material conversion context, and material artifact identity.
- [x] T050 Add `LightGridCommon.hlsli` safe lighting normalization so deferred/clustered lighting cannot reintroduce NaNs from degenerate directions.
- [x] T051 Re-run focused material, Assimp parser, decal/frame-graph, drag/drop, GameObject cache, importer-version, and diff checks after final P1 fixes.
- [x] T052 Re-run required plan-review gate and deeper audit before final completion.

## Phase 14: FBX Decal Queue And Root Naming Follow-Up

**Goal**: Ensure current FBX Sponza-style decals enter the decal pass and generated FBX prefab roots use the source file name instead of Assimp's synthetic `RootNode`.

- [x] T053 Add failing FBX base-color-alpha decal material regression tests in `Tests/Unit/AssetMaterialConversionTests.cpp` covering alpha-evidence positive and missing-alpha-evidence negative cases.
- [x] T054 Add failing generated prefab single `RootNode` display-name regression test in `Tests/Unit/AssetPrefabPipelineTests.cpp`.
- [x] T055 Add importer-version regression proving version 7 FBX decal/root-name artifacts are stale.
- [x] T056 Update material conversion and external FBX import alpha-evidence flow so decal-named FBX parser materials with only an alpha-bearing diffuse/base-color texture serialize as blend `Decal` surfaces while non-alpha-evidence textured materials remain opaque.
- [x] T057 Update generated model prefab naming so single parser `RootNode` roots display the scene key/file stem while preserving hierarchy and deterministic IDs.
- [x] T058 Bump the model-scene importer version for the new FBX decal/root-name artifact semantics.
- [x] T059 Re-run focused material conversion, generated prefab, importer-version, diff-boundary, and required plan-review checks.

## Phase 15: Assimp FBX 3ds Max Opacity Compatibility

**Goal**: Surface 3ds Max FBX `Parameters` transparency/cutout texture connections as Nullus parser opacity channels without modifying Assimp source.

- [x] T060 Add failing Assimp FBX parser regression tests for `3dsMax|Parameters|transparency_map` and `3dsMax|Parameters|cutout_map` opacity textures.
- [x] T061 Implement the Nullus AssimpParser compatibility mapping from Assimp raw UNKNOWN FBX properties into the `opacity` parser channel and dependency list.
- [x] T062 Add importer-version regression proving version 8 artifacts are stale for the new opacity-map compatibility semantics.
- [x] T063 Re-run focused Assimp parser/importer-version tests, diff checks, and required plan-review gate.

## Dependencies

- T002 must fail before T003.
- T005 must run before any drag/drop production change.
- T007 depends on T003 and, if needed, T006.
- T011 must fail before T012.
- T013 depends on T012.
- T015 must fail before T016.
- T017 depends on T016.
- T018 must fail before T019.
- T020 depends on T019.
- T022 must fail before T023.
- T025 must fail before T026.
- T028 must fail before T029.
- T031 must fail before T032.
- T053, T054, and T055 must fail before T056, T057, and T058.
- T060 and T062 must fail before T061 and the importer-version bump.
