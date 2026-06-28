# Tasks: Unity-Aligned Asset Reuse

**Input**: Design documents from `/specs/052-unity-aligned-asset-reuse/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: Required. This feature changes editor asset import behavior; write the listed GoogleTest cases before implementation and verify each new test fails for the expected reason.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to
- Include exact file paths in descriptions

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish focused files and baseline verification without changing behavior.

- [x] T001 Run baseline focused tests for asset import pipeline with `ctest --test-dir build --output-on-failure -R NullusUnitTests` and record current status in implementation notes
- [x] T002 [P] Create resolver declarations in `Project/Editor/Assets/ModelTextureReferenceResolver.h`
- [x] T003 [P] Create resolver implementation skeleton in `Project/Editor/Assets/ModelTextureReferenceResolver.cpp`
- [x] T004 [P] Create resolution report declarations in `Project/Editor/Assets/ModelTextureResolutionReport.h`
- [x] T005 [P] Create resolution report implementation skeleton in `Project/Editor/Assets/ModelTextureResolutionReport.cpp`
- [x] T006 Confirm `Project/Editor/CMakeLists.txt` `GLOB_RECURSE ... CONFIGURE_DEPENDS` picks up the new files; do not edit CMake unless the local generator fails to discover them

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Shared settings, stable source identity, report, and resolver primitives required by all user stories.

**CRITICAL**: No user story integration should begin until this phase is complete.

- [x] T007 Add failing tests for model texture settings defaults, settings version handling, malformed values, and percent-encoded setting payloads including `;` and `|` in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T008 Implement `ModelTextureResolutionSettings`, `ModelEmbeddedTextureMode`, setting version constants, and setting key helpers in `Project/Editor/Assets/AssetImporterSettings.h`
- [x] T009 Implement model texture settings parsing/serialization helpers with percent encoding in `Project/Editor/Assets/AssetImporterSettings.cpp`
- [x] T010 Run `ctest --test-dir build --output-on-failure -R NullusUnitTests` and verify the settings tests pass
- [x] T011 Add failing tests for resolution report round-trip, schema version validation, escaped values including `;` and `|`, malformed report tolerance, and stale-context rejection in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T012 Implement `ModelTextureResolutionReport` serialization/parsing, report version checks, percent encoding, stale-context validation, and display-row helpers in `Project/Editor/Assets/ModelTextureResolutionReport.cpp`
- [x] T013 Run `ctest --test-dir build --output-on-failure -R NullusUnitTests` and verify the report tests pass
- [x] T014 Add failing tests for stable source key derivation covering duplicate names, empty `sourceKey`, display name changes with unchanged URI, path normalization, data URI, buffer-view textures, embedded texture ordinals, semicolon/pipe escaping, stable collision differentiators, order-derived fallback warnings, and repeated import stability in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T015 Implement `TextureSourceKind`, `ModelTextureSourceReference`, `materialTextureKey`, `stableDiscriminator`, `stableKeyStatus`, normalized URI helpers, and `mtxsrc:v1` stable key generation in `Project/Editor/Assets/ModelTextureReferenceResolver.h` and `Project/Editor/Assets/ModelTextureReferenceResolver.cpp`
- [x] T016 Add failing resolver unit tests for explicit-remap, source-path, unique-name, ambiguous-name, disabled-external, embedded-fallback, multi-root ordering, case collision, disabled auto-import, and auto-import failure decisions in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T017 Implement resolver input/output types, remap lookup, deterministic candidate ordering, unique-name rules, and fallback diagnostics in `Project/Editor/Assets/ModelTextureReferenceResolver.h` and `Project/Editor/Assets/ModelTextureReferenceResolver.cpp`
- [x] T018 Run `ctest --test-dir build --output-on-failure -R NullusUnitTests` and verify resolver/stable-key tests pass

**Checkpoint**: Foundation ready. Resolver, stable key, settings, and report logic work independently of full model import.

---

## Phase 3: User Story 1 - Reuse Shared Texture Assets During Model Import (Priority: P1) MVP

**Goal**: Multiple imported models that reference the same project texture resolve to the same texture asset artifact and stop generating duplicate model-owned texture sub-assets.

**Independent Test**: Import two model fixtures referencing the same project texture and verify material paths, manifests, dependencies, committed reports, and generated sub-assets.

### Tests for User Story 1

- [x] T019 [P] [US1] Add failing test `ExternalModelImportReusesProjectTextureAssetBySourcePath` in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T020 [P] [US1] Add failing test `ExternalModelImportAutoImportsMissingProjectTextureAssetWhenEnabled` in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T021 [P] [US1] Add failing test `ExternalModelImportAutoImportFailureWarnsAndFallsBack` in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T022 [P] [US1] Add failing test `ExternalModelImportKeepsModelLocalTextureForEmbeddedFallback` in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T023 [P] [US1] Add failing material-path compatibility tests for glTF base color/normal texture keys in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T024 [P] [US1] Add failing material-path compatibility tests for FBX diffuse/opacity and OBJ MTL texture keys in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T025 [P] [US1] Add failing dependency freshness tests for target texture meta changes, imported artifact changes, source path mapping changes, unique-name candidate-set changes, and same-count different GUID/path candidate sets in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T026 [P] [US1] Add failing committed-report tests for failed import preserving the previous report and successful reimport atomically replacing the report in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T027 [P] [US1] Add failing unsupported texture encoding diagnostic test proving the report is visible and externally resolved duplicate texture sub-assets are not created in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T028 [P] [US1] Add failing legacy reimport test proving externally resolved duplicate texture sub-assets disappear while mesh/material/prefab keys remain stable and embedded/unresolved textures remain model-owned in `Tests/Unit/AssetImportPipelineTests.cpp`

### Implementation for User Story 1

- [x] T029 [US1] Build `ModelTextureSourceReference` values from imported scene textures, preserving `sourceKey`, exact `materialTextureKey`, `TextureSourceKind`, normalized URI, buffer-view key, embedded index, stable discriminator, stable key status, and model-local payload status in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [x] T030 [US1] Build resolver candidates for source-path matches, unique-name matches, and bounded missing texture auto-import using configured project roots and texture-only asset type checks in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [x] T031 [US1] Replace model-owned texture path map construction with resolver-driven `materialTextureKey -> resourcePath` map in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [x] T032 [US1] Limit automatic missing texture import to project Texture assets and convert import failures to diagnostics in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [x] T033 [US1] Filter texture payload loading and `SerializeTextureSubAsset` calls to `ModelEmbeddedFallback` texture references only in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [x] T034 [US1] Add dependency records for `SourceAssetGuid`, `ImportedArtifact`, and `PathToGuidMapping` using the canonical fingerprint schemas in `specs/052-unity-aligned-asset-reuse/data-model.md` in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [x] T035 [US1] Persist the model texture resolution report as an editor-only sidecar with pending-write validation and successful-commit atomic replacement in `Project/Editor/Assets/ExternalAssetImporter.cpp` and `Project/Editor/Assets/ModelTextureResolutionReport.cpp`
- [x] T036 [US1] Preserve legacy non-texture sub-asset keys while pruning only externally resolved current-manifest texture sub-assets during model reimport in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [x] T037 [US1] Run `ctest --test-dir build --output-on-failure -R NullusUnitTests` and verify US1 tests pass

**Checkpoint**: User Story 1 is independently functional and testable.

---

## Phase 4: User Story 2 - Override Texture Resolution With Explicit Remaps (Priority: P2)

**Goal**: Users can bind a model texture source reference to a specific project texture asset, and the remap wins over automatic resolution.

**Independent Test**: Configure a model remap, reimport, and verify the selected texture asset is used even when source path or name candidates exist.

### Tests for User Story 2

- [x] T038 [P] [US2] Add failing test `ExternalModelImportExplicitTextureRemapOverridesSourcePath` in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T039 [P] [US2] Add failing test `ExternalModelImportInvalidTextureRemapWarnsAndFallsBack` in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T040 [P] [US2] Add failing test `ModelTextureRemapSettingsClearReturnsToAutomaticResolution` in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T041 [P] [US2] Add failing test `ModelTextureRemapSettingsRoundTripEscapedStableKeysAndRejectMalformedValues` in `Tests/Unit/AssetImportPipelineTests.cpp`

### Implementation for User Story 2

- [x] T042 [US2] Implement remap setting encode/decode, store, clear, malformed-value rejection, and settings fingerprint helpers in `Project/Editor/Assets/AssetImporterSettings.cpp`
- [x] T043 [US2] Teach `ModelTextureReferenceResolver` to validate remap target asset type and artifact availability in `Project/Editor/Assets/ModelTextureReferenceResolver.cpp`
- [x] T044 [US2] Surface invalid remap, malformed remap, non-texture target, missing artifact, order-derived stable key, and fallback diagnostics in model import diagnostics in `Project/Editor/Assets/ExternalAssetImporter.cpp`

**Checkpoint**: User Story 2 works independently after foundational resolver/report work.

---

## Phase 5: User Story 3 - Understand Texture Resolution Results In Asset Properties (Priority: P3)

**Goal**: Asset Properties shows model texture resolution settings, remap controls, last import resolution results, and warnings.

**Independent Test**: Select a model with known resolution results and confirm Asset Properties exposes settings and report rows; verify report parsing/view-model logic with automated tests and UI with quickstart manual steps.

### Tests for User Story 3

- [x] T046 [P] [US3] Add failing tests for Asset Properties report row extraction covering missing, stale, malformed, invalid-target, order-derived stable key, unsupported-encoding, and warning-bearing reports in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T047 [P] [US3] Add failing tests for model-level texture setting changes, apply/clear remap metadata writes, and reimport-required state in `Tests/Unit/AssetImportPipelineTests.cpp`

### Implementation for User Story 3

- [x] T048 [US3] Add model texture resolution setting controls to `Project/Editor/Panels/AssetProperties.cpp`
- [x] T049 [US3] Add External Textures/Remaps display group that reads only non-stale committed reports in `Project/Editor/Panels/AssetProperties.cpp`
- [x] T050 [US3] Add texture asset selection/remap write, clear behavior, invalid target display, order-derived warning display, and reimport-required state in `Project/Editor/Panels/AssetProperties.cpp`
- [x] T051 [US3] Add any needed declarations/state fields for the new Asset Properties group in `Project/Editor/Panels/AssetProperties.h`
- [x] T052 [US3] Run `ctest --test-dir build --output-on-failure -R NullusUnitTests` and verify US3 tests pass
- [ ] T053 [US3] Perform the manual Asset Properties verification steps from `specs/052-unity-aligned-asset-reuse/quickstart.md`

**Checkpoint**: All user stories are independently functional.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Final compatibility, documentation, and quality gates.

- [x] T054 [P] Update any affected developer documentation or comments in `Docs/` only if implementation exposes new user-visible workflow details not covered by the spec bundle
- [x] T055 Run full `NullusUnitTests` via `ctest --test-dir build --output-on-failure -R NullusUnitTests`
- [x] T056 Inspect `git diff` to confirm no generated files under `Runtime/*/Gen/` were edited
- [x] T057 Run `/plan-review` quality gate for the completed implementation before reporting completion or committing
- [x] T058 Update `specs/052-unity-aligned-asset-reuse/quickstart.md` with final exact validation commands if build directory or test filters differ during implementation

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 Setup**: No dependencies.
- **Phase 2 Foundational**: Depends on Phase 1 skeleton files and CMake registration.
- **US1 MVP**: Depends on Phase 2 resolver/report/settings/stable-key primitives.
- **US2 Remaps**: Depends on Phase 2 and can proceed after resolver primitives exist; it does not require US1 UI work.
- **US3 Asset Properties**: Depends on Phase 2 report/settings and benefits from US1/US2 import outputs.
- **Polish**: Depends on selected user stories being complete.

### Parallel Opportunities

- T002-T005 can be created in parallel.
- T019-T028 can be written in parallel once foundational test helpers exist.
- T038-T041 can be written in parallel once remap helper contracts are defined.
- T046-T047 can be written in parallel with importer integration if report helper contracts are stable.

### MVP Scope

MVP is Phase 1 + Phase 2 + Phase 3. It delivers automatic Unity-aligned external texture reuse by source path with model-local fallback, material key compatibility, committed reports, dependency freshness, legacy reimport behavior, and automated tests.

### Requirement Traceability

| Requirement | Covering Tasks |
|-------------|----------------|
| FR-001 Preserve non-texture model-owned identities | T019, T028, T029, T036, T037 |
| FR-002 Resolution priority order | T016, T017, T030, T031 |
| FR-003 Stable user-authored remaps | T007, T014, T015, T038, T041, T042 |
| FR-004 Remap only to texture assets | T039, T043, T044, T050 |
| FR-005 Clear remap returns to automatic resolution | T040, T042, T050 |
| FR-006 Avoid model-owned texture sub-assets for external project textures | T019, T031, T033, T037 |
| FR-007 Create model-owned texture sub-assets only for fallback payloads | T022, T033, T037 |
| FR-008 No global content-hash dedup | T016, T017, T030, T055 |
| FR-009 Auto-import missing project texture files when enabled | T020, T021, T030, T032, T037 |
| FR-010 Ambiguous name warnings without automatic binding | T016, T017, T044 |
| FR-011 Diagnostics for invalid/missing/stale/unsupported cases | T011, T012, T021, T027, T039, T044, T046, T049 |
| FR-012 Dependency information for texture assets/artifacts/path mappings | T025, T034, T037 |
| FR-013 Show references, targets, kinds, warnings in Asset Properties | T046, T048, T049, T052, T053 |
| FR-014 Existing models unchanged until reimport | T028, T036, T055 |
| FR-015 Legacy reimport removes duplicate model texture sub-assets only when externally resolved | T028, T033, T036, T037 |
| FR-016 Preserve fallback behavior when external resolution fails | T021, T022, T039, T044 |
| FR-017 Model-level texture resolution settings | T007, T008, T009, T047, T048 |

### Implementation Strategy

1. Establish focused resolver/report files.
2. Implement settings, stable-key, resolver, and report tests first.
3. Integrate resolver into model import for US1, including material key compatibility, dependency freshness, report commit semantics, and legacy reimport.
4. Add explicit remap behavior for US2.
5. Add Asset Properties visibility and remap editing for US3.
6. Run full validation and plan-review before completion.
