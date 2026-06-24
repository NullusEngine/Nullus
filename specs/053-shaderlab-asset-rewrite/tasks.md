# Tasks

## Phase 1: Tests

- [X] T001 Add parser unit tests in `Tests/Unit/ShaderLabParserTests.cpp`.
- [X] T002 Add material unit tests in `Tests/Unit/ShaderLabMaterialTests.cpp`.
- [X] T003 Add variant key unit tests in `Tests/Unit/ShaderLabVariantTests.cpp`.
- [X] T004 Add pass and pipeline key unit tests in `Tests/Unit/ShaderLabPassAndPipelineTests.cpp`.
- [X] T005 Add hot reload unit tests in `Tests/Unit/ShaderLabHotReloadTests.cpp`.
- [X] T006 Run targeted ShaderLab tests and confirm RED failure.

## Phase 2: Core Parser and Types

- [X] T010 Add source location, diagnostics, descriptor/runtime types.
- [X] T011 Implement lexer for ShaderLab syntax outside HLSL.
- [X] T012 Implement recursive-descent parser and raw HLSL extraction.
- [X] T013 Implement pragma extraction and `#line` compiler-source helper.

## Phase 3: Runtime Asset and Material

- [X] T020 Implement `ShaderLabAsset` snapshots, pass lookup, and pass handles.
- [X] T021 Implement stable property IDs and material typed values.
- [X] T022 Implement material defaults, Set/Get APIs, keywords, and reload migration.

## Phase 4: Variant, Binding, Pipeline, Hot Reload

- [X] T030 Implement keyword hashing, variant keys, artifact keys, and macro generation.
- [X] T031 Implement material binding layout from `ShaderReflection`.
- [X] T032 Implement ShaderLab graphics pipeline key hashing.
- [X] T033 Implement hot reload service with atomic success/failure behavior.

## Phase 5: Assets and Validation

- [X] T040 Add `NullusShaderLibrary` HLSL files.
- [X] T041 Add five ShaderLab example `.shader` files.
- [X] T042 Run targeted ShaderLab tests.
- [X] T043 Build `NullusUnitTests`.
- [X] T044 Run full available unit tests or document blockers.
- [X] T045 Run required plan-review quality gate.
- [X] T046 Final self-review and summary.

## Phase 6: Compile Key Factory

- [X] T050 Add ShaderLab artifact key builder tests.
- [X] T051 Implement ShaderLab artifact key builder from compiler input and fingerprints.
- [X] T052 Add ShaderLab compile input/request builder tests.
- [X] T053 Implement ShaderLab compile input/request builders from pass, keywords, backend, and fingerprints.
- [X] T054 Run targeted ShaderLab variant and ShaderLab suite tests.

## Phase 7: ShaderLab-Only Material Migration

- [X] T060 Add failing material conversion tests for ShaderLab payload schema and artifact paths.
- [X] T061 Convert imported material payload serialization from legacy `.mat` XML to ShaderLab material payloads.
- [X] T062 Change generated material artifact paths and loader IDs to ShaderLab-only material artifacts.
- [X] T063 Add ShaderLab material artifact loader/manager entrypoint.
- [X] T064 Migrate editor thumbnail and scene drag-preview material consumption to ShaderLab materials.
- [X] T065 Migrate renderer material binding to consume ShaderLab pass/material state.
- [X] T066 Remove legacy material/shader resource fallback references from imported material pipeline.
- [X] T067 Run targeted material conversion and ShaderLab tests.

## Phase 8: Authoritative Extensions and Library Artifact Store

- [X] T070 Make source extensions authoritative and unique: `.shader`, `.mat`, `.prefab`, and standard image/model source extensions only.
- [X] T071 Remove `n*` source and artifact extension compatibility from production paths.
- [X] T072 Store Library artifact payloads as unique hash filenames with no file extension.
- [X] T073 Ensure artifact type comes from ArtifactDB/manifest/importer/native container metadata, not file extension.
- [X] T074 Update tests and examples so Library artifact paths do not encode Prefab/Texture/Shader type in filenames.
- [X] T075 Run production scans for legacy artifact extension assumptions.
